// (this fork, s2/vrr) variable refresh rate: "present when ready". Off by default (settings.ini s2Vrr=1 or the
// Switch 2 tab). On: instead of the game's swap interval (2/60 s at 30 fps, frame_pacing.h), the window presents at
// every refresh at the display's highest rate with VRR allowed (nwindowSetFrameRates(0, 0, true)), so a finished frame
// goes to the display at once (VRR) or at the next 1/120 s refresh, and a late frame no longer waits for the next
// 1/30 s slot. The game's own 30 steps a second pace the frames. On a display without VRR (most TVs) it is the same as
// swap interval 1. Frame interpolation's 120 / 240 fps keep their own modes.
#pragma once
#include <atomic>
#include <cstdint>

namespace s2vrr {
bool on();            // the option (any thread)
void set_on(bool v);  // the menu: live from the next present, saved
// render thread, at present while the option is on: the display's refresh rate and whether VRR is active (every 5 s)
void poll();
extern std::atomic<bool> g_on;
extern std::atomic<int> g_hz, g_active;  // the last poll: Hz (0 unknown), VRR active (-1 unknown)
}  // namespace s2vrr
