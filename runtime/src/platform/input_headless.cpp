// Headless host input (desktop OpenGL debugging build): no controllers, text prompts keep their text.
#include <cstring>

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
bool pro_controller() { return false; }
void set_pro_controller(bool) {}
void release_keys() {}
void controller_values(float* v) { memset(v, 0, sizeof(float) * input_map::kPadCount); }
void held_keys(bool* keys) { memset(keys, 0, 256); }
void prompt_text(const std::u16string& initial, int, std::function<void(bool ok, std::u16string text)> done) {
    done(true, initial);
}
PadState read() {
    PadState s;
    mods::filter_pad(s);
    return s;
}
}  // namespace input
