// The window in which draws wait for their compiling shaders (shader_wait.h).
#include "shader_wait.h"

#include <atomic>
#include <cstdlib>

#include "../../runtime.h"
#include "../../mods/mods.h"
#include "guest_addr.h"

namespace gfxdk::shader_wait {
namespace {

constexpr uint64_t kAfterFrames = 120;  // ~4 s at 30 fps after a fade or a door event ends
constexpr uint64_t kBootFrames = 600;   // ~20 s: the logos and the title screen

int mode() {
    static const int m = [] {
        const char* e = getenv("WWHD_DK_SHADER_WAIT");
        const int v = e && *e ? atoi(e) : 1;
        const int clamped = v < 0 ? 0 : v > 2 ? 2 : v;
        LOG("[dk] shader wait (WWHD_DK_SHADER_WAIT=%d): %s", clamped,
            clamped == 0   ? "draws of compiling shaders are skipped"
            : clamped == 2 ? "draws always wait for their shaders"
                           : "draws wait for their shaders around scene changes, door events and start-up, and are "
                             "skipped during play");
        return clamped;
    }();
    return m;
}

std::atomic<bool> g_active{false};
bool g_signalSeen = false;
uint64_t g_lastSignal = 0;

}  // namespace

void frame(uint64_t frame) {
    const int m = mode();
    if (m != 1) {
        g_active.store(m == 2, std::memory_order_relaxed);
        return;
    }
    static const uint32_t kOverlap = GD(0x101F36CC);  // l_fopOvlpM_overlap[0] (mods/turbo.cpp kOverlap)
    const bool signal = ld32(kOverlap) != 0 || mods::door_event_running();
    if (signal) {
        g_signalSeen = true;
        g_lastSignal = frame;
    }
    const bool on = frame < kBootFrames || (g_signalSeen && frame - g_lastSignal <= kAfterFrames);
    if (on != g_active.load(std::memory_order_relaxed))
        LOG("[dk] shader wait window %s (frame %llu)", on ? "opens" : "closes", (unsigned long long)frame);
    g_active.store(on, std::memory_order_relaxed);
}

bool active() { return g_active.load(std::memory_order_relaxed); }

}  // namespace gfxdk::shader_wait
