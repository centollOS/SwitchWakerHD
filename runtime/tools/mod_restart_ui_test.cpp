// Offscreen production "Restart required" popup: mouse, keyboard and controller answers, the safe
// default focus and the OK-only form. No window, audio, game or mod files.
#include "overlay/restart_dialog.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <filesystem>
#include <iostream>

using namespace overlay::restart;

int main(int argc, char** argv) {
    SDL_Surface* surface = SDL_CreateSurface(960, 640, SDL_PIXELFORMAT_RGBA32); assert(surface);
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface); assert(renderer);
    ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.ConfigFlags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad; io.DisplaySize = ImVec2(960, 640); io.DeltaTime = 1.f / 60;
    assert(ImGui_ImplSDLRenderer3_Init(renderer));
    auto capture = [&](const char* name) {
        if (argc < 2) return;
        std::filesystem::create_directories(argv[1]);
        assert(SDL_SaveBMP(surface, (std::filesystem::path(argv[1]) / (std::string(name) + ".bmp")).string().c_str()));
    };
    Notice notice;
    double now = 0;
    std::vector<Item> items;
    bool can_restart = true, back = false;
    auto frame = [&] {
        ImGui_ImplSDLRenderer3_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(100, 80), ImGuiCond_Always); ImGui::SetNextWindowSize(ImVec2(760, 480));
        ImGui::Begin("Settings");
        now += 1.0 / 60;
        bool open = notice.update(items, now, false);
        auto answer = dialog(notice, open, can_restart, back);
        back = false;
        ImGui::End(); ImGui::Render(); SDL_SetRenderDrawColor(renderer, 20, 23, 29, 255); SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer); SDL_RenderPresent(renderer);
        return answer;
    };
    auto settle = [&] {  // until the popup is up and laid out
        for (int i = 0; i < 80 && !notice.showing(); i++) assert(frame() == Answer::None);
        assert(notice.showing());
        for (int i = 0; i < 4; i++) assert(frame() == Answer::None);
    };
    auto press = [&](ImGuiKey key) {
        io.AddKeyEvent(key, true); auto a = frame(); io.AddKeyEvent(key, false); auto b = frame();
        return a != Answer::None ? a : b;
    };
    frame();  // baseline: nothing pending
    int n = 0;
    auto change = [&](const char* name) {
        std::string id = "mod" + std::to_string(n++);
        items.push_back({"pkg:" + id + ":on", std::string(name) + " will be turned on after a restart.", std::string(name) + " on"});
    };

    // Keyboard Enter and controller A activate the default: Later.
    for (auto key : {ImGuiKey_Enter, ImGuiKey_GamepadFaceDown}) {
        change("Sea minimap"); io.AddMousePosEvent(-100, -100); settle();
        if (key == ImGuiKey_Enter) capture("restart-popup");
        auto* popup = ImGui::FindWindowByName("Restart required"); assert(popup);
        assert(GImGui->NavWindow == popup && GImGui->NavId == popup->GetID("Later"));
        // keyboard: the first Tab shows the navigation cursor, still on Later (as in the setup dialog)
        if (key == ImGuiKey_Enter) assert(press(ImGuiKey_Tab) == Answer::None && GImGui->NavId == popup->GetID("Later"));
        assert(press(key) == Answer::Later); assert(!notice.showing());
        for (int i = 0; i < 80; i++) assert(frame() == Answer::None);  // Later: not again
    }
    // Esc and controller B answer Later.
    change("Heart ticker"); settle(); assert(press(ImGuiKey_Escape) == Answer::Later);
    change("HD textures"); settle(); back = true; assert(frame() == Answer::Later);
    // Mouse on Restart now.
    change("Cel shading"); settle();
    {
        auto* popup = ImGui::FindWindowByName("Restart required"); assert(popup);
        io.AddMousePosEvent(popup->Pos.x + 40, popup->Pos.y + popup->Size.y - ImGui::GetStyle().WindowPadding.y - ImGui::GetFrameHeight() / 2);
        io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false);
        assert(frame() == Answer::RestartNow); io.AddMousePosEvent(-100, -100);
    }
    // No relaunch on this platform: only OK, which is the default.
    can_restart = false;
    change("Sea chart"); settle(); capture("restart-popup-ok");
    {
        auto* popup = ImGui::FindWindowByName("Restart required"); assert(popup);
        assert(GImGui->NavId == popup->GetID("OK"));
        // after the mouse, the first controller input shows the navigation cursor; OK is the only button
        assert(press(ImGuiKey_GamepadDpadDown) == Answer::None && GImGui->NavId == popup->GetID("OK"));
        assert(press(ImGuiKey_GamepadFaceDown) == Answer::Later);
    }
    ImGui_ImplSDLRenderer3_Shutdown(); ImGui::DestroyContext(); SDL_DestroyRenderer(renderer); SDL_DestroySurface(surface);
    std::cout << "Restart popup default focus, keyboard/controller/mouse answers and OK-only form passed\n";
}
