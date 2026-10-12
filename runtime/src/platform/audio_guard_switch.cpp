// The game's DSP-overload guard switched off (audio_guard_switch.h).
#include "audio_guard_switch.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "../runtime.h"
#include "guest_addr.h"

namespace audio_guard {
namespace {

constexpr uint32_t kGameThreshold = 0x3F8CCCCD;  // 1.1f, as the game's data holds it
constexpr uint32_t kNever = 0;                   // 0.0f: a subframe's ratio is never below it

// WWHD_AUDIO_DSP_GUARD=1 keeps the game's guard
std::atomic<bool> g_kept{[] {
    const char* e = getenv("WWHD_AUDIO_DSP_GUARD");
    return e && *e == '1';
}()};
std::atomic<int> g_applied{-1};  // the setting written into the game's data (-1: not yet)
bool g_unexpected = false;

}  // namespace

void frame() {
    const int want = g_kept.load(std::memory_order_relaxed) ? 1 : 0;
    if (g_applied.load(std::memory_order_relaxed) == want || g_unexpected) return;
    const uint32_t a = GD(0x10170030);
    const uint32_t v = ld32(a);
    if (v != kGameThreshold && v != kNever) {
        // not the build this was checked on: leave the game's data alone
        g_unexpected = true;
        LOG("[audio] DSP-overload guard: threshold at %08X is %08X, not 1.1: left as it is", a, v);
        return;
    }
    st32(a, want ? kGameThreshold : kNever);
    g_applied = want;
    LOG("[audio] DSP-overload guard (028137E0, \"kill DSP channel\"): %s", want ? "the game's own (1.1)" : "off");
}

void set_kept(bool on) { g_kept = on; }
bool kept() { return g_kept.load(std::memory_order_relaxed); }

}  // namespace audio_guard
