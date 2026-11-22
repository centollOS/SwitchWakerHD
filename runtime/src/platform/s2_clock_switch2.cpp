// (this fork, combo/all) the Switch 2 tab's CPU clock section: the perf/clocks setting (clocks_switch2.cpp, saved as
// "s2CpuClock", default Boost) moved from the Picture tab next to the s2/cores option, so every Switch 2 option is on
// one page. The clock thread itself starts from main_switch.cpp (switch2_clocks_start), as on perf/clocks.
#include <string>

#include "imgui.h"
#include "clocks_switch2.h"
#include "s2_tab_switch2.h"
#include "../overlay/hostui.h"

namespace s2tab {
void section_clock() {
    heading("CPU clock");
    const int cc = switch2_clocks::choice();
    const char* fixedBy = switch2_clocks::env_override();
    static const char* const names[] = {"Normal##s2cpu", "Boost 1523 MHz##s2cpu", "Maximum 1702 MHz (experimental)##s2cpu"};
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        ImGui::BeginDisabled(fixedBy != nullptr);
        if (ImGui::RadioButton(names[i], cc == i) && cc != i) hostui::post([i] { switch2_clocks::set_choice(i); });
        ImGui::EndDisabled();
    }
    help("Boost (the default) runs the processor at the console's own boost rate, 1523 MHz instead of about\n"
         "1100 MHz: the game logic needs about a fifth less time, the graphics chip keeps its normal rate.\n"
         "Uses more power, so the battery lasts a little less in handheld mode. Normal: the console's rate.\n"
         "Maximum (experimental): the highest rate the console reports, 1702 MHz; if the console does not keep\n"
         "it, the game goes back to the boost rate and says so here. Applies at once; the console's own rate\n"
         "comes back when the game ends. Saved (s2CpuClock).");
    if (fixedBy) note("Set by %s for this start.", fixedBy);
    else note("%s", switch2_clocks::status().c_str());
}
}  // namespace s2tab
