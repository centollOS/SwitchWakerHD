// What the Switch renderer (deko3d, gfx/deko) offers the code around it: the settings overlay's Switch
// tab (overlay/overlay.cpp), the saved options (platform/settings_switch.cpp) and the capture combo
// (platform/input_switch.cpp). gfx/deko defines these functions.
#pragma once
#include <string>

namespace gfxsw {

// picture adjustments applied when presenting (neutral = 1; the Switch tab, saved in settings.ini)
struct PictureGrade {
    float exposure = 1, contrast = 1, saturation = 1, gamma = 1;
};
PictureGrade picture_grade_now();
void set_picture_grade(const PictureGrade& g);
// the counter in the top-left corner: 0 off, 1 frame rate, 2 with render-thread load and draws (WWHD_FPS)
int fps_overlay_mode();
void set_fps_overlay_mode(int mode);
// 16x anisotropic filtering on the game's mipmapped linear samplers (WWHD_ANISO): any thread, from the next frame
bool aniso();
void set_aniso(bool on);
float dynamic_res_scale();  // the internal resolution dynamic resolution has chosen (1 when off)
// any thread: the internal resolution (the most dynamic resolution may use) and whether dynamic resolution
// may lower it, from the next frame on (the handheld / docked profiles, platform/settings_switch.h)
void set_resolution_profile(float scale, bool dynamic);
// per-draw optimizations the Switch tab turns on and off for A/B tests (not saved; settings.ini's [dev] section sets them at start):
// fixed state skipped while unchanged (WWHD_DK_FIXED_SKIP); round 40: depth-only draws without their pixel
// shader (WWHD_DK_DEPTH_ONLY), texture lookups at shared addresses cached (WWHD_DK_TEX_SHARED_CACHE), vertex
// layouts kept per vertex shader (WWHD_DK_VTX_LAYOUT_CACHE), a submit every 1024 draws instead of 256
// (WWHD_DK_SUBMIT_DRAWS; on by default since round 41), the upstream render-thread profiler (WWHD_PROFILE=1; off by
// default: it costs render-thread time); round 42: the draw's shader data prefetched (WWHD_DK_PREFETCH). (A/B
// 2026-10-07 and 2026-10-08: skipping unchanged uniform registers, as a whole and per vec4, gained nothing and was
// removed: the game changes its constants for nearly every draw.) Each change is logged with its frame, so one
// session's log compares both halves.
enum DrawOpt : int {
    kOptFixedSkip, kOptDepthOnly, kOptTexSharedCache, kOptVtxLayoutCache, kOptBigSubmits, kOptProfiler, kOptPrefetch,
    kOptRegGens,  // round 41: targets, fixed state and viewport skipped while their registers are unchanged
    kDrawOpts
};
bool draw_opt(int which);
void set_draw_opt(int which, bool on);
#ifdef __SWITCH__
std::string clock_report_now();  // "CPU x MHz, GPU y MHz, memory z MHz", or "" if unavailable
#endif
// the GamePad screen (the game's second screen: items, map, sea chart). Its picture is drawn while the
// controller acts as the Wii U GamePad; the window then shows the TV picture or, switched (ZL + ZR + Minus, or
// the Switch tab), the GamePad picture full size, with the console's touch screen as the GamePad's.
bool gamepad_picture_drawn();         // the controller acts as the GamePad (render thread, every frame)
bool gamepad_view();                  // the window shows the GamePad picture instead of the TV's
void set_gamepad_view(bool on);       // any thread, from the next frame
// the GamePad picture also in a corner of the TV picture (bottom right, a third of the window's width) while
// the TV picture is shown; the touch screen then works inside that corner (Switch tab, saved)
bool gamepad_pip();
void set_gamepad_pip(bool on);        // any thread, from the next frame
// touch at x, y (0..1 of the screen from the top left) -> the GamePad screen's 0..1 position; false while
// the GamePad picture is not shown or outside it
bool gamepad_touch(float x, float y, float& tx, float& ty);
// any thread: the next frame is captured (both sticks clicked on the Switch)
void request_capture();
// any thread: a coming frame's pictures only (the debug server's screenshot): captures/<n>/frame_<n>_window.png
// (the window as shown) and frame_<n>.png (the game's TV picture). Returns the earliest frame it can be;
// pictures_done_frame() is the frame of the last capture whose files are written (0: none yet).
uint64_t request_pictures();
uint64_t pictures_done_frame();

}  // namespace gfxsw
