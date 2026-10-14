#pragma once
#include "backend.h"
#include "../stereo.h"
namespace gfxvk {
extern int stereoEye;
extern Screen stereoRight;
bool stereo_light_atlas(uint32_t width, uint32_t height, uint32_t layers, bool depth);
decltype(Renderer::surfaces)& stereo_surface_cache(const SurfaceDesc& desc);
decltype(Renderer::surfaces)& stereo_copy_cache(uint32_t addr,uint32_t width,uint32_t height,uint32_t layers);
void stereo_select_eye(int eye);
void stereo_invalidate(uint32_t flags, uint32_t addr, uint32_t size);
// Wrap complete guest operations, including clears/copies, so intermediate
// color, depth, history and alias images stay in the corresponding eye's graph.
template<class F> void stereo_pair(F&& f, bool force = false) {
    if (stereo::frame.mode == stereo::Off && !force) {
        stereo_select_eye(0);
        f();
        return;
    }
    // Continue with the eye whose render pass is already open. L,R then R,L
    // preserves operation order within each eye and avoids an extra pass break
    // between every pair. Canonical guest reads explicitly select the left eye.
    try {
        f();
        stereo_select_eye(1-stereoEye);
        f();
    } catch (...) { stereo_select_eye(0); throw; }
}
void stereo_swap_ao();
void stereo_release_ao(); // eye 0 must be selected; retire only eye 1's scratch images
bool stereo_screen_target(uint32_t width, uint32_t height);
void stereo_latch();
void load_saved_stereo_hdr(); // start-up, both hosts: 3D and HDR settings saved by the overlay
}
