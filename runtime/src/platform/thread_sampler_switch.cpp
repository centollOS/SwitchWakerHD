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
constexpr int kMaxFrames = 64;    // host frames (up to kMaxHost), then guest return addresses
constexpr int kMaxHost = 32;
constexpr uint64_t kGuestTag = 1ull << 63;  // a guest return address in a sample: kGuestTag | address
struct Sample {
    uint32_t thread;
    uint32_t count;
    uint64_t frames[kMaxFrames];
};
struct Target {
    Handle handle;
    char tag[16];
    const uint32_t* guestR1;  // the thread's Cpu r1, or null
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
    while (n < kMaxHost && fp >= lo && fp + 16 <= hi && (fp & 7) == 0) {
        const uint64_t* rec = reinterpret_cast<const uint64_t*>(fp);
        const uint64_t next = rec[0], ret = rec[1];
        if (!ret) break;
        out[n++] = ret;
        if (next <= fp) break;
        fp = next;
    }
    return n;
}

inline uint32_t guest_word(uint32_t a) {
    uint32_t v;
    memcpy(&v, ppc_mem_base_var + a, 4);
    return __builtin_bswap32(v);
}

// a word of the guest's stacks (MEM2 or the runtime's region, where the threads' stacks are)
inline bool stack_word(uint32_t a) {
    return (a & 3) == 0 && ((a >= mem::kMem2Start && a + 8 <= mem::kMem2End) ||
                            (a >= mem::kRuntimeStart && a + 8 <= mem::kRuntimeEnd));
}

// the guest stack from r1: frame -> back chain; the word after the back chain's frame is the return address the frame's
// function saved there (the PowerPC EABI's LR save word). Stops at a null or decreasing chain or outside guest RAM.
uint32_t capture_guest(uint32_t sp, uint64_t* out, uint32_t n) {
    for (int depth = 0; n < kMaxFrames && depth < 48; depth++) {
        if (!stack_word(sp)) break;
        const uint32_t next = guest_word(sp);
        if (next <= sp || !stack_word(next)) break;
        const uint32_t ret = guest_word(next + 4);
        if (ret >= 0x02000000u && ret < 0x10000000u) out[n++] = kGuestTag | ret;
        sp = next;
    }
    return n;
}

void set_status(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_statusMu);
    g_status = s;
}

// one sample of thread i: paused, its context and stacks read, resumed (nothing allocated or locked meanwhile)
bool sample_thread(int i, Sample& s, uint64_t& pausedNs, uint64_t& worstNs, uint64_t& failed, Result& lastError) {
    s.thread = uint32_t(i);
    s.count = 0;
    const Handle h = g_targets[i].handle;
    const uint64_t t0 = armGetSystemTick();
    if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) {
        failed++;
        return false;
    }
    ThreadContext c;
    Result rc = svcGetThreadContext3(&c, h);
    for (int tries = 0; R_FAILED(rc) && tries < 50; tries++) {  // (the pause takes effect at the next switch)
        svcSleepThread(0);
        rc = svcGetThreadContext3(&c, h);
    }
    if (R_SUCCEEDED(rc)) {
        s.count = capture(c, s.frames);
        if (const uint32_t* r1 = g_targets[i].guestR1) s.count = capture_guest(*r1, s.frames, s.count);
    }
    svcSetThreadActivity(h, ThreadActivity_Runnable);
    const uint64_t ns = ticks_ns(armGetSystemTick() - t0);
    pausedNs += ns;
    if (ns > worstNs) worstNs = ns;
    if (R_FAILED(rc)) {
        failed++;
        lastError = rc;
        return false;
    }
    return true;
}

void write_sample(FILE* out, const Sample& s) {
    fwrite(&s.thread, 4, 1, out);
    fwrite(&s.count, 4, 1, out);
    fwrite(s.frames, 8, s.count, out);
}

FILE* open_samples(const char* kind, int n, char* path, size_t pathSize) {
    snprintf(path, pathSize, "logs/%s_%llu.samples", kind, (unsigned long long)(armGetSystemTick() / 19200000));
    FILE* out = fopen(path, "wb");
    if (!out) return nullptr;
    fprintf(out, "WWHDSAMP1 %d\n", n);
    for (int i = 0; i < n; i++) fprintf(out, "%s\n", g_targets[i].tag);
    fprintf(out, "base %llx\n", (unsigned long long)host::executable_base());
    return out;
}

void sampler_main(void*) {
    const int n = g_count.load();
    char path[96];
    FILE* out = open_samples("prof", n, path, sizeof path);
    if (!out) {
        set_status("cannot write logs/");
        g_running = false;
        return;
    }
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
            if (!sample_thread(i, s, pausedNs, worstNs, failed, lastError)) continue;
            samples++;
            batch.push_back(s);
        }
        const uint64_t now = armGetSystemTick();
        const double elapsed = double(ticks_ns(now - start)) / 1e9;
        if (ticks_ns(now - lastFlush) >= 1'000'000'000 || batch.size() >= 2000) {
            for (const Sample& s : batch) write_sample(out, s);
            fflush(out);
            batch.clear();
            lastFlush = now;
        }
        if (elapsed >= g_seconds) break;
    }
    for (const Sample& s : batch) write_sample(out, s);
    fclose(out);
    char b[256];
    snprintf(b, sizeof b, "%s: %llu samples, %llu failed (last 0x%x), paused %.1f us a sample (worst %.0f us)", path,
             (unsigned long long)samples, (unsigned long long)failed, unsigned(lastError),
             samples ? double(pausedNs) / 1e3 / double(samples + failed) : 0.0, double(worstNs) / 1e3);
    LOG("[profile] %s", b);
    set_status(b);
    g_running = false;
}

// ---- hitch mode (docs/hitch-profiler-plan.md): logs/hitch_profile.flag on the SD card at the start. The frame threads
// are sampled every millisecond for the whole session into a ring of the last kRing rounds; a frame longer than the
// threshold (mark_frame, from the render thread's present) has its rounds written to logs/hitch_<n>.samples after a
// marker record, so the file holds only the slow frames, each with what every frame thread was doing in it.
constexpr const char* kFlag = "logs/hitch_profile.flag";
constexpr int kRing = 512;            // rounds kept (~0.5 s at one a millisecond: the longest frame written whole)
constexpr int kMaxMarks = 16;         // frames waiting to be written
constexpr uint32_t kMarker = 0xFFFFFFFEu;  // a marker record's "thread": frames {start tick, end tick, frame, rounds}
constexpr int kMaxWritten = 2000;     // slow frames written in a session (the file's bound, ~1 MB each at most)
struct Mark {
    uint64_t start, end, frame;
};
std::mutex g_markMu;
Mark g_marks[kMaxMarks];
int g_markCount = 0;
std::atomic<bool> g_hitchMode{false};
std::atomic<uint32_t> g_thresholdMs{40};
uint64_t g_droppedMarks = 0;  // (under g_markMu) frames not queued: the queue was full

// the file is written by its own thread: the sampler must not wait for the SD card (a warp's slow frames come in a row
// while the game loads from it, and a sampler blocked in fwrite took no samples in them: 34 of 136 frames, console)
constexpr size_t kMaxPending = 16u << 20;  // bytes waiting for the writer; more are dropped (counted)
std::mutex g_writeMu;
CondVar g_writeCv;
Mutex g_writeCvMu;
std::vector<uint8_t> g_pending;
uint64_t g_droppedBytes = 0;  // (under g_writeMu)
char g_hitchPath[96];

void append(std::vector<uint8_t>& b, const Sample& s) {
    const size_t at = b.size();
    b.resize(at + 8 + 8 * size_t(s.count));
    memcpy(b.data() + at, &s.thread, 4);
    memcpy(b.data() + at + 4, &s.count, 4);
    memcpy(b.data() + at + 8, s.frames, 8 * size_t(s.count));
}

void writer_main(void*) {
    std::vector<uint8_t> take;
    for (;;) {
        mutexLock(&g_writeCvMu);
        condvarWaitTimeout(&g_writeCv, &g_writeCvMu, 1'000'000'000ull);
        mutexUnlock(&g_writeCvMu);
        {
            std::lock_guard<std::mutex> lk(g_writeMu);
            take.swap(g_pending);
        }
        if (take.empty()) continue;
        // opened for each write and closed after it: the console's file system refuses to read a file open for
        // writing (the debug server's "get" during the session), and a HOME close keeps what was written
        if (FILE* out = fopen(g_hitchPath, "ab")) {
            fwrite(take.data(), 1, take.size(), out);
            fclose(out);
        }
        take.clear();
    }
}

void hitch_main(void*) {
    const int n = g_count.load();
    char path[96];
    FILE* out = open_samples("hitch", n, path, sizeof path);
    if (!out) {
        LOG("[profile] hitch mode: cannot write logs/");
        g_hitchMode = false;
        g_running = false;
        return;
    }
    fclose(out);
    snprintf(g_hitchPath, sizeof g_hitchPath, "%s", path);
    mutexInit(&g_writeCvMu);
    condvarInit(&g_writeCv);
    static Thread writer;
    // below the game's threads (it only waits for the SD card), on core 0 as the sampler
    if (R_FAILED(threadCreate(&writer, writer_main, nullptr, nullptr, 64 * 1024, 0x3B, 0)) || R_FAILED(threadStart(&writer))) {
        LOG("[profile] hitch mode: cannot start the writer thread");
        g_hitchMode = false;
        g_running = false;
        return;
    }
    std::vector<uint8_t> chunk;
    LOG("[profile] hitch mode: %d threads sampled every ms, frames over %u ms written to %s", n, g_thresholdMs.load(),
        path);
    std::vector<Sample> ring(size_t(kRing) * n);
    std::vector<uint64_t> ringTick(kRing, 0);
    std::vector<uint8_t> ringOk(size_t(kRing) * n, 0);
    uint64_t round = 0, samples = 0, failed = 0, pausedNs = 0, worstNs = 0, written = 0, truncated = 0;
    uint64_t lastReport = armGetSystemTick();
    Result lastError = 0;
    for (;;) {
        svcSleepThread(1'000'000);
        const int slot = int(round % kRing);
        ringTick[slot] = armGetSystemTick();
        for (int i = 0; i < n; i++) {
            Sample& s = ring[size_t(slot) * n + i];
            const bool ok = sample_thread(i, s, pausedNs, worstNs, failed, lastError);
            ringOk[size_t(slot) * n + i] = ok;
            samples += ok;
        }
        round++;
        // frames that ended before this round: their rounds to the file
        Mark marks[kMaxMarks];
        int count = 0;
        {
            std::lock_guard<std::mutex> lk(g_markMu);
            const uint64_t now = ringTick[slot];
            int keep = 0;
            for (int k = 0; k < g_markCount; k++) {
                if (g_marks[k].end <= now) marks[count++] = g_marks[k];
                else g_marks[keep++] = g_marks[k];
            }
            g_markCount = keep;
        }
        chunk.clear();
        for (int k = 0; k < count && written < uint64_t(kMaxWritten); k++) {
            const Mark& m = marks[k];
            const uint64_t first = round > uint64_t(kRing) ? round - kRing : 0;
            uint64_t inside = 0;
            for (uint64_t r = first; r < round; r++)
                if (const uint64_t t = ringTick[r % kRing]; t >= m.start && t <= m.end) inside++;
            if (ringTick[first % kRing] > m.start) truncated++;
            Sample marker{};
            marker.thread = kMarker;
            marker.count = 4;
            marker.frames[0] = m.start;
            marker.frames[1] = m.end;
            marker.frames[2] = m.frame;
            marker.frames[3] = inside;
            append(chunk, marker);
            for (uint64_t r = first; r < round; r++) {
                const int sl = int(r % kRing);
                if (ringTick[sl] < m.start || ringTick[sl] > m.end) continue;
                for (int i = 0; i < n; i++)
                    if (ringOk[size_t(sl) * n + i]) append(chunk, ring[size_t(sl) * n + i]);
            }
            written++;
            if (written == uint64_t(kMaxWritten)) LOG("[profile] hitch mode: %d slow frames written, no more", kMaxWritten);
        }
        if (!chunk.empty()) {
            {
                std::lock_guard<std::mutex> lk(g_writeMu);
                if (g_pending.size() + chunk.size() <= kMaxPending) g_pending.insert(g_pending.end(), chunk.begin(), chunk.end());
                else g_droppedBytes += chunk.size();
            }
            condvarWakeOne(&g_writeCv);
        }
        // a line a minute: what it costs and what it wrote
        if (ticks_ns(armGetSystemTick() - lastReport) >= 60'000'000'000ull) {
            lastReport = armGetSystemTick();
            uint64_t dropped, droppedBytes;
            {
                std::lock_guard<std::mutex> lk(g_markMu);
                dropped = g_droppedMarks;
            }
            {
                std::lock_guard<std::mutex> lk(g_writeMu);
                droppedBytes = g_droppedBytes;
            }
            LOG("[profile] hitch mode: %llu slow frames written (%llu longer than the ring, %llu not queued, %llu KiB "
                "not written: the SD card behind); %llu samples, %llu failed (last 0x%x), paused %.1f us a sample "
                "(worst %.0f us)",
                (unsigned long long)written, (unsigned long long)truncated, (unsigned long long)dropped,
                (unsigned long long)(droppedBytes >> 10),
                (unsigned long long)samples, (unsigned long long)failed, unsigned(lastError),
                samples ? double(pausedNs) / 1e3 / double(samples + failed) : 0.0, double(worstNs) / 1e3);
        }
    }
}
}  // namespace

void add_current_thread(const char* tag) {
    std::lock_guard<std::mutex> lk(g_mu);
    const int i = g_count.load();
    if (i >= kMaxThreads) return;
    g_targets[i].handle = threadGetCurHandle();
    g_targets[i].guestR1 = nullptr;
    snprintf(g_targets[i].tag, sizeof g_targets[i].tag, "%s", tag);
    g_count.store(i + 1);
}

void set_guest_stack(const uint32_t* r1) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (const int i = g_count.load(); i > 0) g_targets[i - 1].guestR1 = r1;
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

void start_flagged() {
    FILE* f = fopen(kFlag, "r");
    if (!f) return;
    unsigned ms = 0;
    if (fscanf(f, "%u", &ms) == 1 && ms >= 20 && ms <= 1000) g_thresholdMs = ms;  // (an empty file: 40 ms)
    fclose(f);
    if (g_count.load() == 0) {
        LOG("[profile] hitch mode: no frame thread registered yet");
        return;
    }
    if (g_running.exchange(true)) return;
    // core 0, above the game's threads, as "profile"; it runs until the game ends ("profile" refuses meanwhile)
    Result rc = threadCreate(&g_thread, hitch_main, nullptr, nullptr, 64 * 1024, 0x28, 0);
    if (R_SUCCEEDED(rc)) {
        g_threadMade = true;
        g_hitchMode = true;
        rc = threadStart(&g_thread);
    }
    if (R_FAILED(rc)) {
        g_hitchMode = false;
        g_running = false;
        LOG("[profile] hitch mode: cannot start its thread (0x%x)", unsigned(rc));
    }
}

void mark_frame(uint64_t startTick, uint64_t endTick, uint64_t frame) {
    if (!g_hitchMode.load(std::memory_order_relaxed)) return;
    if (armTicksToNs(endTick - startTick) < uint64_t(g_thresholdMs.load(std::memory_order_relaxed)) * 1'000'000ull) return;
    std::lock_guard<std::mutex> lk(g_markMu);
    if (g_markCount < kMaxMarks) g_marks[g_markCount++] = {startTick, endTick, frame};
    else g_droppedMarks++;
}

std::string status() {
    if (g_hitchMode) return "hitch mode (logs/hitch_profile.flag) until the game ends";
    std::lock_guard<std::mutex> lk(g_statusMu);
    return g_running ? "running" : g_status;
}

}  // namespace thread_sampler
