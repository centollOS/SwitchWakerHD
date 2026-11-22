// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Switch 2: the CPU clock setting and the clock probe. Ported to OunceWakerHD (perf/clocks,
// 2026-11-21) from ZeldaWWHDRecomp's Switch 2 runtime (platform/switch2/clocks_switch2.cpp, decomp 9m/9p); changes:
// no apm calls (this fork never initialises apm on the Switch 2: settings_switch.cpp apm_ready), so no
// WWHD_S2_PERFCONF and no apm fields in the log line; the menu entry is in the Switch 2 tab (s2_clock_switch2.cpp);
// a debug-server command "s2clock" (not saved); the boost rate falls back to the console's known 1523.2 MHz when
// the FastLoad measurement is refused (hbmenu applet).
// Original description: the clock probe (decomp 9p). Every 10 s (WWHD_S2_CLOCK_LOG=<s>, 0 = off) a line with the real clock
// rates and the performance state, so a console run shows what the CPU, GPU and memory run at:
//   [switch2 clocks]   20 s: CPU 998.4 MHz, GPU 561.0 MHz, EMC 3200.0 MHz (clkrst); apm performance mode 0 (normal),
//   configuration 0x00010000 (apm) / 0x00010000 (applet); handheld; changed: none
// - rates: clkrst (clkrstGetClockRate, [8.0.0+] on the Switch 1), else pcv (pcvGetClockRate, [1.0.0-7.0.1]); the
//   start-up line says which one answered, and every failure is logged once with its result code.
// - the start-up lines also list the rates the console reports as possible (clkrstGetPossibleClockRates).
// Requests, off unless the variable is given (hen-args: --s2-env NAME=VALUE):
// - (WWHD_S2_PERFCONF: not in this fork, no apm.)
// - WWHD_S2_CPU_BOOST=1: appletSetCpuBoostMode(FastLoad) for the whole run (on the Switch 1 this lowers the GPU to
//   its minimum), then the clocks.
// - WWHD_S2_CPU_MHZ=<n>: clkrstSetClockRate on the CPU, only when <n> MHz is a rate the console lists as possible
//   AND no higher than a rate the console itself uses for an official profile: the start-up rate, the rate under
//   the console's own CPU boost (measured here: boost on for 0.3 s, then off again, unless WWHD_S2_CPU_BOOST keeps
//   it) and the rate under WWHD_S2_PERFCONF. With a range instead of a list (the Switch 2: CPU 115.2-1702.4 MHz)
//   only one of those seen rates is accepted (within 1 MHz). Anything else is refused and logged. No overclocking.
//   WWHD_S2_CPU_MHZ_ALLOW_MAX=1 also accepts the top of the reported range (1702.4 MHz on the Switch 2), with the
//   menu's Maximum checks below.
// The setting "CPU clock (Switch 2)" (Settings > Graphics > Frame rate; saved as "s2CpuClock" = normal | boost | max,
// default boost, user decision 2026-11-20): Normal leaves the console's rate; Boost sets the console's own boost
// rate (1523.2 MHz, measured as above) with clkrst, which keeps the GPU at its normal rate (unlike the FastLoad boost
// mode); Maximum (experimental) sets the top of the reported range. Applied at start and live when changed. Each
// second the CPU rate is read back: if the system does not hold Maximum (two readings in a row), the probe logs it
// and falls back to the boost rate; the menu shows it. WWHD_S2_CPU_MHZ and WWHD_S2_CPU_BOOST win over the setting.
// The GPU and EMC rates are never set.
// Everything changed is undone at exit (switch2_clocks_restore: atexit, the exit report's paths, userAppExit and
// the crash handler): the CPU rate set back to the one before, the performance configuration back to the one
// before, the boost mode back to normal.
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "runtime.h"
#include "overlay/hostui.h"
#include "clocks_switch2.h"

namespace {
enum Clock { Cpu, Gpu, Emc, ClockCount };
const char* const kName[ClockCount] = {"CPU", "GPU", "EMC"};
const PcvModuleId kModuleId[ClockCount] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
const PcvModule kModule[ClockCount] = {PcvModule_CpuBus, PcvModule_GPU, PcvModule_EMC};

enum Backend { None, Clkrst, Pcv };
Backend g_backend = None;
ClkrstSession g_session[ClockCount];
bool g_sessionOpen[ClockCount] = {};
bool g_readFailLogged[ClockCount] = {};
bool g_appletConfFailLogged = false;

std::timed_mutex g_ipc;  // the log thread's service calls and the restore at exit
int g_secs = 10;
std::atomic<bool> g_stop{false}, g_running{false};
std::thread g_thread;

// what was changed, for the restore
std::atomic<bool> g_restored{false};
bool g_cpuSet = false;
u32 g_cpuBefore = 0, g_cpuWanted = 0;
// the CPU target: the setting (0 normal, 1 boost, 2 maximum) or WWHD_S2_CPU_MHZ
std::atomic<int> g_choice{1};
constexpr u32 kKnownBoostHz = 1523200000;  // the Switch 2's FastLoad CPU rate (measured 2026-11-20)
std::atomic<bool> g_choiceDirty{true};
bool g_envCpu = false, g_envBoost = false;  // WWHD_S2_CPU_MHZ / WWHD_S2_CPU_BOOST given: the setting is not used
u32 g_startRate = 0, g_boostRate = 0, g_maxRate = 0, g_confRate = 0;  // CPU rates: seen at start, measured boost,
                                                                      // range top, under WWHD_S2_PERFCONF
bool g_boostMeasured = false;
bool g_wantedIsMax = false, g_fellBack = false;
int g_misses = 0;
std::mutex g_statusMu;
std::string g_status = "Starting...";
bool g_boostOn = false;
char g_changed[160] = "none";

void append_changed(const char* what) {
    if (!strcmp(g_changed, "none")) g_changed[0] = 0;
    else strncat(g_changed, ", ", sizeof g_changed - strlen(g_changed) - 1);
    strncat(g_changed, what, sizeof g_changed - strlen(g_changed) - 1);
}

const char* op_mode_name(AppletOperationMode m) { return m == AppletOperationMode_Console ? "docked" : "handheld"; }

// 0 when the rate cannot be read (logged once per clock)
u32 read_rate(Clock c) {
    u32 hz = 0;
    Result rc = 0;
    if (g_backend == Clkrst && g_sessionOpen[c]) rc = clkrstGetClockRate(&g_session[c], &hz);
    else if (g_backend == Pcv) rc = pcvGetClockRate(kModule[c], &hz);
    else return 0;
    if (R_FAILED(rc)) {
        if (!g_readFailLogged[c]) {
            g_readFailLogged[c] = true;
            LOG("[switch2 clocks] reading the %s rate failed: rc 0x%x (%s; logged once)", kName[c], rc,
                g_backend == Clkrst ? "clkrstGetClockRate" : "pcvGetClockRate");
        }
        return 0;
    }
    return hz;
}

// (clk2) The NRO's __appInit (sm_session_switch2.c replaceSmSession) swaps the loader's sm session for a fresh one
// registered with this process's own access list; that list has no clkrst / pcv, so GetService answers 0x1015
// (sm NotAllowed) under some homebrew loaders, while the loader's session (what libnx keeps, and what ZeldaWWHDRecomp uses) may
// open them. replaceSmSession leaves that session's handle in the override table: ask it for the service and add
// the handle as an override, so clkrstInitialize / pcvInitialize (smGetServiceWrapper) take it.
constexpr Result kSmNotAllowed = 0x1015;
Result via_loader_sm(const char* svc) {
    const SmServiceName name = smEncodeName(svc);
    if (smGetServiceOverride(name) != INVALID_HANDLE) return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    const Handle smh = smGetServiceOverride(smEncodeName("sm:"));
    if (smh == INVALID_HANDLE) return MAKERESULT(Module_Libnx, LibnxError_NotFound);  // (no loader session)
    Service sm;
    serviceCreate(&sm, smh);  // (borrowed: never closed here)
    Handle h = INVALID_HANDLE;
    const Result rc = serviceDispatchIn(&sm, 1, name, .out_handle_attrs = {SfOutHandleAttr_HipcMove}, .out_handles = &h);
    if (R_SUCCEEDED(rc)) smAddOverrideHandle(name, h);
    return rc;
}
// the service's init; on sm NotAllowed once more through the loader's sm session (logged)
template <class Init>
Result init_service(const char* svc, Init init) {
    Result rc = init();
    if (rc != kSmNotAllowed) return rc;
    const Result g = via_loader_sm(svc);
    if (R_FAILED(g)) {
        LOG("[switch2 clocks] %s: sm refused it (rc 0x%x); through the loader's sm session: rc 0x%x", svc, rc, g);
        return rc;
    }
    const Result rc2 = init();
    LOG("[switch2 clocks] %s: sm refused it (rc 0x%x); through the loader's sm session: %s (rc 0x%x)", svc, rc,
        R_SUCCEEDED(rc2) ? "open" : "still failed", rc2);
    return rc2;
}

void open_backend() {
    const Result rc = init_service("clkrst", [] { return clkrstInitialize(); });
    if (R_SUCCEEDED(rc)) {
        g_backend = Clkrst;
        int open = 0;
        for (int c = 0; c < ClockCount; ++c) {
            const Result r = clkrstOpenSession(&g_session[c], kModuleId[c], 3);
            g_sessionOpen[c] = R_SUCCEEDED(r);
            open += g_sessionOpen[c];
            if (R_FAILED(r))
                LOG("[switch2 clocks] clkrstOpenSession(%s, module id 0x%08x) failed: rc 0x%x", kName[c],
                    (unsigned)kModuleId[c], r);
        }
        if (open > 0) {
            LOG("[switch2 clocks] rates from clkrst (clkrstInitialize ok, %d of 3 sessions open)", open);
            return;
        }
        clkrstExit();
    } else {
        LOG("[switch2 clocks] clkrstInitialize failed: rc 0x%x; trying pcv", rc);
    }
    const Result p = init_service("pcv", [] { return pcvInitialize(); });
    if (R_SUCCEEDED(p)) {
        g_backend = Pcv;
        u32 hz = 0;
        const Result r = pcvGetClockRate(PcvModule_CpuBus, &hz);
        if (R_SUCCEEDED(r)) {
            LOG("[switch2 clocks] rates from pcv (pcvGetClockRate; clkrst not available)");
            return;
        }
        LOG("[switch2 clocks] pcvGetClockRate failed: rc 0x%x; no clock rates this run", r);
        pcvExit();
    } else {
        LOG("[switch2 clocks] pcvInitialize failed: rc 0x%x; no clock rates this run", p);
    }
    g_backend = None;
}

// the possible rates of one clock (clkrst only); count 0 when unknown
struct Rates {
    u32 hz[64];
    s32 count = 0;
    PcvClockRatesListType type = PcvClockRatesListType_Invalid;
};
Rates g_cpuRates;

void log_possible(Clock c, Rates* keep) {
    Rates r;
    Result rc;
    if (g_backend == Clkrst && g_sessionOpen[c])
        rc = clkrstGetPossibleClockRates(&g_session[c], r.hz, 64, &r.type, &r.count);
    else if (g_backend == Pcv)
        rc = pcvGetPossibleClockRates(kModule[c], r.hz, 64, &r.type, &r.count);
    else
        return;
    if (R_FAILED(rc)) {
        LOG("[switch2 clocks] possible %s rates: failed, rc 0x%x", kName[c], rc);
        return;
    }
    r.count = std::clamp<s32>(r.count, 0, 64);
    char text[900];
    int n = 0;
    text[0] = 0;
    for (s32 i = 0; i < r.count && n < (int)sizeof text - 16; ++i)
        n += snprintf(text + n, sizeof text - n, "%s%.1f", i ? " " : "", r.hz[i] / 1e6);
    LOG("[switch2 clocks] possible %s rates (%s, %d): %s MHz", kName[c],
        r.type == PcvClockRatesListType_Discrete ? "discrete" : r.type == PcvClockRatesListType_Range ? "range" : "invalid",
        (int)r.count, text);
    if (keep) *keep = r;
}

// the CPU part of the line: what the probe set and whether the system holds it
std::string cpu_text(u32 now) {
    char t[160];
    if (!g_cpuSet) {
        snprintf(t, sizeof t, "; CPU rate: the console's (%s)",
                 g_envBoost ? "WWHD_S2_CPU_BOOST" : g_envCpu ? "WWHD_S2_CPU_MHZ not applied" : "setting Normal or not set");
    } else {
        const bool held = now && std::fabs((double)now - g_cpuWanted) < 1e6;
        snprintf(t, sizeof t, "; CPU set to %.1f MHz (%s%s)%s", g_cpuWanted / 1e6,
                 g_envCpu ? "WWHD_S2_CPU_MHZ" : g_wantedIsMax ? "setting Maximum" : "setting Boost",
                 g_fellBack ? ", Maximum not held: fell back" : "",
                 !now ? "" : held ? ", held" : " (CPU not at the requested rate: the system changed it)");
    }
    return t;
}

void log_line(const char* when) {
    u32 hz[ClockCount];
    for (int c = 0; c < ClockCount; ++c) hz[c] = read_rate((Clock)c);
    u32 appletConf = 0;
    const Result rcApplet = appletGetCurrentPerformanceConfiguration(&appletConf);
    if (R_FAILED(rcApplet) && !g_appletConfFailLogged) {
        g_appletConfFailLogged = true;
        LOG("[switch2 clocks] appletGetCurrentPerformanceConfiguration failed: rc 0x%x (logged once)", rcApplet);
    }
    const ApmPerformanceMode mode = appletGetPerformanceMode();
    char rate[ClockCount][24];
    for (int c = 0; c < ClockCount; ++c) {
        if (hz[c]) snprintf(rate[c], sizeof rate[c], "%.1f MHz", hz[c] / 1e6);
        else snprintf(rate[c], sizeof rate[c], "?");
    }
    char appletText[24];
    if (R_SUCCEEDED(rcApplet)) snprintf(appletText, sizeof appletText, "0x%08x", appletConf);
    else snprintf(appletText, sizeof appletText, "?");
    LOG("[switch2 clocks] %s: CPU %s, GPU %s, EMC %s (%s); performance mode %d (%s), configuration %s (applet); %s; "
        "changed: %s%s",
        when, rate[Cpu], rate[Gpu], rate[Emc], g_backend == Clkrst ? "clkrst" : g_backend == Pcv ? "pcv" : "no rates",
        (int)mode, mode == ApmPerformanceMode_Boost ? "boost" : mode == ApmPerformanceMode_Normal ? "normal" : "invalid",
        appletText, op_mode_name(appletGetOperationMode()), g_changed, cpu_text(hz[Cpu]).c_str());
}


void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(300)); }

void set_status(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_statusMu);
    g_status = s;
}

// the console's own boost rate: FastLoad for 0.3 s (GPU at its minimum meanwhile), the CPU rate read, back to normal
void measure_boost() {
    if (g_boostMeasured) return;
    g_boostMeasured = true;
    const u32 gpuBefore = read_rate(Gpu);
    const Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    if (R_SUCCEEDED(rc)) g_boostOn = true;
    settle();
    const u32 cpuBoost = read_rate(Cpu), gpuBoost = read_rate(Gpu);
    LOG("[switch2 clocks] %s: appletSetCpuBoostMode(FastLoad) rc 0x%x: CPU %.1f MHz, GPU %.1f -> %.1f MHz",
        g_envBoost ? "WWHD_S2_CPU_BOOST" : "the official boost rate (measured)", rc, cpuBoost / 1e6, gpuBefore / 1e6,
        gpuBoost / 1e6);
    if (R_FAILED(rc) || !cpuBoost) {
        if (g_boostOn && !g_envBoost && R_SUCCEEDED(appletSetCpuBoostMode(ApmCpuBoostMode_Normal))) g_boostOn = false;
        // (this fork) refused, e.g. from hbmenu: the console's own boost rate as measured on the Switch 2 under
        // FastLoad (ZeldaWWHDRecomp 9p, 2026-11-20), only when it lies in the reported CPU range
        if (!g_envBoost && g_cpuRates.type == PcvClockRatesListType_Range && g_cpuRates.count >= 2 &&
            kKnownBoostHz >= g_cpuRates.hz[0] && kKnownBoostHz <= g_cpuRates.hz[1]) {
            g_boostRate = kKnownBoostHz;
            LOG("[switch2 clocks] boost rate not measured: the known Switch 2 boost rate %.1f MHz instead", kKnownBoostHz / 1e6);
        }
        return;
    }
    g_boostRate = cpuBoost;
    if (g_envBoost) {
        append_changed("CPU boost FastLoad");
        log_line("after WWHD_S2_CPU_BOOST");
        return;
    }
    const Result off = appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if (R_SUCCEEDED(off)) g_boostOn = false;
    LOG("[switch2 clocks] boost mode off again: appletSetCpuBoostMode(Normal) rc 0x%x", off);
    settle();
}

bool set_cpu(u32 rate, bool isMax, const char* why) {
    if (g_backend != Clkrst || !g_sessionOpen[Cpu]) return false;
    if (!g_cpuSet) {
        g_cpuBefore = read_rate(Cpu);
        if (!g_cpuBefore) {
            LOG("[switch2 clocks] %s: not set, the current CPU rate is unknown (nothing to restore)", why);
            return false;
        }
    }
    const Result rc = clkrstSetClockRate(&g_session[Cpu], rate);
    LOG("[switch2 clocks] %s: clkrstSetClockRate(CPU, %u Hz) rc 0x%x (the console's rate %.1f MHz, restored at exit)", why,
        rate, rc, g_cpuBefore / 1e6);
    if (R_FAILED(rc)) return false;
    g_cpuSet = true, g_cpuWanted = rate, g_wantedIsMax = isMax, g_misses = 0;
    return true;
}

void unset_cpu(const char* why) {
    if (!g_cpuSet) return;
    const Result rc = clkrstSetClockRate(&g_session[Cpu], g_cpuBefore);
    LOG("[switch2 clocks] %s: CPU back to the console's %.1f MHz, rc 0x%x", why, g_cpuBefore / 1e6, rc);
    g_cpuSet = false, g_wantedIsMax = false;
}

// WWHD_S2_CPU_MHZ: a listed rate, or (range / no list) a rate the console was seen running at; with
// WWHD_S2_CPU_MHZ_ALLOW_MAX=1 also the top of the reported range. 0 = refused (logged).
u32 env_rate(const char* m, bool* isMax) {
    const double want = atof(m);
    const char* a = getenv("WWHD_S2_CPU_MHZ_ALLOW_MAX");
    const bool allowMax = a && atoi(a) != 0;
    *isMax = false;
    if (g_backend != Clkrst || !g_sessionOpen[Cpu]) {
        LOG("[switch2 clocks] WWHD_S2_CPU_MHZ=%s refused: no clkrst CPU session", m);
        return 0;
    }
    measure_boost();
    const u32 seen[3] = {g_startRate, g_confRate, g_boostRate};
    u32 official = 0;
    for (u32 r : seen) official = std::max(official, r);
    u32 rate = 0;
    if (allowMax && g_maxRate && std::fabs(g_maxRate / 1e6 - want) < 1.0) {
        *isMax = true;
        LOG("[switch2 clocks] WWHD_S2_CPU_MHZ=%s: the top of the reported range (%.1f MHz), allowed by "
            "WWHD_S2_CPU_MHZ_ALLOW_MAX=1 (experimental; falls back to the boost rate when the system does not hold it)",
            m, g_maxRate / 1e6);
        return g_maxRate;
    }
    if (g_cpuRates.type == PcvClockRatesListType_Discrete)
        for (s32 i = 0; i < g_cpuRates.count; ++i)
            if (std::fabs(g_cpuRates.hz[i] / 1e6 - want) < 1.0) rate = g_cpuRates.hz[i];
    if (!rate && g_cpuRates.type != PcvClockRatesListType_Discrete)
        for (u32 r : seen)
            if (r && std::fabs(r / 1e6 - want) < 1.0) rate = r;
    if (!rate) {
        LOG("[switch2 clocks] WWHD_S2_CPU_MHZ=%s refused: not a rate the console %s (CPU rates seen: start-up %.1f, "
            "WWHD_S2_PERFCONF %.1f, boost %.1f MHz; 0 = not measured; range top %.1f with WWHD_S2_CPU_MHZ_ALLOW_MAX=1)", m,
            g_cpuRates.type == PcvClockRatesListType_Discrete ? "lists as possible" : "was seen running at",
            g_startRate / 1e6, g_confRate / 1e6, g_boostRate / 1e6, g_maxRate / 1e6);
        return 0;
    }
    if (!official || rate > official) {
        LOG("[switch2 clocks] WWHD_S2_CPU_MHZ=%s refused: %.1f MHz is above the highest official rate seen (%.1f MHz)", m,
            rate / 1e6, official / 1e6);
        return 0;
    }
    return rate;
}

const char* const kChoiceName[3] = {"Normal", "Boost", "Maximum"};

// the setting (or WWHD_S2_CPU_MHZ) applied; on the probe thread
void apply_cpu() {
    char s[200];
    if (g_envBoost) {
        set_status("Set by WWHD_S2_CPU_BOOST for this start (the boost mode; GPU at its minimum).");
        return;
    }
    if (g_backend != Clkrst || !g_sessionOpen[Cpu]) {
        // (clk2) no clkrst CPU session: Boost / Maximum through the console's boost mode (applet; FastLoad, the GPU
        // at its minimum meanwhile, the rate the console's 1523 MHz), Normal turns it off again; logged
        const int c = g_choice.load();
        const bool want = c != 0;
        if (want != g_boostOn) {
            const Result rc = appletSetCpuBoostMode(want ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
            if (R_SUCCEEDED(rc)) {
                g_boostOn = want;
                if (want) append_changed("CPU boost FastLoad");
            }
            settle();
            const u32 now = read_rate(Cpu);
            char rate[32] = "rate not readable";
            if (now) snprintf(rate, sizeof rate, "CPU %.1f MHz", now / 1e6);
            LOG("[switch2 clocks] setting %s without clkrst: appletSetCpuBoostMode(%s) rc 0x%x: %s%s", kChoiceName[c],
                want ? "FastLoad" : "Normal", rc, rate, want && c == 2 ? " (Maximum needs clkrst: the boost rate)" : "");
        }
        if (!g_boostOn && want) snprintf(s, sizeof s, "Not available: the console refused the boost mode (see the log).");
        else if (g_boostOn) snprintf(s, sizeof s, "Boost mode on (the console's boost rate, 1523 MHz; GPU at its minimum)%s.",
                                     c == 2 ? "; Maximum needs the clock service" : "");
        else snprintf(s, sizeof s, "Now: the console's rate.");
        set_status(s);
        return;
    }
    if (g_envCpu) {
        bool isMax = false;
        const char* m = getenv("WWHD_S2_CPU_MHZ");
        const u32 rate = env_rate(m, &isMax);
        if (rate && set_cpu(rate, isMax, "WWHD_S2_CPU_MHZ")) {
            settle();
            log_line("after WWHD_S2_CPU_MHZ");
        }
        snprintf(s, sizeof s, "Set by WWHD_S2_CPU_MHZ=%s for this start%s.", m, rate ? "" : " (refused: see the log)");
        set_status(s);
        return;
    }
    const int c = g_choice.load();
    if (c == 0) {
        unset_cpu("setting Normal");
        snprintf(s, sizeof s, "Now: the console's rate (%.1f MHz).", read_rate(Cpu) / 1e6);
        set_status(s);
        return;
    }
    measure_boost();
    u32 rate = g_boostRate;
    bool isMax = false;
    if (c == 2 && !g_fellBack) {
        if (g_maxRate) rate = g_maxRate, isMax = true;
        else LOG("[switch2 clocks] setting Maximum: the top of the CPU range is unknown; the boost rate instead");
    }
    if (!rate) {
        unset_cpu("setting without a boost rate");
        set_status("Not available: the console's boost rate could not be measured (see the log).");
        return;
    }
    const bool ok = set_cpu(rate, isMax, c == 2 ? "setting Maximum (experimental)" : "setting Boost");
    if (ok) {
        settle();
        log_line(c == 2 ? "after setting Maximum" : "after setting Boost");
    }
    if (!ok) snprintf(s, sizeof s, "Could not set the CPU rate (see the log).");
    else if (c == 2 && g_fellBack)
        snprintf(s, sizeof s, "The system did not hold Maximum: back to the boost rate (%.1f MHz).", rate / 1e6);
    else snprintf(s, sizeof s, "Now: CPU %.1f MHz.", rate / 1e6);
    set_status(s);
}

// each second: does the system hold the rate? Maximum not held twice in a row -> the boost rate
void hold_check() {
    if (!g_cpuSet) return;
    const u32 now = read_rate(Cpu);
    if (!now) return;
    if (std::fabs((double)now - g_cpuWanted) < 1e6) {
        g_misses = 0;
        return;
    }
    if (++g_misses == 1) {
        LOG("[switch2 clocks] CPU at %.1f MHz, not the %.1f MHz set (%s)", now / 1e6, g_cpuWanted / 1e6,
            g_wantedIsMax ? "Maximum: falls back to the boost rate if it stays so" : "the system changed it");
        if (!g_wantedIsMax) {
            char s[160];
            snprintf(s, sizeof s, "The system changed the CPU rate to %.1f MHz (set: %.1f MHz).", now / 1e6, g_cpuWanted / 1e6);
            set_status(s);
        }
    }
    if (g_wantedIsMax && g_misses >= 2) {
        LOG("[switch2 clocks] Maximum %.1f MHz not held (CPU %.1f MHz): falling back to the boost rate %.1f MHz",
            g_cpuWanted / 1e6, now / 1e6, g_boostRate / 1e6);
        g_fellBack = true;
        if (g_boostRate && set_cpu(g_boostRate, false, "Maximum not held: the boost rate")) {
            char s[160];
            snprintf(s, sizeof s, "The system did not hold Maximum (it went to %.1f MHz): back to the boost rate (%.1f MHz).",
                     now / 1e6, g_boostRate / 1e6);
            set_status(s);
        }
    }
}

void probe_thread() {
    using clock = std::chrono::steady_clock;
    {
        std::lock_guard<std::timed_mutex> lk(g_ipc);
        open_backend();
        for (int c = 0; c < ClockCount; ++c) log_possible((Clock)c, c == Cpu ? &g_cpuRates : nullptr);
        if (g_cpuRates.count > 0) {
            for (s32 i = 0; i < g_cpuRates.count; ++i) g_maxRate = std::max(g_maxRate, g_cpuRates.hz[i]);
        }
        g_startRate = read_rate(Cpu);
        LOG("[switch2 clocks] CPU clock: %s (setting \"%s\"; WWHD_S2_CPU_MHZ / WWHD_S2_CPU_BOOST win); range top %.1f MHz",
            g_envBoost ? "WWHD_S2_CPU_BOOST" : g_envCpu ? "WWHD_S2_CPU_MHZ" : "the setting", kChoiceName[g_choice.load()],
            g_maxRate / 1e6);
        log_line("start");
        if (!g_restored && g_envBoost) measure_boost();
    }
    int nextLog = g_secs > 0 ? g_secs : -1;
    // (combo/all) no clock service (clkrst and pcv refused): the 10 s probe line (all "?") is left out; the setting
    // still works through the boost mode (apply_cpu, logged there)
    if (g_backend == None) {
        nextLog = -1;
        LOG("[switch2 clocks] no clock rates this run: probe lines off; Boost / Maximum through the boost mode only");
    }
    const auto loopStart = clock::now();  // (the start-up calls above take a while: no catching up after them)
    for (int tick = 1; !g_stop; ++tick) {
        const auto due = loopStart + std::chrono::milliseconds(tick * 50);
        while (!g_stop && clock::now() < due) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (g_stop) break;
        const bool dirty = g_choiceDirty.load(), second = tick % 20 == 0;
        const int secs = tick / 20;
        const bool logNow = second && nextLog > 0 && secs >= nextLog;
        if (!dirty && !second) continue;
        if (!g_ipc.try_lock_for(std::chrono::milliseconds(100))) continue;
        if (!g_restored) {
            if (g_choiceDirty.exchange(false)) apply_cpu();
            if (second) hold_check();
            if (logNow) {
                char when[24];
                snprintf(when, sizeof when, "%4d s", secs);
                log_line(when);
            }
        }
        if (logNow) nextLog += g_secs;
        g_ipc.unlock();
    }
    g_running = false;
}

// the restore itself; quiet from the crash handler (no LOG there)
void restore(bool quiet) {
    if (g_restored.exchange(true)) return;
    const bool locked = g_ipc.try_lock_for(std::chrono::milliseconds(300));  // (an abort may come from inside a call)
    if (g_cpuSet) {
        const Result rc = clkrstSetClockRate(&g_session[Cpu], g_cpuBefore);
        if (!quiet) LOG("[switch2 clocks] exit: CPU back to %.1f MHz, rc 0x%x", g_cpuBefore / 1e6, rc);
        g_cpuSet = false;
    }
    if (g_boostOn) {
        const Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
        if (!quiet) LOG("[switch2 clocks] exit: CPU boost mode back to normal, rc 0x%x", rc);
        g_boostOn = false;
    }
    if (locked) g_ipc.unlock();
}

int parse_choice(const std::string& v) { return v == "normal" ? 0 : v == "max" ? 2 : 1; }  // default boost
}  // namespace

namespace switch2_clocks {
int choice() { return g_choice.load(); }
void set_choice(int c, bool save) {
    c = std::clamp(c, 0, 2);
    if (c == 2) g_fellBack = false;  // (a new try of Maximum)
    g_choice = c;
    g_choiceDirty = true;
    static const char* const saved[3] = {"normal", "boost", "max"};
    if (save) hostui::set("s2CpuClock", saved[c]);
}
u32 cpu_hz() {
    std::lock_guard<std::timed_mutex> lk(g_ipc);
    return read_rate(Clock(0));  // (Cpu: also a name in libounce)
}
const char* env_override() {
    if (getenv("WWHD_S2_CPU_MHZ")) return "WWHD_S2_CPU_MHZ";
    if (const char* b = getenv("WWHD_S2_CPU_BOOST"); b && atoi(b) != 0) return "WWHD_S2_CPU_BOOST";
    return nullptr;
}
std::string status() {
    std::lock_guard<std::mutex> lk(g_statusMu);
    return g_status;
}
}  // namespace switch2_clocks

extern "C" void switch2_clocks_restore(void) { restore(false); }
extern "C" void switch2_clocks_restore_quiet(void) { restore(true); }

// heartbeat_switch2.cpp switch2_heartbeat_stop (atexit and the exit report's abort/_Exit/_exit paths)
extern "C" void switch2_clocks_stop(void) {
    g_stop = true;
    for (int i = 0; i < 60 && g_running.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (g_thread.joinable() && g_thread.get_id() != std::this_thread::get_id()) {
        if (g_running.load()) g_thread.detach();
        else g_thread.join();
    }
    restore(false);
}

// heartbeat_switch2.cpp switch2_heartbeat_start (main.cpp, once the log is open)
extern "C" void switch2_clocks_start(void) {
    if (const char* v = getenv("WWHD_S2_CLOCK_LOG")) g_secs = std::clamp(atoi(v), 0, 600);
    const char* m = getenv("WWHD_S2_CPU_MHZ");
    g_envCpu = m && *m;
    const char* b = getenv("WWHD_S2_CPU_BOOST");
    g_envBoost = b && atoi(b) != 0;
    std::string v;
    if (hostui::get("s2CpuClock", v)) g_choice = parse_choice(v);
    g_choiceDirty = true;
    if (g_running.exchange(true)) return;
    try {
        g_thread = std::thread(probe_thread);
    } catch (...) {
        g_running = false;
        return;
    }
    std::atexit([] { switch2_clocks_stop(); });
}
