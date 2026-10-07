// Gyro (motion) input for the virtual GamePad (see motion.h).
#include "motion.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "dsu.h"

void log_msg(const char* fmt, ...);
namespace mods { double game_time(); }

namespace motion {
namespace {

std::mutex g_mu;
Settings g_settings;
bool g_env_override = false;
bool g_aiming = false;
int g_gyro_controllers = 0;
Fusion g_mouse;
float g_mouse_dx = 0, g_mouse_dy = 0;

struct Device {
    Fusion fusion;
    uint64_t last_ns = 0;
    std::chrono::steady_clock::time_point seen;
};
std::map<uint64_t, Device> g_devices;   // controllers (key: host device id) and the DSU slot (key kDsuDevice)
uint64_t g_active = ~0ull;              // the device VPAD reports
constexpr uint64_t kDsuDevice = ~1ull;
std::unique_ptr<dsu::Client> g_dsu;
VpadMotion g_last;                      // the previous read's values (repeated reads)
std::chrono::steady_clock::time_point g_last_read{};
float g_recenter_prev[2] = {};          // pad, key: held at the previous poll
Fusion g_test;                          // WWHD_TEST_GYRO

double secs(std::chrono::steady_clock::duration d) { return std::chrono::duration<double>(d).count(); }

// a sample from `id` (g_mu held); the most recently active device is the one VPAD shows, a quiet one
// (no samples for half a second) gives way to the next
void feed(uint64_t id, uint64_t t_ns, Vec3 gyro_h, Vec3 acc_h) {
    auto now = std::chrono::steady_clock::now();
    Device& d = g_devices[id];
    float dt = d.last_ns && t_ns > d.last_ns ? (float)((t_ns - d.last_ns) * 1e-9) : 0.0f;
    if (!d.last_ns) dt = 0;
    d.last_ns = t_ns;
    d.seen = now;
    if (g_active == ~0ull || !g_devices.count(g_active) || secs(now - g_devices[g_active].seen) > 0.5) {
        if (g_active != id) log_msg("[gyro] motion from %s", id == kDsuDevice ? "the Cemuhook server" : "a controller");
        g_active = id;
    }
    d.fusion.update(dt, gyro_h, acc_h, g_settings.tuning);
}

void dsu_sink(const dsu::PadData& p) {
    Vec3 g, a;
    from_dsu(p.gyro, p.accel, g, a);
    uint64_t t = p.timestamp_us ? p.timestamp_us * 1000 : (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    std::lock_guard lk(g_mu);
    if (g_settings.source == kCemuhook) feed(kDsuDevice, t, g, a);
}

// the Cemuhook client starts and stops outside g_mu: its thread's sink takes g_mu, and stop() joins it
std::mutex g_dsu_mu;
void apply_dsu(const Settings& s) {
    std::lock_guard lk(g_dsu_mu);
    if (s.source == kCemuhook) {
        if (!g_dsu) g_dsu = std::make_unique<dsu::Client>(dsu_sink);
        g_dsu->start(s.dsu_host, (uint16_t)std::clamp(s.dsu_port, 1, 65535), (uint8_t)std::clamp(s.dsu_slot, 0, 3));
    } else if (g_dsu) {
        g_dsu->stop();
    }
}

// WWHD_TEST_GYRO=from-to:yaw:pitch,... (game-time seconds, degrees per second)
struct TestTurn { double from, to; float yaw, pitch; };
const std::vector<TestTurn>& test_turns() {
    static const std::vector<TestTurn> v = [] {
        std::vector<TestTurn> out;
        const char* e = getenv("WWHD_TEST_GYRO");
        double a, b; float y, p; int n;
        while (e && sscanf(e, "%lf-%lf:%f:%f%n", &a, &b, &y, &p, &n) == 4) {
            out.push_back({a, b, y, p});
            e += n;
            if (*e != ',') break;
            e++;
        }
        if (!out.empty()) log_msg("[gyro] WWHD_TEST_GYRO: %zu turn(s)", out.size());
        return out;
    }();
    return v;
}

}  // namespace

const char* source_id(int s) {
    static const char* ids[] = {"off", "controller", "cemuhook", "mouse"};
    return s >= 0 && s < kSourceCount ? ids[s] : "off";
}
const char* source_label(int s) {
    static const char* labels[] = {"Off", "Controller gyro", "Cemuhook (DSU)", "Mouse (Steam Input gyro to mouse)"};
    return s >= 0 && s < kSourceCount ? labels[s] : "Off";
}
int source_from_id(const std::string& id) {
    for (int i = 0; i < kSourceCount; i++)
        if (id == source_id(i)) return i;
    return -1;
}

std::string value_of(const Settings& s, const std::string& k) {
    auto f = [](float x) { char b[32]; snprintf(b, sizeof b, "%g", x); return std::string(b); };
    if (k == "gyro.source") return source_id(s.source);
    if (k == "gyro.sensitivityX") return f(s.tuning.sensitivity_x);
    if (k == "gyro.sensitivityY") return f(s.tuning.sensitivity_y);
    if (k == "gyro.invertX") return s.tuning.invert_x ? "1" : "0";
    if (k == "gyro.invertY") return s.tuning.invert_y ? "1" : "0";
    if (k == "gyro.mouseDegrees") return f(s.mouse_degrees);
    if (k == "gyro.dsuHost") return s.dsu_host;
    if (k == "gyro.dsuPort") return std::to_string(s.dsu_port);
    if (k == "gyro.dsuSlot") return std::to_string(s.dsu_slot);
    if (k == "gyro.recenterPad") return std::to_string(s.recenter_pad);
    if (k == "gyro.recenterKey") return std::to_string(s.recenter_key);
    return {};
}
std::string to_ini(const Settings& s) {
    std::string out;
    for (const char* k : kKeys) out += std::string(k) + "=" + value_of(s, k) + "\n";
    return out;
}
void from_kv(Settings& s, const std::string& k, const std::string& v) {
    auto num = [&](float lo, float hi, float def) {
        char* end = nullptr;
        float x = strtof(v.c_str(), &end);
        return end && end != v.c_str() && x == x ? std::clamp(x, lo, hi) : def;
    };
    if (k == "gyro.source") { int i = source_from_id(v); if (i >= 0) s.source = i; }
    else if (k == "gyro.sensitivityX") s.tuning.sensitivity_x = num(0.1f, 5.0f, 1.0f);
    else if (k == "gyro.sensitivityY") s.tuning.sensitivity_y = num(0.1f, 5.0f, 1.0f);
    else if (k == "gyro.invertX") s.tuning.invert_x = v == "1";
    else if (k == "gyro.invertY") s.tuning.invert_y = v == "1";
    else if (k == "gyro.mouseDegrees") s.mouse_degrees = num(0.005f, 2.0f, 0.1f);
    else if (k == "gyro.dsuHost") { if (!v.empty() && v.size() < 256) s.dsu_host = v; }
    else if (k == "gyro.dsuPort") s.dsu_port = (int)num(1, 65535, 26760);
    else if (k == "gyro.dsuSlot") s.dsu_slot = (int)num(0, 3, 0);
    else if (k == "gyro.recenterPad") s.recenter_pad = (int)num(0, 64, 0);
    else if (k == "gyro.recenterKey") s.recenter_key = (int)num(-1, 255, -1);
}

Settings settings() {
    std::lock_guard lk(g_mu);
    return g_settings;
}

bool env_override() {
    static const bool e = [] {
        const char* v = getenv("WWHD_GYRO");
        return v && source_from_id(v) >= 0;
    }();
    return e;
}

void set_settings(const Settings& in) {
    Settings s = in;
    if (env_override()) s.source = source_from_id(getenv("WWHD_GYRO"));
    bool restart_dsu;
    {
        std::lock_guard lk(g_mu);
        const bool source_changed = s.source != g_settings.source;
        const bool dsu_changed = s.dsu_host != g_settings.dsu_host || s.dsu_port != g_settings.dsu_port || s.dsu_slot != g_settings.dsu_slot;
        g_settings = s;
        if (source_changed) {
            log_msg("[gyro] source: %s", source_label(s.source));
            g_devices.clear();
            g_active = ~0ull;
            g_mouse.reset();
            g_mouse_dx = g_mouse_dy = 0;
        }
        restart_dsu = source_changed || (dsu_changed && s.source == kCemuhook);
    }
    if (!restart_dsu && s.source == kCemuhook) {
        std::lock_guard lk(g_dsu_mu);
        restart_dsu = !(g_dsu && g_dsu->running());
    }
    if (restart_dsu) apply_dsu(s);
}

void controller_sample(uint64_t device, uint64_t t_ns, const float gyro[3], const float accel[3]) {
    Vec3 g, a;
    from_sdl(gyro, accel, g, a);
    if (!t_ns) t_ns = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    std::lock_guard lk(g_mu);
    if (g_settings.source == kController) feed(device, t_ns, g, a);
}
void controller_gone(uint64_t device) {
    std::lock_guard lk(g_mu);
    g_devices.erase(device);
    if (g_active == device) g_active = ~0ull;
}
bool wants_controller_sensors() {
    std::lock_guard lk(g_mu);
    return g_settings.source == kController;
}
void set_gyro_controllers(int n) {
    std::lock_guard lk(g_mu);
    if (n != g_gyro_controllers) log_msg("[gyro] controllers with motion sensors: %d", n);
    g_gyro_controllers = n;
}
int gyro_controllers() {
    std::lock_guard lk(g_mu);
    return g_gyro_controllers;
}

void mouse_motion(float dx, float dy) {
    std::lock_guard lk(g_mu);
    if (g_settings.source != kMouse || !g_aiming) return;
    g_mouse_dx += dx;
    g_mouse_dy += dy;
}
bool mouse_drives_gyro() {
    std::lock_guard lk(g_mu);
    return g_settings.source == kMouse && g_aiming;
}
void set_aiming(bool a) {
    std::lock_guard lk(g_mu);
    if (a != g_aiming && g_settings.source != kOff) log_msg("[gyro] the game %s", a ? "aims (gyro in use)" : "stopped aiming");
    // the mouse source starts every aim from the rest pose, as the game recentres its aim on entry
    if (a && !g_aiming) { g_mouse.reset(); g_mouse_dx = g_mouse_dy = 0; }
    g_aiming = a;
}
bool aiming() {
    std::lock_guard lk(g_mu);
    return g_aiming;
}

void recenter() {
    std::lock_guard lk(g_mu);
    for (auto& [id, d] : g_devices) d.fusion.recenter();
    g_mouse.reset();
    log_msg("[gyro] recentred");
}

void poll_recenter(const float* pad, const bool* keys) {
    int p, k;
    {
        std::lock_guard lk(g_mu);
        if (g_settings.source == kOff) return;
        p = g_settings.recenter_pad;
        k = g_settings.recenter_key;
    }
    float now[2] = {p > 0 && pad ? pad[p] : 0.0f, k >= 0 && k < 256 && keys && keys[k] ? 1.0f : 0.0f};
    bool press = false;
    for (int i = 0; i < 2; i++) {
        if (now[i] > 0.5f && g_recenter_prev[i] <= 0.5f) press = true;
        g_recenter_prev[i] = now[i];
    }
    if (press) recenter();
}

VpadMotion vpad(bool repeat) {
    std::lock_guard lk(g_mu);
    if (repeat) return g_last;
    auto now = std::chrono::steady_clock::now();
    float dt = g_last_read.time_since_epoch().count() ? (float)secs(now - g_last_read) : 0.0f;
    g_last_read = now;
    VpadMotion m;  // at rest, flat
    if (!test_turns().empty()) {
        double t = mods::game_time();
        float yaw = 0, pitch = 0;
        for (auto& tt : test_turns())
            if (t >= tt.from && t < tt.to) yaw = tt.yaw, pitch = tt.pitch;
        // degrees per second -> points with 1 degree per point
        Vec3 w = mouse_rate(g_test.orientation(), yaw * dt, -pitch * dt, dt, 1.0f);
        g_test.update(dt, w, {}, g_settings.tuning);
        m = g_test.vpad();
    } else if (g_settings.source == kMouse) {
        Vec3 w = mouse_rate(g_mouse.orientation(), g_mouse_dx, g_mouse_dy, dt, g_settings.mouse_degrees);
        g_mouse_dx = g_mouse_dy = 0;
        if (dt > 0) g_mouse.update(dt, w, {}, g_settings.tuning);
        m = g_mouse.vpad();
    } else if (g_settings.source == kController || g_settings.source == kCemuhook) {
        auto it = g_devices.find(g_active);
        if (it != g_devices.end() && secs(now - it->second.seen) < 1.0) m = it->second.fusion.vpad();
        else if (it != g_devices.end()) m.dir[0] = g_last.dir[0], m.dir[1] = g_last.dir[1], m.dir[2] = g_last.dir[2], m.angle = g_last.angle;
    }
    g_last = m;
    return m;
}

std::string status() {
    if (settings().source == kCemuhook) {  // (g_dsu_mu without g_mu: see apply_dsu)
        std::lock_guard dl(g_dsu_mu);
        return g_dsu ? "Cemuhook: " + g_dsu->status() + "." : "Cemuhook: off.";
    }
    std::lock_guard lk(g_mu);
    switch (g_settings.source) {
    case kController: {
        auto it = g_devices.find(g_active);
        if (it != g_devices.end() && secs(std::chrono::steady_clock::now() - it->second.seen) < 1.0)
            return it->second.fusion.calibrated() ? "Receiving motion." : "Receiving motion; calibrating (rest the controller for a second).";
        return g_gyro_controllers ? "A controller with a gyro is connected, no motion yet."
                                  : "No connected controller has a gyro (or this host cannot read it).";
    }
    case kMouse: return g_aiming ? "The game aims: the mouse turns the GamePad." : "Mouse gyro waits for the game to aim.";
    default: return "Gyro off: the GamePad lies still.";
    }
}

}  // namespace motion
