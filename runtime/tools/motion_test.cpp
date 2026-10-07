// Gyro input (runtime/src/motion/): axis mapping of the SDL and Cemuhook sources against direction
// matrices recorded from a real GamePad, orientation integration and bias estimation, sensitivity and
// invert, how WWHD's first-person camera reads the result (frame-to-frame matrix change), the mouse as a
// gyro, the Cemuhook (DSU) packets and a client/server exchange over loopback UDP, and the settings.
#include "motion/dsu.h"
#include "motion/fusion.h"
#include "motion/motion.h"

#include <atomic>
#include <cassert>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
// (windows.h defines near and far as empty macros, min and max as macros)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

void log_msg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
namespace mods { double game_time() { return 0; } }

// explicit names (no using-directive): nothing from the system headers can make them ambiguous
using motion::Fusion;
using motion::Quat;
using motion::Settings;
using motion::Tuning;
using motion::Vec3;
using motion::VpadMotion;
using motion::controller_gone;
using motion::controller_sample;
using motion::from_dsu;
using motion::from_sdl;
using motion::kCemuhook;
using motion::kController;
using motion::kMouse;
using motion::kOff;
using motion::mouse_drives_gyro;
using motion::mouse_motion;
using motion::mouse_rate;
using motion::poll_recenter;
using motion::recenter;
using motion::set_aiming;
using motion::set_settings;
using motion::status;
using motion::to_ini;
using motion::from_kv;
using motion::vpad;
using motion::wants_controller_sensors;
static constexpr float kPi = 3.14159265358979f;
static bool close_to(float a, float b, float tol = 0.03f) { return std::fabs(a - b) < tol; }
static bool close_to(Vec3 a, Vec3 b, float tol = 0.03f) { return close_to(a.x, b.x, tol) && close_to(a.y, b.y, tol) && close_to(a.z, b.z, tol); }
static void print(const char* what, Vec3 v) { fprintf(stderr, "  %s = (%.3f, %.3f, %.3f)\n", what, v.x, v.y, v.z); }

// a physical motion of a controller, as SDL reports it: rotation rate (rad/s, SDL frame) and gravity
// following the pose. The pose starts flat on a table (SDL y up).
struct Controller {
    Quat pose;  // SDL body -> world (world = SDL frame of the flat controller: x right, y up, z towards player)
    Fusion f;
    Tuning t;
    void step(Vec3 rate_sdl, float dt, bool dsu = false) {
        pose = pose * Quat::axis_angle(rate_sdl, rate_sdl.length() * dt);
        pose.normalize();
        Vec3 up_body = pose.unrotate({0, 1, 0});  // specific force at rest: up, 1 g
        float g[3] = {rate_sdl.x, rate_sdl.y, rate_sdl.z};
        float a[3] = {up_body.x * 9.80665f, up_body.y * 9.80665f, up_body.z * 9.80665f};
        Vec3 gh, ah;
        if (dsu) {
            // DS4 conventions as DSU servers send them: deg/s, gyro (x, -y, -z), acceleration -a / g
            float gd[3] = {g[0] * 180 / kPi, -g[1] * 180 / kPi, -g[2] * 180 / kPi};
            float ad[3] = {-up_body.x, -up_body.y, -up_body.z};
            from_dsu(gd, ad, gh, ah);
        } else {
            from_sdl(g, a, gh, ah);
        }
        f.update(dt, gh, ah, t);
    }
    // turn about an SDL body axis by `deg` degrees in `secs` seconds at 250 Hz
    void turn(Vec3 axis, float deg, float secs = 0.5f, bool dsu = false) {
        int n = (int)(secs * 250);
        float rate = deg * kPi / 180 / secs;
        for (int i = 0; i < n; i++) step(axis * rate, 1.0f / 250, dsu);
    }
    void rest(float secs) { for (int i = 0; i < (int)(secs * 250); i++) step({}, 1.0f / 250); }
};

// WWHD's reading (dCamera_c::CalcSubjectAngle via 02618604): R = C^T M with the dir vectors as the
// matrix columns, C = the previous frame's matrix; yaw input = (R[2][0] - R[1][0]) * 30, pitch = R[2][1] * 30
struct Game {
    Vec3 prev[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float yaw = 0, pitch = 0;  // sums of the inputs
    void frame(const VpadMotion& m) {
        auto R = [&](int i, int j) { return prev[i].dot(m.dir[j]); };
        yaw += (R(2, 0) - R(1, 0)) * 30;
        pitch += R(2, 1) * 30;
        for (int i = 0; i < 3; i++) prev[i] = m.dir[i];
    }
};

static void test_recorded_poses() {
    // VPAD direction matrices recorded from a real GamePad (cemu/src/input/motion/MotionSample.h)
    {
        Controller c;
        VpadMotion m = c.f.vpad();
        assert(close_to(m.dir[0], {1, 0, 0}) && close_to(m.dir[1], {0, 1, 0}) && close_to(m.dir[2], {0, 0, 1}));
        c.rest(0.1f);
        m = c.f.vpad();
        assert(close_to(m.acc, {0, -1, 0}) && close_to(m.acc_magnitude, 1));
        assert(close_to(m.dir[0], {1, 0, 0}) && close_to(m.dir[1], {0, 1, 0}) && close_to(m.dir[2], {0, 0, 1}));
    }
    for (bool dsu : {false, true}) {
        // flat, turned 45 degrees to the right: x (0.71, -0.03, 0.71), z (-0.70, 0.14, 0.70)
        Controller c;
        c.turn({0, -1, 0}, 45, 0.5f, dsu);  // SDL yaw is counter-clockwise seen from above: right = negative
        VpadMotion m = c.f.vpad();
        print("right 45: dir x", m.dir[0]);
        assert(close_to(m.dir[0], {0.71f, 0, 0.71f}, 0.05f) && close_to(m.dir[1], {0, 1, 0}, 0.05f) && close_to(m.dir[2], {-0.71f, 0, 0.71f}, 0.05f));
        assert(m.gyro.y < 0 || m.gyro.y > 0);  // a rate is reported
    }
    {
        // tilted up 90 degrees (bottom edge on the table): y (-0.10, 0.01, -0.99), z (0.05, 1.00, 0.01)
        Controller c;
        c.turn({1, 0, 0}, 90, 0.5f);
        VpadMotion m = c.f.vpad();
        print("up 90: dir y", m.dir[1]);
        assert(close_to(m.dir[0], {1, 0, 0}, 0.05f) && close_to(m.dir[1], {0, 0, -1}, 0.05f) && close_to(m.dir[2], {0, 1, 0}, 0.05f));
        // acc (specific force, up): flat it is -y (y points into the table), now -z (z, towards the
        // holder, points down)
        print("up 90: acc", m.acc);
        assert(close_to(m.acc, {0, 0, -1}, 0.05f));
    }
    {
        // leaned 45 degrees on its left side: x (0.66, -0.75, -0.03), y (0.74, 0.66, -0.11)
        Controller c;
        c.turn({0, 0, 1}, 45, 0.5f);
        VpadMotion m = c.f.vpad();
        print("left side 45: dir x", m.dir[0]);
        assert(close_to(m.dir[0], {0.71f, -0.71f, 0}, 0.06f) && close_to(m.dir[1], {0.71f, 0.71f, 0}, 0.06f));
    }
}

static float g_up_pitch = 0;  // the game's pitch input for tilting the top up 30 degrees
static void test_game_reading() {
    // the game's yaw for a right turn has the same sign flat and held upright (screen towards the
    // player), pitch follows tilting the top up, and the totals match the turn (radians x 30)
    float flat_yaw, upright_yaw;
    {
        Controller c;
        Game g;
        g.frame(c.f.vpad());
        for (int i = 0; i < 30; i++) { c.turn({0, -1, 0}, 1, 1.0f / 30); g.frame(c.f.vpad()); }
        flat_yaw = g.yaw;
        assert(close_to(g.pitch, 0, 0.05f));
    }
    {
        Controller c;
        c.turn({1, 0, 0}, 90, 0.5f);  // stand it up, screen towards the player
        Game g;
        g.frame(c.f.vpad());
        // a right turn about the world vertical: the SDL body axis that now points up is -z
        Quat inv = c.pose.conj();
        Vec3 world_up_in_body = inv.rotate({0, 1, 0});
        for (int i = 0; i < 30; i++) { c.turn(world_up_in_body * -1.0f, 1, 1.0f / 30); g.frame(c.f.vpad()); }
        upright_yaw = g.yaw;
    }
    fprintf(stderr, "  game yaw input for 30 degrees right: flat %.3f, upright %.3f\n", flat_yaw, upright_yaw);
    assert(std::fabs(flat_yaw) > 0.5f * 30 * 30 * kPi / 180 && flat_yaw * upright_yaw > 0);
    assert(close_to(flat_yaw, upright_yaw, 0.1f * std::fabs(flat_yaw)));
    {
        Controller c;
        Game g;
        g.frame(c.f.vpad());
        for (int i = 0; i < 30; i++) { c.turn({1, 0, 0}, 1, 1.0f / 30); g.frame(c.f.vpad()); }
        fprintf(stderr, "  game pitch input for 30 degrees up: %.3f\n", g.pitch);
        assert(std::fabs(std::fabs(g.pitch) - 30 * 30 * kPi / 180) < 1.0f && close_to(g.yaw, 0, 0.05f));
        g_up_pitch = g.pitch;
    }
    {
        // sensitivity and invert: twice the yaw, inverted pitch
        Controller c;
        c.t.sensitivity_x = 2;
        c.t.invert_y = true;
        Game g;
        g.frame(c.f.vpad());
        for (int i = 0; i < 30; i++) { c.turn({0, -1, 0}, 0.5f, 1.0f / 30); g.frame(c.f.vpad()); }
        assert(close_to(g.yaw, flat_yaw, 0.08f * std::fabs(flat_yaw)));
        for (int i = 0; i < 30; i++) { c.turn({1, 0, 0}, 1, 1.0f / 30); g.frame(c.f.vpad()); }
        assert(g.pitch * g_up_pitch < 0 && std::fabs(g.pitch) > 0.8f * std::fabs(g_up_pitch));
        c.t.invert_x = true;
        float before = g.yaw;
        for (int i = 0; i < 30; i++) { c.turn({0, -1, 0}, 0.5f, 1.0f / 30); g.frame(c.f.vpad()); }
        assert((g.yaw - before) * flat_yaw < 0);
    }
}

static void test_bias_and_noise() {
    // a controller at rest with a gyro offset (Switch Pro: up to 5 deg/s): after resting the offset is
    // learnt, the camera does not drift, and the VPAD rate is zero
    Controller c;
    Vec3 bias_sdl{0.09f, -0.06f, 0.02f};
    Game g;
    for (int i = 0; i < 250 * 4; i++) {
        float gy[3] = {bias_sdl.x, bias_sdl.y, bias_sdl.z}, a[3] = {0, 9.80665f, 0};
        Vec3 gh, ah;
        from_sdl(gy, a, gh, ah);
        c.f.update(1.0f / 250, gh, ah, c.t);
        if (i % 8 == 0 && i > 250 * 3) g.frame(c.f.vpad());
        if (i == 250 * 3) { c.f.vpad(); g.frame(c.f.vpad()); g.yaw = g.pitch = 0; }
    }
    print("bias", c.f.bias());
    assert(c.f.calibrated());
    assert(close_to(c.f.bias(), {bias_sdl.x, -bias_sdl.y, -bias_sdl.z}, 0.005f));
    VpadMotion m = c.f.vpad();
    assert(close_to(m.gyro, {0, 0, 0}, 0.002f));
    fprintf(stderr, "  drift over the last second: yaw %.4f pitch %.4f\n", g.yaw, g.pitch);
    assert(std::fabs(g.yaw) < 0.05f && std::fabs(g.pitch) < 0.05f);
    // slow aiming after calibration does not move the bias
    Vec3 b = c.f.bias();
    c.turn({0, -1, 0}, 20, 2.0f);  // 10 deg/s
    assert(close_to(c.f.bias(), b, 0.002f));
    // recenter: forward again, tilt kept
    c.turn({1, 0, 0}, 20, 0.2f);
    c.f.recenter();
    m = c.f.vpad();
    assert(close_to(m.dir[0], {1, 0, 0}, 0.05f) && close_to(m.angle, {0, 0, 0}, 1e-4f));
}

static void test_mouse() {
    // the mouse turns the virtual GamePad like a right turn / tilting up of the real one
    Fusion f;
    Tuning t;
    Game g;
    g.frame(f.vpad());
    for (int i = 0; i < 30; i++) {
        Vec3 w = mouse_rate(f.orientation(), 10, 0, 1.0f / 30, 0.1f);  // 10 points right per frame: 1 degree
        f.update(1.0f / 30, w, {}, t);
        g.frame(f.vpad());
    }
    fprintf(stderr, "  mouse: 300 points right -> game yaw %.3f\n", g.yaw);
    Controller c;
    Game gc;
    gc.frame(c.f.vpad());
    for (int i = 0; i < 30; i++) { c.turn({0, -1, 0}, 1, 1.0f / 30); gc.frame(c.f.vpad()); }
    assert(close_to(g.yaw, gc.yaw, 0.05f * std::fabs(gc.yaw)));
    float yaw = g.yaw;
    for (int i = 0; i < 30; i++) {
        Vec3 w = mouse_rate(f.orientation(), 0, -10, 1.0f / 30, 0.1f);  // up
        f.update(1.0f / 30, w, {}, t);
        g.frame(f.vpad());
    }
    fprintf(stderr, "  mouse: 300 points up -> game pitch %.3f\n", g.pitch);
    assert(close_to(g.pitch, g_up_pitch, 0.05f * std::fabs(g_up_pitch)) && close_to(g.yaw, yaw, 0.1f));
    // the mouse source has no accelerometer: acc follows the pose
    VpadMotion m = f.vpad();
    assert(close_to(m.acc_magnitude, 1, 0.01f));
}

static void test_dsu_packets() {
    assert(dsu::crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u);
    auto info = dsu::encode_port_info_request(7, {0});
    assert(info.size() == 25 && !memcmp(info.data(), "DSUC", 4) && info[4] == 0xE9 && info[5] == 0x03 && info[6] == 9);
    auto req = dsu::encode_pad_data_request(7, 2);
    assert(req.size() == 28 && req[16] == 0x02 && req[18] == 0x10 && req[20] == 1 && req[21] == 2);
    // a pad data packet as DS4Windows sends it (built here with the same layout)
    dsu::PadData d;
    d.slot = 1; d.state = 2; d.model = 2; d.connected = true; d.packet = 1234; d.timestamp_us = 0x0123456789ull;
    d.accel[0] = 0.01f; d.accel[1] = -0.98f; d.accel[2] = 0.12f;
    d.gyro[0] = 1.5f; d.gyro[1] = -90.0f; d.gyro[2] = 0.25f;
    auto pkt = dsu::encode_pad_data(99, d);
    assert(pkt.size() == dsu::kPadDataSize && !memcmp(pkt.data(), "DSUS", 4));
    assert(pkt[6] == 84 && pkt[7] == 0);  // length after the header
    // the fixed offsets of the protocol: packet counter at 32, timestamp at 68, accel at 76, gyro at 88
    assert(pkt[32] == (1234 & 0xFF) && pkt[33] == (1234 >> 8) && pkt[68] == 0x89 && pkt[72] == 0x01);
    float gy;
    memcpy(&gy, &pkt[92], 4);
    assert(gy == -90.0f);
    dsu::PadData out;
    assert(dsu::parse_pad_data(pkt.data(), pkt.size(), out));
    assert(out.slot == 1 && out.connected && out.packet == 1234 && out.timestamp_us == 0x0123456789ull);
    assert(out.accel[1] == -0.98f && out.gyro[1] == -90.0f && out.gyro[2] == 0.25f);
    // corrupted, truncated, client packets and other messages are refused
    auto bad = pkt;
    bad[80] ^= 1;
    assert(!dsu::parse_pad_data(bad.data(), bad.size(), out));
    assert(!dsu::parse_pad_data(pkt.data(), 60, out));
    assert(!dsu::parse_pad_data(req.data(), req.size(), out));
    assert(dsu::message_type(info.data(), info.size()) == 0);  // DSUC: not a server message
    bad = pkt;
    bad[16] = 0x01;  // port info type, CRC redone: a valid message but not pad data
    bad[8] = bad[9] = bad[10] = bad[11] = 0;
    uint32_t crc = dsu::crc32(bad.data(), bad.size());
    for (int i = 0; i < 4; i++) bad[8 + i] = (uint8_t)(crc >> (8 * i));
    assert(dsu::message_type(bad.data(), bad.size()) == dsu::kPortInfo && !dsu::parse_pad_data(bad.data(), bad.size(), out));
    assert(!dsu::parse_pad_data(nullptr, 0, out));
}

static void test_dsu_loopback() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    using sock = SOCKET;
#else
    using sock = int;
#endif
    // a tiny DSU server on a free loopback port: answers each pad data request with a sample
    sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    assert(bind(s, (sockaddr*)&a, sizeof a) == 0);
    socklen_t len = sizeof a;
    getsockname(s, (sockaddr*)&a, &len);
    const uint16_t port = ntohs(a.sin_port);
#ifdef _WIN32
    DWORD tv = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
#else
    timeval tv{0, 100000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    std::atomic<int> got{0};
    dsu::PadData last;
    std::mutex mu;
    dsu::Client client([&](const dsu::PadData& d) { std::lock_guard lk(mu); last = d; got++; });
    client.start("127.0.0.1", port, 0);
    int requests = 0;
    uint32_t n = 0;
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (got < 3 && std::chrono::steady_clock::now() < until) {
        uint8_t buf[256];
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        int r = (int)recvfrom(s, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (r == 28 && !memcmp(buf, "DSUC", 4) && buf[16] == 0x02) {
            requests++;
            for (int i = 0; i < 3; i++) {  // a burst, as a 250 Hz server sends between requests
                dsu::PadData d;
                d.slot = 0; d.state = 2; d.model = 2; d.connected = true; d.packet = ++n; d.timestamp_us = 1000 + 4000 * n;
                d.gyro[1] = 45;
                d.accel[1] = -1;
                auto p = dsu::encode_pad_data(5, d);
                sendto(s, (const char*)p.data(), (int)p.size(), 0, (sockaddr*)&from, fl);
            }
        }
    }
    assert(requests >= 1 && got >= 3);
    {
        std::lock_guard lk(mu);
        assert(last.gyro[1] == 45 && last.packet >= 3);
    }
    assert(client.receiving());
    auto t0 = std::chrono::steady_clock::now();
    client.stop();
    assert(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));  // never hangs
    assert(!client.running());
    // a server that is not there: the client keeps asking, stop() still returns quickly
    client.start("127.0.0.1", 9, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(!client.receiving());
    t0 = std::chrono::steady_clock::now();
    client.stop();
    assert(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static void test_settings_and_sources() {
    Settings s;
    assert(s.source == kOff);  // off by default
    s.source = kCemuhook;
    s.tuning.sensitivity_x = 1.5f;
    s.tuning.invert_y = true;
    s.dsu_host = "192.168.1.20";
    s.dsu_port = 26761;
    s.dsu_slot = 2;
    s.recenter_pad = 13;
    s.recenter_key = 15;
    s.mouse_degrees = 0.25f;
    std::string ini = to_ini(s);
    Settings r;
    size_t at = 0;
    while (at < ini.size()) {
        size_t nl = ini.find('\n', at), eq = ini.find('=', at);
        from_kv(r, ini.substr(at, eq - at), ini.substr(eq + 1, nl - eq - 1));
        at = nl + 1;
    }
    assert(r == s);
    from_kv(r, "gyro.sensitivityX", "99");
    from_kv(r, "gyro.dsuPort", "junk");
    from_kv(r, "gyro.source", "nonsense");
    assert(r.tuning.sensitivity_x == 5.0f && r.dsu_port == 26760 && r.source == kCemuhook);

    // the mouse source: movement counts only while the game aims
    Settings m;
    m.source = kMouse;
    m.mouse_degrees = 0.1f;
    set_settings(m);
    vpad(false);
    mouse_motion(100, 0);
    VpadMotion a = vpad(false);
    assert(close_to(a.dir[0], {1, 0, 0}, 1e-4f));  // not aiming: ignored
    assert(!mouse_drives_gyro());
    set_aiming(true);
    assert(mouse_drives_gyro());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mouse_motion(100, 0);  // 10 degrees right
    a = vpad(false);
    print("mouse 10 deg: dir x", a.dir[0]);
    assert(close_to(a.dir[0], {std::cos(10 * kPi / 180), 0, std::sin(10 * kPi / 180)}, 0.01f));
    VpadMotion b = vpad(true);  // a repeated read repeats
    assert(close_to(b.dir[0], a.dir[0], 1e-6f));
    recenter();
    a = vpad(false);
    assert(close_to(a.dir[0], {1, 0, 0}, 1e-3f));
    // recenter binding: rising edge only
    m.recenter_key = 15;
    set_settings(m);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mouse_motion(100, 0);
    a = vpad(false);
    assert(!close_to(a.dir[0], {1, 0, 0}, 1e-3f));
    bool keys[256] = {};
    keys[15] = true;
    poll_recenter(nullptr, keys);
    a = vpad(false);
    assert(close_to(a.dir[0], {1, 0, 0}, 1e-3f));
    set_aiming(false);
    // the controller source: samples from SDL, the latest active controller wins
    Settings c;
    c.source = kController;
    set_settings(c);
    assert(wants_controller_sensors());
    float g[3] = {0, -kPi / 2, 0}, acc[3] = {0, 9.80665f, 0};  // 90 deg/s right
    uint64_t t = 1000000000ull;
    vpad(false);
    for (int i = 0; i < 100; i++) controller_sample(42, t += 4000000, g, acc);  // 0.4 s: 36 degrees
    a = vpad(false);
    assert(close_to(a.dir[0], {std::cos(36 * kPi / 180), 0, std::sin(36 * kPi / 180)}, 0.03f));
    controller_gone(42);
    a = vpad(false);
    assert(close_to(a.dir[0], {1, 0, 0}, 1e-4f));  // gone: at rest
    // the Cemuhook source starts and stops its client (no server on port 9: it keeps asking)
    Settings d;
    d.source = kCemuhook;
    d.dsu_port = 9;
    set_settings(d);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    fprintf(stderr, "  %s\n", status().c_str());
    assert(status().rfind("Cemuhook: ", 0) == 0);
    d.dsu_port = 10;
    set_settings(d);  // restarts on the new port
    a = vpad(false);
    assert(close_to(a.dir[0], {1, 0, 0}, 1e-4f));
    set_settings(Settings{});
    assert(status().rfind("Gyro off", 0) == 0);
    assert(!wants_controller_sensors());
    for (int i = 0; i < 10; i++) controller_sample(42, t += 4000000, g, acc);
    a = vpad(false);
    assert(close_to(a.dir[0], {1, 0, 0}, 1e-4f));  // off: ignored
}

int main() {
    test_recorded_poses();
    test_game_reading();
    test_bias_and_noise();
    test_mouse();
    test_dsu_packets();
    test_dsu_loopback();
    test_settings_and_sources();
    puts("motion_test: axis mapping (recorded GamePad poses), game reading, bias, sensitivity/invert, mouse, DSU packets and loopback, settings passed");
}
