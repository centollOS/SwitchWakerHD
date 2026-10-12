// SwitchWakerHD (issue #23): frame pacing against the display's real vblanks (frame_pacing.cpp).
// Measurement: how many vblanks each presented frame stayed on screen (modelled from when the GPU finished it), and the intervals between presents
// (the 5 s '[dk] pacing' line). Pacing modes (WWHD_DK_PACING=n at boot, debug command "pacing [n]"):
//   bit 0 (A): the swapchain's swap interval follows the game's (2 at 30 fps without frame interpolation)
//   bit 1 (B): gx2_core's virtual vsync follows the display's vblanks instead of a 59.94 Hz steady clock
#pragma once
#include <deko3d.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace frame_pacing {
void start();                                    // after the device: the vblank thread and the report memory
void on_present(DkCmdBuf cmd, uint64_t frame);  // render thread, recording the present pass (before its fence)
uint32_t swap_interval();                        // the swap interval the swapchain should have now
void set_interval(uint32_t interval);            // the swapchain's interval now (for the statistics' model)
std::string report();                            // the 5 s statistics (empty before the first one)
int mode();
void set_mode(int m);

// gx2_core.cpp hooks (Switch only): the virtual vsync on the display's vblanks while mode bit 1 is set.
// ticks: the tick count to use, given the steady clock's (continuous and monotonic across mode changes)
uint64_t ticks(uint64_t steadyTicks, uint64_t perVsync);
// tick_time: true with the time of that tick while the vblanks drive the ticks; otherwise false and
// `tick` turned into the steady clock's own count
bool tick_time(uint64_t& tick, uint64_t perVsync, std::chrono::steady_clock::time_point& out);
bool locked();  // the vblanks drive the ticks: flips are due on them (update_flips skips its 33.4 ms gate)
}  // namespace frame_pacing
