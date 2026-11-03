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

// The game's threads of emulated cores 0 and 2 (WorkerMgr, update_ubo, resource and audio threads) on host cores 0, 2
// and 3 instead of 0 and 2, where the render thread and the host service threads preempt them; the main thread, alone
// on core 1, waits for their jobs (docs/guest-core3-plan.md: +1.9% fps at the Outset village with PGO). On when the
// process has core 3; WWHD_GUEST_CORE3=0 (settings.ini [dev]) or the debug server's "guestcore3 0|1": read each time a
// thread takes its emulated core. The background shader list (also core 3) then goes about half as fast.
inline std::atomic<bool>& guest_wanted() {
    static std::atomic<bool> w{[] {
        const char* e = getenv("WWHD_GUEST_CORE3");
        return !(e && *e && atoi(e) == 0);
    }()};
    return w;
}
// a guest thread of emulated core `core` that just took it: its host cores follow guest_wanted() (the thread's own
// setting is changed, so a switch at run time reaches every thread the next time it runs)
inline void place_guest_thread(uint32_t core) {
    if (core % 3 == 1) return;  // the main thread's core keeps its own placement
    thread_local int applied = -1;
    const int want = int(core % 3) * 2 + (allowed() && guest_wanted().load(std::memory_order_relaxed) ? 1 : 0);
    if (want == applied) return;
    applied = want;
    svcSetThreadCoreMask(threadGetCurHandle(), s32(core % 3), (want & 1) ? 0xD : 0x5);
}
}  // namespace cores
