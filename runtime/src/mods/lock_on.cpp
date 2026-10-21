// Optional enemy lock-on assistance. Native targeting owns visibility and actor lifetime.
#include "mods.h"
#include "lock_on.h"
#include "runtime.h"
#include "guest_addr.h"
#include "input.h"
#include "overlay/overlay.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" void f_024ECCFC_orig(Cpu*);

namespace mods {
namespace {
float startup_range() {
    const char* value = std::getenv("WWHD_MOD_LOCK_ON_RANGE");
    if (!value) return lock_on::kDefaultRange;
    char* end = nullptr;
    const float factor = std::strtof(value, &end);
    return end != value && !*end ? lock_on::clamp_range(factor) : lock_on::kDefaultRange;
}
bool startup_bool(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    return value ? std::atoi(value) != 0 : fallback;
}
std::atomic<bool> enabled{startup_bool("WWHD_MOD_LOCK_ON", false)};
std::atomic<float> range{startup_range()};
std::atomic<bool> cycling{startup_bool("WWHD_MOD_LOCK_ON_CYCLING", true)};
std::mutex input_mutex;
uint32_t available_attention = 0, pending_attention = 0;
uint64_t available_step = 0, pending_step = 0;
bool held = false, consume_held = false, armed = true;
bool recent(uint64_t then, uint64_t now) { return now >= then && now - then <= 2; }
bool playable() {
    // The same native event/pause/scene-transition gates used by fast-forward and save states.
    const uint32_t play = GD(0x1046F0B0);
    return !ld8(play + 0x5292) && !ld8(play + 0x514C) &&
           !ld32(GD(0x101F36CC)) && !ld8(GD(0x101EA069));
}
}
bool extended_lock_on() { return enabled.load(std::memory_order_relaxed); }
void set_extended_lock_on(bool on) {
    if (enabled.exchange(on, std::memory_order_relaxed) != on) lock_on_reset();
}
float lock_on_range() { return range.load(std::memory_order_relaxed); }
void set_lock_on_range(float factor) { range.store(lock_on::clamp_range(factor), std::memory_order_relaxed); }
bool lock_on_cycling() { return cycling.load(std::memory_order_relaxed); }
void set_lock_on_cycling(bool on) {
    if (cycling.exchange(on, std::memory_order_relaxed) != on) lock_on_reset();
}
void lock_on_reset() {
    std::lock_guard lock(input_mutex);
    available_attention = pending_attention = 0;
    armed = false; // a held button must not turn into a new press after load/focus/enable
}
void lock_on_filter_pad(input::PadState& s) {
    std::lock_guard lock(input_mutex);
    const bool down = (s.buttons & input::kStickR) != 0;
    if (!down) { held = consume_held = false; armed = true; return; }
    const bool active = extended_lock_on() && lock_on_cycling() && !overlay::blocks_input() &&
                        available_attention && recent(available_step, step()) && playable();
    if (!active) pending_attention = 0;
    if (!held && armed && active) {
        pending_attention = available_attention;
        pending_step = step();
        consume_held = true;
    }
    held = true;
    // Keep consuming the same physical hold if targeting ends before the button is released.
    if (consume_held) s.buttons &= ~input::kStickR;
}

namespace {
void scale_enemy_distance(Cpu* c, unsigned category, uint32_t attention_info) {
    if (!extended_lock_on() || !lock_on::enemy_channel(category, ld32(attention_info + 0x14))) return;
    const float factor = lock_on_range();
    // The game's predicate still owns angles, height, visibility, masks and ranking. Only its
    // two horizontal distance limits change; no shared game table is patched or left modified.
    c->f[1].ps0 *= factor;
    c->f[2].ps0 *= factor;
}
}

// Called after native button judgement, before the switch/hold targeting state machines. Rebuild
// through native eligibility, then change only the selected list index; Link/camera publication,
// chase/release and target deletion remain native. Never retain actor pointers between frames.
void cycle_after_judgement(Cpu* c, uint32_t attention) {
    // Enabling/disabling resets queued input under input_mutex. Keep the stock path
    // free of guest reads and locking; retain the active check below for concurrent changes.
    if (!extended_lock_on()) return;
    const uint32_t index = ld32(attention + 0xD8);
    const uint32_t previous_count = ld32(attention + 0xD4);
    const uint32_t entry = attention + 0x54 + (index < 8 ? index * 16 : 0);
    const bool active = extended_lock_on() && lock_on_cycling() && !overlay::blocks_input() &&
                        ld8(attention + 0x18) == 1 && previous_count <= 8 && index < previous_count &&
                        ld32(entry + 8) == lock_on::kEnemyCategory && ld32(entry + 12) != UINT32_MAX && playable();
    bool requested;
    {
        std::lock_guard lock(input_mutex);
        requested = active && pending_attention == attention && recent(pending_step, step());
        pending_attention = 0;
        available_attention = active ? attention : 0;
        available_step = step();
    }
    // A simultaneous native Z-target edge has priority (including toggle-mode release).
    if (!requested || ld8(attention + 0x1A) == 1) return;
    const uint32_t old_pid = ld32(entry + 12);
    unsigned char backup[0xD0]; // both native lists, counts and indices; restore on a no-op
    std::memcpy(backup, mem::ptr(attention + 0x54), sizeof backup);
    const Cpu saved = *c;
    guest_call(c, GC(0x024EC6A8), {attention, ld32(attention + 0x10)}); // initList
    guest_call(c, GC(0x024EC72C), {attention}); // makeList: live actors, masks and range/angle checks
    guest_call(c, GC(0x024EC770), {attention}); // sortList: native priority/distance order
    const uint32_t count = ld32(attention + 0xD4);
    uint32_t chosen = UINT32_MAX;
    if (count > 0 && count <= 8) {
        uint32_t start = count - 1;
        for (uint32_t i = 0; i < count; ++i)
            if (ld32(attention + 0x54 + i * 16 + 12) == old_pid) { start = i; break; }
        for (uint32_t offset = 1; offset <= count; ++offset) {
            const uint32_t i = (start + offset) % count;
            const uint32_t candidate = attention + 0x54 + i * 16;
            const uint32_t pid = ld32(candidate + 12);
            if (ld32(candidate + 8) != lock_on::kEnemyCategory || pid == old_pid || pid == UINT32_MAX) continue;
            const uint32_t actor = guest_call(c, GC(0x024EBA14), {candidate}); // resolve PID now
            if (actor && (ld32(actor + 0x39C) & 4)) { chosen = i; break; }
        }
    }
    *c = saved; // guest helpers may clobber volatile registers; the caller sees native judgement
    if (chosen == UINT32_MAX) {
        std::memcpy(mem::ptr(attention + 0x54), backup, sizeof backup);
        return;
    }
    st32(attention + 0xD8, chosen);
    st32(attention + 0x20, ld32(attention + 0x20) | 8); // native target-change notification
    trace("lock-on: cycled enemy %08X -> %08X", old_pid, ld32(attention + 0x54 + chosen * 16 + 12));
}
} // namespace mods

extern "C" void hook_024ECCFC(Cpu* c) {
    const uint32_t attention = c->r[3];
    f_024ECCFC_orig(c);
    mods::cycle_after_judgement(c, attention);
}

// calcWeight: category from the current native channel descriptor, actor in r25.
extern "C" void site_024EB4A8(Cpu* c) {
    mods::scale_enemy_distance(c, ld16(c->r[31]), c->r[25] + 0x388);
}
// chaseAttention: category in the selected native list entry (r28), attention_info in r30.
extern "C" void site_024ECC70(Cpu* c) {
    mods::scale_enemy_distance(c, ld32(c->r[28] + 8), c->r[30]);
}
// EnemyDistance: extend the cutoff, never the measured distance returned to its callers.
extern "C" void site_024EB868(Cpu* c) {
    if (mods::extended_lock_on() && (ld32(c->r[31] + 0x39C) & 4))
        c->f[13].ps0 *= mods::lock_on_range();
}
// Camera advisory: use the same release radius as chaseAttention.
extern "C" void site_024EE574(Cpu* c) {
    if (!mods::extended_lock_on()) return;
    const uint32_t index = ld32(c->r[31] + 0xD8);
    if (index < 8 && ld32(c->r[31] + 0x54 + index * 16 + 8) == mods::lock_on::kEnemyCategory)
        c->f[1].ps0 *= mods::lock_on_range();
}
