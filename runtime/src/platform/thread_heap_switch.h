// A private heap for a thread that allocates a lot and runs next to others doing the same: the deko3d shader
// compile threads (uam / Mesa allocate and free constantly). newlib's malloc has one lock for the whole process, and
// three compile threads spent 62-73% of their time waiting on it (docs/cpu-threading-plan.md). A thread that calls
// attach() gets a region of its own: blocks of 16 B to 4 KiB come from it without a lock; larger blocks, a full
// region and every other thread use newlib's heap as before. free() / realloc() from any thread recognise a region's
// blocks by their address (a block freed by another thread goes back through a lock-free list the owner drains).
// malloc, free and their relatives are reached through -Wl,--wrap (cmake/SwitchPort.cmake).
#pragma once
#include <cstddef>
#include <cstdint>

namespace thread_heap {
// the calling thread allocates from a region of `bytes` (rounded to 64 KiB) from now on; false: no region (the
// thread keeps newlib's heap). At most kMaxRegions threads.
bool attach(size_t bytes);
constexpr int kMaxRegions = 4;
struct Stats {
    uint64_t allocs = 0, frees = 0, remoteFrees = 0, fallbacks = 0;  // since start (all regions)
    size_t chunksUsed = 0, chunksTotal = 0;
};
Stats stats();
}  // namespace thread_heap
