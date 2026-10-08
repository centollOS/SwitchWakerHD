// The deko3d renderer's draw path and frame (P2, draw lane; docs/deko3d-plan.md "P2 lanes"): GX2 draws
// recorded into R.cmd, the state cache (DkRasterizerState, DkDepthStencilState, DkColorState,
// DkColorWriteState, DkBlendState[8], viewports and scissors), vertex and index streams, textures and UBOs
// per stage, the frame's submit and the present pass of the game's picture. A structural port of
// gfx/gl/draw.cpp (memo and combos, index conversion, target and texture caches, feedback copies, GamePad
// skip, trace and probes) and of gfx/gl/backend.cpp (stats, hitch, GPU busy, dynamic resolution hooks).
//
// Files: draw.cpp and backend.cpp (this lane owns both; dk.h, memory.cpp and overlay_dk.cpp change only at
// integration). Render thread only.
//
// What a draw asks the other lanes for, in order:
//   shaders  (dk_shaders.h):  get_fetch_shader, translate (VS, PS); not ready -> shader_wanted + skip
//   targets  (dk_surfaces.h): color_target / depth_target, before_write, fit_scale, upload_surface,
//                             target_view -> dkCmdBufBindRenderTargets (cached by surface, level, layer and
//                             R.textureEpoch)
//   textures (dk_surfaces.h): sampled_texture + upload_surface + sampled_view_id + sampler_id per unit
//                             (cached by words and R.surfaceEpoch), feedback_copy for a bound target, then
//                             commit_descriptors and dkCmdBufBindTextures(stage, sh->textureSlot[unit],
//                             dkMakeTextureHandle(view, sampler))
//   uniforms: guest blocks by stream_guest (dk.h) at sh->uboSlot[i]; the ufBlock by pack_uniforms
//             (dk_shaders.h) at sh->bindings.ufBlockSlot
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "dk.h"

namespace gfxdk {
struct Surface;

// GX2 draw (render::Backend::draw; the table calls begin_commands first, as for every recording entry)
void draw(const uint32_t* regs, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
          uint32_t baseVertex, uint32_t instances);
// draw() skips state it set before; any other code that binds render targets, shaders, viewports, scissors,
// vertex state or 3D state (clears, the present pass, the overlay) calls this so the next draw sets
// everything again (R.stateEpoch++, as gfx/gl forget_gl_state)
inline void forget_state() { R.stateEpoch++; }
// once per frame from begin_commands: the draw path's per-frame caches
void draw_frame_start();
// backend.cpp's 5 s report: the resources stage's parts, binds and ufBlock figures since the last call (P4)
void log_resource_stats(uint64_t executedDraws, uint64_t frames);

// the commands recorded so far go to the GPU now (dkCmdBufFinishList + dkQueueSubmitCommands, after
// check_queue): GX2Flush / GX2DrawDone, and a frame whose command memory grows past a threshold. Recording
// continues in the same frame (same fence, stream slice and descriptor sets).
void submit_commands(const char* why);

// draws skipped since the last call, by reason (backend.cpp's stats line; R.skippedDraws counts them all)
struct DrawSkips {
    uint64_t noFetchShader = 0, shaderPending = 0, shaderFailed = 0, noTarget = 0, scissorEmpty = 0, streamFull = 0,
             unsupported = 0;
};
DrawSkips draw_skips_take();
// the fixed-state skip of draws (WWHD_DK_FIXED_SKIP; the Switch tab turns it on and off for A/B tests)
extern std::atomic<bool> g_fixedSkip;
// round 40 (draw.cpp): depth-only draws with an empty pixel shader (WWHD_DK_DEPTH_ONLY), texture lookups at
// addresses with several surfaces cached (WWHD_DK_TEX_SHARED_CACHE), vertex layouts kept per vertex shader
// (WWHD_DK_VTX_LAYOUT_CACHE); executed draws per mid-frame submit (WWHD_DK_SUBMIT_DRAWS, 0 = only at GX2Flush,
// GX2DrawDone and present; 1024 since round 41: each submit cost ~51 us at 1020 MHz, and in the 2026-10-08 A/B
// 1024 gained 0.6-0.8 fps over 256 in the 25 fps view). All switchable in the Switch tab.
extern std::atomic<bool> g_depthOnly, g_texSharedCache, g_vtxLayoutCache;
// round 41: render targets, fixed state and viewport skipped while their registers are unchanged (gx2.h g_reg_gen;
// WWHD_DK_REG_GENS)
extern std::atomic<bool> g_regGens;
// round 42: the draw's shaders' hot data prefetched once they are known (WWHD_DK_PREFETCH)
extern std::atomic<bool> g_prefetch;
constexpr uint32_t kSmallSubmitDraws = 256, kBigSubmitDraws = 1024, kDefaultSubmitDraws = kBigSubmitDraws;
extern std::atomic<uint32_t> g_submitDraws;

// P4 (lookup and indices): GX2Invalidate's flags and GX2DrawDone, from backend.cpp (index lists go stale at
// attribute-buffer invalidates and GX2DrawDone); every 5 s the '[dk] lookup per frame' line (frames since
// the last one), after the draw path's line
void note_invalidate(uint32_t flags);
void note_wait_idle();
void log_lookup_stats(uint64_t frames);

// the AO quirk fix's mode (WWHD_AO_MODE, read once at start-up as gfx/gl's): 0..2
int ao_mode();

// GamePad-only surfaces (not drawn unless the GamePad picture is shown; as gfx/gl draw.cpp)
bool gamepad_only(const Surface* s);
bool skip_gamepad();

// WWHD_DK_TRACE_FRAMES / capture (backend.cpp, as gfx/gl): the passes of a frame in the log
extern bool g_traceFrame;    // the frame being recorded is traced
extern bool g_captureDraws;  // ... and every draw in it too
void trace_pass(const std::array<Surface*, 8>& colors, const Surface* depth);
void trace_draw(const Surface* sampled);  // nullptr: a draw; else a surface the draw samples
void trace_event(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string trace_name(const Surface* s);
// GPU time per render pass (timestamps by dkCmdBufReportCounter, P3): a pass starts where targets change
void gpu_pass_mark(const char* kind, const Surface* color, const Surface* depth);

}  // namespace gfxdk
