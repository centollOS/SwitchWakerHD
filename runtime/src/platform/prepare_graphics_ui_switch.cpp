// Prepare graphics in the settings menu, Switch tab (prepare_graphics_switch.h ui_section). Fork code: overlay.cpp
// only calls it (one marked line), with Dear ImGui directly and the overlay's colours.
#include "prepare_graphics_switch.h"

#include <switch.h>

#include <cstdio>
#include <string>

#include "imgui.h"

namespace prepare_graphics {
namespace {

const ImVec4 kHeading(0.55f, 0.95f, 0.85f, 1.0f), kNote(0.70f, 0.78f, 0.84f, 1.0f), kWarn(1.0f, 0.80f, 0.35f, 1.0f);

void text(const ImVec4& colour, const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}

// handheld on battery, below 40%: worth a word before 30-40 minutes of compiling
bool battery_low(uint32_t& percent) {
    static bool psm = R_SUCCEEDED(psmInitialize());
    if (!psm || appletGetOperationMode() == AppletOperationMode_Console) return false;  // docked
    PsmChargerType charger = PsmChargerType_Unconnected;
    if (R_FAILED(psmGetBatteryChargePercentage(&percent)) || R_FAILED(psmGetChargerType(&charger))) return false;
    return charger == PsmChargerType_Unconnected && percent < 40;
}

}  // namespace

bool ui_section() {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::SeparatorText("Prepare graphics");
    ImGui::PopStyleColor();
    if (running()) {
        text(kNote, status().c_str());
        if (ImGui::Button("Stop preparing graphics")) stop();
        text(kNote, "What is prepared so far is kept; Prepare graphics continues from here next time.");
        return false;
    }
    const Progress p = progress();
    text(kNote, "The console compiles each graphics effect the first time it is drawn, so new places can look black or "
                "show objects late for a few seconds. Prepare graphics does it for the whole game at once: the game "
                "visits every place by itself (30-40 minutes, best docked), then restarts at the title screen. Your "
                "saves are not touched.");
    uint32_t percent = 0;
    if (battery_low(percent)) {
        char b[120];
        snprintf(b, sizeof b, "The battery is at %u%%: connect the charger or dock the console first.", percent);
        text(kWarn, b);
    }
    char label[96];
    if (p.next > 0 && p.next < p.total)
        snprintf(label, sizeof label, "Continue preparing graphics (%zu of %zu done)", p.next, p.total);
    else
        snprintf(label, sizeof label, p.complete ? "Prepare graphics again" : "Prepare graphics");
    if (p.complete && p.next == 0)
        text(kNote, "Done on this console. Again is only useful after an update that changed the graphics.");
    static std::string why;
    bool close = false;
    if (ImGui::Button(label)) {
        why = start();
        close = why.empty();
    }
    if (!why.empty()) text(kWarn, why.c_str());
    return close;
}

bool draw_screen(float width, float height) {
    if (!running()) return false;
    const Live l = live();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.06f, 0.10f, 1.0f));
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##prepare_graphics_screen", nullptr, fl)) {
        const float w = width * 0.6f, x = (width - w) * 0.5f;
        ImGui::SetCursorPos(ImVec2(x, height * 0.36f));
        ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.6f);
        ImGui::TextUnformatted("Preparing graphics");
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(x);
        char b[96];
        if (l.total) snprintf(b, sizeof b, "%zu of %zu", l.place, l.total);
        else snprintf(b, sizeof b, "starting");
        ImGui::ProgressBar(l.total ? float(l.place - 1) / float(l.total) : 0.0f, ImVec2(w, 0), b);
        ImGui::SetCursorPosX(x);
        ImGui::PushTextWrapPos(x + w);
        if (l.minutesLeft >= 0) snprintf(b, sizeof b, "About %d min left", l.minutesLeft < 1 ? 1 : l.minutesLeft);
        else snprintf(b, sizeof b, "Working out the time left...");
        text(kNote, b);
        ImGui::SetCursorPosX(x);
        text(kNote, "The game is visiting every place by itself to compile its graphics, with the sound and rumble off. "
                    "It restarts at the title screen when it is done. Your saves are not touched.");
        ImGui::SetCursorPosX(x);
        text(kNote, "To stop: hold Minus (-), Switch tab. What is done is kept.");
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    return true;
}

}  // namespace prepare_graphics
