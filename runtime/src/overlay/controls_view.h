// The settings overlay's Controls drawing: the selected Wii U controller (GamePad or Pro Controller)
// with a callout for every input and a chip for each binding (key, alternate key, controller), drawn
// with Dear ImGui's draw list. Same layout as the macOS Controls window (gfx/controls_ui.mm), so it
// works on every host.
#pragma once
#include "../input_map.h"

namespace overlay {

// capture columns: key, alternate key, controller, or "any" (a click on the drawing: a key goes to the
// first key slot, a controller input to the controller slot)
enum { kColKey0, kColKey1, kColPad, kColAny };

struct ControlsView {
    // in
    input_map::Mapping m;
    bool pro = false;
    int cap_action = -1, cap_col = 0;  // input being captured (-1 none)
    // live display, from the raw host state (not what the game sees: its input is blocked while the overlay is open)
    const float* pad = nullptr;        // host controller values (input_map::kPadCount): held controller chips
    const bool* keys = nullptr;        // 256 keys held while the overlay is open: held key chips
    float act[input_map::kActionCount] = {};  // each input pressed (0..1) through the current mapping
    float stick[2][2] = {};            // left / right stick deflection, x right, y up
    double t = 0;                      // seconds, for the capture pulse
    // out (this frame)
    int hover_action = -1, hover_col = -1;  // under the mouse or the keyboard / controller cursor
    int start_action = -1, start_col = 0;   // a chip or part was activated: capture it
    int clear_action = -1, clear_col = 0;   // right-click / Delete / controller X on a chip: unbind it
};

// draws into the current window at the cursor, `w` x `h` points
void draw_controls(ControlsView& v, float w, float h);

}  // namespace overlay
