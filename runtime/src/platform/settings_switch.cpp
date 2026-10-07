// Switch-only settings (settings_switch.h): the handheld GPU profile through apm, and loading the
// Switch tab's saved options at start.
//
// GPU profiles: apm's official configurations for games, the same table centollOS uses:
//   0x00020003  CPU 1020  GPU 307.2  memory 1331.2  (handheld default)
//   0x00020004  CPU 1020  GPU 384.0  memory 1331.2
//   0x92220008  CPU 1020  GPU 460.8  memory 1331.2
//   0x92220007  CPU 1020  GPU 460.8  memory 1600.0  (accepted in handheld: hardware test 2026-10-07)
// Docked (Boost) is left alone. apm keeps one configuration per mode and switches between them itself
// on dock/undock, so a choice is set once. apm needs title mode (an application); in applet mode the
// profile is skipped. A choice falls back to the next lower one when apm refuses it. The handheld
// configuration found at start is restored at exit.
// WWHD_GPU_PROFILE (env.txt) = default | 384 | 460 | 1600 (460 with memory 1600) | 614 | 0x<configuration id>;
// without it, the menu's saved choice; without that, 1600.
//
// Beyond apm (a teammate's sys-clk setup, asked for 2026-10-07): the CPU clock (the system table's steps
// 1020-1785 MHz, default 1224; WWHD_CPU_CLOCK=<MHz>, or the menu) and GPU 614.4 MHz in handheld (profile 614) are set through clkrst, as sys-clk does, on top of
// apm's configuration (memory 1600 comes from 0x92220007). The system sets its own clocks again on
// dock / undock, sleep and apm changes, so tick() sets them again (once a second, when they differ);
// at exit the CPU goes back to 1020 MHz and the handheld configuration found at start is restored.
// sys-clk, when it runs with a profile of its own for this title, fights over the same clocks.
#include "settings_switch.h"

#include <switch.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "gfx/switch_renderer.h"
#include "overlay/hostui.h"
#include "runtime.h"

namespace switch_settings {
namespace {

std::mutex g_mu;
int g_profile = kGpu460Mem1600;
bool g_profile_env = false;
bool g_apm_ready = false, g_apm_failed = false;
u32 g_apm_saved = 0x00020003, g_apm_now = 0;

const char* const kIds[kGpuProfiles] = {"default", "384", "460", "1600", "614"};
const char* const kLabels[kGpuProfiles] = {"System default (GPU 307 MHz)", "GPU 384 MHz", "GPU 460 MHz",
                                           "GPU 460 MHz + memory 1600 MHz",
                                           "GPU 614 MHz + memory 1600 MHz (overclock, as sys-clk)"};
const char* const kCpuIds[kCpuClocks] = {"1020", "1122", "1224", "1326", "1428", "1581", "1683", "1785"};
const char* const kCpuLabels[kCpuClocks] = {"1020 MHz (stock)", "1122 MHz", "1224 MHz (default)", "1326 MHz",
                                            "1428 MHz", "1581 MHz", "1683 MHz", "1785 MHz (loading-screen boost)"};
constexpr u32 kCpuHz[kCpuClocks] = {1020000000, 1122000000, 1224000000, 1326000000,
                                    1428000000, 1581000000, 1683000000, 1785000000};
constexpr u32 kGpu614Hz = 614400000;
int g_cpu = kCpuDefault;
bool g_cpu_env = false;

// picture profiles per mode (g_mu); the mode whose profile the renderer has (-1: none yet)
const char* const kModeIds[kModes] = {"handheld", "docked"};
const char* const kModeLabels[kModes] = {"Handheld", "Docked"};
ResProfile g_res[kModes] = {{1.0f, true}, {1.5f, true}};
int g_res_mode = -1;
bool g_res_scale_env = false, g_dynamic_env = false;

// clkrst sessions for setting clocks (sys-clk's way), opened on first use
bool g_clk_tried = false, g_clk_ok = false;
ClkrstSession g_clk_cpu, g_clk_gpu;
bool clk_ready() {
    if (g_clk_tried) return g_clk_ok;
    g_clk_tried = true;
    if (!hosversionAtLeast(8, 0, 0)) {
        LOG("[switch] clocks: clkrst needs system 8.0.0 or later; CPU / GPU overrides off");
        return false;
    }
    Result rc = clkrstInitialize();
    if (R_SUCCEEDED(rc)) rc = clkrstOpenSession(&g_clk_cpu, PcvModuleId_CpuBus, 3);
    if (R_SUCCEEDED(rc)) rc = clkrstOpenSession(&g_clk_gpu, PcvModuleId_GPU, 3);
    g_clk_ok = R_SUCCEEDED(rc);
    if (!g_clk_ok) LOG("[switch] clocks: clkrst unavailable (rc 0x%x); CPU / GPU overrides off", (unsigned)rc);
    return g_clk_ok;
}
bool docked() { return appletGetOperationMode() == AppletOperationMode_Console; }

// the active mode's picture profile to the renderer (mutex held)
void apply_res_profile(const char* why) {
    g_res_mode = docked() ? kDocked : kHandheld;
    const ResProfile& p = g_res[g_res_mode];
    gfxsw::set_resolution_profile(p.scale, p.dynamic);
    LOG("[switch] picture profile %s (%s): internal resolution %.2f, dynamic resolution %s", kModeIds[g_res_mode], why,
        p.scale, p.dynamic ? "on" : "off");
}
std::string mode_key(const char* key, int m) { return std::string(key) + "." + kModeIds[m]; }

// the clock overrides wanted now (0: none)
u32 wanted_cpu_hz() { return g_cpu != kCpu1020 ? kCpuHz[g_cpu] : 0; }
u32 wanted_gpu_hz() { return g_profile == kGpu614 && !docked() ? kGpu614Hz : 0; }
bool g_overridden = false;  // an override was set (at exit: put the stock clocks back)

void set_clock(ClkrstSession* s, const char* what, u32 hz) {
    Result rc = clkrstSetClockRate(s, hz);
    u32 now = 0;
    clkrstGetClockRate(s, &now);
    LOG("[switch] clocks: %s %u MHz: rc 0x%x, now %u MHz", what, hz / 1000000, (unsigned)rc, now / 1000000);
    g_overridden = true;
}
// sets what differs from the wanted clocks (mutex held)
void enforce(bool log_same) {
    const u32 cpu = wanted_cpu_hz(), gpu = wanted_gpu_hz();
    if (!cpu && !gpu) return;
    if (!clk_ready()) return;
    u32 now = 0;
    if (cpu && R_SUCCEEDED(clkrstGetClockRate(&g_clk_cpu, &now)) && (now != cpu || log_same)) set_clock(&g_clk_cpu, "CPU", cpu);
    if (gpu && R_SUCCEEDED(clkrstGetClockRate(&g_clk_gpu, &now)) && (now != gpu || log_same)) set_clock(&g_clk_gpu, "GPU", gpu);
}

void restore_at_exit() {
    if (g_overridden && g_clk_ok) clkrstSetClockRate(&g_clk_cpu, kCpuHz[kCpu1020]);
    Result rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, g_apm_saved);
    LOG("[switch] gpu profile: handheld configuration 0x%08x restored: rc 0x%x", (unsigned)g_apm_saved, (unsigned)rc);
}

bool apm_ready() {
    if (g_apm_ready) return true;
    if (g_apm_failed) return false;
    g_apm_failed = true;
    AppletType applet = appletGetAppletType();
    if (applet != AppletType_Application && applet != AppletType_SystemApplication) {
        LOG("[switch] gpu profile skipped: apm needs title mode (application)");
        return false;
    }
    Result rc = apmInitialize();
    if (R_FAILED(rc)) {
        LOG("[switch] gpu profile: apmInitialize failed rc 0x%x; system default kept", (unsigned)rc);
        return false;
    }
    rc = apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &g_apm_saved);
    if (R_FAILED(rc)) g_apm_saved = 0x00020003;
    LOG("[switch] gpu profile: handheld configuration at start 0x%08x (rc 0x%x)", (unsigned)g_apm_saved, (unsigned)rc);
    g_apm_now = g_apm_saved;
    atexit(restore_at_exit);
    g_apm_failed = false;
    g_apm_ready = true;
    return true;
}

// configurations to try for a profile, best first; 0 ends the list
void chain_for(int p, u32* out) {
    static const u32 kChains[kGpuProfiles][4] = {
        {0, 0, 0, 0},
        {0x00020004, 0, 0, 0},
        {0x92220008, 0x00020004, 0, 0},
        {0x92220007, 0x92220008, 0x00020004, 0},
        {0x92220007, 0x92220008, 0x00020004, 0},  // 614: memory 1600 from apm, the GPU clock from clkrst
    };
    memcpy(out, kChains[p], sizeof kChains[p]);
}

// what: for the log; chain: configurations (0-terminated), empty = the one found at start
void apply(const char* what, const u32* chain) {
    if (!apm_ready()) return;
    if (!chain[0]) {
        Result rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, g_apm_saved);
        LOG("[switch] gpu profile %s: handheld configuration 0x%08x: rc 0x%x", what, (unsigned)g_apm_saved, (unsigned)rc);
        if (R_SUCCEEDED(rc)) g_apm_now = g_apm_saved;
        return;
    }
    for (int i = 0; chain[i]; i++) {
        Result rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, chain[i]);
        LOG("[switch] gpu profile %s: set handheld configuration 0x%08x: rc 0x%x%s", what, (unsigned)chain[i],
            (unsigned)rc, R_SUCCEEDED(rc) ? "" : " (failed)");
        if (R_SUCCEEDED(rc)) {
            g_apm_now = chain[i];
            return;
        }
    }
    LOG("[switch] gpu profile %s: no configuration accepted; 0x%08x kept", what, (unsigned)g_apm_now);
}

bool env_set(const char* name) {
    const char* e = getenv(name);
    return e && *e;
}

bool saved_float(const char* key, float& out, float lo, float hi) {
    std::string v;
    if (!hostui::get(key, v) || v.empty()) return false;
    out = std::clamp(strtof(v.c_str(), nullptr), lo, hi);
    return true;
}

}  // namespace

const char* gpu_profile_label(int p) { return p >= 0 && p < kGpuProfiles ? kLabels[p] : "?"; }
int gpu_profile() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_profile;
}
bool gpu_profile_env() { return g_profile_env; }
std::string gpu_profile_status() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_apm_ready) return "not set (apm unavailable)";
    char b[64];
    snprintf(b, sizeof b, "handheld configuration 0x%08x", (unsigned)g_apm_now);
    return b;
}

void set_gpu_profile(int p) {
    if (p < 0 || p >= kGpuProfiles) return;
    std::lock_guard<std::mutex> lk(g_mu);
    const int before = g_profile;
    g_profile = p;
    u32 chain[4];
    chain_for(p, chain);
    apply(kIds[p], chain);
    // leaving 614 for a profile apm may consider already set (0x92220007 again): the GPU clock of
    // the new profile directly, as apm would set it
    if (before == kGpu614 && p != kGpu614 && !docked() && clk_ready()) {
        static const u32 kGpuHz[kGpuProfiles] = {307200000, 384000000, 460800000, 460800000, kGpu614Hz};
        set_clock(&g_clk_gpu, "GPU", kGpuHz[p]);
    }
    enforce(true);  // after apm: the GPU 614 / CPU overrides on top
    hostui::set(kKeyGpuProfile, kIds[p]);
}

const char* cpu_clock_label(int c) { return c >= 0 && c < kCpuClocks ? kCpuLabels[c] : "?"; }
int cpu_clock() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cpu;
}
bool cpu_clock_env() { return g_cpu_env; }
void set_cpu_clock(int c) {
    if (c < 0 || c >= kCpuClocks) return;
    std::lock_guard<std::mutex> lk(g_mu);
    const int before = g_cpu;
    g_cpu = c;
    if (c == kCpu1020 && before != kCpu1020 && clk_ready()) set_clock(&g_clk_cpu, "CPU", kCpuHz[kCpu1020]);
    enforce(true);
    hostui::set(kKeyCpuClock, kCpuIds[c]);
}

void tick() {
    static uint64_t last = 0;
    const uint64_t now = armTicksToNs(armGetSystemTick());
    if (now - last < 1'000'000'000ull) return;
    last = now;
    std::lock_guard<std::mutex> lk(g_mu);
    // leaving GPU 614 for the docked clocks: nothing to do (apm sets docked); back in handheld it is set again
    enforce(false);
    if (g_res_mode != (docked() ? kDocked : kHandheld)) apply_res_profile("mode changed");
}

const char* mode_label(int m) { return m >= 0 && m < kModes ? kModeLabels[m] : "?"; }
int active_mode() { return docked() ? kDocked : kHandheld; }
ResProfile res_profile(int m) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_res[std::clamp(m, 0, kModes - 1)];
}
void set_res_profile(int m, ResProfile p) {
    if (m < 0 || m >= kModes) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_res_scale_env) g_res[m].scale = std::clamp(p.scale, 0.5f, 2.0f);
    if (!g_dynamic_env) g_res[m].dynamic = p.dynamic;
    char b[16];
    snprintf(b, sizeof b, "%.2f", g_res[m].scale);
    hostui::set(mode_key(kKeyResScale, m).c_str(), b);
    hostui::set(mode_key(kKeyDynamicRes, m).c_str(), g_res[m].dynamic ? "1" : "0");
    if (m == active_mode()) apply_res_profile("menu");
}
bool res_scale_env() { return g_res_scale_env; }
bool dynamic_res_env() { return g_dynamic_env; }

void save_picture() {
    const gfxsw::PictureGrade g = gfxsw::picture_grade_now();
    char b[32];
    snprintf(b, sizeof b, "%.2f", g.exposure);
    hostui::set(kKeyExposure, b);
    snprintf(b, sizeof b, "%.2f", g.contrast);
    hostui::set(kKeyContrast, b);
    snprintf(b, sizeof b, "%.2f", g.saturation);
    hostui::set(kKeySaturation, b);
    snprintf(b, sizeof b, "%.2f", g.gamma);
    hostui::set(kKeyGamma, b);
}
bool picture_env() {
    return env_set("WWHD_EXPOSURE") || env_set("WWHD_CONTRAST") || env_set("WWHD_SATURATION") || env_set("WWHD_GAMMA");
}
bool fps_counter_env() { return env_set("WWHD_FPS"); }

void apply_at_start() {
    // GPU profile: env.txt, else the saved choice, else 460 MHz with memory 1600
    std::string id;
    if (const char* e = getenv("WWHD_GPU_PROFILE"); e && *e) {
        id = e;
        g_profile_env = true;
    } else if (!hostui::get(kKeyGpuProfile, id) || id.empty()) {
        id = kIds[kGpu460Mem1600];
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        int p = -1;
        for (int i = 0; i < kGpuProfiles; i++)
            if (id == kIds[i]) p = i;
        u32 chain[4] = {};
        if (p >= 0) {
            g_profile = p;
            chain_for(p, chain);
        } else {  // a configuration id from env.txt (0x...)
            chain[0] = (u32)strtoul(id.c_str(), nullptr, 0);
            for (int i = 0; i < kGpuProfiles; i++) {
                u32 c[4];
                chain_for(i, c);
                if (c[0] == chain[0]) g_profile = i;
            }
        }
        LOG("[switch] gpu profile %s (%s)", id.c_str(), g_profile_env ? "env.txt" : "settings");
        if (p != kGpuDefault) apply(id.c_str(), chain);
    }
    // CPU clock: env.txt (WWHD_CPU_CLOCK=<MHz of the table>), else the saved choice, else 1224 MHz
    {
        std::string c;
        if (const char* e = getenv("WWHD_CPU_CLOCK"); e && *e) {
            c = e;
            g_cpu_env = true;
        } else {
            hostui::get(kKeyCpuClock, c);
        }
        std::lock_guard<std::mutex> lk(g_mu);
        g_cpu = kCpuDefault;
        for (int i = 0; i < kCpuClocks; i++)
            if (c == kCpuIds[i]) g_cpu = i;
        LOG("[switch] cpu clock %s MHz (%s)", kCpuIds[g_cpu], g_cpu_env ? "env.txt" : c.empty() ? "default" : "settings");
        apm_ready();  // the exit hook that puts the stock clocks back
        enforce(true);
    }
    // picture profiles per mode: env.txt fixes a value in both modes, else what the menu saved, else the defaults
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const char* rs = getenv("WWHD_RES_SCALE");
        const char* dr = getenv("WWHD_DYNAMIC_RES");
        g_res_scale_env = rs && *rs && atof(rs) > 0;
        g_dynamic_env = dr && *dr;
        for (int m = 0; m < kModes; m++) {
            if (g_res_scale_env)
                g_res[m].scale = std::clamp(float(atof(rs)), 0.5f, 2.0f);
            else
                saved_float(mode_key(kKeyResScale, m).c_str(), g_res[m].scale, 0.5f, 2.0f);
            std::string v;
            if (g_dynamic_env)
                g_res[m].dynamic = !(atof(dr) == 0.0 && *dr == '0');  // gfx/deko's reading of WWHD_DYNAMIC_RES
            else if (hostui::get(mode_key(kKeyDynamicRes, m).c_str(), v) && !v.empty())
                g_res[m].dynamic = v != "0";
        }
        apply_res_profile("start");
    }
    // picture adjustments and the counter: env.txt wins, else what the menu saved
    if (!picture_env()) {
        gfxsw::PictureGrade g = gfxsw::picture_grade_now();
        bool any = false;
        any |= saved_float(kKeyExposure, g.exposure, 0.25f, 4.0f);
        any |= saved_float(kKeyContrast, g.contrast, 0.0f, 2.0f);
        any |= saved_float(kKeySaturation, g.saturation, 0.0f, 3.0f);
        any |= saved_float(kKeyGamma, g.gamma, 0.5f, 2.0f);
        if (any) {
            gfxsw::set_picture_grade(g);
            LOG("[switch] picture (settings): exposure %.2f contrast %.2f saturation %.2f gamma %.2f", g.exposure,
                g.contrast, g.saturation, g.gamma);
        }
    }
    if (!fps_counter_env()) {
        std::string v;
        if (hostui::get(kKeyFpsCounter, v) && !v.empty()) gfxsw::set_fps_overlay_mode(atoi(v.c_str()));
    }
}

}  // namespace switch_settings
