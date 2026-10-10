// GamePad mode on the Switch: no controller picker, play in Off-TV Play (see screen_mode.cpp).
#pragma once
#include <cstdint>

namespace screen_mode {
// the single-screen GamePad experience (the controller choice's default, input_switch.cpp): the hooks below apply
// while it is on and the controller acts as the GamePad. Decided at start (switch_settings::apply_at_start), changed
// only by the next start; off, the game behaves as on the Wii U (TV play, its controller picker, Options).
bool single_screen();
void set_single_screen(bool on);
// a layout pane not to draw (with its children): the GamePad's touch guides over the game in Off-TV Play
bool hidden_pane(uint32_t pane);
}
