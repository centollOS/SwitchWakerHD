// thread_heap_switch.h: per-thread regions of 64 KiB chunks, one block size per chunk (16 B .. 4 KiB, powers of two).
// A chunk starts with its header in its first block; blocks are aligned to their size (the chunk is aligned to 64
// KiB), so memalign up to 4 KiB is served too. The owner allocates and frees without a lock; another thread's free
// goes onto the region's lock-free list for that size, which the owner takes over when its own list is empty.
#include "thread_heap_switch.h"

#if defined(WWHD_HEAP_CENSUS)  // (the heap census wraps malloc itself: no regions there)
namespace thread_heap {
bool attach(size_t) { return false; }
Stats stats() { return {}; }
}  // namespace thread_heap
#else

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <reent.h>

extern "C" {
void* __real_malloc(size_t n);
void __real_free(void* p);
void* __real_calloc(size_t n, size_t s);
void* __real_realloc(void* p, size_t n);
void* __real_memalign(size_t a, size_t n);
void* __real_aligned_alloc(size_t a, size_t n);
int __real_posix_memalign(void** out, size_t a, size_t n);
void __real__free_r(struct _reent* r, void* p);
void* __real__realloc_r(struct _reent* r, void* p, size_t n);
}

namespace thread_heap {
namespace {

constexpr uintptr_t kChunk = 64 * 1024;
constexpr int kMinShift = 4, kMaxShift = 12, kClasses = kMaxShift - kMinShift + 1;  // 16 B .. 4 KiB

struct Free {
    Free* next;
};
struct ChunkHeader {
    uint32_t shift;  // block size 1 << shift
};
struct Region {
    uintptr_t base = 0, end = 0;
    size_t chunks = 0;
    size_t nextChunk = 0;                // the owner's
    Free* local[kClasses] = {};          // the owner's
    std::atomic<Free*> remote[kClasses] = {};
};

Region g_regions[kMaxRegions];
std::atomic<int> g_count{0};
thread_local Region* t_region = nullptr;
std::atomic<uint64_t> g_allocs{0}, g_frees{0}, g_remote{0}, g_fallbacks{0};

inline Region* region_of(const void* p) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    const int n = g_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (a >= g_regions[i].base && a < g_regions[i].end) return &g_regions[i];
    return nullptr;
}

inline int shift_for(size_t n) {  // the smallest class that holds n bytes (n <= 4 KiB)
    int s = kMinShift;
    while ((size_t(1) << s) < n) s++;
    return s;
}

inline size_t block_size(const void* p) {
    const auto* h = reinterpret_cast<const ChunkHeader*>(reinterpret_cast<uintptr_t>(p) & ~(kChunk - 1));
    return size_t(1) << h->shift;
}

// a block of 1 << shift from the owner's region, or nullptr (region full)
void* take(Region* r, int shift) {
    const int c = shift - kMinShift;
    Free* f = r->local[c];
    if (!f) f = r->remote[c].exchange(nullptr, std::memory_order_acquire);
    if (!f) {
        if (r->nextChunk >= r->chunks) return nullptr;
        const uintptr_t chunk = r->base + r->nextChunk++ * kChunk;
        reinterpret_cast<ChunkHeader*>(chunk)->shift = uint32_t(shift);
        const size_t size = size_t(1) << shift;
        const size_t first = size >= sizeof(ChunkHeader) ? size : sizeof(ChunkHeader);  // (the header's block)
        Free* head = nullptr;
        for (uintptr_t b = chunk + kChunk - size; b >= chunk + first; b -= size) {
            Free* blk = reinterpret_cast<Free*>(b);
            blk->next = head;
            head = blk;
        }
        f = head;
    }
    r->local[c] = f->next;
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return f;
}

void give(Region* r, void* p) {
    const auto* h = reinterpret_cast<const ChunkHeader*>(reinterpret_cast<uintptr_t>(p) & ~(kChunk - 1));
    const int c = int(h->shift) - kMinShift;
    Free* blk = static_cast<Free*>(p);
    if (t_region == r) {
        blk->next = r->local[c];
        r->local[c] = blk;
    } else {  // another thread's block (a compile's result freed by the render thread)
        Free* head = r->remote[c].load(std::memory_order_relaxed);
        do blk->next = head;
        while (!r->remote[c].compare_exchange_weak(head, blk, std::memory_order_release, std::memory_order_relaxed));
        g_remote.fetch_add(1, std::memory_order_relaxed);
    }
    g_frees.fetch_add(1, std::memory_order_relaxed);
}

// the calling thread's block for n bytes aligned to `align` (0: none), or nullptr: newlib's heap
inline void* try_alloc(size_t n, size_t align) {
    // (no thread attached yet: no thread-local read, also during libnx's and newlib's own start-up)
    if (g_count.load(std::memory_order_relaxed) == 0) return nullptr;
    Region* r = t_region;
    if (!r) return nullptr;
    const size_t need = n > align ? n : align;
    if (need > (size_t(1) << kMaxShift)) return nullptr;
    void* p = take(r, shift_for(need ? need : 1));
    if (!p) g_fallbacks.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void* resize(void* p, size_t n, bool reent, struct _reent* re) {
    if (Region* r = region_of(p)) {
        const size_t old = block_size(p);
        if (n <= old && n) return p;
        void* q = try_alloc(n, 0);
        if (!q) q = __real_malloc(n);
        if (!q) return nullptr;
        memcpy(q, p, old < n ? old : n);
        give(r, p);
        return q;
    }
    if (p && g_count.load(std::memory_order_relaxed) && t_region) {  // newlib's block, grown on a compile thread
        return reent ? __real__realloc_r(re, p, n) : __real_realloc(p, n);
    }
    if (!p) {
        if (void* q = try_alloc(n, 0)) return q;
    }
    return reent ? __real__realloc_r(re, p, n) : __real_realloc(p, n);
}

}  // namespace

bool attach(size_t bytes) {
    if (t_region) return true;
    const size_t chunks = (bytes + kChunk - 1) / kChunk;
    void* mem = __real_memalign(kChunk, chunks * kChunk);
    if (!mem) return false;
    static std::atomic<int> claimed{0};
    const int i = claimed.fetch_add(1);
    if (i >= kMaxRegions) {
        __real_free(mem);
        return false;
    }
    Region& r = g_regions[i];
    r.base = reinterpret_cast<uintptr_t>(mem);
    r.end = r.base + chunks * kChunk;
    r.chunks = chunks;
    // published in order: region_of reads g_count with acquire (regions are only ever added)
    int expect = i;
    while (!g_count.compare_exchange_weak(expect, i + 1, std::memory_order_release)) expect = i;
    t_region = &r;
    return true;
}

Stats stats() {
    Stats s;
    s.allocs = g_allocs.load();
    s.frees = g_frees.load();
    s.remoteFrees = g_remote.load();
    s.fallbacks = g_fallbacks.load();
    for (int i = 0; i < g_count.load(); i++) {
        s.chunksUsed += g_regions[i].nextChunk;
        s.chunksTotal += g_regions[i].chunks;
    }
    return s;
}

}  // namespace thread_heap

using namespace thread_heap;

extern "C" {
void* __wrap_malloc(size_t n) {
    if (void* p = try_alloc(n, 0)) return p;
    return __real_malloc(n);
}
void __wrap_free(void* p) {
    if (!p) return;
    if (Region* r = region_of(p)) return give(r, p);
    __real_free(p);
}
void* __wrap_calloc(size_t n, size_t s) {
    if (s && n > SIZE_MAX / s) {
        errno = ENOMEM;
        return nullptr;
    }
    if (void* p = try_alloc(n * s, 0)) return memset(p, 0, n * s);
    return __real_calloc(n, s);
}
void* __wrap_realloc(void* p, size_t n) { return resize(p, n, false, nullptr); }
void* __wrap_memalign(size_t a, size_t n) {
    if (void* p = try_alloc(n, a)) return p;
    return __real_memalign(a, n);
}
void* __wrap_aligned_alloc(size_t a, size_t n) {
    if (void* p = try_alloc(n, a)) return p;
    return __real_aligned_alloc(a, n);
}
int __wrap_posix_memalign(void** out, size_t a, size_t n) {
    if (void* p = try_alloc(n, a)) {
        *out = p;
        return 0;
    }
    return __real_posix_memalign(out, a, n);
}
// newlib's own calls (stdio, strdup's frees ...) of a block from a region
void __wrap__free_r(struct _reent* re, void* p) {
    if (p)
        if (Region* r = region_of(p)) return give(r, p);
    __real__free_r(re, p);
}
void* __wrap__realloc_r(struct _reent* re, void* p, size_t n) { return resize(p, n, true, re); }
}
#endif  // !WWHD_HEAP_CENSUS
