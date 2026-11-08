// SPDX-License-Identifier: MPL-2.0
// The first-start question on a touch device (controls plan P8; when to ask: platform/touch_style.h
// ask_first_start): "Use the new touch controls?" The new ones (Flexible: sticks under the thumbs, the swipe
// camera, context buttons; recommended, the default) or the classic fixed sticks. Closing it without an answer
// keeps the new ones and doesn't ask again. The overlay asks it (overlay.cpp touch_question_frame);
// touch_question_test draws it offscreen.
// Idea from the GreenNaugahyde Android fork (StarterQuestions), reimplemented.
#pragma once
#include "imgui.h"
#include "popup.h"

namespace overlay::touch_question {

constexpr const char* kTitle = "Touch controls##first_start";
enum Answer : int { kNone = -1, kClassic = 0, kNew = 1 };

struct Marks {  // where the answers were drawn (tests click them)
    ImRect yes, no;
};

// one frame of the dialog while it is asked; `back`: the controller's B this frame
inline Answer frame(bool back, Marks* marks = nullptr) {
    if (!popup::begin(kTitle, true, ImGui::GetFontSize() * 22)) return kNone;
    const bool dismissed = popup::dismissed(back);
    ImGui::PushTextWrapPos(popup::wrap_pos(30));
    ImGui::TextWrapped("Use the new touch controls?");
    ImGui::TextDisabled("%s", "New: a stick appears where your left thumb lands, swipe the right half to turn the "
                              "camera, and only the buttons that do something now show.");
    ImGui::TextDisabled("%s", "Classic: the sticks and buttons stay where the layout puts them.");
    ImGui::TextDisabled("%s", "You can change it any time: Settings, Display, Touch controls.");
    Answer answer = kNone;
    if (ImGui::Button("Yes, the new controls (recommended)")) answer = kNew;
    if (marks) marks->yes = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::Button("No, the classic controls")) answer = kClassic;
    if (marks) marks->no = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (dismissed && answer == kNone) answer = kNew;
    ImGui::PopTextWrapPos();
    if (answer != kNone) ImGui::CloseCurrentPopup();
    popup::end();
    return answer;
}

}  // namespace overlay::touch_question
