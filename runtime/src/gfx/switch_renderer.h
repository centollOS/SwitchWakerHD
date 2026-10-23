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
// tests (Developer options; off / 1 by default, not saved): the ambient-occlusion pass replaced by "no occlusion", and
// the shadow maps' scale (0.5, 1, 2 x their 1024x1024); any thread, from the next frame
bool skip_ao();
void set_skip_ao(bool on);
float shadow_scale();
void set_shadow_scale(float scale);
// 16x anisotropic filtering on the game's mipmapped linear samplers (WWHD_ANISO): any thread, from the next frame
bool aniso();
void set_aniso(bool on);   // on: 16x
int aniso_level();         // 1 (off), 2, 4, 8 or 16
void set_aniso_level(int level);
// edge smoothing (FXAA, issue #21) on the 3D picture before the HUD (WWHD_FXAA): any thread, from the next frame
bool fxaa();
void set_fxaa(bool on);
// the bloom and the distance blur (issue #15): the game samples its bloom chain and its half-size picture with more
// levels than the render targets have; gfx/deko mip_chain makes those levels. On by default (WWHD_BLOOM=0,
// WWHD_DISTANCE_BLUR=0 off at start); any thread
bool bloom();
void set_bloom(bool on);
bool distance_blur();
void set_distance_blur(bool on);
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
    kOptSkipTv,   // the TV picture not drawn while the window shows the GamePad's (Off-TV Play; WWHD_DK_SKIP_TV)
    // the draw-memo round (gfx/deko dk_draw.h): the target memo by state words and its same-frame skip
    // (WWHD_DK_TARGET_MEMO2), the fixed state in three memos (WWHD_DK_FIXED_MEMO), vertex states by layout identity
    // (WWHD_DK_VTX_IDENTITY)
    kOptTargetMemo2, kOptFixedMemo, kOptVtxIdentity,
    kDrawOpts
};
// shader compile threads for the shaders draws ask for (gfx/deko/shaders_dk.cpp helper_main): 0 auto (the default:
// three while draws wait for their shaders, one otherwise), 1 (as before) to 3; any thread (the Switch tab's Debug
// section, debug server "shaderthreads")
int shader_compile_threads();
void set_shader_compile_threads(int n);
bool draw_opt(int which);
void set_draw_opt(int which, bool on);
// texture eviction (gfx/deko/surfaces.cpp, issue #11): frames a texture may stay unused before it leaves the image
// heap (0: only under memory pressure); evict_textures_now: every texture unused for a few frames goes at the next
// frame. Any thread (the debug server's "evict")
uint64_t texture_idle_frames();
void set_texture_idle_frames(uint64_t frames);
void evict_textures_now();
std::string texture_eviction_report();
#ifdef __SWITCH__
std::string clock_report_now();  // "CPU x MHz, GPU y MHz, memory z MHz", or "" if unavailable
#endif
// the GamePad screen (the game's second screen: items, map, sea chart). Its picture is drawn while the
// controller acts as the Wii U GamePad, and with the Pro Controller while the window shows it; the window shows
// the TV picture or, switched (ZL + ZR + Minus, or the Switch tab), the GamePad picture full size, with the
// console's touch screen as the GamePad's.
bool gamepad_picture_drawn();         // the controller acts as the GamePad, or the window shows its picture (render thread, every frame)
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
