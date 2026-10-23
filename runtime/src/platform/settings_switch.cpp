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
// The profile is not a choice: GPU 384 MHz for every player (2026-10-23, the owner; perf_test.py gpu-profile at
// Outset's tall grass: 307 -> 384 +4.7 fps, 384 -> 460 +0.5 fps; docs/perf-test.md). From 2026-10-08 to 2026-10-23
// it was the stock 307 MHz, before that 460 + memory 1600, and the menu offered the profiles. A switchGpuProfile
// saved by those menus is not applied and is set to "384" at start. Test only: WWHD_TEST_GPU_PROFILE in settings.ini's
// [dev] section (307 = stock, nothing set | 384 | 460 | 1600 (460 with memory 1600); perf_test.py gpu-profile).
//
// Clocks set through clkrst (CPU 1020-1785 MHz steps, GPU 614.4 MHz in handheld, as sys-clk does; 2026-10-07)
// were removed 2026-10-13: with the CPU at 1224 MHz an Erista console did not wake from sleep (black screen,
// forced power off; #33; at stock clocks it woke). Putting the stock clocks back on the focus change before sleep
// (appletHook) did not help: the program was suspended before the hook ran. Overclocks are left to sys-clk. A saved switchCpuClock is ignored (logged); a saved GPU profile 614
// becomes 1600 (the same apm configuration without the raised GPU clock).
#include "settings_switch.h"
#include "shader_wait_switch.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "gfx/switch_renderer.h"
#include "input.h"
#include "overlay/hostui.h"
#include "screen_mode.h"
#include "runtime.h"

namespace switch_settings {
namespace {

std::mutex g_mu;
int g_profile = kGpuFixed;
bool g_apm_ready = false, g_apm_failed = false;
u32 g_apm_saved = 0x00020003, g_apm_now = 0;

const char* const kIds[kGpuProfiles] = {"307", "384", "460", "1600"};

// picture profiles per mode (g_mu); the mode whose profile the renderer has (-1: none yet)
const char* const kModeIds[kModes] = {"handheld", "docked"};
const char* const kModeLabels[kModes] = {"Handheld", "Docked"};
ResProfile g_res[kModes] = {{1.0f, true}, {1.5f, true}};
int g_res_mode = -1;

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

bool saved_float(const char* key, float& out, float lo, float hi) {
    std::string v;
    if (!hostui::get(key, v) || v.empty()) return false;
    out = std::clamp(strtof(v.c_str(), nullptr), lo, hi);
    return true;
}

}  // namespace

int gpu_profile() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_profile;
}
std::string gpu_profile_status() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_apm_ready) return "not set (apm unavailable)";
    char b[64];
    snprintf(b, sizeof b, "handheld configuration 0x%08x", (unsigned)g_apm_now);
    return b;
}

void tick() {
    static uint64_t last = 0;
    const uint64_t now = armTicksToNs(armGetSystemTick());
    if (now - last < 1'000'000'000ull) return;
    last = now;
    std::lock_guard<std::mutex> lk(g_mu);
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
    g_res[m].scale = std::clamp(p.scale, 0.5f, 2.0f);
    g_res[m].dynamic = p.dynamic;
    char b[16];
    snprintf(b, sizeof b, "%.2f", g_res[m].scale);
    hostui::set(mode_key(kKeyResScale, m).c_str(), b);
    hostui::set(mode_key(kKeyDynamicRes, m).c_str(), g_res[m].dynamic ? "1" : "0");
    if (m == active_mode()) apply_res_profile("menu");
}

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
namespace {
std::atomic<bool> g_captureCombo{false};
}
bool capture_combo() { return g_captureCombo.load(std::memory_order_relaxed); }
void set_capture_combo(bool on) {
    g_captureCombo = on;
    hostui::set(kKeyCaptureCombo, on ? "1" : "0");
    LOG("[switch] capture combo (both sticks): %s", on ? "on" : "off");
}

void set_gamepad_pip(bool on) {
    gfxsw::set_gamepad_pip(on);
    hostui::set(kKeyGamepadPip, on ? "1" : "0");
}

void apply_at_start() {
    // GPU profile: always 384 MHz. One saved by an older version's menu (default, 307, 460, 1600, 614) is not
    // applied and the setting is set to 384, which older versions also read as 384
    std::string id;
    if (hostui::get(kKeyGpuProfile, id) && !id.empty() && id != kIds[kGpuFixed]) {
        LOG("[switch] gpu profile %s (settings) is no longer a choice: 384 MHz, setting reset (for more, use sys-clk)",
            id.c_str());
        hostui::set(kKeyGpuProfile, kIds[kGpuFixed]);
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        int p = kGpuFixed;
        const char* test = getenv("WWHD_TEST_GPU_PROFILE");  // ([dev], perf_test.py gpu-profile)
        if (test && *test) {
            int t = -1;
            for (int i = 0; i < kGpuProfiles; i++)
                if (!strcmp(test, kIds[i])) t = i;
            if (t < 0) LOG("[switch] WWHD_TEST_GPU_PROFILE=%s ignored: one of 307, 384, 460, 1600", test);
            else p = t;
        }
        g_profile = p;
        u32 chain[4] = {};
        chain_for(p, chain);
        LOG("[switch] gpu profile %s (%s)", kIds[p], p != kGpuFixed || (test && *test) ? "test: WWHD_TEST_GPU_PROFILE" : "fixed");
        if (p != kGpuStock) apply(kIds[p], chain);
    }
    // a CPU clock saved by an older version: no longer set (#33)
    {
        std::string c;
        if (hostui::get(kKeyCpuClock, c) && !c.empty() && c != "1020")
            LOG("[switch] cpu clock %s MHz (settings) ignored: the CPU clock option was removed (sleep, #33); the "
                "stock 1020 MHz stays. For more, use sys-clk", c.c_str());
        std::lock_guard<std::mutex> lk(g_mu);
        apm_ready();  // the exit hook that puts the handheld configuration found at start back
    }
    // picture profiles per mode: what the menu saved, else the defaults
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (int m = 0; m < kModes; m++) {
            saved_float(mode_key(kKeyResScale, m).c_str(), g_res[m].scale, 0.5f, 2.0f);
            std::string v;
            if (hostui::get(mode_key(kKeyDynamicRes, m).c_str(), v) && !v.empty()) g_res[m].dynamic = v != "0";
        }
        apply_res_profile("start");
    }
    // picture adjustments and the counter: what the menu saved
    {
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
    {
        std::string v;
        if (hostui::get(kKeyFpsCounter, v) && !v.empty()) gfxsw::set_fps_overlay_mode(atoi(v.c_str()));
    }
    {
        std::string v;
        if (hostui::get(kKeyCaptureCombo, v)) g_captureCombo = v == "1";
    }
    {
        std::string v;
        if (hostui::get(kKeyGamepadPip, v)) gfxsw::set_gamepad_pip(v == "1");
    }
    // the saved controller choice, before the game starts (screen_mode.cpp decides the first title screen's mode from
    // it): controllerMode single (the default: the GamePad, single screen), gamepad or pro (the Wii U's own GamePad /
    // Pro Controller play). An older proController=1 is pro, =0 single; WWHD_PRO_CONTROLLER wins.
    {
        std::string v;
        ControllerMode m = ControllerMode::kSingle;
        if (const char* e = getenv("WWHD_PRO_CONTROLLER")) m = strcmp(e, "0") ? ControllerMode::kPro : ControllerMode::kSingle;
        else if (hostui::get(kKeyControllerMode, v)) m = v == "pro" ? ControllerMode::kPro : v == "gamepad" ? ControllerMode::kGamePad : ControllerMode::kSingle;
        else if (hostui::get("proController", v) && v == "1") m = ControllerMode::kPro;
        screen_mode::set_single_screen(m == ControllerMode::kSingle);
        input::set_pro_controller(m == ControllerMode::kPro);
    }
    {
        std::string v;
        if (hostui::get(kKeyAniso, v) && !v.empty()) gfxsw::set_aniso(v == "1");
        if (hostui::get(kKeyFxaa, v) && !v.empty()) gfxsw::set_fxaa(v == "1");
        if (hostui::get(kKeyBloom, v) && !v.empty()) gfxsw::set_bloom(v == "1");
        if (hostui::get(kKeyDistanceBlur, v) && !v.empty()) gfxsw::set_distance_blur(v == "1");
        if (hostui::get(kKeyShaderWait, v) && !v.empty()) shader_wait::set_mode(atoi(v.c_str()));
    }
}

}  // namespace switch_settings
