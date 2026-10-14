#pragma once
#include "imgui.h"
namespace overlay {
// Called on the render thread in host event order. Queued events before focus loss
// must be discarded too, or a delayed key-down can restore a stuck key next frame.
inline void release_focus_inputs(ImGuiIO& io) {
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
}
}
