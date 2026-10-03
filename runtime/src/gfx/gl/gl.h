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

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
    uint32_t dataSize = 0;
    FormatInfo fmt;
    std::unordered_map<uint32_t, GLuint> views;  // sampled views by (target, swizzle)
    std::shared_ptr<GuestLayout> guest;
};
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
    std::unique_ptr<Surface> tvScan;
    GLuint vao = 0, drawFbo = 0, readFbo = 0, blitFbo = 0;
    std::array<GLuint, 4> streams{};
    std::array<uint8_t*, 4> streamPtr{};  // persistent coherent mappings (ARB_buffer_storage), else null
    std::array<GLsync, 4> streamFence{};  // signalled when the GPU is done with a stream buffer
    uint32_t streamIndex = 0;
    uint64_t streamGen = 1;  // advances when uploaded guest data may be stale (stream_guest)
    GLintptr streamOffset = 0;
    GLint uboAlignment = 256;
    GLint scratchUnit = 79;  // texture unit for creation/uploads, never used by draws
    uint64_t drawCount = 0, skippedDraws = 0, scanCopies = 0;
    GLuint windowFbo = 0;  // presentation target: the EGL window (0) or the headless stand-in
    int windowW = 1280, windowH = 720;
    uint64_t stateEpoch = 1;   // advances when code outside draw() changes GL state (forget_gl_state)
    uint64_t shaderEpoch = 1;  // advances when shader lookups must be redone (reset_shader_memoization)
    // render-thread time since the last 5 s report; draw time is split into its stages
    struct Perf {
        uint64_t drawNs = 0, shaderNs = 0, uploadNs = 0, presentNs = 0, shaders = 0, uploads = 0, streamBytes = 0, reusedBytes = 0, compiled = 0, linked = 0;
        uint64_t lookupNs = 0, indexNs = 0, resourceNs = 0, stateNs = 0, submitNs = 0, uboBytes = 0, indexBytes = 0,
                 vertexBytes = 0, memoHits = 0;
    } perf;
};
extern Renderer R;

inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct ScopedTime {
    uint64_t& total;
    uint64_t start = now_ns();
    ~ScopedTime() { total += now_ns() - start; }
};

void make_current();  // GX2 render thread: binds the context on first use
// draw() skips GL calls that would set state it set before; anything else that changes GL state
// (clears, blits, presentation, deleting textures) calls this so the next draw sets everything again
inline void forget_gl_state() { R.stateEpoch++; }
uint64_t next_write_seq();
inline void mark_gpu_written(Surface* s) {
    s->gpuWritten = true;
    s->writeSeq = next_write_seq();
}

// one-frame data (vertices, indices, uniform blocks) in an orphaned streaming buffer
struct StreamSlice {
    GLuint buffer = 0;
    GLintptr offset = 0;
};
StreamSlice stream_upload(const void* data, size_t size, size_t alignment);
// guest memory, uploaded once per frame per address
StreamSlice stream_guest(uint32_t addr, size_t size, size_t alignment);

// surfaces.cpp
Surface* find_or_create_surface(const SurfaceDesc& d, bool forRendering);
Surface* color_target(const uint32_t* regs, int i, uint32_t* slice);
Surface* depth_target(const uint32_t* regs, uint32_t* slice);
Surface* surface_from_color_buffer(uint32_t addr, uint32_t* firstSlice = nullptr, uint32_t* numSlices = nullptr);
Surface* surface_from_depth_buffer(uint32_t addr, uint32_t* firstSlice = nullptr, uint32_t* numSlices = nullptr);
Surface* sampled_texture(const uint32_t* texWords, bool isDepthSampler);
void upload_surface(Surface* s);
// the texture (or swizzled/retargeted view) a texture descriptor samples; target: its GL target
GLuint sampled_view(Surface* s, const uint32_t* texWords, GLenum& target);
void create_surface_texture(Surface* s);
void destroy_surface_texture(Surface* s);
// attach one level/layer of a surface to the bound framebuffer (attachment: GL_COLOR_ATTACHMENTn or depth)
void attach(GLenum fbTarget, GLenum attachment, Surface* s, uint32_t level, uint32_t layer);
GLenum depth_attachment(const Surface* s);
// copy (scaled) between two surfaces' levels/layers through framebuffer blits
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
void dump_surface(Surface* s, const std::string& path);
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
