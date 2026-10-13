// SwitchWakerHD (this fork): the settings overlay's Switch parts (overlay_switch.cpp), called from marked hooks
// in overlay/overlay.cpp: the Switch and Warp tabs, the console's font, the system keyboard for text fields
// and the Prepare graphics / first-start screens over the game. They map onto overlay::ext (upstream's
// overlay/extensions.h: add_tab, set_font_loader, set_text_input, add_frame_layer) at the next sync.
#pragma once
#include <cstddef>
#include <string>

#include "imgui.h"

namespace overlay_switch {
void tab_switch();  // render thread, inside the tab's page
void tab_warp();
bool load_font(ImGuiIO& io);  // the system's shared font into io.Fonts; false: the overlay's usual font
// a text field without a keyboard: read-only, choosing it opens the system keyboard; true when its text arrived
bool text_field(const char* label, char* buf, size_t size, ImGuiInputTextFlags flags);
// while the menu is closed: whether the Prepare graphics or first-start screen covers the game (`toast` may
// become Prepare graphics' line), and drawing it
bool frame_layer_wanted(std::string& toast);
void frame_layer_draw(float w, float h);
}  // namespace overlay_switch
