// SwitchWakerHD (this fork): host thread placement and priorities, included from host.h. They act on the
// Switch (Horizon cores and priorities) and do nothing elsewhere, so shared code calls them without #ifdefs.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <thread>
#ifdef __SWITCH__
#include <pthread.h>
#include <switch.h>
#endif

namespace host {
// Horizon starts every thread on the process's default core; let the scheduler use the three
// application cores, preferring `core` (the emulated Espresso core, or any host thread's choice).
inline void set_thread_core(uint32_t core) {
#ifdef __SWITCH__
 svcSetThreadCoreMask(threadGetCurHandle(),(s32)(core%3),0x7);
#else
 (void)core;
#endif
}
// WWHD_CORE_LAYOUT (on by default on the Switch, =0 for the old placement): the game's main thread
// (emulated core 1) gets host core 1 to itself; the render thread, Mesa's GL thread and the guest
// threads of emulated cores 0 and 2 share host cores 0 and 2. Before, the higher-priority host
// threads kept landing on the main thread's core and preempting it (it held its emulated core 70%
// of the time but ran only 63%), and the main thread is what limits the frame rate in busy views.
inline bool core_layout_on() {
#ifdef __SWITCH__
    static const bool on = [] {
        const char* e = getenv("WWHD_CORE_LAYOUT");
        return !(e && *e == '0');
    }();
    return on;
#else
    return false;
#endif
}
// a thread's preferred host core and the set it may run on (bit n: core n); Switch only
inline void set_thread_cores(uint32_t preferred, uint32_t mask) {
#ifdef __SWITCH__
 svcSetThreadCoreMask(threadGetCurHandle(), (s32)preferred, mask);
#else
 (void)preferred; (void)mask;
#endif
}
// placement of a thread of emulated core `core` (guest threads, the render and GL threads)
inline void place_thread(uint32_t core) {
    if (!core_layout_on()) return set_thread_core(core);
    if (core % 3 == 1) set_thread_cores(1, 0x2);
    else set_thread_cores(core % 3, 0x5);
}
// libnx creates every pthread at priority 59, the only one Horizon time-slices (10 ms) on cores 0-2.
// Host service threads (GX2 render, audio, alarms) move above the guest threads so they run as soon
// as they have work instead of waiting for a busy guest thread's slice to end.
inline void raise_thread_priority() {
#ifdef __SWITCH__
 svcSetThreadPriority(threadGetCurHandle(),0x2C);
 // these run above the guest threads: off the main thread's core (WWHD_CORE_LAYOUT)
 if (core_layout_on()) set_thread_cores(0, 0x5);
#endif
}
// A detached host thread. Horizon commits a small default stack for std::thread, so threads that run
// guest code or the shader compilers ask for `stack` bytes there.
inline void start_thread(void (*fn)(), size_t stack) {
#ifdef __SWITCH__
 pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr,stack);
 pthread_t t;
 if(pthread_create(&t,&attr,[](void* f)->void* { raise_thread_priority(); ((void(*)())f)(); return nullptr; },(void*)fn)==0) pthread_detach(t);
 pthread_attr_destroy(&attr);
#else
 (void)stack; std::thread(fn).detach();
#endif
}
}  // namespace host
