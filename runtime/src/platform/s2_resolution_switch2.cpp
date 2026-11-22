// (this fork, s2/resolution) Switch 2 output and render resolution (s2_resolution_switch2.h) and their options in
// the settings overlay's Switch 2 tab
#include "s2_resolution_switch2.h"

#include <cstdlib>

#include <switch.h>

#include <atomic>
#include <cmath>
#include <string>

#include "imgui.h"
#include "s2_display_switch2.h"
#include "s2_tab_switch2.h"
#include "settings_switch.h"
#include "../gfx/switch_renderer.h"
#include "../overlay/hostui.h"
#include "../runtime.h"

namespace s2res {
namespace {
const char* const kOutIds[kOutputs] = {"auto", "1080", "1440", "2160", "previous"};
const char* const kRenderIds[kRenders] = {"auto", "native", "2160", "1800", "1440", "1080", "720"};
std::atomic<int> g_output{kOutAuto}, g_render{kRenderAuto};
std::atomic<bool> g_showSize{false};
// the TV's resolution (appletGetDefaultDisplayResolution, read while docked; 0 not read yet)
std::atomic<uint32_t> g_tvW{0}, g_tvH{0};
// the handheld screen's largest buffer resolution level (s2display, read handheld; -1 not read yet)
std::atomic<int> g_handheldLevel{-1};

// WWHD_S2_HANDHELD_BIG: 1 always a larger handheld swapchain, 0 never (supersampling), unset: by the screen's level
int handheld_big_forced() {
    static const int f = [] {
        const char* e = getenv("WWHD_S2_HANDHELD_BIG");
        return e && *e == '1' ? 1 : e && *e == '0' ? 0 : -1;
    }();
    return f;
}
// whether the handheld screen takes a w x h swapchain: the level's code (1, 2, 4, 8) read as that many times
// 1920x1080's pixels (an assumption: VI2 does not document the levels' sizes). Unknown level: no (the safe path:
// deko3d aborts on a swapchain the display refuses)
bool handheld_big_ok(uint32_t w, uint32_t h) {
    if (const int f = handheld_big_forced(); f >= 0) return f == 1;
    const int l = g_handheldLevel.load(std::memory_order_relaxed);
    return l >= 0 && uint64_t(w) * h <= (uint64_t(1) << l) * 1920u * 1080u;
}
const char* handheld_path() {
    const int o = output();
    if (o != kOut1440 && o != kOut2160) return "1920x1080 (the screen)";
    uint32_t w, h, cw, ch;
    output_size(false, w, h);
    chosen_output_size(false, cw, ch);
    return w == cw ? "a swapchain of the chosen size, the display scales it to the screen"
                   : "1920x1080 swapchain, the game rendered at the chosen size and downscaled (supersampling)";
}

int find(const char* const* ids, int n, const std::string& v, int def) {
    for (int i = 0; i < n; i++)
        if (v == ids[i]) return i;
    return def;
}
void log_choice(const char* why) {
    uint32_t hw, hh, dw, dh;
    output_size(false, hw, hh);
    output_size(true, dw, dh);
    char r[160];
    int n = 0;
    for (int d = 0; d < 2; d++) {
        float sc = 0;
        if (render_scale(d == 1, sc))
            n += snprintf(r + n, sizeof r - size_t(n), "%s%ux%u %s (dynamic resolution off)", d ? ", " : "",
                          unsigned(1280 * sc + 0.5f), unsigned(720 * sc + 0.5f), d ? "docked" : "handheld");
        else
            n += snprintf(r + n, sizeof r - size_t(n), "%sthe Picture tab's profile %s", d ? ", " : "", d ? "docked" : "handheld");
        if (n >= int(sizeof r)) n = int(sizeof r) - 1;
    }
    uint32_t cw, ch;
    chosen_output_size(false, cw, ch);
    LOG("[s2] display mode (%s): output %s: %ux%u handheld (chosen %ux%u: %s; screen buffer level %d%s), %ux%u docked "
        "(TV reports %ux%u); render %s: %s", why, kOutIds[output()], hw, hh, cw, ch, handheld_path(),
        g_handheldLevel.load(), handheld_big_forced() >= 0 ? ", forced by WWHD_S2_HANDHELD_BIG" : g_handheldLevel.load() < 0 ? ", not read yet" : "",
        dw, dh, unsigned(g_tvW.load()), unsigned(g_tvH.load()), kRenderIds[render()], r);
}
}  // namespace

int output() { return g_output.load(std::memory_order_relaxed); }
int render() { return g_render.load(std::memory_order_relaxed); }
bool show_size() { return g_showSize.load(std::memory_order_relaxed); }

void chosen_output_size(bool docked, uint32_t& w, uint32_t& h) {
    const int o = output();
    if (o == kOutPrevious) {
        w = docked ? 1920 : 1280;
        h = docked ? 1080 : 720;
        return;
    }
    w = 1920;
    h = 1080;
    switch (o) {
    case kOut1440: w = 2560, h = 1440; break;
    case kOut2160: w = 3840, h = 2160; break;
    case kOut1080: break;
    default: {
        if (!docked) break;  // auto handheld: the screen's 1920x1080
        const uint32_t tw = g_tvW.load(std::memory_order_relaxed), th = g_tvH.load(std::memory_order_relaxed);
        if (tw >= 1280 && th >= 720 && tw <= 3840 && th <= 2160) w = tw, h = th;  // else 1080p until it is read
    }
    }
}

void output_size(bool docked, uint32_t& w, uint32_t& h) {
    chosen_output_size(docked, w, h);
    if (!docked && (w > 1920 || h > 1080) && !handheld_big_ok(w, h)) w = 1920, h = 1080;  // supersampling instead
}

bool render_scale(bool docked, float& scale) {
    const int r = render();
    // handheld, Render auto with Output 1440p / 4K: the game renders at that size (presented at it, or downscaled
    // into the screen's 1920x1080 when the screen can't take that swapchain)
    if (r == kRenderNative || (r == kRenderAuto && !docked && (output() == kOut1440 || output() == kOut2160))) {
        uint32_t w, h;
        chosen_output_size(docked, w, h);
        scale = float(h) / 720.0f;
        return true;
    }
    switch (r) {
    case kRender2160: scale = 3.0f; return true;
    case kRender1800: scale = 2.5f; return true;
    case kRender1440: scale = 2.0f; return true;
    case kRender1080: scale = 1.5f; return true;
    case kRender720: scale = 1.0f; return true;
    default: return false;
    }
}

bool tick(bool docked) {
    if (!docked) {
        // the handheld screen's buffer level (read by s2display a few frames after the start and on undocking)
        const s2display::Caps c = s2display::caps();
        if (!c.valid || c.docked) return false;
        static bool seen = false;  // (logged at the first read even when the level is unknown)
        if (g_handheldLevel.exchange(c.maxResLevel) == c.maxResLevel && seen) return false;
        seen = true;
        LOG("[s2] handheld screen: largest buffer resolution level %d (code %d, read as %dx 1920x1080's pixels; -1: "
            "the query failed, unknown); a "
            "handheld 2560x1440 swapchain %s, 3840x2160 %s%s", c.maxResLevel, c.maxResLevel >= 0 ? 1 << c.maxResLevel : 0,
            c.maxResLevel >= 0 ? 1 << c.maxResLevel : 0, handheld_big_ok(2560, 1440) ? "allowed" : "not allowed (supersampling)",
            handheld_big_ok(3840, 2160) ? "allowed" : "not allowed (supersampling)",
            handheld_big_forced() >= 0 ? " (forced by WWHD_S2_HANDHELD_BIG)" : "");
        log_choice("handheld screen");
        return output() == kOut1440 || output() == kOut2160;
    }
    s32 w = 0, h = 0;
    const Result rc = appletGetDefaultDisplayResolution(&w, &h);
    if (R_FAILED(rc) || w <= 0 || h <= 0) {
        static bool logged = false;
        if (!logged) LOG("[s2] TV resolution: appletGetDefaultDisplayResolution rc 0x%x (%dx%d); output auto stays 1920x1080", rc, w, h);
        logged = true;
        return false;
    }
    const bool changed = g_tvW.exchange(uint32_t(w)) != uint32_t(w);
    if (g_tvH.exchange(uint32_t(h)) == uint32_t(h) && !changed) return false;
    LOG("[s2] TV resolution: %dx%d (AM's default display resolution)", w, h);
    log_choice("TV resolution");
    return output() == kOutAuto;
}
}  // namespace s2res

namespace s2tab {
using namespace s2res;
void start_resolution() {
    std::string v;
    if (saved("s2Output", v)) g_output = find(kOutIds, kOutputs, v, kOutAuto);
    if (saved("s2Render", v)) g_render = find(kRenderIds, kRenders, v, kRenderAuto);
    if (saved("s2ShowSize", v)) g_showSize = v == "1";
    if (appletGetOperationMode() == AppletOperationMode_Console) tick(true);
    log_choice("start");
    if (render() != kRenderAuto) switch_settings::reapply_res_profile("start (s2Render)");
}
void section_resolution() {
    heading("Output and render resolution");
    // output
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Output:");
    static const char* const outNames[kOutputs] = {"Auto", "1080p", "1440p", "4K", "Previous"};
    static const char* const outTips[kOutputs] = {
        "Default. Handheld: 1920x1080 (the screen). Docked: the TV's resolution as set in the console's settings\n"
        "(3840x2160 on a 4K TV). The game's picture is scaled up to it; the render resolution below decides its detail.",
        "Docked: 1920x1080. Handheld: 1920x1080.",
        "Docked: 2560x1440 (the console's scaler fits it to the TV).\n"
        "Handheld: the game renders at 2560x1440 (with Render Auto); shown at that size when the screen takes it,\n"
        "else downscaled to the screen's 1920x1080 (supersampling). Dynamic resolution off.",
        "Docked: 3840x2160, also on a TV set to less (the console scales it down).\n"
        "Handheld: the game renders at 3840x2160 (with Render Auto); shown at that size when the screen takes it,\n"
        "else downscaled to the screen's 1920x1080 (supersampling). Dynamic resolution off. Expensive.",
        "The port's earlier sizes: 1280x720 handheld, 1920x1080 docked."};
    const int o = output();
    for (int i = 0; i < kOutputs; i++) {
        ImGui::SameLine();
        char id[32];
        snprintf(id, sizeof id, "%s##s2out%d", outNames[i], i);
        if (ImGui::RadioButton(id, o == i) && o != i) {
            g_output = i;
            save("s2Output", kOutIds[i]);
            log_choice("menu");
            hostui::post([] { switch_settings::reapply_res_profile("output changed"); });  // a native render follows it
        }
        help(outTips[i]);
    }
    // render
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Render:");
    static const char* const rNames[kRenders] = {"Auto (dynamic)", "Native output", "2160p (4K)", "1800p", "1440p", "1080p", "720p"};
    static const char* const rTips[kRenders] = {
        "Default. The Picture tab's resolution profile for handheld / docked, with dynamic resolution as set there\n"
        "(handheld with Output 1440p / 4K: that size, see Output).",
        "The game renders at the chosen output's size: 1920x1080 handheld (Output Auto / 1080p), 3840x2160 at 4K.\n"
        "Dynamic resolution off. Expensive: watch the frame rate (the size line below).",
        "3840x2160 in both modes, whatever the output: handheld it is downscaled to the screen's 1920x1080\n"
        "(supersampling, a 2x2 box per pixel). Dynamic resolution off. Very expensive: watch the frame rate.",
        "3200x1800 in both modes, downscaled to the output (handheld 1920x1080). Dynamic resolution off.",
        "2560x1440 in both modes, dynamic resolution off.",
        "1920x1080 in both modes, dynamic resolution off.",
        "1280x720 (the Wii U's own) in both modes, dynamic resolution off."};
    const int r = render();
    for (int i = 0; i < kRenders; i++) {
        if (i && i != 4) ImGui::SameLine();  // (two rows)
        char id[32];
        snprintf(id, sizeof id, "%s##s2render%d", rNames[i], i);
        if (ImGui::RadioButton(id, r == i) && r != i) {
            g_render = i;
            save("s2Render", kRenderIds[i]);
            log_choice("menu");
            hostui::post([] { switch_settings::reapply_res_profile("render resolution changed"); });
        }
        help(rTips[i]);
    }
    bool v = show_size();
    if (ImGui::Checkbox("Show render size and fps in the corner##s2size", &v)) {
        g_showSize = v;
        save("s2ShowSize", v ? "1" : "0");
    }
    help("The FPS counter's corner also shows the render size now (it follows dynamic resolution) and the output\n"
         "size, e.g. RES 3840X2160 > 1920X1080 (4K downscaled to the handheld screen). Shown even with the counter\n"
         "off. Saved (s2ShowSize).");
    const bool docked = switch_settings::active_mode() == switch_settings::kDocked;
    uint32_t w, h;
    output_size(docked, w, h);
    const float s = gfxsw::dynamic_res_scale();
    note("Now: render %ux%u, output %ux%u (%s). All of these apply at once and are saved (s2Output, s2Render).",
         unsigned(1280 * s + 0.5f), unsigned(720 * s + 0.5f), w, h, docked ? "docked" : "handheld");
    if (!docked && (output() == kOut1440 || output() == kOut2160)) note("Handheld output: %s.", handheld_path());
}
}  // namespace s2tab
