#pragma once
#include "imgui.h"
namespace overlay {
inline bool tooltip_hovered(ImGuiHoveredFlags flags = 0) {
    return ImGui::GetIO().MouseSource != ImGuiMouseSource_TouchScreen && ImGui::IsItemHovered(flags);
}
}
