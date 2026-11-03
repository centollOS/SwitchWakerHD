#pragma once
// SwitchWakerHD: core 3 for background work (docs/core3-plan.md). The forwarder's NPDM allows cores 0-3
// (tools/switch/forwarder/patch_hbl.py); an older forwarder or hbmenu gives 0-2. Measured on the console
// ("coreprobe"): a thread on core 3 runs 81-89% of the time at priority 0x2C, 76-78% at 0x3B, in the title and
// in play. WWHD_CORE3=0 (settings.ini [dev]) or the debug server's "core3 0" keeps everything on cores 0-2.
#include <switch.h>

#include <atomic>
#include <cstdlib>

namespace cores {
// whether the process may use core 3 (the forwarder's NPDM)
inline bool allowed() {
    static const bool a = [] {
        u64 mask = 0;
        return R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && (mask & 0x8);
    }();
    return a;
}
// WWHD_CORE3=0 at the start, or the debug server's "core3 0|1" (comparisons): read at each decision
inline std::atomic<bool>& wanted() {
    static std::atomic<bool> w{[] {
        const char* e = getenv("WWHD_CORE3");
        return !(e && *e && atoi(e) == 0);
    }()};
    return w;
}
// true when background work goes to core 3 now
inline bool core3() { return allowed() && wanted().load(std::memory_order_relaxed); }
}  // namespace cores
