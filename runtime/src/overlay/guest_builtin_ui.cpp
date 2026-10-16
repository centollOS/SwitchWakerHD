// SwitchWakerHD (this fork): see guest_builtin_ui.h.
#include "guest_builtin_ui.h"

#include <algorithm>
#include <string>

#include "../mods/guest_builtin.h"
#include "imgui.h"

namespace guestbuiltinui {

// the Mods tab's colours (mods_tab.h)
static const ImVec4 kMuted(0.70f, 0.78f, 0.84f, 1.0f), kWarn(1.0f, 0.80f, 0.35f, 1.0f), kBad(1.0f, 0.50f, 0.45f, 1.0f);

static void wrapped(const ImVec4& colour, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

static void option_control(guestbuiltin::Mod& m, guestbuiltin::Option& o, std::string& error) {
    using mods::json::Value;
    ImGui::SetNextItemWidth(std::min(ImGui::GetFontSize() * 14, ImGui::GetContentRegionAvail().x * 0.6f));
    if (o.type == "bool") {
        bool v = o.value.type == Value::Bool && o.value.boolean;
        if (ImGui::Checkbox(o.name.c_str(), &v)) guestbuiltin::set_option(m, o, Value(v), true, error);
    } else if (o.type == "number") {
        double v = o.value.type == Value::Number ? o.value.number : o.minimum;
        if (ImGui::SliderScalar(o.name.c_str(), ImGuiDataType_Double, &v, &o.minimum, &o.maximum, "%.3g"))
            guestbuiltin::set_option(m, o, Value(v), false, error);
        if (ImGui::IsItemDeactivatedAfterEdit()) guestbuiltin::set_option(m, o, o.value, true, error);
    } else if (o.type == "enum") {
        const std::string current = o.value.string();
        if (ImGui::BeginCombo(o.name.c_str(), current.c_str())) {
            for (const auto& c : o.choices)
                if (ImGui::Selectable(c.c_str(), c == current)) guestbuiltin::set_option(m, o, Value(c), true, error);
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextUnformatted((o.name + ": " + o.value.string()).c_str());  // text options: options.json
    }
    if (!o.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", o.description.c_str());
}

void draw() {
    auto& list = guestbuiltin::list();
    if (list.empty()) return;
    ImGui::SeparatorText("Code mods in this build");
    wrapped(kMuted, "Built into the game when it was made (make_sd.py --guest-mod). To add or remove one, build the "
                    "game again.");
    // above the options: the last one a controller reaches is at the bottom of the tab, where a line under it is
    // out of view
    if (std::any_of(list.begin(), list.end(), [](const guestbuiltin::Mod& m) { return m.changed; }))
        wrapped(kWarn, "Saved. The mods read their options when the game starts: close the game (HOME) and start it "
                       "again to see the change.");
    for (auto& m : list) {
        ImGui::PushID(m.id.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted((m.name + "  " + m.version).c_str());
        if (!m.status.empty()) wrapped(kBad, "Not running: " + m.status);
        else if (!m.description.empty()) wrapped(kMuted, m.description);
        static std::string error;
        for (auto& o : m.options) {
            ImGui::PushID(o.id.c_str());
            option_control(m, o, error);
            ImGui::PopID();
        }
        if (!error.empty()) wrapped(kBad, error);
        ImGui::PopID();
    }
}

}  // namespace guestbuiltinui
