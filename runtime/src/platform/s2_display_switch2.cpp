// (this fork, s2/hdr-info) the Switch 2 display's capabilities (s2_display_switch2.h) and their read-only lines in
// the settings overlay's Switch 2 tab (s2_tab_switch2.h)
#include "s2_display_switch2.h"

#include <switch.h>

#include <mutex>
#include <string>

#include "imgui.h"
#include "s2_tab_switch2.h"
#include "../runtime.h"

namespace s2display {
namespace {
std::mutex g_mu;
Caps g_caps;
uint64_t g_lastNs = 0;
int g_lastDocked = -1;

std::string bits(uint64_t v) {
    std::string s;
    for (int b = 0; b < 64; b++)
        if (v >> b & 1) s += (s.empty() ? "bit " : ", bit ") + std::to_string(b);
    return s.empty() ? "none" : s;
}
std::string refresh_names(uint64_t v) {
    std::string s;
    if (v & 1) s += "60 Hz";
    if (v & 2) s += s.empty() ? "120 Hz" : ", 120 Hz";
    if (v >> 63 & 1) s += s.empty() ? "VRR" : ", VRR";
    if (v & ~(3ull | 1ull << 63)) s += (s.empty() ? "" : ", ") + std::string("other ") + bits(v & ~(3ull | 1ull << 63));
    return s.empty() ? "none" : s;
}
}  // namespace

void poll(uint64_t frame, bool docked) {
    if (frame < 3) return;  // the swapchain's first presents opened the window's VI2 layer and display
    const uint64_t now = armTicksToNs(armGetSystemTick());
    if (g_lastDocked == int(docked) && now - g_lastNs < 10'000'000'000ull) return;
    const bool modeChanged = g_lastDocked != int(docked);
    g_lastNs = now;
    g_lastDocked = int(docked);
    Vi2Display* d = nullptr;
    Result rc = nwindowGetVi2Display(nwindowGetDefault(), &d);
    if (R_FAILED(rc) || !d) {
        static bool logged = false;
        if (!logged) LOG("[s2] display: no VI2 display (nwindowGetVi2Display rc 0x%x); capabilities unknown", rc);
        logged = true;
        return;
    }
    Caps c;
    c.docked = docked;
    Vi2DisplayModeCapability mc = {};
    const Result rcCaps = vi2QueryDisplayModeCapability(d, &mc);
    if (R_SUCCEEDED(rcCaps)) {
        c.refresh = mc.refresh_rates;
        c.refreshSecondary = mc.refresh_rates_secondary;
        c.hdr = mc.hdr_output_formats;
    }
    u32 hz = 0;
    bool vrr = false;
    const Result rcHz = vi2QueryDisplayRefreshRate(d, &hz, &vrr);
    c.hz = R_SUCCEEDED(rcHz) ? hz : 0;
    c.vrrActive = vrr;
    u32 level = 0;
    c.maxResLevel = R_SUCCEEDED(vi2QueryDisplayBufferMaxResolution(d, &level)) ? int(level) : -1;
    c.valid = R_SUCCEEDED(rcCaps);
    Caps before;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        before = g_caps;
        g_caps = c;
    }
    if (!modeChanged && before.valid == c.valid && before.refresh == c.refresh && before.hdr == c.hdr &&
        before.refreshSecondary == c.refreshSecondary && before.hz == c.hz && before.vrrActive == c.vrrActive &&
        before.maxResLevel == c.maxResLevel)
        return;
    LOG("[s2] display (%s): refresh rates 0x%llx (%s), secondary 0x%llx (%s) (rc 0x%x); now %u Hz, VRR %s (rc 0x%x); "
        "largest buffer resolution level %d", docked ? "docked" : "handheld", (unsigned long long)c.refresh,
        refresh_names(c.refresh).c_str(), (unsigned long long)c.refreshSecondary, refresh_names(c.refreshSecondary).c_str(),
        rcCaps, c.hz, c.vrrActive ? "active" : "not active", rcHz, c.maxResLevel);
    LOG("[s2] display HDR formats: 0x%llx (%s)%s", (unsigned long long)c.hdr, bits(c.hdr).c_str(),
        c.hdr ? "; this build presents SDR RGBA8 only: HDR output needs FP16/HDR swapchains in libounce and drako3d"
              : "");
}

Caps caps() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_caps;
}
}  // namespace s2display

namespace s2tab {
void section_display() {
    heading("Display");
    const s2display::Caps c = s2display::caps();
    if (!c.valid) {
        note("The display has not answered yet (it is read a moment after the game starts).");
        return;
    }
    auto yes = [](bool b) { return b ? "yes" : "no"; };
    note("%s: now %u Hz, VRR %s. Can do: 60 Hz %s, 120 Hz %s, VRR %s.", c.docked ? "TV (docked)" : "Screen (handheld)",
         c.hz, c.vrrActive ? "active" : "not active", yes(c.refresh & 1), yes(c.refresh & 2), yes(c.refresh >> 63 & 1));
    help("What the display reports to the console (VI2). Read only, again on dock / undock and every 10 s.\n"
         "120 Hz: the Frame rate option (120 fps interpolation) uses it. VRR: see the VRR option below.");
    note("HDR formats reported: 0x%llx. HDR output: not available yet.", (unsigned long long)c.hdr);
    help("The display's HDR capability bits (their formats are not documented yet). The game is shown in SDR:\n"
         "HDR output needs 16-bit (FP16 / HDR10) swapchains, which libounce and drako3d do not have yet.\n"
         "Read only; nothing is saved.");
}
}  // namespace s2tab
