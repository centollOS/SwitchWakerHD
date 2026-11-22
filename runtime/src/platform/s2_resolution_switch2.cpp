// (this fork, s2/resolution) Switch 2 output and render resolution (s2_resolution_switch2.h) and their options in
// the settings overlay's Switch 2 tab
#include "s2_resolution_switch2.h"

#include <switch.h>

#include <atomic>
#include <cmath>
#include <string>

#include "imgui.h"
#include "s2_tab_switch2.h"
#include "settings_switch.h"
#include "../gfx/switch_renderer.h"
#include "../overlay/hostui.h"
#include "../runtime.h"

namespace s2res {
namespace {
const char* const kOutIds[kOutputs] = {"auto", "1080", "1440", "2160", "previous"};
const char* const kRenderIds[kRenders] = {"auto", "native", "1440", "1080", "720"};
std::atomic<int> g_output{kOutAuto}, g_render{kRenderAuto};
std::atomic<bool> g_showSize{false};
// the TV's resolution (appletGetDefaultDisplayResolution, read while docked; 0 not read yet)
std::atomic<uint32_t> g_tvW{0}, g_tvH{0};

int find(const char* const* ids, int n, const std::string& v, int def) {
    for (int i = 0; i < n; i++)
        if (v == ids[i]) return i;
    return def;
}
void log_choice(const char* why) {
    uint32_t hw, hh, dw, dh;
    output_size(false, hw, hh);
    output_size(true, dw, dh);
    float hs = 0, ds = 0;
    const bool fixed = render_scale(false, hs);
    render_scale(true, ds);
    char r[96];
    if (fixed)
        snprintf(r, sizeof r, "%ux%u handheld, %ux%u docked, dynamic resolution off", unsigned(1280 * hs + 0.5f),
                 unsigned(720 * hs + 0.5f), unsigned(1280 * ds + 0.5f), unsigned(720 * ds + 0.5f));
    else
        snprintf(r, sizeof r, "the Picture tab's profile (dynamic resolution as set there)");
    LOG("[s2] display mode (%s): output %s: %ux%u handheld, %ux%u docked (TV reports %ux%u); render %s: %s", why,
        kOutIds[output()], hw, hh, dw, dh, unsigned(g_tvW.load()), unsigned(g_tvH.load()), kRenderIds[render()], r);
}
}  // namespace

int output() { return g_output.load(std::memory_order_relaxed); }
int render() { return g_render.load(std::memory_order_relaxed); }
bool show_size() { return g_showSize.load(std::memory_order_relaxed); }

void output_size(bool docked, uint32_t& w, uint32_t& h) {
    const int o = output();
    if (o == kOutPrevious) {
        w = docked ? 1920 : 1280;
        h = docked ? 1080 : 720;
        return;
    }
    w = 1920;
    h = 1080;
    if (!docked) return;  // the handheld screen is 1920x1080
    switch (o) {
    case kOut1440: w = 2560, h = 1440; break;
    case kOut2160: w = 3840, h = 2160; break;
    case kOut1080: break;
    default: {
        const uint32_t tw = g_tvW.load(std::memory_order_relaxed), th = g_tvH.load(std::memory_order_relaxed);
        if (tw >= 1280 && th >= 720 && tw <= 3840 && th <= 2160) w = tw, h = th;  // else 1080p until it is read
    }
    }
}

bool render_scale(bool docked, float& scale) {
    switch (render()) {
    case kRenderNative: {
        uint32_t w, h;
        output_size(docked, w, h);
        scale = float(h) / 720.0f;
        return true;
    }
    case kRender1440: scale = 2.0f; return true;
    case kRender1080: scale = 1.5f; return true;
    case kRender720: scale = 1.0f; return true;
    default: return false;
    }
}

bool tick(bool docked) {
    if (!docked) return false;
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
    ImGui::TextUnformatted("Output (docked):");
    static const char* const outNames[kOutputs] = {"Auto", "1080p", "1440p", "4K", "Previous"};
    static const char* const outTips[kOutputs] = {
        "Default. Handheld: 1920x1080 (the screen). Docked: the TV's resolution as set in the console's settings\n"
        "(3840x2160 on a 4K TV). The game's picture is scaled up to it; the render resolution below decides its detail.",
        "Docked: 1920x1080. Handheld: 1920x1080.",
        "Docked: 2560x1440 (the console's scaler fits it to the TV). Handheld: 1920x1080.",
        "Docked: 3840x2160, also on a TV set to less (the console scales it down). Handheld: 1920x1080.",
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
    static const char* const rNames[kRenders] = {"Auto (dynamic)", "Native output", "1440p", "1080p", "720p"};
    static const char* const rTips[kRenders] = {
        "Default. The Picture tab's resolution profile for handheld / docked, with dynamic resolution as set there.",
        "The game renders at the output's size: 1920x1080 handheld, the output size docked (3840x2160 at 4K).\n"
        "Dynamic resolution off. Expensive: watch the frame rate (the size line below).",
        "2560x1440 in both modes, dynamic resolution off.",
        "1920x1080 in both modes, dynamic resolution off.",
        "1280x720 (the Wii U's own) in both modes, dynamic resolution off."};
    const int r = render();
    for (int i = 0; i < kRenders; i++) {
        if (i) ImGui::SameLine();
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
         "size, e.g. RES 1920X1080 > 3840X2160. Shown even with the counter off. Saved (s2ShowSize).");
    const bool docked = switch_settings::active_mode() == switch_settings::kDocked;
    uint32_t w, h;
    output_size(docked, w, h);
    const float s = gfxsw::dynamic_res_scale();
    note("Now: render %ux%u, output %ux%u (%s). All of these apply at once and are saved (s2Output, s2Render).",
         unsigned(1280 * s + 0.5f), unsigned(720 * s + 0.5f), w, h, docked ? "docked" : "handheld");
}
}  // namespace s2tab
