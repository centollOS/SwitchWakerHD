// uamtest2: the deko3d renderer's shader path on the Switch, without the game (docs/deko3d-plan.md P0, risks
// 1 and 4). Reads the console's shader cache (sdmc:/switch/wwhd/shadercache_gl.bin, WGS1), converts every
// source with gfxdk::glsl_to_deko and compiles it with uamlib on ONE worker thread with an 8 MB stack, as the
// renderer will. Logs the heap before init, after init, after the first compile (Mesa's built-ins are made
// then), every 200 compiles and at the end (a leak shows as live bytes growing), the per-shader times and
// every failure. If sdmc:/switch/uamtest/shadercache_dksh.bin (tools/switch/dksh_cache on the Mac) is there,
// checks that the console's DKSH bytes and binding maps equal the Mac's for the same hashes. Writes what it
// compiled to sdmc:/switch/uamtest/shadercache_dksh_switch.bin (WDK1) for a diff on the Mac.
// Log: sdmc:/switch/uamtest/uamtest2.log (and on screen). + exits.
#include <switch.h>

#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "glsl_convert.h"
#include "shader_files.h"
#include "uam_api.h"

extern char* fake_heap_start;  // libnx: the heap malloc grows into (sbrk)
extern char* fake_heap_end;

namespace {
constexpr char kRoot[] = "sdmc:/switch/uamtest";
constexpr char kGlCache[] = "sdmc:/switch/wwhd/shadercache_gl.bin";
constexpr char kHostDksh[] = "sdmc:/switch/uamtest/shadercache_dksh.bin";
constexpr char kSwitchDksh[] = "sdmc:/switch/uamtest/shadercache_dksh_switch.bin";
constexpr char kLog[] = "sdmc:/switch/uamtest/uamtest2.log";
constexpr size_t kWorkerStack = 8 << 20;
constexpr int kHeapEvery = 200;

FILE* g_log = nullptr;
int g_screenLines = 0;
void sayf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void sayf(const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (g_log) {
        fputs(b, g_log);
        fflush(g_log);
    }
    // the screen gets the first lines and then only the summary-ish ones (the console is slow)
    if (g_screenLines++ < 40 || b[0] != ' ') {
        printf("%s", b);
        consoleUpdate(nullptr);
    }
}

double ms_since(u64 t0) { return armTicksToNs(armGetSystemTick() - t0) / 1e6; }
double mib(size_t b) { return b / 1048576.0; }

// malloc's view: live = bytes handed out now; arena = what malloc took from the heap (sbrk), its high-water
// mark while nothing is trimmed; never used = heap above sbrk(0)
struct Heap {
    size_t live, arena, neverUsed;
};
Heap heap_now() {
    struct mallinfo mi = mallinfo();
    char* top = static_cast<char*>(sbrk(0));
    Heap h{size_t(mi.uordblks), size_t(mi.arena), 0};
    if (fake_heap_end && top && top != reinterpret_cast<char*>(-1)) h.neverUsed = size_t(fake_heap_end - top);
    return h;
}
size_t g_maxArena = 0;
Heap log_heap(const char* when) {
    Heap h = heap_now();
    g_maxArena = std::max(g_maxArena, h.arena);
    sayf("[heap] %s: live %.2f MiB, malloc arena %.2f MiB, never used %.1f MiB\n", when, mib(h.live), mib(h.arena),
         mib(h.neverUsed));
    return h;
}

std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> v;
    FILE* f = fopen(path, "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    v.resize(size_t(ftell(f)));
    fseek(f, 0, SEEK_SET);
    if (!v.empty() && fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    fclose(f);
    return v;
}

// "0:123(4): error: ..." -> "error: ...", so that equal errors on different lines group together
std::string first_error(const std::string& log) {
    std::istringstream in(log);
    std::string line, first;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (first.empty()) first = line;
        if (line.find("error") != std::string::npos) {
            first = line;
            break;
        }
    }
    size_t e = first.find("error");
    if (e != std::string::npos) first = first.substr(e);
    return first.empty() ? "(no output)" : first;
}

void stats(const char* what, std::vector<double> v) {
    if (v.empty()) {
        sayf("%s: none\n", what);
        return;
    }
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    auto p = [&](double q) { return v[std::min(v.size() - 1, size_t(v.size() * q))]; };
    sayf("%s: %zu shaders, sum %.1f s; per shader mean %.1f ms, p50 %.1f, p90 %.1f, p99 %.1f, max %.1f\n", what,
         v.size(), sum / 1000, sum / v.size(), p(.5), p(.9), p(.99), v.back());
    // the distribution in buckets (the renderer's worker budget: how many stall a first visit how long)
    const double edges[] = {10, 25, 50, 100, 200, 400, 800};
    size_t lo = 0;
    std::string line = "  buckets:";
    for (double e : edges) {
        size_t hi = std::lower_bound(v.begin(), v.end(), e) - v.begin();
        char b[48];
        snprintf(b, sizeof b, " <%.0f ms %zu,", e, hi - lo);
        line += b;
        lo = hi;
    }
    char b[48];
    snprintf(b, sizeof b, " >=800 ms %zu\n", v.size() - lo);
    sayf("%s%s", line.c_str(), b);
}

void run(void*) {
    {
        u32 hz = 0;
        if (R_SUCCEEDED(clkrstInitialize())) {
            ClkrstSession s;
            if (R_SUCCEEDED(clkrstOpenSession(&s, PcvModuleId_CpuBus, 3))) {
                clkrstGetClockRate(&s, &hz);
                clkrstCloseSession(&s);
            }
            clkrstExit();
        }
        sayf("uamtest2: CPU %u MHz, heap %.1f MiB (applet type %d), worker stack %zu MiB, %s\n", hz / 1000000,
             mib(size_t(fake_heap_end - fake_heap_start)), int(appletGetAppletType()), kWorkerStack >> 20,
             gfxdk::kDkshCompilerName);
    }
    log_heap("start");

    // the console's sources (all inflated up front: the heap numbers below start from here)
    std::vector<gfxdk::Wgs1Source> src;
    {
        std::vector<uint8_t> data = read_file(kGlCache);
        std::string err;
        if (data.empty()) {
            sayf("ERROR: cannot read %s\n", kGlCache);
            return;
        }
        if (!gfxdk::read_wgs1(data, &src, &err)) {
            sayf("ERROR: %s: %s\n", kGlCache, err.c_str());
            return;
        }
        size_t vs = 0;
        for (auto& s : src) vs += s.vertex;
        sayf("%s: %zu bytes, %zu sources (%zu vertex, %zu pixel)\n", kGlCache, data.size(), src.size(), vs,
             src.size() - vs);
    }
    // the Mac's results for the same hashes (optional)
    std::unordered_map<uint64_t, gfxdk::DkshRecord> host;
    const uint64_t uamId = gfxdk::dksh_uam_id();
    {
        std::vector<uint8_t> data = read_file(kHostDksh);
        if (data.empty()) {
            sayf("%s: not there, no comparison with the Mac\n", kHostDksh);
        } else {
            std::vector<gfxdk::DkshRecord> recs;
            uint64_t id = 0;
            std::string err;
            if (!gfxdk::read_wdk1(data, &recs, &id, &err)) {
                sayf("ERROR: %s: %s; no comparison with the Mac\n", kHostDksh, err.c_str());
            } else {
                for (auto& r : recs) host.emplace(r.glslHash, std::move(r));
                sayf("%s: %zu records, uamId %016llx, this build %016llx%s\n", kHostDksh, host.size(),
                     (unsigned long long)id, (unsigned long long)uamId,
                     id == uamId ? "" : " -- DIFFERENT: the comparison below is expected to fail");
            }
        }
    }
    const Heap base = log_heap("before uam::init (sources loaded)");

    u64 t0 = armGetSystemTick();
    uam::init(true);
    sayf("uam::init: %.1f ms\n", ms_since(t0));
    log_heap("after uam::init");

    std::vector<double> compileMs, convertMs;
    std::map<std::string, std::pair<int, uint64_t>> failures;  // first error -> count, an example hash
    size_t ok = 0, same = 0, differ = 0, notInHost = 0, hostFailed = 0, mapsDiffer = 0;
    // written record by record: the heap numbers below are uam's alone
    FILE* outFile = fopen(kSwitchDksh, "wb");
    size_t outBytes = 0;
    bool outOk = outFile != nullptr;
    auto put = [&](const std::vector<uint8_t>& bytes) {
        if (outFile && fwrite(bytes.data(), 1, bytes.size(), outFile) != bytes.size()) outOk = false;
        outBytes += bytes.size();
    };
    put(gfxdk::wdk1_header(uamId));
    if (!outFile) sayf("ERROR: cannot write %s\n", kSwitchDksh);
    Heap at200{};
    const u64 tAll = armGetSystemTick();
    for (size_t i = 0; i < src.size(); i++) {
        const gfxdk::Wgs1Source& s = src[i];
        gfxdk::DkshRecord r;
        r.stage = uint8_t(s.vertex ? uam::Stage::Vertex : uam::Stage::Fragment);
        r.glslHash = s.hash;
        gfxdk::ConvertedBindings b;
        u64 t = armGetSystemTick();
        const std::string glsl = gfxdk::glsl_to_deko(s.glsl, s.vertex, &b);
        convertMs.push_back(ms_since(t));
        if (glsl.empty()) {
            auto& f = failures["glsl_to_deko: " + b.error];
            if (!f.first++) f.second = s.hash;
            sayf("  FAILED %016llx_%s in glsl_to_deko: %s\n", (unsigned long long)s.hash, s.vertex ? "vs" : "ps",
                 b.error.c_str());
        } else {
            r.set_bindings(b);
            t = armGetSystemTick();
            uam::Result res = uam::compile(s.vertex ? uam::Stage::Vertex : uam::Stage::Fragment, glsl.c_str());
            const double ms = ms_since(t);
            if (res.ok && !res.dksh.empty()) {
                compileMs.push_back(ms);
                r.dksh = std::move(res.dksh);
                ok++;
            } else {
                const std::string why = first_error(res.log);
                auto& f = failures["uam: " + why];
                if (!f.first++) f.second = s.hash;
                sayf("  FAILED %016llx_%s in uam (%.0f ms): %s\n", (unsigned long long)s.hash, s.vertex ? "vs" : "ps",
                     ms, why.c_str());
            }
        }
        // the Mac's bytes for this hash
        auto h = host.find(s.hash);
        if (!host.empty() && !r.dksh.empty()) {
            if (h == host.end()) {
                notInHost++;
            } else if (h->second.dksh.empty()) {
                hostFailed++;
            } else if (h->second.dksh == r.dksh) {
                same++;
            } else {
                differ++;
                const auto& a = h->second.dksh;
                size_t at = 0;
                while (at < a.size() && at < r.dksh.size() && a[at] == r.dksh[at]) at++;
                if (differ <= 20)
                    sayf("  DIFFERENT from the Mac: %016llx_%s (Mac %zu bytes, Switch %zu, first difference at %zu)\n",
                         (unsigned long long)s.hash, s.vertex ? "vs" : "ps", a.size(), r.dksh.size(), at);
            }
            if (h != host.end() &&
                (memcmp(h->second.ubo, r.ubo, sizeof r.ubo) || memcmp(h->second.sampler, r.sampler, sizeof r.sampler) ||
                 h->second.ufBlockSlot != r.ufBlockSlot || h->second.ufBlockVkBinding != r.ufBlockVkBinding)) {
                if (++mapsDiffer <= 20)
                    sayf("  BINDING MAPS DIFFERENT from the Mac: %016llx\n", (unsigned long long)s.hash);
            }
        }
        std::vector<uint8_t> rec;
        gfxdk::append_wdk1_record(&rec, r);
        put(rec);
        if (i == 0) log_heap("after the first compile (Mesa's built-ins made)");
        if ((i + 1) % kHeapEvery == 0) {
            char when[96];
            snprintf(when, sizeof when, "after %zu compiles (%.0f s)", i + 1, ms_since(tAll) / 1000);
            Heap hp = log_heap(when);
            if (i + 1 == kHeapEvery) at200 = hp;
        }
    }
    const double wall = ms_since(tAll);
    const Heap end = log_heap("after the last compile");
    if (at200.live) {
        // a leak: live bytes keep growing with the compiles (the time lists add 16 bytes per compile)
        const long long grow = (long long)end.live - (long long)at200.live;
        const long long n = (long long)src.size() - kHeapEvery;
        sayf("[heap] live growth from compile %d to the end: %+.3f MiB, %+lld bytes per compile%s\n", kHeapEvery,
             grow / 1048576.0, n > 0 ? grow / n : 0, n > 0 && grow / n > 256 ? "  <-- LEAK?" : "");
    }
    uam::shutdown();
    log_heap("after uam::shutdown");
    sayf("[heap] max malloc arena seen: %.2f MiB; above the loaded sources: %.2f MiB\n", mib(g_maxArena),
         mib(g_maxArena - std::min(g_maxArena, base.arena)));

    sayf("compiled %zu / %zu in %.1f s wall; failed %zu\n", ok, src.size(), wall / 1000, src.size() - ok);
    for (auto& [why, f] : failures)
        sayf("  %5d x %s (e.g. %016llx)\n", f.first, why.c_str(), (unsigned long long)f.second);
    stats("uam compile", compileMs);
    stats("glsl_to_deko", convertMs);
    if (!host.empty())
        sayf("vs the Mac: %zu identical, %zu DIFFERENT, %zu not in the Mac's file, %zu failed on the Mac; binding maps "
             "different: %zu%s\n",
             same, differ, notInHost, hostFailed, mapsDiffer, differ || mapsDiffer ? "  <-- NOT BIT-EXACT" : "");

    if (outFile && fclose(outFile) != 0) outOk = false;
    sayf("%s: %zu bytes%s\n", kSwitchDksh, outBytes, outOk ? "" : " -- WRITE FAILED");
}
}  // namespace

int main() {
    mkdir(kRoot, 0777);
    // the log first: a crash later still leaves a trace
    g_log = fopen(kLog, "w");
    consoleInit(nullptr);
    sayf("uamtest2: started\n");
    // the compiler on a thread with a large stack (Mesa's GLSL parser and nv50_ir recurse deeply), below the
    // main thread's priority as the renderer's worker will be
    Thread t;
    Result rc = threadCreate(&t, run, nullptr, nullptr, kWorkerStack, 0x2C, -2);
    if (R_SUCCEEDED(rc)) {
        threadStart(&t);
        threadWaitForExit(&t);
        threadClose(&t);
    } else {
        sayf("ERROR: worker thread create rc 0x%x\n", unsigned(rc));
    }
    sayf("done. + exits\n");
    if (g_log) fclose(g_log);
    g_log = nullptr;
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
    return 0;
}
