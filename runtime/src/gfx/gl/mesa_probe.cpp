// Where the time goes inside Mesa on the Switch (switch-mesa 20.1, nouveau), for the 5 s stats.
//
// In the busiest scenes the render thread is "busy" far longer than it runs on the CPU, with every
// core half idle: it is blocked somewhere below the GL calls. Mesa and libdrm_nouveau are linked
// statically, so the link wraps (CMakeLists.txt, -Wl,--wrap=...) their internal entry points and
// times them:
// - _mesa_glthread_flush_batch (render thread): hands a full 8 KB command batch to Mesa's GL thread;
//   with only four batches it waits whenever the GL thread is behind.
// - _mesa_glthread_finish_before (render thread): a GL call that cannot be queued waits for the GL
//   thread to run everything first; counted per function name.
// - _mesa_glthread_finish: an explicit full wait (presentation, queries).
// - nouveau_pushbuf_kick (GL thread): submits a command buffer to the GPU (a call to nvservices).
// - nouveau_pushbuf_space (GL thread): the command buffer is full; may wait for the GPU to free one.
// - nouveau_fence_wait, nouveau_bo_wait (GL thread): waits for the GPU to finish work.
#ifdef __SWITCH__
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {
inline uint64_t ticks_ns() {
    uint64_t t;
    asm volatile("mrs %0, cntpct_el0" : "=r"(t));
    return t * 625 / 12;  // 19.2 MHz
}

struct Counter {
    std::atomic<uint64_t> n{0}, ns{0};
    uint64_t lastN = 0, lastNs = 0;
    void add(uint64_t start) {
        n.fetch_add(1, std::memory_order_relaxed);
        ns.fetch_add(ticks_ns() - start, std::memory_order_relaxed);
    }
    // calls and ms since the previous take
    void take(uint64_t& calls, double& ms) {
        const uint64_t a = n.load(std::memory_order_relaxed), b = ns.load(std::memory_order_relaxed);
        calls = a - lastN;
        ms = double(b - lastNs) / 1e6;
        lastN = a;
        lastNs = b;
    }
};
Counter g_flush, g_finish, g_sync, g_kick, g_space, g_fence, g_boWait;

// synchronous GL calls by name (the names are string literals: keyed by address)
struct SyncName {
    std::atomic<const char*> name{nullptr};
    Counter c;
};
SyncName g_syncNames[24];
void count_sync(const char* name, uint64_t start) {
    for (auto& s : g_syncNames) {
        const char* cur = s.name.load(std::memory_order_acquire);
        if (!cur) {
            const char* expected = nullptr;
            if (s.name.compare_exchange_strong(expected, name) || expected == name) {
                s.c.add(start);
                return;
            }
            cur = expected;
        }
        if (cur == name) {
            s.c.add(start);
            return;
        }
    }
}
}  // namespace

extern "C" {
void __real__mesa_glthread_flush_batch(void* ctx);
void __real__mesa_glthread_finish(void* ctx);
void __real__mesa_glthread_finish_before(void* ctx, const char* func);
int __real_nouveau_pushbuf_kick(void* push, void* chan);
int __real_nouveau_pushbuf_space(void* push, uint32_t dwords, uint32_t relocs, uint32_t pushes);
bool __real_nouveau_fence_wait(void* fence, void* debug);
int __real_nouveau_bo_wait(void* bo, uint32_t access, void* client);

void __wrap__mesa_glthread_flush_batch(void* ctx) {
    const uint64_t t = ticks_ns();
    __real__mesa_glthread_flush_batch(ctx);
    g_flush.add(t);
}
void __wrap__mesa_glthread_finish(void* ctx) {
    const uint64_t t = ticks_ns();
    __real__mesa_glthread_finish(ctx);
    g_finish.add(t);
}
void __wrap__mesa_glthread_finish_before(void* ctx, const char* func) {
    const uint64_t t = ticks_ns();
    __real__mesa_glthread_finish_before(ctx, func);
    g_sync.add(t);
    count_sync(func, t);
}
int __wrap_nouveau_pushbuf_kick(void* push, void* chan) {
    const uint64_t t = ticks_ns();
    int r = __real_nouveau_pushbuf_kick(push, chan);
    g_kick.add(t);
    return r;
}
int __wrap_nouveau_pushbuf_space(void* push, uint32_t dwords, uint32_t relocs, uint32_t pushes) {
    const uint64_t t = ticks_ns();
    int r = __real_nouveau_pushbuf_space(push, dwords, relocs, pushes);
    g_space.add(t);
    return r;
}
bool __wrap_nouveau_fence_wait(void* fence, void* debug) {
    const uint64_t t = ticks_ns();
    bool r = __real_nouveau_fence_wait(fence, debug);
    g_fence.add(t);
    return r;
}
int __wrap_nouveau_bo_wait(void* bo, uint32_t access, void* client) {
    const uint64_t t = ticks_ns();
    int r = __real_nouveau_bo_wait(bo, access, client);
    g_boWait.add(t);
    return r;
}
}

namespace gfxgl {
// "; Mesa: ..." for the stats line: calls and ms per second since the previous report
std::string mesa_probe_report(double secs) {
    uint64_t n[7];
    double ms[7];
    Counter* cs[7] = {&g_flush, &g_sync, &g_finish, &g_kick, &g_space, &g_fence, &g_boWait};
    for (int i = 0; i < 7; i++) cs[i]->take(n[i], ms[i]);
    char buf[512];
    snprintf(buf, sizeof buf,
             "; Mesa per second: batches %.0f (render thread waited %.0f ms), syncs %.0f (%.0f ms), finishes %.0f "
             "(%.0f ms); nouveau: submits %.0f (%.0f ms), buffer full %.0f (%.0f ms), fence waits %.0f (%.0f ms), "
             "buffer waits %.0f (%.0f ms)",
             n[0] / secs, ms[0] / secs, n[1] / secs, ms[1] / secs, n[2] / secs, ms[2] / secs, n[3] / secs, ms[3] / secs,
             n[4] / secs, ms[4] / secs, n[5] / secs, ms[5] / secs, n[6] / secs, ms[6] / secs);
    std::string out = buf;
    std::string names;
    for (auto& s : g_syncNames) {
        const char* name = s.name.load(std::memory_order_acquire);
        if (!name) break;
        uint64_t calls;
        double t;
        s.c.take(calls, t);
        if (!calls) continue;
        char item[96];
        snprintf(item, sizeof item, "%s%s %llu (%.0f ms)", names.empty() ? "" : ", ", name, (unsigned long long)calls, t);
        names += item;
    }
    if (!names.empty()) out += "; synchronous calls: " + names;
    return out;
}
}  // namespace gfxgl
#endif
