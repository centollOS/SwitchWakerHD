// The deko3d renderer (Switch), docs/deko3d-plan.md. Phase P1: the device, its memory, the swapchain
// and a present pass that draws an orientation / depth test pattern, the FPS counter and the settings
// overlay; GX2 draws, clears and copies are counted but not executed yet (the game runs behind the
// pattern). Namespace gfxdk; one render thread (GX2's) records and submits everything.
// Phase P2 (the game's picture) is built in three lanes behind dk_surfaces.h, dk_shaders.h and dk_draw.h
// (plan, "P2 lanes").
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
bool frame_open();                  // between frame_begin and frame_end: commands and stream memory may be used
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

// One-frame data (vertices, indices, uniform blocks) in this frame's stream slice (memory.cpp, as gfx/gl's
// stream_upload / stream_guest). Empty (gpu 0) when the slice is full: stream_alloc logged it, the caller
// skips what needed it.
struct StreamSlice {
    DkGpuAddr gpu = 0;
    uint32_t size = 0;  // bytes in the slice: the data and the zero tail
    explicit operator bool() const { return gpu != 0; }
};
// zeroTail: that many zero bytes follow the data
StreamSlice stream_upload(const void* data, uint32_t size, uint32_t alignment, uint32_t zeroTail = 0);
// guest memory, uploaded once per frame per (address, alignment, zero tail) while R.streamGen is unchanged
StreamSlice stream_guest(uint32_t addr, uint32_t size, uint32_t alignment, uint32_t zeroTail = 0);

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
    // P2 (docs/deko3d-plan.md, "P2 lanes"): the epochs the caches of the draw path check, as gfx/gl's
    // advances when a surface lookup could give a different answer (a surface created, or one becoming or
    // ceasing to be GPU-written): texture lookups cached in draw.cpp are valid while it is unchanged
    uint64_t surfaceEpoch = 1;
    uint64_t textureEpoch = 1;  // a surface got a new image or image descriptor (rescale): targets bound again
    uint64_t stateEpoch = 1;    // code outside draw() changed command buffer state (forget_state, dk_draw.h)
    uint64_t shaderEpoch = 1;   // shader lookups must be redone (reset_shader_memoization, dk_shaders.h)
    uint64_t streamGen = 1;     // guest data in the stream slice may be stale (GX2DrawDone, GX2Invalidate)
    uint64_t drawCount = 0, skippedDraws = 0, scanCopies = 0;
    bool timedDraw = true;      // this draw is one the per-draw timers measure (SampledTime)
    // render-thread time and work since the last 5 s report. Each lane adds to its own fields only.
    struct Perf {
        // draw lane (draw.cpp, backend.cpp)
        uint64_t drawNs = 0, lookupNs = 0, indexNs = 0, resourceNs = 0, stateNs = 0, submitNs = 0, presentNs = 0;
        uint64_t memoHits = 0, comboHits = 0, textureLookups = 0, textureCacheHits = 0;
        uint64_t uboBytes = 0, indexBytes = 0, vertexBytes = 0, streamBytes = 0, reusedBytes = 0, copyNs = 0;
        uint64_t streamFullSkips = 0;  // draws skipped because the stream slice was full
        uint64_t gamepadDraws = 0, gamepadDrawNs = 0, gamepadSkipped = 0, gamepadClearsSkipped = 0;
        uint64_t flushNs = 0, flushes = 0, midFrameSubmits = 0;
        // surface lane (surfaces.cpp, formats.cpp, descriptors)
        uint64_t uploadNs = 0, uploads = 0, uploadBytes = 0;
        uint64_t clearNs = 0, clears = 0, surfaceCopyNs = 0, surfaceCopies = 0, cpuSurfaceCopies = 0;
        uint64_t invalidateNs = 0, invalidates = 0, scanNs = 0, scans = 0, scanBlits = 0, feedbackCopies = 0;
        uint64_t rescales = 0, imageDescriptorWrites = 0, samplerDescriptorWrites = 0;
        // shader lane (shaders_dk.cpp; the worker's own figures are in ShaderStats)
        uint64_t shaderNs = 0, shaders = 0, dkshLoads = 0, uniformPackNs = 0;
    } perf;
};
extern Renderer R;

// The frame's command recording (backend.cpp). A frame's commands start at the first GX2 command that
// records anything after a present (or at the present itself) and end at the present: begin_commands opens
// the frame when it is not open (frame_begin: the slot's fence, its stream and command memory), binds the
// descriptor sets and calls the lanes' frame-start hooks. Every render-thread entry that records commands
// or allocates stream memory calls it first (the Backend table does for the GX2 entries).
void begin_commands();

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
struct ScopedTime {
    uint64_t& total;
    uint64_t start = now_ns();
    ~ScopedTime() { total += now_ns() - start; }
};
// The per-draw timers time one draw in kDrawTimeSample and count it that many times (as gfx/gl)
constexpr uint64_t kDrawTimeSample = 16;
struct SampledTime {
    uint64_t& total;
    const bool on;
    const uint64_t start;
    SampledTime(uint64_t& t, bool timed) : total(t), on(timed), start(timed ? now_ns() : 0) {}
    ~SampledTime() {
        if (on) total += (now_ns() - start) * kDrawTimeSample;
    }
};

// Open-addressing hash table whose entries live for one stamp (a frame), as gfx/gl's: entries with
// another stamp count as empty, so starting a new frame frees nothing and later inserts allocate nothing.
// Entry needs `uint64_t key, stamp` (stamp ~0 when unused).
template <class Entry> struct FrameTable {
    std::vector<Entry> slots = std::vector<Entry>(1 << 12);
    size_t used = 0;
    uint64_t usedStamp = ~0ull;
    // the entry holding key, or the empty entry where it would go
    Entry* find(uint64_t key, uint64_t stamp) {
        size_t mask = slots.size() - 1, i = size_t((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;
        for (;; i = (i + 1) & mask) {
            Entry& e = slots[i];
            if (e.stamp != stamp || e.key == key) return &e;
        }
    }
    void put(const Entry& entry) {
        if (usedStamp != entry.stamp) {
            used = 0;
            usedStamp = entry.stamp;
        }
        if ((used + 1) * 2 > slots.size()) {
            std::vector<Entry> old(slots.size() * 2);
            old.swap(slots);
            for (auto& e : old)
                if (e.stamp == entry.stamp) *find(e.key, e.stamp) = e;
        }
        Entry* e = find(entry.key, entry.stamp);
        if (e->stamp != entry.stamp) used++;
        *e = entry;
    }
};

}  // namespace gfxdk
