// Native fast paths for small game functions the main thread calls very often (docs/cpu-threading-plan.md, phase
// 2.1). The recompiled form of a tiny accessor still builds a guest stack frame, stores and reloads registers and
// checks for preemption; at night at sea the star field's projection calls these two thousands of times a frame
// (~4% of the main thread together at stock clocks).
//
// The calls are redirected at link time (cmake/SwitchPort.cmake: -Wl,--wrap=f_X): every call from another
// generated file goes to __wrap_f_X, which takes the fast path when it applies and the original (__real_f_X)
// otherwise. Calls inside the function's own file stay direct. No change to the generated code.
//
// What each fast path leaves: the registers and condition bits the original's path leaves (r3, r0, r12, cr0), the
// same preemption check (PPC_ENTER); not the original's stores into its own stack frame below the caller's stack
// pointer (dead data no code reads). A/B: the Switch tab's Debug section, the debug server's "native 0|1".
#include "native_helpers_switch.h"

#include <atomic>
#include <cstdint>

#include "../runtime.h"
#include "guest_addr.h"

extern "C" {
void __real_f_025200D4(Cpu* c);
void __real_f_02524628(Cpu* c);
void __wrap_f_025200D4(Cpu* c);
void __wrap_f_02524628(Cpu* c);
}

namespace native_helpers {
namespace {
std::atomic<bool> g_on{false};

// dComIfGp_get (025200D4): &g_dComIfG_gameInfo once its one-time set-up has run (a guard word)
const uint32_t kGameInfo = GD(0x1046F0B0);
const uint32_t kGameInfoGuard = GD(0x104753A8);
// hdGameInfo_dComIfGd_getProjViewMtx (02524628): the HD view-projection matrix when the draw info has one
constexpr uint32_t kProjView = 0x5FA4;      // g_dComIfG_gameInfo + 0x5FA4: the projection view (or 0)
constexpr uint32_t kProjViewMtx = 0x1A4;    // its matrix

inline void enter(Cpu* c) {
    if (__builtin_expect(c->preempt, 0)) ppc_preempt(c);
}
}  // namespace

bool on() { return g_on.load(std::memory_order_relaxed); }
void set_on(bool v) {
    if (g_on.exchange(v) != v) LOG("[native] game helpers %s (dComIfGp_get, getProjViewMtx)", v ? "on" : "off");
}

}  // namespace native_helpers

using namespace native_helpers;

extern "C" void __wrap_f_025200D4(Cpu* c) {
    if (on()) {
        const uint32_t guard = ld32(kGameInfoGuard);
        if (guard) {
            enter(c);
            c->r[0] = c->lr;            // (the original's epilogue reloads the saved lr into r0)
            c->r[12] = kGameInfoGuard;  // (its load with update left the guard's address)
            cr_set_s_m(c, 0, (int32_t)guard, 0, 4);
            c->r[3] = kGameInfo;
            return;
        }
    }
    __real_f_025200D4(c);
}

extern "C" void __wrap_f_02524628(Cpu* c) {
    if (on() && ld32(kGameInfoGuard)) {
        const uint32_t view = ld32(kGameInfo + kProjView);
        if (view) {
            enter(c);
            c->r[0] = c->lr;
            c->r[12] = view;
            cr_set_s_m(c, 0, (int32_t)view, 0, 4);
            c->r[3] = view + kProjViewMtx;
            return;
        }
    }
    __real_f_02524628(c);
}
