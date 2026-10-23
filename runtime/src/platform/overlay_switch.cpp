// SwitchWakerHD (this fork): the settings overlay's Switch parts, registered through overlay::ext
// (overlay_switch.h).
#include "overlay_switch.h"

#include <switch.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <atomic>
#include <mutex>

#include "imgui.h"
#include "guest_addr.h"
#include "../gfx/switch_renderer.h"
#include "../build_info.h"
#include "../input.h"
#include "../interp.h"
#include "../mods/mods.h"
#include "../mods/warps.h"
#include "../motion/motion.h"
#include "../hle/olv_park.h"
#include "../input_map.h"
#include "../overlay/extensions.h"
#include "../overlay/release_gate.h"
#include "../overlay/hostui.h"
#include "../overlay/overlay.h"
#include "../render_prof.h"
#include "../report_header.h"
#include "../rumble.h"
#include "../runtime.h"
#include "../screen_mode.h"
#include "background_shaders_switch.h"
#include "debug_switch.h"
#include "prepare_graphics_switch.h"
#include "settings_switch.h"
#include "shader_wait_switch.h"

namespace overlay_switch {
namespace {
using overlay::set_open;

// the overlay's widgets (overlay.cpp keeps its own in an unnamed namespace): the same look
double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void help(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", text);
}
void heading(const char* text) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.95f, 0.85f, 1.0f));
    ImGui::SeparatorText(text);
    ImGui::PopStyleColor();
}
void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.78f, 0.84f, 1.0f));
    ImGui::TextWrappedV(fmt, ap);
    ImGui::PopStyleColor();
    va_end(ap);
}
void warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.80f, 0.35f, 1.0f));
    ImGui::TextWrappedV(fmt, ap);
    ImGui::PopStyleColor();
    va_end(ap);
}
bool radio(const char* label, bool active, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::RadioButton(label, active) && !active;
    ImGui::EndDisabled();
    return clicked;
}
// a radio button whose own tooltip says what that choice does
bool radio_tip(const char* label, bool active, const char* tip, bool enabled = true) {
    const bool clicked = radio(label, active, enabled);
    help(tip);
    return clicked;
}
bool check(const char* label, bool value, bool* out, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    bool v = value;
    bool changed = ImGui::Checkbox(label, &v);
    ImGui::EndDisabled();
    *out = v;
    return changed;
}
// the gyro settings, saved as the Controls tab saves them (overlay.cpp save_gyro)
void save_gyro(const motion::Settings& g) {
    hostui::post([g] {
        motion::set_settings(g);
        for (const char* k : motion::kKeys) hostui::set(k, motion::value_of(g, k));
    });
}
// "Show developer options" (Picture tab): the Dev tab appears; saved as switchDevOptions, off by
// default. Only the menu: a debug server that is running keeps running either way.
std::atomic<int> g_dev{-1};  // -1: not read yet
}  // namespace

bool dev_options() {
    int v = g_dev.load(std::memory_order_relaxed);
    if (v < 0) {
        std::string s;
        v = hostui::get(switch_settings::kKeyDevOptions, s) && s == "1" ? 1 : 0;
        g_dev.store(v, std::memory_order_relaxed);
    }
    return v == 1;
}
void set_dev_options(bool on) {
    g_dev.store(on ? 1 : 0, std::memory_order_relaxed);
    hostui::post([on] { hostui::set(switch_settings::kKeyDevOptions, on ? "1" : "0"); });
}
namespace {
}  // namespace

// Warp (mods/warps.h): the game's own scene change to the chosen stage, room and spawn point. The list stays
// hidden until the player ticks the box (not saved: it is off again at every start); the debug server's
// `warp` command does not need it.
void tab_warp() {
    static bool unlocked = false;
    warn("Warning: warping can break the story. Going somewhere before the game gets there can leave events in "
         "that place stuck or out of order, and the game may save it. Try it on a copy of your save.");
    bool v;
    if (check("I understand, show the warp destinations", unlocked, &v)) unlocked = v;
    if (!unlocked) return;
    const char* cur = reinterpret_cast<const char*>(mem::ptr(GD(0x1046F0B0) + 0x5134));  // the start stage
    note("Now in: %.8s%s", cur, mods::warp_pending() ? "   (a warp is waiting for a loaded file or the current scene change)" : "");
    auto go = [](const mods::Warp& w) {
        mods::request_warp(w.stage, w.room, w.point);
        set_open(false);  // the game takes over: fade, load, spawn
    };
    heading("Main places");
    for (const mods::Warp& w : mods::kMainWarps) {
        char label[96];
        snprintf(label, sizeof label, "%s##main%s%d", w.label, w.stage, w.room);
        if (ImGui::Selectable(label)) go(w);
    }
    heading("Every stage");
    for (const mods::Warp& w : mods::kAllWarps) {
        char label[64];
        snprintf(label, sizeof label, "%s  (room %d, point %d)##all", w.stage, w.room, w.point);
        if (ImGui::Selectable(label)) go(w);
    }
}

// ---- tabs (registered in register_ext, in this order): Picture, Controls, then Warp and
// Developer (hidden until "Show developer options" is ticked)

// Picture: resolution per mode, colours and effects
void tab_picture() {
    using namespace switch_settings;
    // first, as the most useful thing here: it removes the black or late-appearing effects of places seen the first time
    note("Recommended on the Switch: prepare the graphics once and new places stop showing black spots or objects that "
         "appear late.");
    if (prepare_graphics::ui_section()) set_open(false);  // platform/prepare_graphics_ui_switch.cpp
    // one picture profile per mode (as SwitchWaker): the one shown starts as the active mode's
    heading("Resolution");
    static int edit = -1;
    const int active = active_mode();
    if (edit < 0) edit = active;
    for (int m = 0; m < kModes; m++) {
        if (m) ImGui::SameLine();
        std::string label = mode_label(m);
        if (m == active) label += "  (active)";
        if (radio(label.c_str(), edit == m)) edit = m;
    }
    help("Each mode keeps its own internal resolution and dynamic resolution;\n"
         "they apply by themselves when the console is docked or undocked.");
    const ResProfile rp = res_profile(edit);
    static const float scales[] = {1.0f, 1.25f, 1.5f};  // (a saved 0.75 still applies; the menu no longer offers it)
    static const char* const names[] = {"1x  (1280x720)", "1.25x  (1600x900)", "1.5x  (1920x1080)"};
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        const float s = scales[i];
        if (radio(names[i], std::fabs(rp.scale - s) < 0.01f))
            hostui::post([m = edit, rp, s] { set_res_profile(m, {s, rp.dynamic}); });
    }
    bool dyn;
    if (check("Dynamic resolution", rp.dynamic, &dyn))
        hostui::post([m = edit, rp, dyn] { set_res_profile(m, {rp.scale, dyn}); });
    help("Lowers the internal resolution while the GPU cannot hold 30 fps, and raises it again when it can.");
    if (edit != active) note("Applies when the console is %s.", edit == kDocked ? "docked" : "in handheld mode");
    else note("Internal resolution now: %.2f.", gfxsw::dynamic_res_scale());

    heading("Colours and effects");
    gfxsw::PictureGrade g = gfxsw::picture_grade_now();
    bool changed = false, done = false;
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    changed |= ImGui::SliderFloat("Exposure", &g.exposure, 0.5f, 1.5f, "%.2f");
    done |= ImGui::IsItemDeactivatedAfterEdit();
    help("Below 1 tames bright areas (sky, sea)");
    changed |= ImGui::SliderFloat("Contrast", &g.contrast, 0.5f, 2.0f, "%.2f");
    done |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::SliderFloat("Saturation", &g.saturation, 0.0f, 2.0f, "%.2f");
    done |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::SliderFloat("Gamma", &g.gamma, 0.5f, 2.0f, "%.2f");
    done |= ImGui::IsItemDeactivatedAfterEdit();
    help("Above 1 deepens mid-tones and shadows, below 1 lifts them");
    ImGui::PopItemWidth();
    if (changed) gfxsw::set_picture_grade(g);
    if (ImGui::Button("Original colours")) {
        gfxsw::set_picture_grade(gfxsw::PictureGrade{});
        done = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Vivid")) {
        gfxsw::set_picture_grade(gfxsw::PictureGrade{0.85f, 1.3f, 1.1f, 1.0f});
        done = true;
    }
    if (done) hostui::post([] { save_picture(); });
    {
        // texture sharpness at an angle: 1x (off, the default) to 16x; saved as switchAnisoLevel
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Anisotropic filtering");
        const int cur = gfxsw::aniso_level();
        for (int n : {1, 2, 4, 8, 16}) {
            ImGui::SameLine();
            char label[16];
            snprintf(label, sizeof label, "%dx##aniso", n);
            if (radio_tip(label, cur == n,
                          n == 1 ? "Off, as the game draws it. Default."
                                 : "Sharpens textures seen at an angle (the ground ahead, walls along a path) instead of letting them\n"
                                   "blur. It costs GPU time, which can lower the frame rate in the busiest views (Outset's tall\n"
                                   "grass, the lookout): measured about 5% at 2x, 15% at 4x and 30% at 16x there; the village\n"
                                   "and the sea are not affected. Default: 1x (off)."))
                {
                    gfxsw::set_aniso_level(n);
                    hostui::post([n] { hostui::set(kKeyAnisoLevel, aniso_level_setting(n)); });
                }
        }
    }
    {
        bool v;
        if (check("Edge smoothing (FXAA)", gfxsw::fxaa(), &v)) {
            gfxsw::set_fxaa(v);
            hostui::post([v] { hostui::set(kKeyFxaa, v ? "1" : "0"); });
        }
        help("Smoother edges in the 3D world; the HUD stays sharp.\n"
             "Costs a little GPU time (about 2 ms a frame).");
    }
    {
        bool v;
        if (check("Bloom", gfxsw::bloom(), &v)) {
            gfxsw::set_bloom(v);
            hostui::post([v] { hostui::set(kKeyBloom, v ? "1" : "0"); });
        }
        help("The glow around the sky and bright places, as on the Wii U.\nCosts a little GPU time.");
    }
    {
        bool v;
        if (check("Distance blur", gfxsw::distance_blur(), &v)) {
            gfxsw::set_distance_blur(v);
            hostui::post([v] { hostui::set(kKeyDistanceBlur, v ? "1" : "0"); });
        }
        help("Far places softly out of focus, as on the Wii U.\nCosts a little GPU time.");
    }
    {
        // the shadow maps' size (gfxsw::shadow_scale); saved as switchShadowScale ("0.5" the default, "1" or "2")
        const float cur = gfxsw::shadow_scale();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Shadow maps");
        struct Q { float f; const char* label; const char* tip; };
        static const Q quals[] = {
            {0.5f, "Fast (default)", "Draws the shadow maps at half the size. Looks the same in handheld play, and gives about 5-7% more\n"
                                     "frames per second in the busiest views (Outset's tall grass, the lookout)."},
            {1.0f, "Original", "The game's own shadow map size: sharper shadow edges, a little slower in the busiest views."},
            {2.0f, "High", "Draws the shadow maps at twice the size: sharper shadows, but about 17-20% slower in the busiest views\n(Outset's tall grass, the lookout) and more memory."}};
        for (const Q& q : quals) {
            if (q.f > 1.0f && !dev_options() && cur <= 1.0f) continue;  // High: a test, with developer options
            ImGui::SameLine();
            char label[24];
            snprintf(label, sizeof label, "%s##shadowq", q.label);
            if (radio_tip(label, std::fabs(cur - q.f) < 0.01f, q.tip)) {
                gfxsw::set_shadow_scale(q.f);
                hostui::post([f = q.f] { hostui::set(kKeyShadowScale, f < 1 ? "0.5" : f > 1 ? "2" : "1"); });
            }
        }
    }
    heading("Frame rate counter");
    static const char* const modes[] = {"Off", "Frame rate", "Frame rate and load"};
    const int fm = gfxsw::fps_overlay_mode();
    const int shown = dev_options() || fm == 2 ? 3 : 2;  // "and load" is a diagnostic: with developer options
    for (int i = 0; i < shown; i++) {
        if (i) ImGui::SameLine();
        static const char* const tips[] = {"No counter.", "Frames per second, top left.",
                                           "Frames per second plus the render thread's load, draws and GPU lag."};
        if (radio_tip(modes[i], fm == i, tips[i])) {
            gfxsw::set_fps_overlay_mode(i);
            hostui::post([i] { hostui::set(kKeyFpsCounter, std::to_string(i)); });
        }
    }
    help("Shows the frames per second in the top left corner of the picture.");
    heading("Experimental features");
    warn("Unfinished: 60 fps needs a big overclock (sys-clk or similar) to hold. Not saved.");
    {
        // the upstream 60 fps modes (the screen is 60 Hz: 60 fps only)
        const int m = interp::mode();
        auto choose = [](int mode) {
            hostui::post([mode] {
                if (mode == 1) interp::set_fps(60);
                interp::set_mode(mode);
                LOG("[interp] frame rate mode from the Switch tab: %s (not saved)", interp::mode_name());
            });
        };
        if (radio_tip("30 fps (original)", m == 0, "The game as it was: 30 logic steps and 30 drawn frames a second.")) choose(0);
        ImGui::SameLine();
        if (radio_tip("60 fps interpolation", m == 1, "The game logic keeps 30 steps a second and a blended frame is drawn between them:\ndoubles the drawing work, so it also needs a big overclock to hold 60 fps.")) choose(1);
        ImGui::SameLine();
        if (radio_tip("True 60 (experimental)", m == 2, "Runs the game logic at 60 steps a second. Needs a big CPU and GPU overclock (sys-clk) to hold 60 fps.")) choose(2);
        help("Frame rate modes the game was not made for (the screen is 60 Hz). Not saved: 30 fps at the next start.");
        if (m == 1) {
            bool paced;
            if (check("Keep game speed", interp::paced_interpolation(), &paced, !getenv("WWHD_INTERP_PACED")))
                hostui::post([paced] {
                    interp::set_paced_interpolation(paced);
                    LOG("[interp] keep game speed from the Switch tab: %s (not saved)", paced ? "on" : "off");
                });
            help("When the console cannot draw 60 frames a second, skip in-between frames instead of slowing\n"
                 "the whole game down: the game logic keeps its 30 steps a second. Off: every in-between frame\n"
                 "is drawn and the game runs slower than real time. Not saved: off at the next start.");
            if (float share = interp::paced_drawn_share(); share >= 0)
                note("In-between frames drawn lately: %.0f%%", share * 100.0f);
        }
    }
}

// Controls: rumble and gyro aiming
void tab_controller() {
    // the controller's motors follow the game's rumble requests (input_switch.cpp update_rumble; saved as
    // "rumble" as on the desktop)
    heading("Rumble");
    {
        bool v;
        if (check("Rumble", rumble::enabled(), &v)) {
            rumble::set_enabled(v);
            hostui::post([v] { hostui::set("rumble", v ? "1" : "0"); });
        }
        help("The Joy-Con or the Pro Controller vibrates when the game asks for it, as the Wii U\n"
             "controllers did. Off keeps the motors still.");
    }

    // gyro aiming with the controller's own motion sensors (input_switch.cpp feeds motion.h); the
    // Cemuhook and mouse sources of the desktop Controls tab make no sense on the console
    heading("Gyro aiming");
    {
        motion::Settings g = motion::settings(), before = g;
        bool v;
        if (check("Aim with the controller's gyro", g.source == motion::kController, &v))
            g.source = v ? motion::kController : motion::kOff;
        help("First-person aiming (bow, hookshot, boomerang, telescope, Picto Box, grappling hook) follows\n"
             "the Joy-Con, the Pro Controller or the console in handheld mode, as with the Wii U GamePad.\n"
             "The game's own Options > Gyro switch still applies, and the game ignores the motion while\n"
             "the right stick is pushed.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Turn left/right by");
        for (int a = 0; a < motion::kAxisModeCount; a++) {
            ImGui::SameLine();
            if (radio(motion::axis_label(a), g.tuning.axis == a)) g.tuning.axis = a;
        }
        help("Player space: turning the controller left or right about the real vertical, however you hold it\n"
             "(recommended). Yaw: turning it about its own vertical axis (as if it lay flat). Roll: tilting it to\n"
             "the side like a steering wheel. Tilting its top up or down always looks up or down.");
        const float lo = motion::Tuning::kMinSensitivity, hi = motion::Tuning::kMaxSensitivity;
        ImGui::SetNextItemWidth(220);
        ImGui::SliderFloat("Sensitivity left/right", &g.tuning.sensitivity_x, lo, hi, "%.2fx", ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine(0, 16);
        if (check("Invert##gyrox", g.tuning.invert_x, &v)) g.tuning.invert_x = v;
        ImGui::SetNextItemWidth(220);
        ImGui::SliderFloat("Sensitivity up/down", &g.tuning.sensitivity_y, lo, hi, "%.2fx", ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine(0, 16);
        if (check("Invert##gyroy", g.tuning.invert_y, &v)) g.tuning.invert_y = v;
        if (ImGui::Button("Default sensitivity"))
            g.tuning.sensitivity_x = g.tuning.sensitivity_y = motion::Tuning::kDefaultSensitivity;
        ImGui::SameLine();
        if (ImGui::Button("Recalibrate now")) motion::recalibrate();
        help("Recalibrate if the view drifts while the controller rests, then put it down for a second:\n"
             "the gyro's offset is learnt anew.");
        ImGui::TextUnformatted(motion::status().c_str());
        if (!(g == before)) save_gyro(g);
    }
    heading("Menu");
    note("Minus held half a second opens this menu (a short Minus goes to the game); B or Minus closes it.\n"
         "L / R change tabs.");
}

// Developer: tests and diagnostics, none of it for normal play
void tab_developer() {
    using namespace switch_settings;
    warn("Tests and diagnostics for developers. Nothing here is needed to play.");
    bool dev;
    if (check("Show developer options", dev_options(), &dev)) set_dev_options(dev);
    help("Shows the tests and diagnostics below (debug server, A/B switches, captures). Off by default.\n"
         "A debug server that is already running keeps running whether this is ticked or not.");
    if (!dev_options()) return;
    heading("Debug server");
    {
        // the saved choice (read at start, debug_switch.cpp), not whether the server runs now
        static int saved = -1;
        if (saved < 0) {
            std::string v;
            saved = hostui::get(kKeyDebugServer, v) && v == "1" ? 1 : 0;
        }
        bool v;
        if (check("Debug server (network)", saved == 1, &v)) {
            saved = v ? 1 : 0;
            hostui::post([v] { hostui::set(kKeyDebugServer, v ? "1" : "0"); });
        }
        help("Development only. From the next start, the game listens on port 6543 of the local network\n"
             "for tools/switch/wwhd_debug.py (logs, files, screenshots, warps, restart), with NO password:\n"
             "anyone on the same network can use it. Leave it off otherwise. See docs/debug-server.md.");
        if (debug_switch::running()) note("Running now on port %d.", debug_switch::port());
        if ((saved == 1) != debug_switch::running()) note("Applies at the next start.");
    }
    heading("Draw optimizations (A/B test)");
    for (int i = 0; i < gfxsw::kDrawOpts; i++) {
        static const char* const names[] = {"Skip unchanged fixed state", "Shadow draws without the pixel shader",
                                            "Cache textures of shared surfaces", "Keep vertex layouts",
                                            "Submit every 1024 draws", "Render profiler (slower)",
                                            "Prefetch shader data", "Skip unchanged target/state registers",
                                            "Skip the TV picture (Off-TV Play)", "Target memo by state words",
                                            "Fixed state in three parts", "Vertex states by layout"};
        static_assert(sizeof names / sizeof names[0] == gfxsw::kDrawOpts, "a name per switch");
        static const char* const what[] = {
            "Skips setting the GPU's fixed state (blend, depth, culling) again when a draw's is the same as the last.",
            "Draws that only write depth (shadow maps) run without a pixel shader.",
            "Texture lookups at addresses shared by several surfaces are remembered instead of searched again.",
            "Each vertex shader keeps its vertex layout instead of building it for every draw.",
            "Hands the GPU a batch of draws every 1024 draws instead of every 256: fewer submits, more latency.",
            "The render thread's profiler: counts commands, draws and register changes and logs a [prof] line.\n"
            "Slower while on; its report is what Save performance report writes.",
            "Reads ahead the shader data a draw is about to use, so the memory is already in the cache.",
            "Render targets, fixed state and viewport are looked up again only when their registers changed.",
            "In Off-TV Play the TV picture nobody sees is not drawn (about 1.8 ms of GPU a frame docked).",
            "Remembers the render target of a draw by the words of its state and skips repeats in the same frame.",
            "Keeps the fixed state in three separate remembered parts instead of one.",
            "Remembers vertex states by the identity of their layout.",
        };
        static_assert(sizeof what / sizeof what[0] == gfxsw::kDrawOpts, "a text per switch");
        bool on;
        if (i % 2) ImGui::SameLine(0, 30);
        if (check(names[i], gfxsw::draw_opt(i), &on)) gfxsw::set_draw_opt(i, on);
        help(what[i]);
    }
    help("Turn them off and on again in the same spot: the log marks each change with its frame.\n"
         "Not saved: they are on again at the next start.");
    heading("Shaders");
    {
        const int cur = gfxsw::shader_compile_threads();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Shader compile threads");
        for (int n = 0; n <= 3; n++) {
            ImGui::SameLine();
            char label[16];
            snprintf(label, sizeof label, n ? "%d##shthr" : "Auto##shthr", n);
            static const char* const tips[] = {
                "Automatic (default): three threads while the picture waits for its shaders (the option below, the\n"
                "start-up), one otherwise.",
                "One thread compiles the shaders the game is waiting for (how it was before the test).",
                "Two threads, always. In play they take about 3 fps while they run.",
                "Three threads, always. In play they take about 3 fps while they run."};
            if (radio_tip(label, cur == n, tips[n])) gfxsw::set_shader_compile_threads(n);
        }
        help("How many threads compile the shaders the game is waiting for (a new place, an item seen for the\n"
             "first time). Not saved.");
    }
    {
        // (#49) a model seen for the first time: wait for its shaders instead of drawing it piece by piece
        const int cur = shader_wait::mode();
        ImGui::TextUnformatted("Draws of a shader still compiling");
        for (int m = 0; m < shader_wait::kModes; m++) {
            if (m) ImGui::SameLine();
            static const char* const tips[] = {
                "Draws of a shader that is still compiling are skipped (as before): the item or clothes appear piece\n"
                "by piece while the shaders compile.",
                "The picture waits for the shaders (a short freeze, only the first time) while an item is shown: Link\n"
                "holding up an item, opening a chest.",
                "As the previous choice, and also in every cutscene, talk and door.",
                "Always waits for the shaders, everywhere (new places freeze for a moment)."};
            if (radio_tip(shader_wait::label(m), cur == m, tips[m])) {
                shader_wait::set_mode(m);
                hostui::post([m] { hostui::set(kKeyShaderWait, std::to_string(m)); });
            }
        }
        help("Issue #49: what a draw does while its shader compiles. Saved. The log's [shader-wait] lines say how\n"
             "long it waited.");
    }
    heading("Performance tests");
    {
        // not saved: off at the next start (debug server: noao, miiverse; shadowscale is in the Speed tab)
        bool skip;
        if (check("Skip ambient occlusion", gfxsw::skip_ao(), &skip)) gfxsw::set_skip_ao(skip);
        help("Test: the ambient-occlusion pass is not drawn and the game sees no occlusion (the soft dark corners\n"
             "and creases); the real shadows stay.\n"
             "Measured: about +1% fps in the busiest views, nothing elsewhere. Not saved: off at the next start.");
        bool stop;
        if (check("Stop the Miiverse thread", hle_olv::parked(), &stop)) hle_olv::set_parked(stop);
        help("Test: the game's Miiverse thread (Tingle Bottle and Miiverse requests, which always fail here) stops\n"
             "instead of running a step every millisecond. Measured: no change in fps or\n"
             "main-thread time in the four Outset views. Not saved: off at the next start.");
    }
    heading("Controller and screen (test)");
    // (Debug: the GamePad view is not offered to players) which Wii U controller the Switch controller is: the game's own control mode must match it (with
    // the Pro Controller chosen here the GamePad lies on the table and sends nothing, system_stubs.cpp)
    {
        // saved as controllerMode. The single screen is fixed at start (switch_settings::apply_at_start): the game's
        // screen mode, its controller picker and its options follow it from the title on (screen_mode.cpp), not in
        // the middle of play. Between the Wii U's own GamePad and Pro Controller the choice applies at once.
        using switch_settings::ControllerMode;
        const bool single = screen_mode::single_screen(), pro = input::pro_controller();
        static int saved = -1;  // the saved choice (ControllerMode)
        if (saved < 0) saved = int(single ? ControllerMode::kSingle : pro ? ControllerMode::kPro : ControllerMode::kGamePad);
        auto choose = [&](ControllerMode m, const char* value) {
            saved = int(m);
            hostui::post([single, m, value] {
                if (!single && m != ControllerMode::kSingle) hostui::set_pro_controller(m == ControllerMode::kPro);
                hostui::set(switch_settings::kKeyControllerMode, value);
            });
        };
        ImGui::TextUnformatted("The controller acts as");
        if (radio_tip("GamePad, single screen", saved == int(ControllerMode::kSingle), "Plays in Off-TV Play from the title screen on: no controller choice, Minus is the item menu.\nTakes effect at the next start.")) choose(ControllerMode::kSingle, "single");
        ImGui::SameLine();
        if (radio_tip("Wii U GamePad", saved == int(ControllerMode::kGamePad), "The game as on the Wii U with the GamePad (TV play, its controller choice). Applies at once\nfrom the Pro Controller choice.")) choose(ControllerMode::kGamePad, "gamepad");
        ImGui::SameLine();
        if (radio_tip("Wii U Pro Controller", saved == int(ControllerMode::kPro), "The game as on the Wii U with the Pro Controller. The GamePad lies on the table and sends nothing.")) choose(ControllerMode::kPro, "pro");
        help("For testing (not offered to players). GamePad, single screen: play in Off-TV Play from the\n"
             "title screen on, no controller choice, Minus the item menu. Wii U GamePad and Pro Controller:\n"
             "the game as on the Wii U (TV play, its controller choice); between those two the choice applies\n"
             "at once, to or from the single screen from the next start.");
        if ((saved == int(ControllerMode::kSingle)) != single) note("Applies at the next start.");
        // the GamePad screen (items, map), full size instead of the TV picture
        const bool view = gfxsw::gamepad_view();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("The screen shows");
        ImGui::SameLine();
        if (radio_tip("TV picture", !view, "The window shows the TV picture. ZL + ZR + Minus switches between the two while playing.")) gfxsw::set_gamepad_view(false);
        ImGui::SameLine();
        if (radio_tip("GamePad screen", view, "The window shows the GamePad screen (items, map) full size; the touch screen works as the GamePad's.")) gfxsw::set_gamepad_view(true);
        help("ZL + ZR + Minus switches between them while playing. On the GamePad screen the touch\n"
             "screen works as the GamePad's (items, map). With the Pro Controller chosen above, the GamePad\n"
             "picture is drawn only while it is shown (test: what the game puts there in that mode).");
        bool pip = false;
        if (check("GamePad screen in a corner of the TV picture", gfxsw::gamepad_pip(), &pip, !pro))
            hostui::post([pip] { switch_settings::set_gamepad_pip(pip); });
        help("While the TV picture is shown, the GamePad's (items, map) also appears small in the\n"
             "bottom right corner, and the touch screen works inside it. Only with the GamePad chosen above.");
    }
    heading("Diagnostics");
    // the clocks, asked at most once a second (clkrst)
    static double clocks_t = -10;
    static std::string clocks;
    if (now_s() - clocks_t >= 1.0) {
        clocks_t = now_s();
        clocks = gfxsw::clock_report_now();
    }
    if (!clocks.empty()) note("Now: %s (%s).", clocks.c_str(), gpu_profile_status().c_str());
    {
        bool v;
        if (check("Log the main thread's runtime calls", threads::main_sampler(), &v)) {
            threads::set_main_sampler(v);
            hostui::post([v] { hostui::set(kKeyMainSampler, v ? "1" : "0"); });
        }
        help("Every 5 s the log says where the game's main thread spends its time in the runtime ([main] lines).\n"
             "Saved: it also starts with the game when on.");
        ImGui::SameLine(0, 30);
        if (check("Log the game threads' CPU use", threads::sched_stats(), &v)) threads::set_sched_stats(v);
        help("Every 5 s the log says each game thread's share of its core ([sched] lines).\n"
             "Not saved: off at the next start.");
    }
    {
        bool v;
        if (check("Capture a frame with both sticks clicked", switch_settings::capture_combo(), &v))
            hostui::post([v] { switch_settings::set_capture_combo(v); });
        help("Writes the next frame's pictures to sdmc:/switch/wwhd/captures for a picture that\n"
             "goes wrong; the game freezes for a few seconds meanwhile. Off: no accidental captures.");
    }
    {
        // the render profiler's latest report (render_prof.h; on with "Render profiler" above) to the SD card,
        // with the build and settings header the desktop's "Copy performance report" puts on the clipboard
        static std::string saved;
        static double savedAt = -10;
        if (ImGui::Button("Save performance report")) {
            const std::string report = rprof::latest_report();
            reporthdr::Info h;
            h.version = build::version();
            h.commit = build::commit();
            h.os = reporthdr::os_description();
            h.gpu = "Tegra X1";
            h.renderer = "deko3d";
            h.host = "Switch";
            h.fps = interp::mode() == 2 ? "true 60 fps" : interp::mode() == 1 ? "60 fps interpolation" : "30 fps";
            h.scale = hostui::res_scale();
            if (const int g = motion::settings().source; g != motion::kOff) h.gyro = motion::source_id(g);
            const std::string text = reporthdr::format(h) +
                (report.empty() ? std::string("No report: turn on \"Render profiler (slower)\" above, play a few "
                                              "seconds in the spot to measure, then save again.\n")
                                : report);
            const time_t now = time(nullptr);
            struct tm t;
            localtime_r(&now, &t);
            char path[96];
            snprintf(path, sizeof path, "logs/perf_%04d-%02d-%02d_%02d-%02d-%02d.txt", t.tm_year + 1900, t.tm_mon + 1,
                     t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
            if (FILE* f = fopen(path, "w")) {
                fwrite(text.data(), 1, text.size(), f);
                fclose(f);
                saved = std::string("sdmc:/switch/wwhd/") + path;
                LOG("[perf] performance report saved to %s%s", saved.c_str(), report.empty() ? " (no report yet)" : "");
            } else {
                saved = "could not write logs/";
            }
            savedAt = ImGui::GetTime();
        }
        if (ImGui::GetTime() - savedAt < 4.0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", saved.c_str());
        }
        help("Where the renderer spends its time over the last few seconds, as text for a bug report:\n"
             "turn on \"Render profiler (slower)\", play in the spot, then save. File in sdmc:/switch/wwhd/logs/.");
    }
}

bool load_font(ImGuiIO& io) {
    // the console's own font (shared font service), kept by the system: the atlas does not own it
    if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
        PlFontData font;
        if (R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) {
            ImFontConfig cfg;
            cfg.FontDataOwnedByAtlas = false;
            if (io.Fonts->AddFontFromMemoryTTF(font.address, int(font.size), 17.0f, &cfg)) {
                LOG("[overlay] font: the system's shared font");
                return true;
            }
        }
    }
    return false;
}

bool frame_layer_wanted(std::string& toast) {
    if (!prepare_graphics::screen_line().empty()) toast = prepare_graphics::screen_line();
    return prepare_graphics::running() || background_shaders::card().up;
}
void frame_layer_draw(float w, float h) {
    if (!background_shaders::draw_screen(w, h)) prepare_graphics::draw_screen(w, h);
}

void register_ext() {
    namespace ext = overlay::ext;
    // the Switch renderer has its own options (Switch tab); one screen, and the controller maps by position
    // Saves, Picture, Controls, Audio, Mods, Warp, Dev (its content only with "Show developer options"),
    // Language / About
    ext::add_tab({"picture", "Picture", tab_picture, nullptr}, "audio");
    ext::add_tab({"controller", "Controls", tab_controller, nullptr}, "audio");
    ext::add_tab({"warp", "Warp", tab_warp, nullptr});
    ext::add_tab({"developer", "Dev", tab_developer, nullptr});
    // Shortcuts: keyboard keys and desktop actions (full screen, screenshot); the controller column is unbound but for Home
    for (const char* id : {"graphics", "display", "controls", "shortcuts"}) ext::hide_tab(id);
    ext::Features f;
    f.clipboard = false;    // the paths are shown instead
    f.file_picker = false;
    f.screenshots = false;  // the console's own capture button
    f.full_states = false;  // portable states only (savestate.cpp)
    f.code_mods = false;       // the code mods built into the NRO instead (guest_builtin_ui.h)
    f.mod_downloads = false;   // no catalogue or Story Notebook download: mods from the mods folder
    f.speaker_choice = false;  // audout plays stereo
    f.keyboard = false;        // a controller only
    ext::set_features(f);
    ext::set_font_loader(load_font);
    // ZL + ZR held, Minus: the window switches between the TV and the GamePad picture (not the menu)
    ext::add_shortcut([](const overlay::PadValues& pad, bool menu_open) {
        static bool latched = false;
        const bool combo = pad[input_map::kPadLT] > 0.5f && pad[input_map::kPadRT] > 0.5f &&
                           pad[input_map::kPadOptions] > 0.5f;
        if (!combo || menu_open) {
            latched = latched && combo;
            return false;
        }
        if (!latched) gfxsw::set_gamepad_view(!gfxsw::gamepad_view());
        latched = true;
        return true;
    });
    // the system keyboard for the overlay's text fields (no physical keyboard)
    ext::set_text_input([](const text_entry::Request& r, text_entry::Done done) {
        input::prompt_text(r.initial, r.max_len, std::move(done));
    });
    // Prepare graphics and the first start's card cover the game while the menu is closed
    ext::add_frame_layer({frame_layer_wanted, [](float w, float h) {
                              frame_layer_draw(w, h);
                              return true;
                          }});
}
}  // namespace overlay_switch
