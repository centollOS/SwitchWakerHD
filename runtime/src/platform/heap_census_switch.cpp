// (this fork, memory round 2) WWHD_HEAP_CENSUS builds only (cmake -DWWHD_HEAP_CENSUS=ON, never a release): malloc,
// free and friends are wrapped (-Wl,--wrap) and every live block is counted against the call site that made it
// (its return address and three more from the frame records), so a growing session says which code holds the
// memory. Debug server: `heap mark` remembers each site's bytes, `heap diff [n]` lists the sites that grew since
// (`heap top [n]`: the biggest). Addresses are printed against `anchor`, whose address in wwhd.elf
// (nm) turns them into ELF addresses for addr2line (tools/switch/heap_census.py does both).
//
// Blocks newlib allocates for itself (_malloc_r: stdio buffers) are not seen; freeing them is (no match, passed on).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include <switch.h>

#include "debug_server.h"

extern "C" {
void* __real_malloc(size_t);
void __real_free(void*);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void* __real_memalign(size_t, size_t);
void* __real_aligned_alloc(size_t, size_t);
int __real_posix_memalign(void**, size_t, size_t);
}

namespace heap_census {
namespace {
constexpr int kDepth = 4;
constexpr uint32_t kBlocks = 1u << 21;  // live blocks tracked (32 MiB); past that, new ones are not counted
constexpr uint32_t kSites = 1u << 15;

struct Block {
    uintptr_t ptr;  // 0 = empty
    uint32_t size;
    uint32_t site;
};
struct Site {
    uintptr_t pc[kDepth];
    int64_t bytes, count, markBytes, markCount;
};
Block g_blocks[kBlocks];
Site g_sites[kSites];
// a kernel mutex, not a spin lock: Horizon does not time-slice a higher-priority thread spinning on a core away
// from the lower-priority one holding the lock (the first census build hung that way); this one inherits priority
Mutex g_lock = 0;
uint64_t g_untracked = 0, g_live = 0;

struct Guard {
    Guard() { mutexLock(&g_lock); }
    ~Guard() { mutexUnlock(&g_lock); }
};

inline uint32_t hash_ptr(uintptr_t p) { return uint32_t((p >> 4) * 0x9E3779B97F4A7C15ull >> 43) & (kBlocks - 1); }

// the call site: the wrapper's return address, then the frame records' (x29 chain) while they look sane
__attribute__((noinline)) uint32_t site_here() {
    uintptr_t pc[kDepth] = {};
    auto** fp = static_cast<void**>(__builtin_frame_address(0));
    fp = static_cast<void**>(fp[0]);  // the wrapper's frame (site_here is called from it)
    for (int i = 0; i < kDepth && fp; i++) {
        pc[i] = reinterpret_cast<uintptr_t>(fp[1]);
        auto** next = static_cast<void**>(fp[0]);
        if (next <= fp || reinterpret_cast<uintptr_t>(next) - reinterpret_cast<uintptr_t>(fp) > (1u << 20) ||
            (reinterpret_cast<uintptr_t>(next) & 7))
            break;
        fp = next;
    }
    uint64_t h = 0xCBF29CE484222325ull;
    for (uintptr_t p : pc) h = (h ^ p) * 0x100000001B3ull;
    for (uint32_t i = uint32_t(h >> 40) & (kSites - 1), n = 0; n < kSites; i = (i + 1) & (kSites - 1), n++) {
        Site& s = g_sites[i];
        if (!memcmp(s.pc, pc, sizeof pc)) return i;
        if (!s.pc[0] && !s.count && !s.bytes) {
            memcpy(s.pc, pc, sizeof pc);
            return i;
        }
    }
    return 0;  // full: counted as site 0
}

void add(void* p, size_t size, uint32_t site) {
    if (!p) return;
    const uintptr_t u = reinterpret_cast<uintptr_t>(p);
    if (g_live >= kBlocks * 3 / 4) {
        g_untracked++;
        return;
    }
    uint32_t i = hash_ptr(u);
    while (g_blocks[i].ptr) i = (i + 1) & (kBlocks - 1);
    g_blocks[i] = {u, uint32_t(std::min<size_t>(size, 0xFFFFFFFFu)), site};
    g_live++;
    g_sites[site].bytes += int64_t(size);
    g_sites[site].count++;
}

void remove(void* p) {
    if (!p) return;
    const uintptr_t u = reinterpret_cast<uintptr_t>(p);
    uint32_t i = hash_ptr(u);
    while (g_blocks[i].ptr && g_blocks[i].ptr != u) i = (i + 1) & (kBlocks - 1);
    if (!g_blocks[i].ptr) return;  // not ours (newlib's own, or not tracked)
    Site& s = g_sites[g_blocks[i].site];
    s.bytes -= g_blocks[i].size;
    s.count--;
    g_live--;
    // backward-shift deletion (linear probing without tombstones)
    for (uint32_t j = (i + 1) & (kBlocks - 1); g_blocks[j].ptr; j = (j + 1) & (kBlocks - 1)) {
        const uint32_t home = hash_ptr(g_blocks[j].ptr);
        if (((j - home) & (kBlocks - 1)) >= ((j - i) & (kBlocks - 1))) {
            g_blocks[i] = g_blocks[j];
            i = j;
        }
    }
    g_blocks[i].ptr = 0;
}

void anchor() {}

debugsrv::Reply command(const debugsrv::Args& a) {
    const std::string what = a.empty() ? "top" : a[0];
    const size_t n = a.size() > 1 ? size_t(strtoul(a[1].c_str(), nullptr, 10)) : 40;
    // nothing here may allocate while g_lock is held: the wrapped malloc takes it too (libnx mutexes are not
    // recursive; the second census build hung on a vector's push_back here)
    static uint32_t order[kSites];
    size_t count = 0;
    int64_t total = 0, totalMark = 0;
    {
        Guard g;
        if (what == "mark") {
            for (Site& s : g_sites) {
                s.markBytes = s.bytes;
                s.markCount = s.count;
            }
            count = ~size_t(0);  // (the reply is made after the lock: its string allocates)
        }
        for (uint32_t i = 0; count != ~size_t(0) && i < kSites; i++) {
            const Site& s = g_sites[i];
            total += s.bytes;
            totalMark += s.markBytes;
            if (s.bytes || s.markBytes) order[count++] = i;
        }
        const bool diff = what == "diff";
        if (count != ~size_t(0)) std::sort(order, order + count, [&](uint32_t x, uint32_t y) {
            const Site &a = g_sites[x], &b = g_sites[y];
            return diff ? a.bytes - a.markBytes > b.bytes - b.markBytes : a.bytes > b.bytes;
        });
        if (count != ~size_t(0)) count = std::min(count, n);
    }
    if (count == ~size_t(0)) return debugsrv::ok("marked\n");
    char line[256];
    std::string out;
    snprintf(line, sizeof line, "anchor %p; live %lld bytes in %llu blocks (%lld at the mark); untracked %llu\n",
             reinterpret_cast<void*>(&anchor), (long long)total, (unsigned long long)g_live, (long long)totalMark,
             (unsigned long long)g_untracked);
    out += line;
    for (size_t k = 0; k < count; k++) {
        const Site& s = g_sites[order[k]];
        snprintf(line, sizeof line, "%12lld %+12lld %8lld %+8lld  %p %p %p %p\n", (long long)s.bytes,
                 (long long)(s.bytes - s.markBytes), (long long)s.count, (long long)(s.count - s.markCount),
                 reinterpret_cast<void*>(s.pc[0]), reinterpret_cast<void*>(s.pc[1]), reinterpret_cast<void*>(s.pc[2]),
                 reinterpret_cast<void*>(s.pc[3]));
        out += line;
    }
    return debugsrv::ok(out);
}
}  // namespace

void register_commands() {
    debugsrv::add_command("heap", "heap mark | diff [n] | top [n]   malloc by call site (WWHD_HEAP_CENSUS builds)",
                          command);
}
}  // namespace heap_census

extern "C" {
void* __wrap_malloc(size_t size) {
    void* p = __real_malloc(size);
    heap_census::Guard g;
    heap_census::add(p, size, heap_census::site_here());
    return p;
}
void __wrap_free(void* p) {
    if (!p) return;
    {
        heap_census::Guard g;
        heap_census::remove(p);
    }
    __real_free(p);
}
void* __wrap_calloc(size_t n, size_t size) {
    void* p = __real_calloc(n, size);
    heap_census::Guard g;
    heap_census::add(p, n * size, heap_census::site_here());
    return p;
}
void* __wrap_realloc(void* old, size_t size) {
    void* p = __real_realloc(old, size);
    if (!p && size) return p;  // failed: the old block stays as it was
    heap_census::Guard g;
    heap_census::remove(old);
    heap_census::add(p, size, heap_census::site_here());
    return p;
}
void* __wrap_memalign(size_t align, size_t size) {
    void* p = __real_memalign(align, size);
    heap_census::Guard g;
    heap_census::add(p, size, heap_census::site_here());
    return p;
}
void* __wrap_aligned_alloc(size_t align, size_t size) {
    void* p = __real_aligned_alloc(align, size);
    heap_census::Guard g;
    heap_census::add(p, size, heap_census::site_here());
    return p;
}
int __wrap_posix_memalign(void** out, size_t align, size_t size) {
    const int r = __real_posix_memalign(out, align, size);
    if (r == 0) {
        heap_census::Guard g;
        heap_census::add(*out, size, heap_census::site_here());
    }
    return r;
}
}
