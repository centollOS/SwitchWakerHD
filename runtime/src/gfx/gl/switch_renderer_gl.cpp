// gfx/switch_renderer.h for the OpenGL renderer: forwards to gfxgl (settings.h).
#include "settings.h"

namespace gfxsw {
PictureGrade picture_grade_now() { return gfxgl::picture_grade_now(); }
void set_picture_grade(const PictureGrade& g) { gfxgl::set_picture_grade(g); }
int fps_overlay_mode() { return gfxgl::fps_overlay_mode(); }
void set_fps_overlay_mode(int mode) { gfxgl::set_fps_overlay_mode(mode); }
float dynamic_res_scale() { return gfxgl::dynamic_res_scale(); }
#ifdef __SWITCH__
std::string clock_report_now() { return gfxgl::clock_report_now(); }
#endif
void request_capture() { gfxgl::request_capture(); }
}  // namespace gfxsw
