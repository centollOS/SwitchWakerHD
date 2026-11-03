#pragma once
// SwitchWakerHD: the frame rate for the console's overlays (Status Monitor and its forks) through SaltyNX's shared
// memory (docs/saltynx-plan.md). SaltyNX never loads into a homebrew program, so the game writes the block its
// NX-FPS plugin writes in a retail game. Nothing happens without SaltyNX (its port does not exist).
// WWHD_SALTYNX=0 (settings.ini [dev]) turns it off.
#include <cstdint>

namespace saltynx {
// once at start-up: a thread connects to SaltyNX (retrying for a while: it may start after the game) and maps
// the block
void start();
// the render thread, at each present: the frame counts, and the resolutions when an overlay asks for them
// (render: the game's picture, output: the window)
void frame(uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH);
}  // namespace saltynx
