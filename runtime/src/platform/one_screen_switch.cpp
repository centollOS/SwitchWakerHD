// (this fork, upstream sync bfd7dd6) upstream's one screen mode (mods/one_screen.cpp, left out of the Switch build:
// cmake/SwitchPort.cmake) as "off": the Switch has its own single-screen mode (screen_mode.cpp, the Dev tab's
// controller choice), which hooks the same game functions. These keep upstream's callers (the HUD fade, VPADRead,
// the Display tab the Switch does not show) as they are without it.
#include "input.h"
#include "mods/mods.h"

namespace mods {
bool one_screen_enabled() { return false; }
bool one_screen_active() { return false; }
bool one_screen_off_tv() { return false; }
bool one_screen_next() { return false; }
void set_one_screen_next(bool) {}
void one_screen_pad(input::PadState&) {}
bool one_screen_hidden_pane(const char*) { return false; }
}  // namespace mods
