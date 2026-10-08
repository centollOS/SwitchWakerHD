// Internal interfaces between the GX2 layer and the renderer.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>

struct LatteFetchShader;
namespace GX2 {
struct GX2ColorBuffer;
struct GX2DepthBuffer;
}

namespace gx2 {
struct ShaderKeyDirtyStats {
    uint64_t changedBatches = 0, baselineWouldBumps = 0, actualBumps = 0;
    uint64_t avoidedBumps = 0, maskedWords = 0;
};
ShaderKeyDirtyStats shader_key_dirty_stats(); // Render-thread diagnostics.
uint64_t render_thread_wait_ns();  // total time the GX2 render thread has waited for commands
uint64_t game_sync_wait_ns();      // total time game threads waited for the render thread (GX2DrawDone...)
uint64_t game_syncs();             // how many times they did
// what the render thread is doing, for the hang watchdog: the renderer names its blocking steps
// (GPU fence waits, buffer swaps, shader compiles); null while it runs ordinary commands
extern std::atomic<const char*> g_render_stage;
// Register categories for the renderer's per-draw fast paths (round 41): g_reg_gen[c] advances whenever a register
// of category c changes value (and at context and save-state loads), so a draw can tell a whole category unchanged
// since an earlier draw with one compare. Targets: the CB_COLORn and DB_DEPTH buffer registers, CB_COLOR_CONTROL,
// CB_TARGET_MASK, DB_DEPTH_CONTROL, the scissor's bottom right (LatteMRT's active-buffer masks). Fixed: deko3d
// draw.cpp's fixed-state registers (rasterizer, depth/stencil, blend). Viewport: PA_CL_VPORT_*, PA_CL_CLIP_CNTL and
// the scissor. Render thread only.
enum RegCategory : int { kRegCatTargets, kRegCatFixed, kRegCatViewport, kRegCats };
extern uint64_t g_reg_gen[kRegCats];
uint32_t color_buffer_address(const GX2::GX2ColorBuffer* cb);
// the layers a color buffer is drawn as: a 2D array's slices, a 3D buffer's depth slices at its view's level, else 1
uint32_t color_buffer_slices(const GX2::GX2ColorBuffer* cb);
LatteFetchShader* build_fetch_shader(uint32_t program);  // from our encoded fetch "program"
// Uncapped (debug only: settings overlay > Graphics; WWHD_UNCAPPED=1 for any renderer, and the older
// WWHD_VK_UNCAPPED=1 with Vulkan): flips no longer wait for the virtual vsync and presentation no
// longer waits for the display (Metal: displaySyncEnabled off; Vulkan: immediate or mailbox), so the
// renderer runs as fast as it can. The game is frame-locked: it then runs faster than real time.
// Not saved.
bool uncapped();
void set_uncapped(bool on);
}  // namespace gx2

// The renderer backend (Metal). All calls come from the thread executing GX2
// commands, in submission order. Guest structures are passed by guest address.
namespace gx2 {
constexpr uint32_t kDepthSlicesReg = 0xA002;  // our convention (unused register): depth buffer array size
// our convention: CB_COLORn_TILE = view width | slices << 16 (bits 16..30) | kColorTarget3D. Slices are the
// array size of a 2D array buffer, or the depth of a volume (3D) buffer, whose slice the view selects
// (CB_COLORn_VIEW slice start). The game renders its 8x8x8 colour-grading volumes slice by slice that way.
constexpr uint32_t kColorTarget3D = 0x80000000u;
constexpr uint32_t color_target_slices(uint32_t tile) { return std::max<uint32_t>((tile >> 16) & 0x7FFF, 1); }
}

namespace gfx {
void init();                     // create device; call on the main thread before the game starts
void run_main_loop();            // window/event loop; runs on the main thread, never returns
void draw(const uint32_t* regs, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
          uint32_t baseVertex, uint32_t instances);
void clear_color(const uint32_t* regs, uint32_t colorBuffer, const float rgba[4]);
void clear_depth_stencil(const uint32_t* regs, uint32_t depthBuffer, float depth, uint32_t stencil, uint32_t flags);
void copy_surface(uint32_t src, uint32_t srcMip, uint32_t srcSlice, uint32_t dst, uint32_t dstMip, uint32_t dstSlice);
void copy_to_scan(uint32_t colorBuffer, uint32_t target);  // target: 1 = TV, 4 = DRC (GamePad)
void write_back_linear_targets();  // GX2DrawDone: linear render targets the CPU reads, to guest memory
void swap();                     // present the TV scan buffer
void set_frame_aspect(float a);  // aspect ratio of the TV picture from the next frame on (aspect.cpp)
// render thread: a target of this guest size is made wider/taller this frame (kx, ky != 1)
bool target_aspect_factors(uint32_t w, uint32_t h, float& kx, float& ky);
uint64_t frames_completed();     // swaps whose GPU work has finished
void with_autorelease_pool(void (*fn)());  // render thread: drain Objective-C temporaries per batch
void set_tv_format(uint32_t gx2Format, bool tv);  // GX2SetTVBuffer / GX2SetDRCBuffer
void invalidate(uint32_t flags, uint32_t addr, uint32_t size);
void flush();                    // submit queued GPU work
void wait_idle();                // GX2DrawDone: block until the GPU finished
}  // namespace gfx
