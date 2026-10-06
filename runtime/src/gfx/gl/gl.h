// The OpenGL renderer (Switch: Mesa nouveau through EGL). GX2 state is translated as in the
// Vulkan renderer (gfx/vulkan); shaders are Cemu's GLSL in its OpenGL flavour (no VULKAN define).
//
// Window coordinates: guest images keep their memory layout (row 0 = top), so textures uploaded
// from guest memory and images rendered by the GPU agree. Each draw picks the clip-control origin
// that maps the guest viewport (usually a negative y scale) onto memory rows without a negative
// GL viewport height; presentation flips the picture once into the window.
#pragma once
#include <glad/glad.h>
#include <EGL/egl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gx2/gx2.h"

namespace gfxgl {

enum class Convert : uint8_t { NONE, RGB565, RGBA5551, ABGR1555, RGBA4, RG4, D24S8, D24_R32F, X24_8_32F };

struct FormatInfo {
    GLenum internal = 0, format = 0, type = 0;  // type 0: compressed (internal format only)
    uint32_t bytesPerBlock = 0;                 // guest bytes per texel or 4x4 block
    uint32_t hostBytesPerBlock = 0;
    bool compressed = false, depth = false, stencil = false;
    Convert convert = Convert::NONE;
    enum Kind : uint8_t { FLOAT, UINT, SINT } kind = FLOAT;
};
FormatInfo format_info(uint32_t gx2Format, bool isDepth);
void convert_row(Convert c, const uint8_t* src, uint8_t* dst, uint32_t count);

struct GuestLayout;  // address-library geometry of each mip level (surfaces.cpp)
struct Surface {
    GLuint tex = 0;
    GLenum target = GL_TEXTURE_2D;
    uint32_t layers = 1;  // array layers of the texture (1 for 3D)
    uint32_t addr = 0, mipAddr = 0, width = 0, height = 0, slices = 1, pitch = 0, mips = 1, format = 0, dim = 1,
             tileMode = 0, swizzle = 0;
    bool isDepth = false, gpuWritten = false, dirty = true;
    uint64_t writeSeq = 0, contentHash = 0, lastCheckedFrame = ~0ull, sparseHash = 0;
    uint64_t changedFrame = 0;   // the last frame its guest data was uploaded (upload_surface)
    uint64_t drawFrame = ~0ull;  // the last frame a draw rendered to it, and how many draws did
    uint32_t frameDraws = 0;
    // the GamePad picture (gamepad_only): the last frame it was copied to the GamePad's / the TV's scan
    // buffer, and read by a draw (other than the GamePad picture's) or a copy
    uint64_t drcScanFrame = ~0ull, tvScanFrame = ~0ull, readFrame = ~0ull;
    bool gamepadSource = false;  // sampled by draws of the GamePad picture (and by nothing else)
    uint64_t gamepadSourceSince = ~0ull;  // the first frame a GamePad-picture draw sampled it
    // read by something other than the GamePad picture after it became a GamePad source: shared with
    // the TV picture, never skipped again (round 24: a texture the game renders once, at an area
    // change, must not have that draw skipped)
    bool tvShared = false;
    bool skipLogged = false;  // its first skipped draw is in the log
    // rendered (or copied) from a GamePad-only surface by something that is not the GamePad picture
    // (the GamePad's own transitions capture its picture): reading this one reads that one (note_read)
    Surface* derivedFrom = nullptr;
    uint32_t dataSize = 0;
    // internal resolution (surfaces.cpp): the texture has pw x ph pixels for the guest's width x height.
    // Only screen-shaped render targets get a scale other than 1; everything that talks to the game
    // (lookups, guest memory) keeps the guest size, draws scale their viewport and scissor.
    float scale = 1.0f;
    uint32_t pw = 0, ph = 0;
    bool renderTarget = false;  // created or used as a render target
    bool scalable = false;      // screen-shaped: takes res_scale() as a render target
    bool hudFull = false;       // the TV picture's buffer at full resolution for the HUD (draw.cpp)
    // the other texture of the TV picture's buffer (scaled scene / full-resolution HUD), kept to switch
    GLuint twinTex = 0;
    uint32_t twinPw = 0, twinPh = 0;
    float twinScale = 0;
    std::unordered_map<uint32_t, GLuint> twinViews;
    FormatInfo fmt;
    std::unordered_map<uint32_t, GLuint> views;  // sampled views by (target, swizzle)
    std::shared_ptr<GuestLayout> guest;
};
// Texture and sampler names from a pool (surfaces.cpp): with Mesa's GL thread every glGen* call
// waits for it to run everything queued, so names are generated 256 at a time
GLuint gen_texture_name();
GLuint gen_sampler_name();
// once a frame (present): errors of the texture allocations made in the frame
void check_texture_errors();

// GPU time per render pass (WWHD_GL_GPU_PASSES, backend.cpp): a pass starts where the render targets
// change (draw.cpp), at a clear and at present; cheap unless a frame is being sampled
extern bool g_gpuPassSampling;
void gpu_pass_mark(const char* kind, const Surface* color, const Surface* depth);

// WWHD_GL_TRACE_FRAMES=n,... (testing): the passes of those frames in the log, with the surfaces each
// renders to and samples, and the clears and copies between them (backend.cpp)
extern bool g_traceFrame;  // the frame being built is one of them
extern bool g_captureDraws;  // ... and it is a capture: every draw in the log too (as WWHD_GL_TRACE_DRAWS)
int ao_mode();  // ambient-occlusion quirk mode (draw.cpp, WWHD_AO_MODE)
// the next frame traced into the log and its pictures written to PNGs (backend.cpp; any thread)
void request_capture();
// A capture's probe frames (draw.cpp): from this frame on, one probe mode per frame, the draws of the probed
// pixel shaders are drawn with a diagnostic output and the target is written to a PNG after each
// (backend.cpp sets it after a capture; WWHD_GL_PROBE_FRAME=n on the desktop)
extern uint64_t g_probeStart;
// render-target textures kept for the next resolution step (surfaces.cpp): their size, and dropping them
size_t texture_pool_bytes();
void texture_memory(size_t& total, size_t& targets, size_t& count);  // textures of all surfaces, bytes
void shader_memory(size_t& sources, size_t& storeBytes, size_t& objects, size_t& programs);  // shaders.cpp
void texture_pool_clear();
void trace_pass(const std::array<Surface*, 8>& colors, const Surface* depth);
void trace_draw(const Surface* sampled);  // nullptr: a draw; else a surface the draw samples
void trace_event(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string trace_name(const Surface* s);

// internal resolution (surfaces.cpp): WWHD_RES_SCALE, or dynamic (WWHD_DYNAMIC_RES) when the GPU is the limit
inline uint32_t scaled_size(uint32_t v, float scale) {
    return scale == 1.0f ? v : std::max<uint32_t>(1, uint32_t(v * scale + 0.99f));
}
float res_scale();                 // the factor render targets get this frame
void set_res_scale(float scale);   // from the next frame on (frame_start)
void latch_res_scale();            // present: apply a requested change
void rescale_surface(Surface* s, float scale, bool keepContents, bool keepOld = false);
// a render target about to be written: the scale it should have, unless that needs a new texture and this
// frame's allocations are used up (false: still at its old scale); force: in any case
bool fit_scale(Surface* s, bool keepContents, bool force = false);
// the TV picture: GX2CopyColorBufferToScanBuffer only notes its buffer; presenting reads that buffer
// unless something writes to it first (scan_flush makes the copy then)
void scan_flush();

// GamePad-only surfaces (WWHD_GL_SKIP_GAMEPAD): see draw.cpp
bool gamepad_only(const Surface* s);
bool skip_gamepad();
struct SurfaceDesc {
    uint32_t addr = 0, mipAddr = 0, width = 0, height = 0, slices = 1, pitch = 0, mips = 1, format = 0, dim = 1,
             tileMode = 0, swizzle = 0;
    bool isDepth = false;
};

struct Renderer {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    uint64_t frame = 0;
    std::atomic<uint64_t> completed{0};
    std::unordered_multimap<uint32_t, std::unique_ptr<Surface>> surfaces;
    std::vector<Surface*> surfaceList;  // the same surfaces, for the loops over all of them
    // advances when a surface lookup could give a different answer: a surface is created, or one
    // becomes (or stops being) GPU-written. Lookups cached in draw.cpp are valid while it is unchanged.
    uint64_t surfaceEpoch = 1;
    uint64_t textureEpoch = 1;  // advances when a surface gets a new texture (rescale_surface): re-attach
    std::unique_ptr<Surface> tvScan;
    Surface* tvSource = nullptr;  // the buffer last copied to the TV scan buffer
    Surface* scanSrc = nullptr;   // ... while its copy is not made yet (present reads it directly)
    GLuint vao = 0, drawFbo = 0, readFbo = 0, blitFbo = 0;
    std::array<GLuint, 4> streams{};
    std::array<uint8_t*, 4> streamPtr{};  // persistent coherent mappings (ARB_buffer_storage), else null
    std::array<uint64_t, 4> streamLastFrame{~0ull, ~0ull, ~0ull, ~0ull};  // last frame that wrote to each
    uint64_t gpuDoneFrame = 0;  // the GPU finished every frame up to this one (per-frame fences)
    uint32_t streamIndex = 0;
    uint64_t streamGen = 1;  // advances when uploaded guest data may be stale (stream_guest)
    GLintptr streamOffset = 0;
    GLint uboAlignment = 256;
    GLint scratchUnit = 79;  // texture unit for creation/uploads, never used by draws
    uint64_t drawCount = 0, skippedDraws = 0, scanCopies = 0;
    bool timedDraw = true;  // this draw is one the per-draw timers measure (SampledTime)
    GLuint windowFbo = 0;  // presentation target: the EGL window (0) or the headless stand-in
    int windowW = 1280, windowH = 720;
    // the game's TV scan buffer has an sRGB format (GX2SetTVBuffer): scan-out encodes the linear
    // picture to sRGB, so presentation must too (the Vulkan renderer uses an sRGB swapchain)
    std::atomic<bool> tvSrgb{false};
    uint64_t stateEpoch = 1;   // advances when code outside draw() changes GL state (forget_gl_state)
    uint64_t shaderEpoch = 1;  // advances when shader lookups must be redone (reset_shader_memoization)
    // render-thread time since the last 5 s report; draw time is split into its stages
    struct Perf {
        uint64_t drawNs = 0, shaderNs = 0, uploadNs = 0, presentNs = 0, shaders = 0, uploads = 0, streamBytes = 0, reusedBytes = 0, compiled = 0, linked = 0,
                 knownProgramShaders = 0;  // translations of a program already translated (this session or a saved one)
        uint64_t lookupNs = 0, indexNs = 0, resourceNs = 0, stateNs = 0, submitNs = 0, uboBytes = 0, indexBytes = 0,
                 vertexBytes = 0, memoHits = 0;
        // draws that changed each kind of GL state (each makes Mesa revalidate that state)
        uint64_t batches = 0, batchedDraws = 0;  // multi-draws issued and the draws in them
        uint64_t rebasedDraws = 0;               // draws whose vertex buffer is bound at offset 0 (vertex rebasing)
        uint64_t chgProgram = 0, chgVertexBuffers = 0, chgTextures = 0, chgSamplers = 0, chgUbos = 0,
                 chgUniformBlocks = 0, chgAttribFormats = 0;
        uint64_t clearNs = 0, surfaceCopyNs = 0, invalidateNs = 0, scanNs = 0;  // GX2 operations other than draws
        uint64_t clears = 0, surfaceCopies = 0, cpuSurfaceCopies = 0, invalidates = 0, scans = 0;
        uint64_t gamepadDraws = 0, gamepadDrawNs = 0, gamepadSkipped = 0, gamepadClearsSkipped = 0;  // WWHD_GL_SKIP_GAMEPAD
        uint64_t copyNs = 0;         // memcpy into stream buffers (part of the draw stages)
        uint64_t fenceWaitNs = 0;    // waiting for the GPU to release a stream buffer
        uint64_t glThreadWaitNs = 0; // waiting for Mesa's GL thread to catch up (WWHD_GL_THREAD)
        uint64_t feedbackCopies = 0, textureCacheHits = 0, textureLookups = 0, comboHits = 0;
        uint64_t rescales = 0, poolHits = 0;  // render targets given another internal resolution (rescale_surface)
        uint64_t hudSwitches = 0, scanBlits = 0;  // HUD at full resolution; TV pictures copied (not presented directly)
        uint64_t flushNs = 0, flushes = 0;  // GX2Flush / GX2DrawDone
        uint64_t streamWraps = 0;
    } perf;
};
extern Renderer R;
// s is read by something other than the GamePad picture (gamepad_only)
inline void note_read(Surface* s) {
    s->readFrame = R.frame;
    if (s->gamepadSource && R.frame > s->gamepadSourceSince) s->tvShared = true;
    if (s->derivedFrom) s->derivedFrom->readFrame = R.frame;
}
inline void before_write(const Surface* s) {  // see scan_flush
    if (s && s == R.scanSrc) scan_flush();
}

// the draw path times its stages several times per draw: on the Switch the tick counter is read directly
#ifdef __SWITCH__
inline uint64_t now_ns() {
    uint64_t ticks;
    asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks * 625 / 12;  // 19.2 MHz
}
#else
inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
#endif
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
// The per-draw timers (draw stages, stream copies) time one draw in kDrawTimeSample and count it
// that many times: thousands of draws a frame each read the clock about a dozen times otherwise
// (on the desktop that was a fifth of the render thread's time).
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

void make_current();  // GX2 render thread: binds the context on first use
// draw() skips GL calls that would set state it set before; anything else that changes GL state
// (clears, blits, presentation, deleting textures) calls this so the next draw sets everything again
// issues the draws recorded for a multi-draw (draw.cpp, WWHD_GL_BATCH): anything that changes GL
// state or a resource those draws use calls this first
void flush_draws();
inline void forget_gl_state() {
    flush_draws();
    R.stateEpoch++;
}
uint64_t next_write_seq();
inline void mark_gpu_written(Surface* s) {
    if (!s->gpuWritten) {
        s->gpuWritten = true;
        R.surfaceEpoch++;
    }
    s->writeSeq = next_write_seq();
}

// Open-addressing hash table whose entries live for one stamp (a frame): entries with another stamp
// count as empty, so starting a new frame frees nothing and later inserts allocate nothing.
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

// one-frame data (vertices, indices, uniform blocks) in an orphaned streaming buffer
struct StreamSlice {
    GLuint buffer = 0;
    GLintptr offset = 0;
};
// zeroTail: that many zero bytes follow the data in the slice
StreamSlice stream_upload(const void* data, size_t size, size_t alignment, size_t zeroTail = 0);
// guest memory, uploaded once per frame per address
StreamSlice stream_guest(uint32_t addr, size_t size, size_t alignment, size_t zeroTail = 0);

// surfaces.cpp
Surface* find_or_create_surface(const SurfaceDesc& d, bool forRendering);
Surface* color_target(const uint32_t* regs, int i, uint32_t* slice);
Surface* depth_target(const uint32_t* regs, uint32_t* slice);
Surface* surface_from_color_buffer(uint32_t addr, uint32_t* firstSlice = nullptr, uint32_t* numSlices = nullptr);
Surface* surface_from_depth_buffer(uint32_t addr, uint32_t* firstSlice = nullptr, uint32_t* numSlices = nullptr);
// unique: whether it is the only surface at its address (the lookup then depends only on the words)
Surface* sampled_texture(const uint32_t* texWords, bool isDepthSampler, bool* unique = nullptr);
void upload_surface(Surface* s);
// the texture (or swizzled/retargeted view) a texture descriptor samples; target: its GL target
GLuint sampled_view(Surface* s, const uint32_t* texWords, GLenum& target);
void create_surface_texture(Surface* s);
void destroy_surface_texture(Surface* s);
// attach one level/layer of a surface to the bound framebuffer (attachment: GL_COLOR_ATTACHMENTn or depth)
void attach(GLenum fbTarget, GLenum attachment, Surface* s, uint32_t level, uint32_t layer);
GLenum depth_attachment(const Surface* s);
// copy (scaled) between two surfaces' levels/layers through framebuffer blits
// Depth copies go through the 3D engine (glBlitFramebuffer) instead of glCopyImageSubData (round 26).
// On the Switch, nouveau may keep depth buffers compressed, and a raw copy of the memory would carry the
// compressed tiles into a texture read without decompression: an effect that samples the copy (the
// shore foam, depth-aware composites) then reads wrong depth in blocks. WWHD_GL_DEPTH_COPY=0 copies as
// before. (surfaces.cpp)
bool depth_copy_by_blit();
void blit(Surface* src, uint32_t srcLevel, uint32_t srcLayer, uint32_t sw, uint32_t sh, Surface* dst,
          uint32_t dstLevel, uint32_t dstLayer, uint32_t dw, uint32_t dh);
void clear_color(const uint32_t* regs, uint32_t colorBuffer, const float rgba[4]);
void clear_depth_stencil(const uint32_t* regs, uint32_t depthBuffer, float depth, uint32_t stencil, uint32_t flags);
void copy_surface(uint32_t src, uint32_t srcMip, uint32_t srcSlice, uint32_t dst, uint32_t dstMip, uint32_t dstSlice);
void invalidate(uint32_t flags, uint32_t addr, uint32_t size);
void ss_reset_surfaces();

// shaders.cpp / draw.cpp
void draw(const uint32_t* regs, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
          uint32_t baseVertex, uint32_t instances);
void reset_shader_memoization();

// dump.cpp: debugging pictures (PNG, row 0 at the top) and statistics
void dump_surface(Surface* s, const std::string& path, bool encodeSrgb = false);
void dump_framebuffer(GLuint fbo, int width, int height, const std::string& path);
void surface_mean(Surface* s, float rgba[4]);
void framebuffer_mean(GLuint fbo, int width, int height, float rgba[4]);

// ARB_clip_control (core in 4.5; glad is generated for 4.3); null without the extension
typedef void(APIENTRYP PFNGLCLIPCONTROLPROC_WWHD)(GLenum origin, GLenum depth);
#ifndef GL_NEGATIVE_ONE_TO_ONE
#define GL_NEGATIVE_ONE_TO_ONE 0x935E
#define GL_ZERO_TO_ONE 0x935F
#endif
PFNGLCLIPCONTROLPROC_WWHD clip_control();

}  // namespace gfxgl
