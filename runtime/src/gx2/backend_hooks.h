// The two GX2 decisions that depend on the renderer beyond its draw calls (gx2_core.cpp), here so a
// test can run them with a stub backend (renderer_ext_test): which register changes count for the
// shader key, and how GX2DrawDone waits.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "gfx/renderer.h"
#include "shader_key_dirty.h"

namespace gx2 {

using ShaderKeyMask = bool (*)(uint32_t reg, uint32_t& mask);

// The mask for a register change: the backend's own (Backend::shader_key_mask), else Vulkan's with
// WWHD_VK_SHADER_KEY_DIRTY=1; null: every change of a shader-relevant register counts. gx2_core.cpp
// picks it once, when the renderer runs.
inline ShaderKeyMask pick_shader_key_mask(const render::Backend* backend) {
    if (!backend) return nullptr;
    if (backend->shader_key_mask) return backend->shader_key_mask;
    const char* e = getenv("WWHD_VK_SHADER_KEY_DIRTY");
    return backend->api == render::Api::Vulkan && e && !strcmp(e, "1") ? vulkan_shader_key_mask : nullptr;
}

// Whether a shader-relevant register going from `old` to `value` changes the shader key.
inline bool shader_key_changed(ShaderKeyMask keyMask, uint32_t reg, uint32_t old, uint32_t value) {
    uint32_t mask;
    return !keyMask || !keyMask(reg, mask) || ((old ^ value) & mask) != 0;
}

// OP_DRAW_DONE. `lazy`: queue the work instead of waiting (Vulkan, lazy_draw_done()); `idle`: the
// payload asks for an idle GPU (save states), which a platform backend's gpu_idle provides.
inline void draw_done(bool lazy, bool idle) {
    if (lazy && !idle) render::guest_flush();
    else if (idle) render::gpu_idle();
    else render::wait_idle();
    // except what the CPU reads back: linear render targets (the Picto Box picture, issue #53)
    render::write_back();
}

}  // namespace gx2
