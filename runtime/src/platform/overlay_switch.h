// SwitchWakerHD (this fork): the settings overlay's Switch parts (overlay_switch.cpp), registered through
// upstream's overlay/extensions.h: the Switch and Warp tabs (Graphics, Display and Controls hidden), the
// console's font, the TV / GamePad shortcut, the system keyboard for text fields and the Prepare graphics /
// first-start screens over the game.
#pragma once

namespace overlay_switch {
void register_ext();  // main_switch::start(), before the overlay's first frame
}  // namespace overlay_switch
