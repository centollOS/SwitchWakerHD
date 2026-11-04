// SPDX-License-Identifier: MPL-2.0
// The first-start touch controls question (overlay/touch_question.h), offscreen with real ImGui frames as
// controller_setup_test does: on screen at phone and desktop sizes, each answer by a click, closing it keeps the
// new controls. Optional argument: a folder for BMP pictures.
#include "overlay/touch_question.h"
#include "overlay/mobile_ui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>

namespace tq = overlay::touch_question;

namespace {
struct Case { const char* name; int w, h; float dpi; };

struct Ui {
    Case c;
    SDL_Surface* surface = nullptr;
    SDL_Renderer* renderer = nullptr;
    float scale = 1;
    explicit Ui(const Case& cs) : c(cs) {
        const bool android = c.dpi > 0;
        mobile_ui::Layout layout;
        if (android) layout = mobile_ui::layout(c.dpi, c.dpi / 160, mobile_ui::kAutomatic, float(c.w), float(c.h));
        scale = android ? layout.scale : mobile_ui::desktop_scale(2, float(c.h));
        surface = SDL_CreateSurface(c.w, c.h, SDL_PIXELFORMAT_RGBA32);
        renderer = SDL_CreateSoftwareRenderer(surface);
        assert(surface && renderer);
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(c.w / scale, c.h / scale);
        io.DeltaTime = 1.f / 60;
        SDL_SetRenderScale(renderer, scale, scale);
        ImGui::StyleColorsDark();
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowPadding = ImVec2(16, 14); s.FramePadding = ImVec2(9, 5); s.ItemSpacing = ImVec2(10, 7);
        if (android) mobile_ui::apply_touch_style(s, layout);
        const char* fonts[] = {"/System/Library/Fonts/Supplemental/Arial.ttf", "C:/Windows/Fonts/arial.ttf",
                               "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf"};
        bool font = false;
        for (const char* f : fonts)
            if (!font && std::filesystem::exists(f)) font = io.Fonts->AddFontFromFileTTF(f, mobile_ui::kText) != nullptr;
        if (!font) { ImFontConfig cfg; cfg.SizePixels = mobile_ui::kText; io.Fonts->AddFontDefaultVector(&cfg); }
        s.FontSizeBase = mobile_ui::kText;
        assert(ImGui_ImplSDLRenderer3_Init(renderer));
    }
    ~Ui() {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
    }
    void frame(const std::function<void()>& draw) {
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui::NewFrame();
        draw();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 23, 29, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }
    void save(const char* dir, const std::string& what) {
        if (!dir) return;
        std::filesystem::create_directories(dir);
        assert(SDL_SaveBMP(surface, (std::filesystem::path(dir) / (std::string(c.name) + "-" + what + ".bmp")).string().c_str()));
    }
};

// one question from opening to an answer; `how`: "yes", "no", "back"
tq::Answer ask(Ui& ui, const char* how, const char* dir) {
    tq::Marks marks;
    tq::Answer got = tq::kNone;
    bool asked = true, back = false;
    auto draw = [&] {
        if (!asked) return;
        const tq::Answer a = tq::frame(back, &marks);
        back = false;
        if (a != tq::kNone) got = a, asked = false;
    };
    for (int i = 0; i < 30; i++) ui.frame(draw);  // (settled: a tap in the first quarter second is ignored)
    // whole on screen
    ImGuiWindow* w = ImGui::FindWindowByName(tq::kTitle);
    assert(w);
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    assert(w->Rect().Min.x >= -0.5f && w->Rect().Min.y >= -0.5f && w->Rect().Max.x <= ds.x + 0.5f && w->Rect().Max.y <= ds.y + 0.5f);
    assert(marks.yes.GetWidth() > 0 && marks.no.GetWidth() > 0 && marks.yes.Max.y <= ds.y && marks.no.Max.y <= ds.y);
    if (!strcmp(how, "yes")) ui.save(dir, "question");
    if (!strcmp(how, "back")) {
        back = true;
        ui.frame(draw);
    } else {
        const ImVec2 p = (!strcmp(how, "yes") ? marks.yes : marks.no).GetCenter();
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(p.x, p.y);
        ui.frame(draw);
        io.AddMouseButtonEvent(0, true);
        ui.frame(draw);
        io.AddMouseButtonEvent(0, false);
        ui.frame(draw);
        ui.frame(draw);
    }
    return got;
}
}  // namespace

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : nullptr;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    const Case cases[] = {{"phone", 2400, 1080, 440}, {"phone-portrait", 1080, 2400, 440}, {"handheld", 1280, 800, 216}, {"desktop", 1280, 720, 0}};
    for (const Case& c : cases) {
        {
            Ui ui(c);
            assert(ask(ui, "yes", dir) == tq::kNew);
        }
        {
            Ui ui(c);
            assert(ask(ui, "no", dir) == tq::kClassic);
        }
        {
            Ui ui(c);
            assert(ask(ui, "back", dir) == tq::kNew);  // closed without an answer: the new ones
        }
        std::printf("touch question %s: on screen, yes, no, closed\n", c.name);
    }
    std::printf("touch_question_test: ok\n");
    return 0;
}
