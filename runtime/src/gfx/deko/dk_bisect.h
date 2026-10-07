// Grass-speck bisection switches (fix-grass2; docs/deko3d-plan.md, Estado). Flickering 4x8-pixel blocks on the
// islands' grass/sand and in Dragon Roost Cavern lose the depth-biased ground decals' layers; gfx/gl is clean.
// Each switch changes ONE suspect, is read once (env.txt) and is announced by a '[dk] grass bisect:' line:
//   WWHD_DK_SHADER_SCHED=0|1|2|3   uam's Maxwell scheduling words, patched in every DKSH as it is loaded:
//                                  0 as compiled; 1 no dual issue; 2 = 1 + every instruction waits on all six
//                                  scoreboards (no read of a TEX/IPA/MUFU result before it lands); 3 = 2 + stall 15
//   WWHD_DK_EARLY_Z=0|1|2          0 as compiled; 1 API-mandated early Z on fragment shaders that neither kill
//                                  nor write depth; 2 late Z everywhere (the SPH's KillsPixels bit set)
//   WWHD_DK_PS_KNOB=0|mesa         subtiling perf knob A of fragment shaders: uam's 0x087F6080 or Mesa's 0x20164010
//   WWHD_DK_DEPTH_BIAS=gl|latte|off|units2|noslope   polygon offset: gl = as gfx/gl (Mesa: no clamp); latte = clamp
//                                  from PA_SU_POLY_OFFSET_CLAMP (before); off; units x2; slope factor 0
//   WWHD_DK_DECAL_BARRIER=0|1|2    a Fragments (1) or Full (2) barrier before every depth-biased draw and before
//                                  the first draw after a run of them
//   WWHD_DK_SHADER_INVALIDATE=0|1  shader caches invalidated before the next draw after shader code was loaded
//                                  mid-frame (deko3d only does it at dkQueueFlush)
//   WWHD_DK_DESC_WFI=0|1           descriptor writes committed with a wait for idle before the header/sampler cache
//                                  invalidate (Mesa's TIC_FLUSH/TSC_FLUSH wait; deko3d's are NoWfi)
//   WWHD_DK_TILED_OFF=0|1          the tiled cache explicitly disabled at every frame start (deko3d never writes it)
//   WWHD_DK_SHADOW_BARRIER=0|1     a draw that samples a target written since the last barrier (the sun-shadow
//                                  mask 960x540 after #1654-1656, the shadow map, depth) gets a Full barrier that
//                                  invalidates image, shader, descriptor and L2 caches instead of a Fragments one
#pragma once
#include <deko3d.h>

#include <cstdint>

namespace gfxdk {

enum class DepthBiasMode : uint8_t { Gl, Latte, Off, Units2, NoSlope };

struct Bisect {
    int shaderSched = 2;
    int earlyZ = 0;
    bool psKnobMesa = false;
    DepthBiasMode depthBias = DepthBiasMode::Gl;
    int decalBarrier = 0;
    bool shaderInvalidate = true;
    bool descWfi = true;
    bool tiledOff = true;
    bool shadowBarrier = true;
};
const Bisect& bisect();
void bisect_log();  // the startup lines (draw_frame_start)

// code_load: patch the DKSH just copied to code memory (scheduling words, early Z, knob) before dkShaderInitialize
void bisect_patch_dksh(uint8_t* dksh, uint32_t size);
// draw.cpp, right before dkCmdBufDraw*: the shader invalidate after loads and the decal barriers
void bisect_before_draw(bool depthBiased);

}  // namespace gfxdk
