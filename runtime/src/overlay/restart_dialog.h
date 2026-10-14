// The "Restart required" popup (Mods tab). Same ImGui navigation path as the other overlay dialogs;
// the host supplies controller B.
#pragma once
#include "restart_notice.h"
#include "imgui.h"

namespace overlay::restart {
enum class Answer { None, RestartNow, Later };

// open: Notice::update() returned true this frame. can_restart: false where the game cannot relaunch
// itself (Windows without a finished code-mod build, Android): then only OK.
// Keyboard and controller start on Later (or OK); Esc and B answer Later.
inline Answer dialog(Notice& notice, bool open, bool can_restart, bool controller_back) {
    const char* title = "Restart required";
    if (open) ImGui::OpenPopup(title);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return Answer::None;
    if (!notice.showing()) {  // the change was undone meanwhile
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return Answer::None;
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
    for (const auto& item : notice.fresh()) ImGui::TextWrapped("%s", item.text.c_str());
    if (can_restart) ImGui::TextDisabled("Progress since your last save is lost.");
    else ImGui::TextWrapped("Restart the game to apply.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    auto answer = Answer::None;
    if (can_restart) {
        if (ImGui::Button("Restart now", ImVec2(120, 0))) answer = Answer::RestartNow;
        ImGui::SameLine();
    }
    if (ImGui::Button(can_restart ? "Later" : "OK", ImVec2(120, 0))) answer = Answer::Later;
    ImGui::SetItemDefaultFocus();
    if (controller_back || ImGui::IsKeyPressed(ImGuiKey_Escape)) answer = Answer::Later;
    if (answer != Answer::None) {
        notice.dismiss();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return answer;
}
}  // namespace overlay::restart
