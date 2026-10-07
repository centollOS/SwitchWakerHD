// The deko3d renderer (Switch), docs/deko3d-plan.md. Phase P1: the device, its memory, the swapchain
// and a present pass that draws an orientation / depth test pattern, the FPS counter and the settings
// overlay; GX2 draws, clears and copies are counted but not executed yet (the game runs behind the
// pattern). Namespace gfxdk; one render thread (GX2's) records and submits everything.
//
// Device conventions (plan section 2): DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne: window
// and image row 0 at the top, clip-space z from 0 to 1, but clip-space y points UP as in OpenGL (deko3d 0.5.0
// Primer.md; its viewport transform has scaleY = -height/2 with this origin, so normalized y = +1 is row 0;
// the YAxisPointsDown flag came after 0.5.0). The renderer's vertex shaders take y-down positions (Vulkan's,
// row 0 at y = -1) and negate y; P2's SET_POSITION does the same. The test pattern shows it.
#pragma once
#include <deko3d.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "gx2/gx2.h"

struct ImDrawData;

namespace gfxdk {

// ---- memory (memory.cpp)
// Per-frame resources come in kFrames slots; a slot is reused after its fence (the frame kFrames
// earlier) has signalled.
constexpr uint32_t kFrames = 4;
constexpr uint32_t kStreamSliceSize = 32u << 20;   // per frame: vertices, indices, uniforms, uploads
constexpr uint32_t kCmdSliceSize = 4u << 20;       // per frame: command memory
constexpr uint32_t kCmdChunk = 64u << 10;          // fed to the command buffer at frame_begin, then as it asks
constexpr uint32_t kImageChunkSize = 64u << 20;    // image heap chunks
constexpr uint32_t kCodeSize = 32u << 20;          // shader code (DKSH), bump allocated
constexpr uint32_t kImageDescriptors = 8192, kSamplerDescriptors = 1024;
constexpr uint32_t kQuerySize = 64u << 10;         // counters / timestamps

struct StreamAlloc {
    void* cpu = nullptr;
    DkGpuAddr gpu = 0;
    explicit operator bool() const { return cpu != nullptr; }
};
// a suballocation of the image heap
struct ImageAlloc {
    DkMemBlock block = nullptr;
    uint32_t offset = 0, size = 0;
    int chunk = -1;
};

void memory_init();
void frame_begin(uint64_t frame);   // waits for the slot's fence, resets its stream and command memory
void frame_end();                   // the fence after this frame's commands (recorded into the command buffer)
bool frame_done(uint64_t frame);    // the GPU has finished that frame's commands (no wait)
StreamAlloc stream_alloc(uint32_t size, uint32_t alignment);  // this frame's slice; empty when full (logged)
ImageAlloc image_alloc(uint32_t size, uint32_t alignment);
void image_free_later(const ImageAlloc& a);  // freed when the GPU is done with the current frame
bool code_load(DkShader& shader, const void* dksh, uint32_t size, const char* name);
DkGpuAddr image_descriptors();
DkGpuAddr sampler_descriptors();
struct MemoryStats {
    uint64_t cmdBytesMax = 0, cmdBytesSum = 0;  // command memory fed per frame (64 KB steps)
    uint64_t cmdOverflows = 0;                  // frames that needed more than kCmdSliceSize
    uint64_t streamBytesSum = 0, streamFull = 0;
    uint64_t frames = 0;
    uint64_t imageBytes = 0, imageChunks = 0, codeBytes = 0;
};
MemoryStats memory_stats_take();  // per-frame figures since the last call; totals as they are

// ---- the device (backend.cpp)
struct Renderer {
    DkDevice device = nullptr;
    DkQueue queue = nullptr;
    DkCmdBuf cmd = nullptr;  // the render thread's command buffer (one list per frame)
    uint64_t frame = 0;      // frames presented (GX2 swaps)
    std::atomic<uint64_t> completed{0};
    std::atomic<bool> tvSrgb{false};
    // GX2 commands this phase does not execute (counted for the stats)
    struct Counts {
        uint64_t draws = 0, clears = 0, copies = 0, scans = 0, invalidates = 0, flushes = 0, waits = 0;
    } counts;
};
extern Renderer R;

// ---- the settings overlay's renderer (overlay_dk.cpp), inside the present pass
void overlay_renderer_init();
void overlay_draw(ImDrawData* d, int ww, int wh);

// ---- the renderer's own shaders (embedded DKSH, backend.cpp)
enum ShaderId { kPatternVs, kPatternFs, kTextVs, kTextFs, kImguiVs, kImguiFs, kShaderCount };
const DkShader* shader(ShaderId id);  // null if it did not load (logged)

// the draw path times its stages: on the Switch the tick counter is read directly
inline uint64_t now_ns() {
    uint64_t ticks;
    asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks * 625 / 12;  // 19.2 MHz
}
// names a blocking step for the hang watchdog (gx2::g_render_stage)
struct Stage {
    const char* prev;
    explicit Stage(const char* what) : prev(gx2::g_render_stage.exchange(what, std::memory_order_relaxed)) {}
    ~Stage() { gx2::g_render_stage.store(prev, std::memory_order_relaxed); }
    Stage(const Stage&) = delete;
};

}  // namespace gfxdk
