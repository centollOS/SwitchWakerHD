// Switch input: Joy-Cons / Pro Controller read as a Wii U Pro Controller (WPAD/KPAD, see
// hle/padscore.cpp) so the game puts everything on the one TV screen; WWHD_PRO_CONTROLLER=0 in
// env.txt makes them act as the Wii U GamePad instead. Buttons map by position, which matches the
// Wii U's labels. Text prompts go through the system software keyboard.
#include <switch.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "../input.h"
#include "../input_map.h"
#include "../runtime.h"
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
}  // namespace

void init() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    // read here, not at static init: main() applies env.txt first
    if (const char* e = getenv("WWHD_PRO_CONTROLLER"); e && *e) g_pro = strcmp(e, "0") != 0;
    LOG("[input] Switch controller acts as %s", g_pro.load() ? "Pro Controller" : "GamePad");
}

void update() {
    static ::PadState pad;
    static const bool initialized = [] { padInitializeDefault(&pad); return true; }();
    (void)initialized;
    padUpdate(&pad);
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
    HidAnalogStickState l = padGetStickPos(&pad, 0), r = padGetStickPos(&pad, 1);
    s.lx = l.x / (float)JOYSTICK_MAX;
    s.ly = l.y / (float)JOYSTICK_MAX;
    s.rx = r.x / (float)JOYSTICK_MAX;
    s.ry = r.y / (float)JOYSTICK_MAX;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pad = s;
    }
    show_keyboard();
}

void set_touch(bool, float, float) {}
bool pro_controller() { return g_pro.load(std::memory_order_relaxed); }
void set_pro_controller(bool on) { g_pro = on; LOG("[input] Switch controller acts as %s", on ? "Pro Controller" : "GamePad"); }
void release_keys() {}
void controller_values(float* v) { memset(v, 0, sizeof(float) * input_map::kPadCount); }
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
