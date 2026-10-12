// SwitchWakerHD (issue #23): frame pacing against the display's real vblanks (frame_pacing.cpp).
// Measurement: how many vblanks each presented frame stayed on screen (modelled from when the GPU finished it),
// and the intervals between presents (the 5 s '[dk] pacing' line).
// Pacing mode (WWHD_DK_PACING=n at boot, debug command "pacing [0|1]"):
//   0: swap interval 1 (the behaviour before #23)
//   1: the swapchain's swap interval follows the game's (2 at 30 fps; 1 with frame interpolation / true 60)
#pragma once
#include <deko3d.h>

#include <cstdint>
#include <string>

namespace frame_pacing {
void start();                                    // after the device: the vblank thread and the report memory
void on_present(DkCmdBuf cmd, uint64_t frame);  // render thread, recording the present pass (before its fence)
uint32_t swap_interval();                        // the swap interval the swapchain should have now
void set_interval(uint32_t interval);            // the swapchain's interval now (for the statistics' model)
std::string report();                            // the 5 s statistics
int mode();
void set_mode(int m);
}  // namespace frame_pacing
