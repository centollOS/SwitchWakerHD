// Offscreen Settings > Quit Game (overlay/quit_dialog.h, issue #231): the bottom row's button is there
// (desktop and Android forms), a controller reaches it from the page with the D-pad, and its
// confirmation starts on Cancel; A / Enter / B / Esc / Cancel return to the menu, only Quit (mouse,
// touch-style click or the controller moved onto it) quits. No window, audio or game files.
// Optional argument: a folder for BMP pictures of the menu and the confirmation.
#include "overlay/quit_dialog.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <cmath>
#include <cstring>
#include <string>

using namespace overlay::quit;

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
    bool android = false, back = false, in_game = true, closed = false;
    Answer forced = Answer::None;
    int quits = 0;
    // the Settings window as overlay.cpp lays it out: tabs, a page one row shorter, the bottom row
    auto frame = [&] {
        ImGui_ImplSDLRenderer3_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(50, 40), ImGuiCond_Always); ImGui::SetNextWindowSize(ImVec2(860, 560));
        ImGui::Begin("Wind Waker HD  -  Settings", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Audio")) {
                ImGui::BeginChild("page", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_NavFlattened);
                ImGui::Button("Mute"); ImGui::Button("Default"); ImGui::Button("Test speakers");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        bool close = false;
        const bool ask = bottom_row(android, &close);
        if (close) closed = true;
        const auto answer = dialog(ask, in_game, back, forced);
        back = false;
        if (answer == Answer::Quit) quits++;
        ImGui::End(); ImGui::Render(); SDL_SetRenderDrawColor(renderer, 20, 23, 29, 255); SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer); SDL_RenderPresent(renderer);
        return answer;
    };
    auto settings = [] { return ImGui::FindWindowByName("Wind Waker HD  -  Settings"); };
    auto popup = [] { return ImGui::FindWindowByName(quitprompt::kTitle); };
    auto popup_up = [&] { ImGuiWindow* p = popup(); return p && p->Active; };  // drawn in the last frame
    auto press = [&](ImGuiKey key) {
        io.AddKeyEvent(key, true); auto a = frame(); io.AddKeyEvent(key, false); auto b = frame();
        return a != Answer::None ? a : b;
    };
    auto settle = [&] { for (int i = 0; i < 4; i++) assert(frame() == Answer::None); };
    auto quit_button = [&] { return settings()->GetID(kButton); };
    auto click_at = [&](ImVec2 p) {
        io.AddMousePosEvent(p.x, p.y); frame();
        io.AddMouseButtonEvent(0, true); auto a = frame(); io.AddMouseButtonEvent(0, false); auto b = frame();
        io.AddMousePosEvent(-100, -100);
        return a != Answer::None ? a : b;
    };
    // where a button of a window was drawn: its last item rectangle, read through the navigation data
    auto button_center = [&](ImGuiWindow* w, const char* label) {
        ImGui::SetNavCursorVisible(false);
        ImGuiID id = w->GetID(label);
        ImGui::SetFocusID(id, w);
        frame();
        assert(GImGui->NavId == id);
        return ImGui::WindowRectRelToAbs(GImGui->NavWindow, GImGui->NavWindow->NavRectRel[0]).GetCenter();
    };

    frame(); settle();
    // The desktop row: Quit Game at the bottom right of the window, no Close Settings.
    {
        ImGuiWindow* w = settings();
        ImVec2 c = button_center(w, kButton);
        assert(c.x > w->Pos.x + w->Size.x * 0.75f && c.y > w->Pos.y + w->Size.y - ImGui::GetFrameHeightWithSpacing() * 1.5f);
        capture("quit-menu");
    }
    // Controller: from the page, D-pad Down reaches Quit Game; A opens the question on Cancel; A again
    // answers Cancel and the menu is back with Quit Game in focus.
    {
        // the navigation cursor on the page's first button, as when the menu opens on a tab
        ImGuiWindow* page = ImGui::FindWindowByID(settings()->GetID("page"));
        if (!page) for (ImGuiWindow* c : settings()->DC.ChildWindows) if (strstr(c->Name, "page")) page = c;
        assert(page);
        ImGui::SetFocusID(page->GetID("Mute"), page);
        settle();
        assert(GImGui->NavId == page->GetID("Mute"));
        ImGuiID quit = quit_button();
        for (int i = 0; i < 8 && GImGui->NavId != quit; i++) press(ImGuiKey_GamepadDpadDown);
        assert(GImGui->NavId == quit);
        assert(press(ImGuiKey_GamepadFaceDown) == Answer::None);
        settle();
        assert(popup_up());
        assert(GImGui->NavWindow == popup() && GImGui->NavId == popup()->GetID(kCancel));
        capture("quit-confirm");
        assert(press(ImGuiKey_GamepadFaceDown) == Answer::Cancel);
        settle();
        assert(!popup_up() && quits == 0);
        assert(GImGui->NavWindow == settings() && GImGui->NavId == quit);
    }
    // B (the host's controller back) and Esc answer Cancel.
    {
        assert(press(ImGuiKey_GamepadFaceDown) == Answer::None); settle(); assert(popup_up());
        back = true; assert(frame() == Answer::Cancel); settle(); assert(!popup_up());
        assert(press(ImGuiKey_Enter) == Answer::None); settle(); assert(popup_up());
        assert(GImGui->NavId == popup()->GetID(kCancel));
        assert(press(ImGuiKey_Escape) == Answer::Cancel); settle(); assert(!popup_up());
    }
    // Keyboard / controller: Right moves onto Quit, which then quits.
    {
        assert(press(ImGuiKey_Enter) == Answer::None); settle(); assert(popup_up());
        press(ImGuiKey_GamepadDpadRight);
        assert(GImGui->NavId == popup()->GetID(kQuit));
        assert(press(ImGuiKey_GamepadFaceDown) == Answer::Quit); settle();
        assert(!popup_up() && quits == 1);
    }
    // Mouse / touch: click Quit Game, then Cancel, then Quit Game and Quit.
    {
        ImVec2 q = button_center(settings(), kButton);
        assert(click_at(q) == Answer::None); settle(); assert(popup_up());
        ImVec2 cancel = button_center(popup(), kCancel);
        assert(click_at(cancel) == Answer::Cancel); settle(); assert(!popup_up() && quits == 1);
        assert(click_at(q) == Answer::None); settle(); assert(popup_up());
        ImVec2 yes = button_center(popup(), kQuit);
        assert(click_at(yes) == Answer::Quit); settle(); assert(quits == 2);
    }
    // A test run's answer (WWHD_TEST_QUIT_ANSWER) once the question is up.
    {
        in_game = false;
        // after the mouse, Quit Game (clicked last) keeps the focus; the keyboard picks it up from there
        assert(GImGui->NavId == quit_button());
        ImGui::SetNavCursorVisible(true);
        assert(press(ImGuiKey_Enter) == Answer::None); settle(); assert(popup_up());
        capture("quit-confirm-title-screen");
        forced = Answer::Cancel; assert(frame() == Answer::Cancel); forced = Answer::None;
        settle(); assert(!popup_up());
    }
    // Android: Close Settings fills the row before Quit Game; both work.
    {
        android = true; settle();
        ImVec2 close = button_center(settings(), "Close Settings");
        ImVec2 q = button_center(settings(), kButton);
        assert(close.x < q.x && std::abs(close.y - q.y) < 1);
        capture("quit-menu-android");
        assert(click_at(close) == Answer::None); assert(closed);
        assert(click_at(q) == Answer::None); settle(); assert(popup_up());
        // after a touch there is no navigation cursor: A does nothing then (never Quit); a D-pad input
        // brings the cursor back on Cancel, and A answers Cancel
        assert(!GImGui->NavCursorVisible);
        for (int i = 0; i < 3; i++) assert(press(ImGuiKey_GamepadFaceDown) == Answer::None && popup_up() && quits == 2);
        press(ImGuiKey_GamepadDpadLeft);
        assert(GImGui->NavCursorVisible && GImGui->NavId == popup()->GetID(kCancel));
        Answer first = press(ImGuiKey_GamepadFaceDown);
        assert(first == Answer::Cancel && quits == 2);
        settle(); assert(!popup_up());
    }
    ImGui_ImplSDLRenderer3_Shutdown(); ImGui::DestroyContext(); SDL_DestroyRenderer(renderer); SDL_DestroySurface(surface);
    std::cout << "Quit Game: button in the bottom row (desktop and Android), controller reach, Cancel first, "
                 "A/B/Enter/Esc/mouse answers passed\n";
}
