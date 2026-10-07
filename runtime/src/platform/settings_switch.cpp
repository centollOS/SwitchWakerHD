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
// WWHD_GPU_PROFILE (env.txt) = default | 384 | 460 | 1600 (460 with memory 1600) | 0x<configuration id>;
// without it, the menu's saved choice; without that, 1600.
#include "settings_switch.h"

#include <switch.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "gfx/gl/settings.h"
#include "overlay/hostui.h"
#include "runtime.h"

namespace switch_settings {
namespace {

std::mutex g_mu;
int g_profile = kGpu460Mem1600;
bool g_profile_env = false;
bool g_apm_ready = false, g_apm_failed = false;
u32 g_apm_saved = 0x00020003, g_apm_now = 0;

const char* const kIds[kGpuProfiles] = {"default", "384", "460", "1600"};
const char* const kLabels[kGpuProfiles] = {"System default (GPU 307 MHz)", "GPU 384 MHz", "GPU 460 MHz",
                                           "GPU 460 MHz + memory 1600 MHz"};

void restore_at_exit() {
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
    g_profile = p;
    u32 chain[4];
    chain_for(p, chain);
    apply(kIds[p], chain);
    hostui::set(kKeyGpuProfile, kIds[p]);
}

void save_picture() {
    const gfxgl::PictureGrade g = gfxgl::picture_grade_now();
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
    // picture adjustments and the counter: env.txt wins, else what the menu saved
    if (!picture_env()) {
        gfxgl::PictureGrade g = gfxgl::picture_grade_now();
        bool any = false;
        any |= saved_float(kKeyExposure, g.exposure, 0.25f, 4.0f);
        any |= saved_float(kKeyContrast, g.contrast, 0.0f, 2.0f);
        any |= saved_float(kKeySaturation, g.saturation, 0.0f, 3.0f);
        any |= saved_float(kKeyGamma, g.gamma, 0.5f, 2.0f);
        if (any) {
            gfxgl::set_picture_grade(g);
            LOG("[switch] picture (settings): exposure %.2f contrast %.2f saturation %.2f gamma %.2f", g.exposure,
                g.contrast, g.saturation, g.gamma);
        }
    }
    if (!fps_counter_env()) {
        std::string v;
        if (hostui::get(kKeyFpsCounter, v) && !v.empty()) gfxgl::set_fps_overlay_mode(atoi(v.c_str()));
    }
}

}  // namespace switch_settings
