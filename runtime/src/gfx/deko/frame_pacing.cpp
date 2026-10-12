// SwitchWakerHD (issue #23): frame pacing against the display's real vblanks (frame_pacing.h).
//
// A thread waits on the display's vsync event (viGetDisplayVsyncEvent) and, at each vblank, reads the number
// of the last frame whose present pass the GPU has finished (written by the GPU itself, dkCmdBufReportValue
// at the end of the present pass). With swap interval 1 a frame reaches the screen at the first vblank after
// the GPU finished it, so the runs of the same number give how many vblanks each frame stayed on screen
// (plus a constant compositor latency, which does not change the counts). A frame finished within the wake-up
// latency of the thread (microseconds) before a vblank can be counted one vblank late.
//
// Mode 1 (the default) gives the swapchain the game's swap interval, so the display holds every frame at least
// two vblanks at 30 fps instead of showing a frame that finished early after one (docs/frame-pacing-plan.md).
#include "frame_pacing.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "dk.h"
#include "runtime.h"

namespace interp { uint32_t effective_swap_interval(uint32_t game); int vsync_rate(); }

namespace frame_pacing {
using gfxdk::now_ns;
using gfxdk::R;
namespace {
std::atomic<int> g_mode{[] {
    const char* e = getenv("WWHD_DK_PACING");
    return e ? std::clamp(atoi(e), 0, 1) : 1;  // mode 1 by default (#23)
}()};

// GPU-written report: the frame number (word 0) and the GPU's timestamp (bytes 8..15)
DkMemBlock g_block = nullptr;
volatile uint8_t* g_cpu = nullptr;
DkGpuAddr g_gpu = 0;

// ---- vblank thread state (g_m)
std::mutex g_m;
constexpr int kRing = 64;
struct Vblanks {
    uint64_t count = 0;        // vblanks since the thread started (missed wake-ups included)
    uint64_t lastNs = 0;       // when the thread saw the latest one (now_ns)
    double period = 16666666.7;  // measured over the last kRing vblanks
    uint64_t ringNs[kRing] = {}, ringCount[kRing] = {};
    int ringPos = 0, ringFill = 0;
} g_v;
struct Shown {
    uint64_t vblanks = 0, missedWakes = 0;
    uint64_t gapMin = ~0ull, gapMax = 0;  // ns between consecutive vblanks the thread saw
    uint64_t holds[5] = {};               // frames shown for (0), 1, 2, 3, 4+ vblanks
    uint64_t queued = 0;                  // frames that waited for the swap interval after the GPU finished them
    uint64_t holdChanges = 0;             // a frame shown for a different number of vblanks than the one before it
    int64_t leadMin = INT64_MAX, leadMax = INT64_MIN;
    int64_t leadSum = 0;
    uint64_t leads = 0, leadsEdge = 0;  // GPU finish to the vblank that showed it; within 1.5 ms of the vblank
} g_s;
uint32_t g_cur = 0;
uint64_t g_lastDisplay = 0, g_prevHold = 0;  // the vblank the latest frame reached the screen at (model above)
std::atomic<uint32_t> g_interval{1};         // the swapchain's swap interval now (backend.cpp: set_interval)
std::atomic<bool> g_vsyncOk{false};

void vblank_thread(void* arg) {
    Event* ev = static_cast<Event*>(arg);
    for (;;) {
        if (R_FAILED(eventWait(ev, 200'000'000ull))) continue;  // (no vblank in 200 ms: asleep, or the display off)
        const uint64_t t = now_ns();
        const uint32_t shown = *reinterpret_cast<volatile uint32_t*>(g_cpu);
        uint64_t gpuTs;
        memcpy(&gpuTs, const_cast<const uint8_t*>(g_cpu) + 8, 8);
        std::lock_guard<std::mutex> lk(g_m);
        uint64_t n = 1;
        if (g_v.lastNs) {
            const uint64_t gap = t - g_v.lastNs;
            n = std::max<uint64_t>(1, uint64_t(std::llround(double(gap) / g_v.period)));
            if (n == 1) {
                g_s.gapMin = std::min(g_s.gapMin, gap);
                g_s.gapMax = std::max(g_s.gapMax, gap);
            } else
                g_s.missedWakes += n - 1;
        }
        g_v.count += n;
        g_v.lastNs = t;
        // the period over the ring (the oldest entry to this one)
        const int oldest = g_v.ringFill < kRing ? 0 : g_v.ringPos;
        if (g_v.ringFill) {
            const uint64_t dc = g_v.count - g_v.ringCount[oldest];
            if (dc >= 8) g_v.period = double(t - g_v.ringNs[oldest]) / double(dc);
        }
        g_v.ringNs[g_v.ringPos] = t;
        g_v.ringCount[g_v.ringPos] = g_v.count;
        g_v.ringPos = (g_v.ringPos + 1) % kRing;
        g_v.ringFill = std::min(g_v.ringFill + 1, kRing);
        g_s.vblanks += n;
        // Frames the GPU finished since the previous vblank reach the screen in order (the window's queue is
        // FIFO): each at this vblank at the earliest, and `swap interval` vblanks after the one before it
        if (!g_cur && shown) {  // the first frame seen
            g_cur = shown;
            g_lastDisplay = g_v.count;
        } else if (shown > g_cur) {
            const uint64_t interval = g_interval.load(std::memory_order_relaxed);
            for (uint32_t f = g_cur + 1; f <= shown; f++) {
                const uint64_t at = g_lastDisplay ? std::max(g_v.count, g_lastDisplay + interval) : g_v.count;
                if (g_lastDisplay && g_cur) {
                    const uint64_t hold = at - g_lastDisplay;
                    g_s.holds[std::min<uint64_t>(hold, 4)]++;
                    if (g_prevHold && hold != g_prevHold) g_s.holdChanges++;
                    g_prevHold = hold;
                }
                g_s.queued += at > g_v.count;
                g_lastDisplay = at;
            }
            g_cur = shown;
            // GPU ticks: 614.4 MHz (32 x the CPU's 19.2 MHz counter: the same clock as now_ns, it seems)
            const int64_t lead = int64_t(t) - int64_t(gpuTs * 625 / 384);
            g_s.leadMin = std::min(g_s.leadMin, lead);
            g_s.leadMax = std::max(g_s.leadMax, lead);
            g_s.leadSum += lead;
            g_s.leads++;
            g_s.leadsEdge += lead >= 0 && lead < 1'500'000;
        } else if (shown < g_cur)
            g_cur = shown;  // (cannot happen: the numbers only grow)
    }
}

// ---- present cadence (render thread only)
constexpr double kEdgesMs[] = {12, 22, 30, 37, 45, 58};  // ~17 between 12-22, ~33 between 30-37, ~50 between 45-58
struct Presents {
    uint64_t lastNs = 0;
    uint64_t buckets[7] = {};
    double sum = 0, sumSq = 0;
    uint64_t n = 0;
} g_p;

}  // namespace

int mode() { return g_mode.load(std::memory_order_relaxed); }
void set_mode(int m) {
    m = std::clamp(m, 0, 1);
    if (g_mode.exchange(m) != m)
        LOG("[dk] pacing mode %d: swap interval %s", m, m ? "the game's (2 at 30 fps)" : "1 (as before #23)");
}

void start() {
    LOG("[dk] pacing mode %d (WWHD_DK_PACING; 1: swap interval the game's, the default; 0: swap interval 1)", mode());
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, R.device, DK_MEMBLOCK_ALIGNMENT);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_block = dkMemBlockCreate(&mm);
    g_cpu = static_cast<volatile uint8_t*>(dkMemBlockGetCpuAddr(g_block));
    g_gpu = dkMemBlockGetGpuAddr(g_block);
    memset(const_cast<uint8_t*>(g_cpu), 0, 16);
    static ViDisplay display;
    static Event vsync;
    // the default display first; the panel's and the TV's by name if the system refuses it. Measured handheld:
    // "Default" is taken by the default window (rc 0x1272) and "Internal" gives the panel's vblanks at 60.000 Hz.
    // NOT TESTED DOCKED: whether "Internal" still ticks, or the TV's rate, there (docs/frame-pacing-plan.md).
    Result rc = 0;
    bool got = false;
    for (const char* name : {"Default", "Internal", "External"}) {
        rc = viOpenDisplay(name, &display);
        if (R_FAILED(rc)) {
            LOG("[dk] pacing: display \"%s\" not opened (rc 0x%X)", name, rc);
            continue;
        }
        rc = viGetDisplayVsyncEvent(&display, &vsync);
        if (R_SUCCEEDED(rc)) {
            LOG("[dk] pacing: vsync event of the display \"%s\" (id %llu)", name, (unsigned long long)display.display_id);
            got = true;
            break;
        }
        LOG("[dk] pacing: display \"%s\" (id %llu): no vsync event (rc 0x%X)", name, (unsigned long long)display.display_id, rc);
        viCloseDisplay(&display);
    }
    if (!got) {
        LOG("[dk] pacing: no display vsync event: no vblank statistics, the virtual vsync stays on the steady clock");
        return;
    }
    static Thread thread;
    // high priority: it wakes for microseconds at each vblank, and its timestamps should be the vblank's
    rc = threadCreate(&thread, vblank_thread, &vsync, nullptr, 0x4000, 0x20, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&thread);
    if (R_FAILED(rc)) {
        LOG("[dk] pacing: no vblank thread (rc 0x%X)", rc);
        return;
    }
    g_vsyncOk = true;
    LOG("[dk] pacing: vblank thread on the display's vsync event");
}

void on_present(DkCmdBuf cmd, uint64_t frame) {
    if (g_block) dkCmdBufReportValue(cmd, uint32_t(frame), g_gpu);
    const uint64_t t = now_ns();
    if (g_p.lastNs) {
        const double ms = double(t - g_p.lastNs) / 1e6;
        int b = 0;
        while (b < 6 && ms >= kEdgesMs[b]) b++;
        g_p.buckets[b]++;
        g_p.sum += ms;
        g_p.sumSq += ms * ms;
        g_p.n++;
    }
    g_p.lastNs = t;
}

// The game's own interval is not exported by gx2_core (an upstream file): Wind Waker HD sets 2
// (GX2SetSwapInterval) in every log so far (530 of 530 '[gx2] ... swap interval' lines, title and gameplay).
constexpr uint32_t kGameSwapInterval = 2;
uint32_t swap_interval() {
    if (!mode() || interp::vsync_rate() > 1) return 1;
    return std::clamp<uint32_t>(interp::effective_swap_interval(kGameSwapInterval), 1, 2);
}

void set_interval(uint32_t interval) { g_interval.store(interval, std::memory_order_relaxed); }

std::string report() {
    Shown s;
    double period;
    {
        std::lock_guard<std::mutex> lk(g_m);
        s = g_s;
        period = g_v.period;
        g_s = {};
    }
    Presents p = g_p;
    g_p = {};
    g_p.lastNs = p.lastNs;
    const double mean = p.n ? p.sum / double(p.n) : 0;
    const double sd = p.n ? std::sqrt(std::max(0.0, p.sumSq / double(p.n) - mean * mean)) : 0;
    char out[640];
    int k = snprintf(out, sizeof out, "mode %d (swap interval %u); present intervals (ms) <12 %llu, "
                     "12-22 %llu, 22-30 %llu, 30-37 %llu, 37-45 %llu, 45-58 %llu, 58+ %llu; mean %.2f sd %.2f",
                     mode(), swap_interval(),
                     (unsigned long long)p.buckets[0], (unsigned long long)p.buckets[1], (unsigned long long)p.buckets[2],
                     (unsigned long long)p.buckets[3], (unsigned long long)p.buckets[4], (unsigned long long)p.buckets[5],
                     (unsigned long long)p.buckets[6], mean, sd);
    if (g_vsyncOk && s.vblanks)
        snprintf(out + k, sizeof out - k, "; display %.3f Hz (vblank gaps %.2f-%.2f ms, %llu missed wakes); frames on "
                 "screen for 1/2/3/4+ vblanks %llu/%llu/%llu/%llu, held back by the swap interval %llu, hold changes %llu; GPU done %.2f/%.2f/%.2f "
                 "ms before its vblank (min/avg/max), %llu within 1.5 ms",
                 1e9 / period, double(s.gapMin) / 1e6, double(s.gapMax) / 1e6, (unsigned long long)s.missedWakes,
                 (unsigned long long)s.holds[1], (unsigned long long)s.holds[2], (unsigned long long)s.holds[3],
                 (unsigned long long)s.holds[4], (unsigned long long)s.queued, (unsigned long long)s.holdChanges,
                 s.leads ? double(s.leadMin) / 1e6 : 0.0, s.leads ? double(s.leadSum) / double(s.leads) / 1e6 : 0.0,
                 s.leads ? double(s.leadMax) / 1e6 : 0.0, (unsigned long long)s.leadsEdge);
    return out;
}

}  // namespace frame_pacing
