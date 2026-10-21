// thread_sampler.h on the Switch (docs/cpu-threading-plan.md, step 0.2): the frame threads ("game", the main thread,
// and "render") register at their start; the debug server's "profile <seconds>" samples them about 1000 times a
// second: each thread is paused (svcSetThreadActivity), its registers read (svcGetThreadContext3), and resumed. A
// sample keeps the pc, the lr and the frame-pointer chain inside the thread's stack. No allocation or lock while a
// thread is paused. Files: logs/prof_<n>.samples (the Windows backend's format, WWHDSAMP1, plus the executable's
// base on a "base" line) for tools/switch/main_profile.py.
#include "thread_sampler.h"

#include <switch.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../runtime.h"
#include "host.h"

namespace thread_sampler {
namespace {
constexpr int kMaxThreads = 8;
constexpr int kMaxFrames = 32;
struct Sample {
    uint32_t thread;
    uint32_t count;
    uint64_t frames[kMaxFrames];
};
struct Target {
    Handle handle;
    char tag[16];
};
std::mutex g_mu;  // registration
Target g_targets[kMaxThreads];
std::atomic<int> g_count{0};
std::atomic<bool> g_running{false};
std::string g_status = "not run";
std::mutex g_statusMu;
Thread g_thread;
bool g_threadMade = false;  // (joined and closed before the next run)
double g_seconds = 0;

uint64_t ticks_ns(uint64_t t) { return armTicksToNs(t); }

// pc, lr, then the frame-pointer chain (x29 -> {x29, x30}) while it stays inside the stack's mapping
uint32_t capture(const ThreadContext& c, uint64_t* out) {
    uint32_t n = 0;
    out[n++] = c.pc.x;
    if (c.lr) out[n++] = c.lr;
    MemoryInfo mi{};
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&mi, &page, c.sp))) return n;
    const uint64_t lo = mi.addr, hi = mi.addr + mi.size;
    uint64_t fp = c.fp;
    while (n < kMaxFrames && fp >= lo && fp + 16 <= hi && (fp & 7) == 0) {
        const uint64_t* rec = reinterpret_cast<const uint64_t*>(fp);
        const uint64_t next = rec[0], ret = rec[1];
        if (!ret) break;
        out[n++] = ret;
        if (next <= fp) break;
        fp = next;
    }
    return n;
}

void set_status(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_statusMu);
    g_status = s;
}

void sampler_main(void*) {
    const int n = g_count.load();
    char path[96];
    snprintf(path, sizeof path, "logs/prof_%llu.samples", (unsigned long long)(armGetSystemTick() / 19200000));
    FILE* out = fopen(path, "wb");
    if (!out) {
        set_status("cannot write logs/");
        g_running = false;
        return;
    }
    fprintf(out, "WWHDSAMP1 %d\n", n);
    for (int i = 0; i < n; i++) fprintf(out, "%s\n", g_targets[i].tag);
    fprintf(out, "base %llx\n", (unsigned long long)host::executable_base());
    std::vector<Sample> batch;
    batch.reserve(2048);
    uint64_t samples = 0, failed = 0, pausedNs = 0, worstNs = 0;
    const uint64_t start = armGetSystemTick();
    uint64_t lastFlush = start;
    Result lastError = 0;
    for (;;) {
        svcSleepThread(1'000'000);
        for (int i = 0; i < n; i++) {
            Sample s{};
            s.thread = uint32_t(i);
            const Handle h = g_targets[i].handle;
            const uint64_t t0 = armGetSystemTick();
            if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) {
                failed++;
                continue;
            }
            ThreadContext c;
            Result rc = svcGetThreadContext3(&c, h);
            for (int tries = 0; R_FAILED(rc) && tries < 50; tries++) {  // (the pause takes effect at the next switch)
                svcSleepThread(0);
                rc = svcGetThreadContext3(&c, h);
            }
            if (R_SUCCEEDED(rc)) s.count = capture(c, s.frames);
            svcSetThreadActivity(h, ThreadActivity_Runnable);
            const uint64_t ns = ticks_ns(armGetSystemTick() - t0);
            pausedNs += ns;
            if (ns > worstNs) worstNs = ns;
            if (R_FAILED(rc)) {
                failed++;
                lastError = rc;
                continue;
            }
            samples++;
            batch.push_back(s);
        }
        const uint64_t now = armGetSystemTick();
        const double elapsed = double(ticks_ns(now - start)) / 1e9;
        if (ticks_ns(now - lastFlush) >= 1'000'000'000 || batch.size() >= 2000) {
            for (const Sample& s : batch) {
                fwrite(&s.thread, 4, 1, out);
                fwrite(&s.count, 4, 1, out);
                fwrite(s.frames, 8, s.count, out);
            }
            fflush(out);
            batch.clear();
            lastFlush = now;
        }
        if (elapsed >= g_seconds) break;
    }
    for (const Sample& s : batch) {
        fwrite(&s.thread, 4, 1, out);
        fwrite(&s.count, 4, 1, out);
        fwrite(s.frames, 8, s.count, out);
    }
    fclose(out);
    char b[256];
    snprintf(b, sizeof b, "%s: %llu samples, %llu failed (last 0x%x), paused %.1f us a sample (worst %.0f us)", path,
             (unsigned long long)samples, (unsigned long long)failed, unsigned(lastError),
             samples ? double(pausedNs) / 1e3 / double(samples + failed) : 0.0, double(worstNs) / 1e3);
    LOG("[profile] %s", b);
    set_status(b);
    g_running = false;
}
}  // namespace

void add_current_thread(const char* tag) {
    std::lock_guard<std::mutex> lk(g_mu);
    const int i = g_count.load();
    if (i >= kMaxThreads) return;
    g_targets[i].handle = threadGetCurHandle();
    snprintf(g_targets[i].tag, sizeof g_targets[i].tag, "%s", tag);
    g_count.store(i + 1);
}

std::string start(double seconds) {
    if (g_count.load() == 0) return "no thread registered yet";
    if (g_running.exchange(true)) return "already running";
    if (g_threadMade) {
        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
        g_threadMade = false;
    }
    g_seconds = seconds > 0 ? seconds : 10;
    set_status("running");
    // core 0 (the main thread has core 1, the render thread core 2), above the game's threads
    Result rc = threadCreate(&g_thread, sampler_main, nullptr, nullptr, 64 * 1024, 0x28, 0);
    if (R_SUCCEEDED(rc)) {
        g_threadMade = true;
        rc = threadStart(&g_thread);
    }
    if (R_FAILED(rc)) {
        g_running = false;
        return "cannot start its thread";
    }
    LOG("[profile] sampling %d threads for %.0f s", g_count.load(), g_seconds);
    return "";
}

std::string status() {
    std::lock_guard<std::mutex> lk(g_statusMu);
    return g_running ? "running" : g_status;
}

}  // namespace thread_sampler
