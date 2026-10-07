// Switch input: Joy-Cons / Pro Controller read as a Wii U Pro Controller (WPAD/KPAD, see
// hle/padscore.cpp) so the game puts everything on the one TV screen; WWHD_PRO_CONTROLLER=0 in
// env.txt makes them act as the Wii U GamePad instead. Buttons map by position, which matches the
// Wii U's labels. Text prompts go through the system software keyboard.
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "../input.h"
#include "../input_map.h"
#include "../motion/motion.h"
#include "../overlay/overlay.h"
#include "../runtime.h"
#include "../gfx/switch_renderer.h"
#include "input_switch.h"

namespace mods {
void filter_pad(input::PadState&);
bool mouse_captured() { return false; }  // no mouse camera on the console
void mouse_release() {}
void mouse_init(void*) {}
bool host_key_down(uint16_t) { return false; }
}
namespace input {
namespace {
std::mutex g_mu;
PadState g_pad;
std::atomic<bool> g_pro{true};
float g_values[input_map::kPadCount] = {};  // the settings overlay's view of the controller (controller_values)
std::function<void(bool, std::u16string)> g_pending;
std::u16string g_initial;
int g_max_len = 0;

std::string utf8(const std::u16string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        uint8_t buf[4];
        ssize_t n = encode_utf8(buf, c);
        if (n > 0) out.append((const char*)buf, (size_t)n);
    }
    return out;
}
std::u16string utf16(const char* s) {
    std::u16string out;
    for (;;) {
        uint32_t c;
        ssize_t n = decode_utf8(&c, (const uint8_t*)s);
        if (n <= 0 || !c) break;
        s += n;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(char16_t(0xD800 + (c >> 10)));
            out.push_back(char16_t(0xDC00 + (c & 0x3FF)));
        } else
            out.push_back(char16_t(c));
    }
    return out;
}

void show_keyboard() {
    std::function<void(bool, std::u16string)> done;
    std::u16string initial;
    int max_len;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_pending) return;
        done = std::move(g_pending);
        g_pending = nullptr;
        initial = g_initial;
        max_len = g_max_len;
    }
    SwkbdConfig kbd;
    bool ok = false;
    std::u16string text;
    if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetInitialText(&kbd, utf8(initial).c_str());
        if (max_len > 0) swkbdConfigSetStringLenMax(&kbd, (u32)max_len);
        char out[512] = {};
        ok = R_SUCCEEDED(swkbdShow(&kbd, out, sizeof out));
        swkbdClose(&kbd);
        if (ok) text = utf16(out);
    }
    if (max_len >= 0 && text.size() > (size_t)max_len) text.resize((size_t)max_len);
    done(ok, text);
}
// ---- gyro: the controller's six-axis sensor feeds motion.h (the virtual GamePad's motion) while the
// controller source is chosen (Settings > Switch > Gyro aiming); the sensors run only then
struct Gyro {
    bool started = false;
    HidSixAxisSensorHandle handles[4];  // handheld, Pro Controller (full key), Joy-Con pair: left, right
    int current = -1;                   // the handle read now
    u64 last_sampling = 0;
    uint64_t t_ns = 0;                  // sensor time, from the samples' delta times
};
Gyro g_gyro;

void gyro_start(bool on) {
    if (on == g_gyro.started) return;
    if (on && !g_gyro.t_ns) {
        hidGetSixAxisSensorHandles(&g_gyro.handles[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        hidGetSixAxisSensorHandles(&g_gyro.handles[1], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
        hidGetSixAxisSensorHandles(&g_gyro.handles[2], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
        g_gyro.t_ns = 1;
    }
    for (auto& h : g_gyro.handles) {
        const Result rc = on ? hidStartSixAxisSensor(h) : hidStopSixAxisSensor(h);
        if (R_FAILED(rc)) LOG("[gyro] six-axis sensor %s: rc 0x%x", on ? "start" : "stop", rc);
    }
    g_gyro.started = on;
    g_gyro.current = -1;
    LOG("[gyro] Switch motion sensors %s", on ? "on" : "off");
}

// HOS axes (x right, y forward, z up; rotations per second, g) -> SDL's (x right, y up, z toward the
// player; rad/s, m/s^2), which motion.cpp expects; at rest, lying flat, HOS reads acceleration (0, 0, -1)
void gyro_sample(int device, const HidSixAxisSensorState& st) {
    constexpr float kTau = 6.28318531f, kG = 9.80665f;
    const float gyro[3] = {st.angular_velocity.x * kTau, st.angular_velocity.z * kTau, -st.angular_velocity.y * kTau};
    const float accel[3] = {-st.acceleration.x * kG, -st.acceleration.z * kG, st.acceleration.y * kG};
    // delta_time is in ns (200 Hz: ~5 ms); clamped against a missing or bogus value
    g_gyro.t_ns += std::clamp<u64>(st.delta_time, 1000000, 50000000);
    motion::controller_sample(0x5357000 + device, g_gyro.t_ns, gyro, accel);
}

void update_gyro(::PadState& pad) {
    gyro_start(motion::wants_controller_sensors());
    const u32 style = padGetStyleSet(&pad);
    int which = -1;
    if (style & HidNpadStyleTag_NpadHandheld) which = 0;
    else if (style & HidNpadStyleTag_NpadFullKey) which = 1;
    else if (style & HidNpadStyleTag_NpadJoyDual)  // the right Joy-Con when there is one (it aims)
        which = (padGetAttributes(&pad) & HidNpadAttribute_IsRightConnected) ? 3 : 2;
    static int controllers = -1;
    if (const int n = which >= 0; n != controllers) motion::set_gyro_controllers(controllers = n);
    if (!g_gyro.started || which < 0) return;
    if (which != g_gyro.current) {
        g_gyro.current = which;
        g_gyro.last_sampling = 0;
        LOG("[gyro] reading the %s", which == 0 ? "handheld Joy-Con" : which == 1 ? "Pro Controller" : which == 3 ? "right Joy-Con" : "left Joy-Con");
    }
    // the samples since the last read (200 Hz, ~7 per 30 fps frame), oldest first
    HidSixAxisSensorState states[16];
    const size_t n = hidGetSixAxisSensorStates(g_gyro.handles[which], states, 16);
    for (size_t i = n; i-- > 0;) {
        if (states[i].sampling_number <= g_gyro.last_sampling && g_gyro.last_sampling) continue;
        if (!(states[i].attributes & HidSixAxisSensorAttribute_IsConnected)) continue;
        gyro_sample(which, states[i]);
        g_gyro.last_sampling = states[i].sampling_number;
    }
    // the first sample after a start: one raw line to check the axes on hardware
    static bool logged = false;
    if (n && !logged) {
        logged = true;
        const auto& st = states[0];
        LOG("[gyro] first sample: accel %.2f %.2f %.2f g, gyro %.3f %.3f %.3f rot/s (HOS axes)",
            st.acceleration.x, st.acceleration.y, st.acceleration.z,
            st.angular_velocity.x, st.angular_velocity.y, st.angular_velocity.z);
    }
}

}  // namespace

void init() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    hidInitializeTouchScreen();  // the GamePad's touch screen while its picture is shown (gfxsw::gamepad_view)
    // read here, not at static init: main() applies env.txt first
    if (const char* e = getenv("WWHD_PRO_CONTROLLER"); e && *e) g_pro = strcmp(e, "0") != 0;
    LOG("[input] Switch controller acts as %s", g_pro.load() ? "Pro Controller" : "GamePad");
}

void update() {
    static ::PadState pad;
    static const bool initialized = [] { padInitializeDefault(&pad); return true; }();
    (void)initialized;
    padUpdate(&pad);
    update_gyro(pad);
    u64 held = padGetButtons(&pad);
    static const struct { u64 hid; uint32_t vpad; } kMap[] = {
        {HidNpadButton_A, kA},         {HidNpadButton_B, kB},          {HidNpadButton_X, kX},
        {HidNpadButton_Y, kY},         {HidNpadButton_L, kL},          {HidNpadButton_R, kR},
        {HidNpadButton_ZL, kZL},       {HidNpadButton_ZR, kZR},        {HidNpadButton_Plus, kPlus},
        {HidNpadButton_Minus, kMinus}, {HidNpadButton_Up, kUp},        {HidNpadButton_Down, kDown},
        {HidNpadButton_Left, kLeft},   {HidNpadButton_Right, kRight},  {HidNpadButton_StickL, kStickL},
        {HidNpadButton_StickR, kStickR},
    };
    PadState s;
    for (auto& m : kMap)
        if (held & m.hid) s.buttons |= m.vpad;
    // ZL + ZR + Minus switches the window between the TV and the GamePad picture (overlay.cpp): the game does
    // not get that Minus (in GamePad mode it would switch to Off-TV Play)
    if ((held & HidNpadButton_ZL) && (held & HidNpadButton_ZR)) s.buttons &= ~uint32_t(kMinus);
    // both sticks clicked together: a capture of the next frame (its passes in the log, its pictures on the
    // SD card) for a picture that goes wrong; the game gets the clicks as usual
    {
        static bool was = false;
        const bool combo = (held & HidNpadButton_StickL) && (held & HidNpadButton_StickR);
        if (combo && !was) {
            LOG("[input] both sticks clicked: capturing the next frame");
            gfxsw::request_capture();
        }
        was = combo;
    }
    HidAnalogStickState l = padGetStickPos(&pad, 0), r = padGetStickPos(&pad, 1);
    s.lx = l.x / (float)JOYSTICK_MAX;
    s.ly = l.y / (float)JOYSTICK_MAX;
    s.rx = r.x / (float)JOYSTICK_MAX;
    s.ry = r.y / (float)JOYSTICK_MAX;
    // the same controller for the settings overlay (by position, as the game sees it): Minus held half
    // a second opens it, B or Minus closes it; while it is open the game gets no buttons
    float v[input_map::kPadCount] = {};
    {
        using namespace input_map;
        static const struct { u64 hid; int pad; } kPads[] = {
            {HidNpadButton_A, kPadA},       {HidNpadButton_B, kPadB},           {HidNpadButton_X, kPadX},
            {HidNpadButton_Y, kPadY},       {HidNpadButton_L, kPadLB},          {HidNpadButton_R, kPadRB},
            {HidNpadButton_ZL, kPadLT},     {HidNpadButton_ZR, kPadRT},         {HidNpadButton_Plus, kPadMenu},
            {HidNpadButton_Minus, kPadOptions}, {HidNpadButton_StickL, kPadL3}, {HidNpadButton_StickR, kPadR3},
            {HidNpadButton_Up, kPadDUp},    {HidNpadButton_Down, kPadDDown},    {HidNpadButton_Left, kPadDLeft},
            {HidNpadButton_Right, kPadDRight},
        };
        for (auto& p : kPads)
            if (held & p.hid) v[p.pad] = 1.0f;
        auto axis = [&](float a, int neg, int pos) {
            if (a < 0) v[neg] = -a;
            else v[pos] = a;
        };
        axis(s.lx, kPadLSLeft, kPadLSRight);
        axis(-s.ly, kPadLSUp, kPadLSDown);  // stick up is +y on the Switch
        axis(s.rx, kPadRSLeft, kPadRSRight);
        axis(-s.ry, kPadRSUp, kPadRSDown);
    }
    // the console's touch screen (1280x720 panel) is the GamePad's while the window shows the GamePad picture
    if (gfxsw::gamepad_view()) {
        HidTouchScreenState ts = {};
        if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0)
            s.touch = gfxsw::gamepad_touch(ts.touches[0].x / 1280.0f, ts.touches[0].y / 720.0f, s.tx, s.ty);
    }
    if (overlay::blocks_input()) s = PadState{};
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pad = s;
        memcpy(g_values, v, sizeof g_values);
    }
    show_keyboard();
}

void set_touch(bool, float, float) {}
bool pro_controller() { return g_pro.load(std::memory_order_relaxed); }
void set_pro_controller(bool on) { g_pro = on; LOG("[input] Switch controller acts as %s", on ? "Pro Controller" : "GamePad"); }
void release_keys() {}
void controller_values(float* v) {
    std::lock_guard<std::mutex> lk(g_mu);
    memcpy(v, g_values, sizeof g_values);
}
void host_controller_values(float* v) { controller_values(v); }
void held_keys(bool* keys) { memset(keys, 0, 256); }

void prompt_text(const std::u16string& initial, int max_len, std::function<void(bool ok, std::u16string text)> done) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_initial = initial;
    g_max_len = max_len;
    g_pending = std::move(done);
}

PadState read() {
    PadState s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s = g_pad;
    }
    mods::filter_pad(s);
    return s;
}
}  // namespace input
