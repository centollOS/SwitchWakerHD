// (this fork, s2/cores) the Switch 2 tab's cores 4-5 option (cores_switch.h: extra_mask): off by default, saved as
// s2Cores45. It acts only when the process's core mask (the loader's NPDM) includes cores 4 and 5.
#include <switch.h>

#include <string>

#include "imgui.h"
#include "cores_switch.h"
#include "s2_tab_switch2.h"
#include "../runtime.h"

namespace s2tab {
void start_cores() {
    std::string v;
    const bool on = saved("s2Cores45", v) && v == "1";
    cores::extra_wanted().store(on, std::memory_order_relaxed);
    const uint64_t m = cores::process_mask();
    LOG("[s2] cores: process core mask 0x%llx, cores 4-5 %s; option s2Cores45 %s%s", (unsigned long long)m,
        (m & 0x30) == 0x30 ? "available" : (m & 0x30) ? "partly available" : "not available", on ? "on" : "off",
        on && !(m & 0x30) ? " (no effect)" : on ? ": worker threads and shader compiles may use them" : "");
}
void section_cores() {
    heading("CPU cores");
    const uint64_t m = cores::process_mask();
    const bool have = (m & 0x30) != 0;
    bool v = cores::extra_wanted().load(std::memory_order_relaxed);
    ImGui::BeginDisabled(!have);
    if (ImGui::Checkbox("Use cores 4-5 for worker threads##s2cores", &v)) {
        cores::extra_wanted().store(v, std::memory_order_relaxed);
        LOG("[s2] cores 4-5 for worker threads: %s (menu)", v ? "on" : "off");
        save("s2Cores45", v ? "1" : "0");
    }
    ImGui::EndDisabled();
    help("Off (default): the game's threads use cores 0-3 as before.\n"
         "On: the game's worker threads (emulated cores 0 and 2) and the shader compiles may also run on cores 4\n"
         "and 5, and background shader work moves there. Worker threads and compiles: at once (the next time each\n"
         "one runs); the background shader list's thread and the shader helper threads: at the next start.\n"
         "Only when the process may use cores 4-5. Saved (s2Cores45).");
    note("This process may use cores: 0x%llx (%s).", (unsigned long long)m,
         have ? "4-5 included" : "no 4-5: the loader does not give them, the option has no effect");
}
}  // namespace s2tab
