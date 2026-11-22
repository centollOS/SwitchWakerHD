// (this fork, s2/vrr) VRR, present when ready (s2_vrr_switch2.h), and its option in the Switch 2 tab
#include "s2_vrr_switch2.h"

#include <switch.h>

#include <atomic>
#include <string>

#include "imgui.h"
#include "s2_tab_switch2.h"
#include "../runtime.h"

namespace s2vrr {
constexpr const char* kKey = "s2Vrr";  // "1" on, "0" / absent off (the default)
std::atomic<bool> g_on{false};
std::atomic<int> g_hz{0};          // the display's rate (0: not read)
std::atomic<int> g_active{-1};     // VRR active (-1: not read)

bool on() { return g_on.load(std::memory_order_relaxed); }
void set_on(bool v) {
    if (g_on.exchange(v) == v) return;
    LOG("[s2] VRR (present when ready): %s (menu; from the next present)", v ? "on" : "off");
    s2tab::save(kKey, v ? "1" : "0");
}

void poll() {
    static uint64_t last = 0;
    const uint64_t now = armTicksToNs(armGetSystemTick());
    if (last && now - last < 5'000'000'000ull) return;
    last = now;
    Vi2Display* d = nullptr;
    u32 hz = 0;
    bool vrr = false;
    if (R_FAILED(nwindowGetVi2Display(nwindowGetDefault(), &d)) || !d) return;
    const Result rc = vi2QueryDisplayRefreshRate(d, &hz, &vrr);
    const int h = R_SUCCEEDED(rc) ? int(hz) : 0, a = vrr ? 1 : 0;
    const bool changed = g_hz.exchange(h) != h;
    if (g_active.exchange(a) != a || changed)
        LOG("[s2] VRR mode: display now %d Hz, VRR %s (rc 0x%x)", h, a ? "active" : "not active", rc);
}
}  // namespace s2vrr

namespace s2tab {
void start_vrr() {
    std::string v;
    const bool on = saved("s2Vrr", v) && v == "1";
    s2vrr::g_on.store(on, std::memory_order_relaxed);
    LOG("[s2] VRR (present when ready): %s (settings.ini s2Vrr)", on ? "on" : "off, the game's swap interval");
}
void section_vrr() {
    heading("Variable refresh rate (VRR)");
    bool v = s2vrr::on();
    if (ImGui::Checkbox("Present when ready (VRR)##s2vrr", &v)) s2vrr::set_on(v);
    help("Off (default): frames are shown on the game's own 1/30 s rhythm (swap interval 2).\n"
         "On: each frame is shown as soon as it is finished; with VRR (the handheld screen) the display waits for it,\n"
         "so a late frame costs a few ms instead of a whole 1/30 s. Asks the display for 120 Hz. On a display\n"
         "without VRR this is the same as swap interval 1 (frames that finish early are shown early).\n"
         "Applies at once; saved (s2Vrr). Frame interpolation at 120 / 240 fps keeps its own mode.");
    const int hz = s2vrr::g_hz.load(std::memory_order_relaxed), a = s2vrr::g_active.load(std::memory_order_relaxed);
    if (v && hz) note("Display now %d Hz, VRR %s.", hz, a == 1 ? "active" : "not active (this display or mode has none)");
    else if (v) note("Display state: read within a few seconds.");
}
}  // namespace s2tab
