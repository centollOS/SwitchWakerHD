#pragma once
#include "imgui.h"

namespace overlay {
// Both tabs use the Controls gesture: right-click, or Delete/Backspace/X on a focused binding.
// A right click during capture belongs to the binding, rather than clearing it.
inline bool binding_clear_requested(bool capturing, bool right_released = false) {
    if (capturing) return false;
    return right_released || ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
        (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)));
}
}
