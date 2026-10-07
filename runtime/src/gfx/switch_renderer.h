// What the Switch renderer (deko3d, gfx/deko) offers the code around it: the settings overlay's Switch
// tab (overlay/overlay.cpp), the saved options (platform/settings_switch.cpp) and the capture combo
// (platform/input_switch.cpp). gfx/deko defines these functions.
#pragma once
#include <string>

namespace gfxsw {

// picture adjustments applied when presenting (neutral = 1; env.txt: WWHD_EXPOSURE, WWHD_CONTRAST,
// WWHD_SATURATION, WWHD_GAMMA)
struct PictureGrade {
    float exposure = 1, contrast = 1, saturation = 1, gamma = 1;
};
PictureGrade picture_grade_now();
void set_picture_grade(const PictureGrade& g);
// the counter in the top-left corner: 0 off, 1 frame rate, 2 with render-thread load and draws (WWHD_FPS)
int fps_overlay_mode();
void set_fps_overlay_mode(int mode);
float dynamic_res_scale();  // the internal resolution dynamic resolution has chosen (1 when off)
// any thread: the internal resolution (the most dynamic resolution may use) and whether dynamic resolution
// may lower it, from the next frame on (the handheld / docked profiles, platform/settings_switch.h)
void set_resolution_profile(float scale, bool dynamic);
// per-draw optimizations the Switch tab turns on and off for A/B tests (not saved; env.txt sets them at start):
// 0 uniform registers skipped while unchanged (WWHD_DK_ALU_GEN), 1 fixed state skipped (WWHD_DK_FIXED_SKIP).
// Each change is logged with its frame, so one session's log compares both halves.
enum DrawOpt : int { kOptAluGen, kOptFixedSkip, kDrawOpts };
bool draw_opt(int which);
void set_draw_opt(int which, bool on);
#ifdef __SWITCH__
std::string clock_report_now();  // "CPU x MHz, GPU y MHz, memory z MHz", or "" if unavailable
#endif
// any thread: the next frame is captured (both sticks clicked on the Switch)
void request_capture();

}  // namespace gfxsw
