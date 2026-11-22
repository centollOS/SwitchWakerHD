// (this fork, s2/*) the "Switch 2" tab of the settings overlay (s2_tab_switch2.h)
#include "s2_tab_switch2.h"

#include <cstdarg>

#include "imgui.h"
#include "../overlay/hostui.h"

namespace s2tab {
// the sections a build has: the others stay these empty weak versions
__attribute__((weak)) void section_display() {}
__attribute__((weak)) void section_resolution() {}
__attribute__((weak)) void section_vrr() {}
__attribute__((weak)) void section_clock() {}
__attribute__((weak)) void section_cores() {}
__attribute__((weak)) void start_display() {}
__attribute__((weak)) void start_resolution() {}
__attribute__((weak)) void start_vrr() {}
__attribute__((weak)) void start_cores() {}

void heading(const char* text) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.95f, 0.85f, 1.0f));
    ImGui::SeparatorText(text);
    ImGui::PopStyleColor();
}
void help(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", text);
}
void note(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.78f, 0.84f, 1.0f));
    ImGui::TextWrappedV(fmt, ap);
    ImGui::PopStyleColor();
    va_end(ap);
}
void warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.80f, 0.35f, 1.0f));
    ImGui::TextWrappedV(fmt, ap);
    ImGui::PopStyleColor();
    va_end(ap);
}
bool saved(const char* key, std::string& value) { return hostui::get(key, value) && !value.empty(); }
void save(const char* key, std::string value) {
    hostui::post([k = std::string(key), v = std::move(value)] { hostui::set(k.c_str(), v); });
}

void tab() {
    note("Options only the Switch 2 has. Hold the pointer on an option for what it does. Each one is saved in "
         "settings.ini (s2* keys); the defaults keep the port's usual behaviour.");
    section_display();
    section_resolution();
    section_vrr();
    section_clock();
    section_cores();
}

void apply_at_start() {
    start_display();
    start_resolution();
    start_vrr();
    start_cores();
}
}  // namespace s2tab
