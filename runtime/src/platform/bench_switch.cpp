// (this fork) The performance test's console side (bench_switch.h, docs/perf-test.md).
#include "bench_switch.h"

#include <switch.h>

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../gx2/gx2.h"
#include "../mods/mods.h"
#include "../runtime.h"
#include "../savestate.h"
#include "debug_server.h"
#include "guest_addr.h"
#include "host.h"

namespace render { uint64_t frame_count(); }

namespace bench {
namespace {

// the live save data's clock: dSv_player_status_b_c (save area + dSv_info_c 0x20 + dSv_player_status_a_c 0x18):
// mTime (f32, degrees 0..360) at +0xC and mDate (u16) at +0x10, the layout of the card block (savestate.cpp
// kCardStatusB)
const uint32_t kSaveInfoPtr = GD(0x101F84DC);
constexpr uint32_t kStatusB = 0x20 + 0x18;
bool in_mem2(uint32_t a) { return a >= mem::kMem2Start && a < mem::kMem2End; }

std::atomic<bool> g_frozen{false};
std::atomic<float> g_frozenTime{0};
std::atomic<float> g_time{-1};
std::atomic<int> g_date{-1};

// the main thread's CPU time (ticks) and the game frames it ran, totals read at each present
std::atomic<uint64_t> g_mainTicks{0}, g_passes{0};
// the player's control (savestate control_check), updated by the game thread every few frames
std::atomic<bool> g_control{false};
std::atomic<uint64_t> g_noControl{0};  // checks without control while recording
std::mutex g_mu;
std::string g_why;

struct Frame {
    uint32_t dtUs, renderUs, mainUs, gpuUs;  // gpuUs: ~0u when this frame was not sampled
    uint16_t passes, draws;
    uint8_t behind, control, res;  // res: internal resolution x 100 (>255 clamped)
};
std::vector<Frame> g_frames;  // render thread while recording; read after g_done
std::atomic<int> g_want{0};   // frames still to record (render thread counts down)
std::atomic<bool> g_done{false};
std::atomic<bool> g_armed{false};

uint64_t thread_ticks() {
    u64 t = 0;
    if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX)))
        svcGetInfo(&t, InfoType_ThreadTickCountDeprecated, CUR_THREAD_HANDLE, UINT64_MAX);
    return t;
}
double ticks_us(uint64_t t) { return double(t) * 1e6 / double(armGetSystemTickFreq()); }

std::string bench_dir() {
    const std::string d = host::config_dir() + "/bench";
    mkdir(d.c_str(), 0777);
    return d;
}
bool valid_name(const std::string& n) {
    if (n.empty() || n.size() > 40) return false;
    for (char ch : n)
        if (!(isalnum((unsigned char)ch) || ch == '_' || ch == '-')) return false;
    return true;
}

std::string status_text() {
    char b[512];
    std::string why;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        why = g_why;
    }
    snprintf(b, sizeof b,
             "stage %s\nframe %llu\ncontrol %d%s%s\narrivals %llu\ntime %.2f\ndate %d\nfrozen %d %.2f\n",
             render::frame_count() ? mods::current_stage().c_str() : "-", (unsigned long long)render::frame_count(),
             g_control.load() ? 1 : 0, why.empty() ? "" : " ", why.c_str(), (unsigned long long)ss::portable_arrivals(),
             double(g_time.load()), g_date.load(), g_frozen.load() ? 1 : 0, double(g_frozenTime.load()));
    return b;
}

// "bench status | save NAME | load NAME | clock [freeze [T] | run] | rec N"
debugsrv::Reply command(const debugsrv::Args& a) {
    const std::string what = a.empty() ? "status" : a[0];
    if (what == "status") return debugsrv::ok(status_text());
    if (!render::frame_count()) return debugsrv::err("the game has not started yet");
    if (what == "save" || what == "load") {
        if (a.size() < 2 || !valid_name(a[1])) return debugsrv::err("usage: bench save|load NAME (letters, digits, _ -)");
        const std::string path = bench_dir() + "/" + a[1] + ".wwstate";
        if (what == "load") {
            struct stat st{};
            if (stat(path.c_str(), &st) != 0) return debugsrv::err("no " + path);
            LOG("[bench] loading %s", path.c_str());
            ss::request_load_portable_file(path, {}, true);  // (the states were made on a USA console)
            return debugsrv::ok("load requested: " + path + " (bench status: arrivals goes up once Link is there)");
        }
        struct Wait {
            std::mutex m;
            bool done = false, ok = false;
            std::string why;
        };
        auto w = std::make_shared<Wait>();
        ss::request_save_portable_path(path, [w](bool ok, const std::string& why) {
            std::lock_guard<std::mutex> lk(w->m);
            w->done = true, w->ok = ok, w->why = why;
        });
        for (int i = 0; i < 200; i++) {
            {
                std::lock_guard<std::mutex> lk(w->m);
                if (w->done) {
                    LOG("[bench] save %s: %s", path.c_str(), w->ok ? "written" : w->why.c_str());
                    return w->ok ? debugsrv::ok("saved " + path) : debugsrv::err("not saved: " + w->why);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return debugsrv::err("no answer within 10 s (is a Quest Log being played?)");
    }
    if (what == "clock") {
        if (a.size() >= 2 && a[1] == "run") {
            g_frozen = false;
            LOG("[bench] the clock runs");
        } else if (a.size() >= 2 && a[1] == "freeze") {
            float t = a.size() >= 3 ? float(atof(a[2].c_str())) : g_time.load();
            if (t < 0 || t >= 360) return debugsrv::err("time of day: 0 to 360 degrees (no Quest Log yet?)");
            g_frozenTime = t;
            g_frozen = true;
            LOG("[bench] the clock is held at %.2f", double(t));
        }
        return debugsrv::ok(status_text());
    }
    if (what == "rec") {
        const int n = a.size() >= 2 ? atoi(a[1].c_str()) : 0;
        if (n < 1 || n > 10000) return debugsrv::err("usage: bench rec N (1..10000 frames)");
        if (g_armed.exchange(true)) return debugsrv::err("a recording is running");
        g_frames.clear();
        g_frames.reserve(size_t(n));
        g_noControl = 0;
        g_done = false;
        g_want = n;  // the render thread starts at its next present
        LOG("[bench] recording %d frames", n);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(n / 10 + 30);
        while (!g_done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!g_done.load(std::memory_order_acquire)) {
            g_want = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            g_armed = false;
            return debugsrv::err("recording timed out (is the game presenting frames?)");
        }
        std::string out = "# dt_us render_us main_us gpu_us passes draws behind control res\n";
        char line[96];
        for (const Frame& f : g_frames) {
            snprintf(line, sizeof line, "%u %u %u %d %u %u %u %u %u\n", f.dtUs, f.renderUs, f.mainUs,
                     f.gpuUs == ~0u ? -1 : int(f.gpuUs), f.passes, f.draws, f.behind, f.control, f.res);
            out += line;
        }
        snprintf(line, sizeof line, "# no_control_checks %llu\n", (unsigned long long)g_noControl.load());
        out += line;
        LOG("[bench] recorded %zu frames", g_frames.size());
        g_armed = false;
        return debugsrv::ok(out);
    }
    return debugsrv::err("usage: bench status | save NAME | load NAME | clock [freeze [T] | run] | rec N");
}

}  // namespace

void game_frame(Cpu* c) {
    static uint64_t last = 0, n = 0;
    const uint64_t t = thread_ticks();
    if (last) g_mainTicks.fetch_add(t - last, std::memory_order_relaxed);
    last = t;
    g_passes.fetch_add(1, std::memory_order_relaxed);
    const uint32_t sv = ld32(kSaveInfoPtr);
    if (in_mem2(sv)) {
        if (g_frozen.load(std::memory_order_relaxed)) stf32(sv + kStatusB + 0xC, g_frozenTime.load());
        g_time = float(ldf32(sv + kStatusB + 0xC));
        g_date = int(ld16(sv + kStatusB + 0x10));
    }
    const bool recording = g_want.load(std::memory_order_relaxed) > 0;
    if (++n % 10 == 0 || recording) {
        std::string why;
        const bool ok = ss::control_check(c, why);
        g_control = ok;
        if (!ok && recording) g_noControl++;
        std::lock_guard<std::mutex> lk(g_mu);
        g_why = ok ? "" : why;
    }
    // the guest_call in control_check spends main-thread time: left out of the measurement
    last = thread_ticks();
}

void present(uint64_t nowNs, bool gpuBehind, double gpuBusyMs, uint64_t gpuSamples, float resScale, uint64_t draws) {
    static uint64_t lastNs = 0, lastWait = 0, lastMain = 0, lastPasses = 0, lastSamples = 0, lastDraws = 0;
    const uint64_t wait = gx2::render_thread_wait_ns();
    const uint64_t mainTicks = g_mainTicks.load(std::memory_order_relaxed);
    const uint64_t passes = g_passes.load(std::memory_order_relaxed);
    const int want = g_want.load(std::memory_order_relaxed);
    if (want > 0 && lastNs) {
        Frame f{};
        const uint64_t dt = nowNs - lastNs, waited = wait - lastWait;
        f.dtUs = uint32_t(dt / 1000);
        f.renderUs = uint32_t((dt > waited ? dt - waited : 0) / 1000);
        f.mainUs = uint32_t(ticks_us(mainTicks - lastMain));
        f.gpuUs = gpuSamples != lastSamples && gpuBusyMs >= 0 ? uint32_t(gpuBusyMs * 1000.0) : ~0u;
        f.passes = uint16_t(passes - lastPasses);
        f.draws = uint16_t(std::min<uint64_t>(draws - lastDraws, 65535));
        f.behind = gpuBehind ? 1 : 0;
        f.control = g_control.load(std::memory_order_relaxed) ? 1 : 0;
        f.res = uint8_t(std::min(255.0f, resScale * 100.0f + 0.5f));
        g_frames.push_back(f);
        if (g_want.fetch_sub(1) == 1) g_done.store(true, std::memory_order_release);
    }
    lastNs = nowNs, lastWait = wait, lastMain = mainTicks, lastPasses = passes, lastSamples = gpuSamples, lastDraws = draws;
}

void register_commands() {
    debugsrv::add_command("bench", "bench status|save NAME|load NAME|clock [freeze [T]|run]|rec N   performance test",
                          command);
}

}  // namespace bench
