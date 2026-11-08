// SwitchWakerHD (this fork): see guest_builtin_ui.h.
#include "guest_builtin_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../menu_i18n.h"
#include "../mods/guest_builtin.h"
#include "imgui.h"

#ifdef __SWITCH__
namespace overlay_switch { bool dev_options(); }  // "Show developer options" (platform/overlay_switch.cpp)
#endif

namespace guestbuiltinui {
namespace {

// the Mods tab's colours (mods_tab.h)
const ImVec4 kMuted(0.70f, 0.78f, 0.84f, 1.0f), kGood(0.55f, 0.95f, 0.70f, 1.0f), kWarn(1.0f, 0.80f, 0.35f, 1.0f),
    kBad(1.0f, 0.50f, 0.45f, 1.0f);

void wrapped(const ImVec4& colour, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// How the page of a mod this fork knows shows its options (issue #121: the manifests give one flat list of English
// names and raw values): the groups in order, the options that are only for bug reports (shown with "Show developer
// options"), and the names of enum values. A mod or option not listed here is shown as its manifest gives it, under
// "Other". Names and groups go through menu_i18n::tr (rows in menu_i18n_mods.inc).
struct Group { const char* name; std::vector<const char*> options; };
struct Choice { const char* option; const char* value; const char* label; };
// credit: the original work the mod is based on, shown on its page (name and where to find it, not translated)
struct Layout { const char* mod; std::vector<Group> groups; std::vector<const char*> developer; std::vector<Choice> choices;
                const char* credit = nullptr; };

const std::vector<Layout>& layouts() {
    static const std::vector<Layout> all = {
        {"hd-minimap",
         {{"Position and size", {"x", "y", "size", "size_expanded", "start_expanded"}},
          {"Look", {"opacity", "faded_opacity", "fade", "background", "background_opacity", "parchment", "smooth", "wind"}},
          {"Dungeons", {"dungeons", "zoom_turns", "zoom_turn_with"}},
          {"Buttons", {"key_toggle", "key_size", "key_view", "key_chart"}}},
         {"debug"},
         {{"zoom_turn_with", "camera", "Camera"}, {"zoom_turn_with", "link", "Link"},
          {"*key", "left", "D-pad left"}, {"*key", "right", "D-pad right"}, {"*key", "up", "D-pad up"},
          {"*key", "down", "D-pad down"}, {"*key", "l3", "Left stick click"}, {"*key", "r3", "Right stick click"},
          {"*key", "none", "None"}},
         "Minimap HD by Chironeyes (gamebanana.com/mods/727687)"},
        {"gc-minimap",
         {{"Position and size", {"x", "y", "size"}}, {"Look", {"opacity", "style"}}, {"Dungeons", {"dungeons"}}},
         {},
         {{"style", "vector", "Vector"}, {"style", "original", "Original"}}},
    };
    return all;
}
const Layout* layout_of(const std::string& mod) {
    for (const auto& l : layouts()) if (mod == l.mod) return &l;
    return nullptr;
}
// an enum value's name ("*key": every option whose id starts with "key_")
std::string choice_label(const Layout* l, const std::string& option, const std::string& value) {
    if (l)
        for (const auto& c : l->choices) {
            const std::string o = c.option;
            const bool match = o == "*key" ? option.rfind("key_", 0) == 0 : o == option;
            if (match && value == c.value) return menu_i18n::tr(c.label);
        }
    return value;
}
bool developer_option(const Layout* l, const std::string& option) {
    if (!l) return false;
    for (const char* d : l->developer) if (option == d) return true;
    return false;
}
bool show_developer_options() {
#ifdef __SWITCH__
    return overlay_switch::dev_options();
#else
    return false;
#endif
}

void option_control(guestbuiltin::Mod& m, guestbuiltin::Option& o, const Layout* l, std::string& error) {
    using mods::json::Value;
    const std::string label = std::string(menu_i18n::tr(o.name.c_str())) + "##" + o.id;
    ImGui::SetNextItemWidth(std::min(ImGui::GetFontSize() * 14, ImGui::GetContentRegionAvail().x * 0.6f));
    if (o.type == "bool") {
        bool v = o.value.type == Value::Bool && o.value.boolean;
        if (ImGui::Checkbox(label.c_str(), &v)) guestbuiltin::set_option(m, o, Value(v), true, error);
    } else if (o.type == "number") {
        double v = o.value.type == Value::Number ? o.value.number : o.minimum;
        // whole numbers for whole steps (pixels), two decimals for the rest (opacities); the value snaps to the step
        const char* format = o.step >= 1 ? "%.0f" : "%.2f";
        if (ImGui::SliderScalar(label.c_str(), ImGuiDataType_Double, &v, &o.minimum, &o.maximum, format)) {
            if (o.step > 0) v = std::clamp(o.minimum + std::round((v - o.minimum) / o.step) * o.step, o.minimum, o.maximum);
            guestbuiltin::set_option(m, o, Value(v), false, error);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) guestbuiltin::set_option(m, o, o.value, true, error);
    } else if (o.type == "enum") {
        const std::string current = o.value.string();
        if (ImGui::BeginCombo(label.c_str(), choice_label(l, o.id, current).c_str())) {
            for (const auto& c : o.choices)
                if (ImGui::Selectable(choice_label(l, o.id, c).c_str(), c == current))
                    guestbuiltin::set_option(m, o, Value(c), true, error);
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextUnformatted((std::string(menu_i18n::tr(o.name.c_str())) + ": " + o.value.string()).c_str());  // text options: options.json
    }
    if (!o.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", menu_i18n::tr(o.description.c_str()));
}

std::string g_open;      // the mod whose page is open ("" the list)
int g_page_frame = -10;  // the frame its page was last drawn: a page not drawn last frame (another tab) is closed
bool g_scroll_top = false;

}  // namespace

void draw() {
    auto& list = guestbuiltin::list();
    for (auto& m : list) {
        ImGui::PushID(m.id.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        // the whole row opens the page: one item that starts in the switches' column, so a controller going down
        // the built-in mods' switches reaches it (a disabled switch can't take the focus)
        const ImVec2 at = ImGui::GetCursorPos();
        const float height = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetTextLineHeight();
        if (ImGui::Selectable("##open", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, height))) {
            g_open = m.id;
            g_page_frame = ImGui::GetFrameCount();  // drawn from the next frame on: not taken for a page left behind
            g_scroll_top = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", menu_i18n::tr("Built into this game: to remove it, build the game again without it."));
        // always on: it is part of this build (the mod's own options turn its parts off)
        ImGui::SetCursorPos(at);
        bool on = m.status.empty();
        ImGui::BeginDisabled();
        ImGui::Checkbox("##on", &on);
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted((m.name + "  \xE2\x80\xBA").c_str());
        if (!m.status.empty()) wrapped(kBad, menu_i18n::tr("Not running: ") + m.status);
        else if (m.changed) wrapped(kWarn, menu_i18n::tr("Changes apply after a restart."));
        else wrapped(kGood, menu_i18n::tr("On"));
        ImGui::PopID();
    }
}

bool page(bool back, bool& back_used) {
    const int frame = ImGui::GetFrameCount();
    if (g_page_frame < frame - 1) g_open.clear();  // the tab or the area changed since: the list again
    if (g_open.empty()) return false;
    auto& list = guestbuiltin::list();
    auto it = std::find_if(list.begin(), list.end(), [](const guestbuiltin::Mod& m) { return m.id == g_open; });
    if (it == list.end()) { g_open.clear(); return false; }
    g_page_frame = frame;
    if (ImGui::Button((std::string("\xE2\x80\xB9 ") + menu_i18n::tr("Built-in") + "##code_mod_back").c_str()) ||
        (back && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))) {
        back_used = back;
        g_open.clear();
        return false;
    }
    if (g_scroll_top) { ImGui::SetScrollHereY(0); g_scroll_top = false; }
    ImGui::Separator();
    guestbuiltin::Mod& m = *it;
    const Layout* l = layout_of(m.id);
    ImGui::TextUnformatted(m.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m.version.c_str());
    if (!m.status.empty()) wrapped(kBad, menu_i18n::tr("Not running: ") + m.status);
    else if (!m.description.empty()) ImGui::TextWrapped("%s", menu_i18n::tr(m.description.c_str()));
    if (l && l->credit) {
        char line[256];
        std::snprintf(line, sizeof line, menu_i18n::tr("Based on %s"), l->credit);
        wrapped(kMuted, line);
    }
    // the mods read their options when the game starts
    if (m.changed)
        wrapped(kWarn, menu_i18n::tr("Saved. The mods read their options when the game starts: close the game (HOME) and start it "
                                     "again to see the change."));
    else
        wrapped(kMuted, menu_i18n::tr("Changes apply after a restart."));
    static std::string error;
    std::vector<bool> shown(m.options.size(), false);
    auto draw_option = [&](size_t i) {
        shown[i] = true;
        ImGui::PushID(m.options[i].id.c_str());
        option_control(m, m.options[i], l, error);
        ImGui::PopID();
    };
    const bool developer = show_developer_options();
    if (l)
        for (const auto& g : l->groups) {
            bool header = false;
            for (const char* id : g.options)
                for (size_t i = 0; i < m.options.size(); i++)
                    if (!shown[i] && m.options[i].id == id) {
                        if (!header) { ImGui::SeparatorText(menu_i18n::tr(g.name)); header = true; }
                        draw_option(i);
                    }
        }
    bool other = false;
    for (size_t i = 0; i < m.options.size(); i++) {
        if (shown[i] || (developer_option(l, m.options[i].id) && !developer)) continue;
        if (!other) { ImGui::SeparatorText(menu_i18n::tr(l ? "Other" : "Options")); other = true; }
        draw_option(i);
    }
    if (!error.empty()) wrapped(kBad, error);
    return true;
}

}  // namespace guestbuiltinui
