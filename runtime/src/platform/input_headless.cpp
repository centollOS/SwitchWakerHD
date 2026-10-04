// Headless host input (desktop OpenGL debugging build): no controllers, text prompts keep their text.
// WWHD_SCRIPT_INPUT scripts the GamePad by renderer frame, for reproducible runs into gameplay:
//   "100-110:A,300-305:DOWN+B,400-800:LY=1"  (buttons A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT;
//   sticks LX LY RX RY = -1..1; ranges are inclusive)
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gfx/renderer.h"

#include "../input.h"
#include "../input_map.h"
#include "input_switch.h"

namespace mods {
void filter_pad(input::PadState&);
bool mouse_captured() { return false; }
void mouse_release() {}
void mouse_init(void*) {}
bool host_key_down(uint16_t) { return false; }
}
namespace input {
void init() {}
void update() {}
void set_touch(bool, float, float) {}
// WWHD_PRO_CONTROLLER=1: the script drives a Wii U Pro Controller, as on the Switch
static bool g_pro = [] {
    const char* e = getenv("WWHD_PRO_CONTROLLER");
    return e && *e && strcmp(e, "0") != 0;
}();
bool pro_controller() { return g_pro; }
void set_pro_controller(bool on) { g_pro = on; }
void release_keys() {}
void controller_values(float* v) { memset(v, 0, sizeof(float) * input_map::kPadCount); }
void held_keys(bool* keys) { memset(keys, 0, 256); }
void prompt_text(const std::u16string& initial, int, std::function<void(bool ok, std::u16string text)> done) {
    done(true, initial);
}
namespace {
struct ScriptStep {
    uint64_t from = 0, to = 0;
    uint32_t buttons = 0;
    float axis[4] = {0, 0, 0, 0};
    bool hasAxis[4] = {false, false, false, false};
};
const std::vector<ScriptStep>& script() {
    static const std::vector<ScriptStep> steps = [] {
        std::vector<ScriptStep> out;
        const char* e = getenv("WWHD_SCRIPT_INPUT");
        if (!e) return out;
        static const struct { const char* name; uint32_t bit; } kButtons[] = {
            {"A", kA}, {"B", kB}, {"X", kX}, {"Y", kY}, {"L", kL}, {"R", kR}, {"ZL", kZL}, {"ZR", kZR},
            {"PLUS", kPlus}, {"MINUS", kMinus}, {"UP", kUp}, {"DOWN", kDown}, {"LEFT", kLeft}, {"RIGHT", kRight}};
        static const char* kAxes[] = {"LX", "LY", "RX", "RY"};
        std::string all(e);
        size_t pos = 0;
        while (pos < all.size()) {
            size_t end = all.find(',', pos);
            std::string item = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? all.size() : end + 1;
            ScriptStep st;
            size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            char* p = nullptr;
            st.from = strtoull(item.c_str(), &p, 10);
            st.to = *p == '-' ? strtoull(p + 1, nullptr, 10) : st.from;
            std::string what = item.substr(colon + 1);
            size_t q = 0;
            while (q <= what.size()) {
                size_t plus = what.find('+', q);
                std::string tok = what.substr(q, plus == std::string::npos ? std::string::npos : plus - q);
                q = plus == std::string::npos ? what.size() + 1 : plus + 1;
                size_t eq = tok.find('=');
                if (eq != std::string::npos) {
                    for (int i = 0; i < 4; i++)
                        if (tok.substr(0, eq) == kAxes[i]) {
                            st.axis[i] = strtof(tok.c_str() + eq + 1, nullptr);
                            st.hasAxis[i] = true;
                        }
                    continue;
                }
                for (auto& b : kButtons)
                    if (tok == b.name) st.buttons |= b.bit;
            }
            out.push_back(st);
        }
        return out;
    }();
    return steps;
}
}  // namespace

PadState read() {
    PadState s;
    if (!script().empty()) {
        const uint64_t frame = render::frame_count();
        for (const auto& st : script()) {
            if (frame < st.from || frame > st.to) continue;
            s.buttons |= st.buttons;
            float* axes[4] = {&s.lx, &s.ly, &s.rx, &s.ry};
            for (int i = 0; i < 4; i++)
                if (st.hasAxis[i]) *axes[i] = st.axis[i];
        }
    }
    mods::filter_pad(s);
    return s;
}
}  // namespace input
