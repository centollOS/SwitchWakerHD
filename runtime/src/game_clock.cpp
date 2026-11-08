// SPDX-License-Identifier: MPL-2.0
#include "game_clock.h"
#include <chrono>
#include <atomic>
#include <mutex>
namespace game_clock {
namespace {
std::mutex mutex;  // rate changes
Clock clock;
std::atomic<bool> has_shift{false};
// (this fork) The clock is read on every guest time-base read (ppc_timebase, OSGetTime) and timed wait once fast
// forward ran once, from every thread: under the mutex those reads queued for one lock, also back at rate 1.
// Readers take the published state instead: Clock::now(host) = host * rate + now(0), the pair kept under a
// sequence number (odd while a rate change writes it; rate changes are rare: fast forward on or off).
std::atomic<uint32_t> seq{0};
std::atomic<int64_t> pub_base{0};  // Clock::now(0)
std::atomic<unsigned> pub_rate{1};
int64_t host_now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
Sample sample() {
    for (;;) {
        const uint32_t s0 = seq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        const int64_t base = pub_base.load(std::memory_order_relaxed);
        const unsigned rate = pub_rate.load(std::memory_order_relaxed);
        // the host time inside the window: a rate change anchored before it bumps seq first, so this read retries
        // (with the time read after the check, the old rate could run past the change's anchor: time went back)
        const int64_t host = host_now();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (seq.load(std::memory_order_relaxed) != s0) continue;
        return {host, host * int64_t(rate) + base, rate};
    }
}
void set_rate(unsigned rate) {
    std::lock_guard<std::mutex> lock(mutex);
    const uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);  // (before the change's anchor time is read)
    std::atomic_thread_fence(std::memory_order_seq_cst);
    clock.set_rate(host_now(), rate);
    pub_base.store(clock.now(0), std::memory_order_relaxed);
    pub_rate.store(clock.rate(), std::memory_order_relaxed);
    seq.store(s + 2, std::memory_order_release);
    if (rate > 1 && rate <= 4) has_shift = true;
}
unsigned rate() { return has_shift.load(std::memory_order_relaxed) ? pub_rate.load(std::memory_order_relaxed) : 1; }
bool shifted() { return has_shift.load(std::memory_order_relaxed); }
}
