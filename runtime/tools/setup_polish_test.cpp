#include "gfx/shader_setup_hint.h"
#include "overlay/tooltip.h"
#include <cassert>
#include <string_view>
int main() {
    assert(std::string_view(shaderprebuild::setup_hint_for(true)).find("Start / resume / retry setup") != std::string_view::npos);
    assert(std::string_view(shaderprebuild::setup_hint_for(false)).find("Repair") != std::string_view::npos);
    assert(std::string_view(shaderprebuild::setup_hint_for(false)).find("Update to this release") != std::string_view::npos);
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    int frame = 0;
    for (auto source : {ImGuiMouseSource_Mouse, ImGuiMouseSource_Mouse, ImGuiMouseSource_TouchScreen, ImGuiMouseSource_Mouse}) {
        io.AddMouseSourceEvent(source);
        io.AddMousePosEvent(100 + frame, 100);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(400, 300));
        ImGui::Begin("Tooltip");
        ImGui::SetCursorScreenPos(ImVec2(50, 50));
        ImGui::Button("Rebuild", ImVec2(200, 100));
        if (frame++ > 0) assert(overlay::tooltip_hovered() == (source != ImGuiMouseSource_TouchScreen));
        ImGui::End();
        ImGui::Render();
    }
    ImGui::DestroyContext();
}
