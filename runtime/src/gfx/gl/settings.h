// What the OpenGL renderer lets the settings overlay change while the game runs (Switch: the
// "Switch" tab, platform/settings_switch.cpp), and draws it with (overlay_gl.cpp).
#pragma once
#include <string>

struct ImDrawData;

namespace gfxgl {

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
#ifdef __SWITCH__
std::string clock_report_now();  // "CPU x MHz, GPU y MHz, memory z MHz", or "" if unavailable
#endif

// render thread, inside present()
void overlay_renderer_init();
void overlay_draw(ImDrawData* d, int ww, int wh);

}  // namespace gfxgl
