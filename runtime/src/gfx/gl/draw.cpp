// OpenGL draw submission: GX2 register state -> GL state (the translation of gfx/vulkan/draw.cpp).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "gl.h"
#include "runtime.h"
#include "shaders.h"

using namespace Latte;
extern "C" uint64_t g_shader_state_gen;  // gx2_core.cpp: bumped by shader-relevant register changes
extern "C" uint64_t g_shader_regs_gen;   // the same, except for changes of shader program registers
namespace gfxgl {
PFNGLCLIPCONTROLPROC_WWHD clip_control() {
    static auto fn = (PFNGLCLIPCONTROLPROC_WWHD)eglGetProcAddress("glClipControl");
    return fn;
}

namespace {
float f32(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}
GLenum blend_factor(uint32_t v) {
    static const GLenum t[] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                               GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
                               GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                               GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR,
                               GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA};
    return v < std::size(t) ? t[v] : GL_ONE;
}
GLenum blend_op(uint32_t v) {
    static const GLenum t[] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_REVERSE_SUBTRACT};
    return v < 5 ? t[v] : GL_FUNC_ADD;
}
GLenum stencil_op(uint32_t v) {
    static const GLenum t[] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
    return t[v & 7];
}
GLenum logic_op(uint32_t rop) {
    switch (rop) {
    case 0x00: return GL_CLEAR;
    case 0x88: return GL_AND;
    case 0x44: return GL_AND_REVERSE;
    case 0x22: return GL_AND_INVERTED;
    case 0xAA: return GL_NOOP;
    case 0x66: return GL_XOR;
    case 0xEE: return GL_OR;
    case 0x11: return GL_NOR;
    case 0x99: return GL_EQUIV;
    case 0x55: return GL_INVERT;
    case 0xDD: return GL_OR_REVERSE;
    case 0x33: return GL_COPY_INVERTED;
    case 0xBB: return GL_OR_INVERTED;
    case 0x77: return GL_NAND;
    case 0xFF: return GL_SET;
    default: return GL_COPY;
    }
}
// raw unsigned fetch of each Latte vertex format; the GLSL decodes endianness and type
bool vertex_format(E_HWFMT f, GLint& comps, GLenum& type, uint32_t& bytes) {
    switch (f) {
    case E_HWFMT::HWFMT_32_32_32_32_FLOAT: case E_HWFMT::HWFMT_32_32_32_32: comps = 4; type = GL_UNSIGNED_INT; break;
    case E_HWFMT::HWFMT_32_32_32_FLOAT: case E_HWFMT::HWFMT_32_32_32: comps = 3; type = GL_UNSIGNED_INT; break;
    case E_HWFMT::HWFMT_32_32_FLOAT: case E_HWFMT::HWFMT_32_32: comps = 2; type = GL_UNSIGNED_INT; break;
    case E_HWFMT::HWFMT_32_FLOAT: case E_HWFMT::HWFMT_32: case E_HWFMT::HWFMT_2_10_10_10:
        comps = 1; type = GL_UNSIGNED_INT; break;
    case E_HWFMT::HWFMT_8_8_8_8: comps = 4; type = GL_UNSIGNED_BYTE; break;
    case E_HWFMT::HWFMT_8_8_8: comps = 3; type = GL_UNSIGNED_BYTE; break;
    case E_HWFMT::HWFMT_8_8: comps = 2; type = GL_UNSIGNED_BYTE; break;
    case E_HWFMT::HWFMT_8: comps = 1; type = GL_UNSIGNED_BYTE; break;
    case E_HWFMT::HWFMT_16_16_16_16: case E_HWFMT::HWFMT_16_16_16_16_FLOAT: comps = 4; type = GL_UNSIGNED_SHORT; break;
    case E_HWFMT::HWFMT_16_16_16: case E_HWFMT::HWFMT_16_16_16_FLOAT: comps = 3; type = GL_UNSIGNED_SHORT; break;
    case E_HWFMT::HWFMT_16_16: case E_HWFMT::HWFMT_16_16_FLOAT: comps = 2; type = GL_UNSIGNED_SHORT; break;
    case E_HWFMT::HWFMT_16: case E_HWFMT::HWFMT_16_FLOAT: comps = 1; type = GL_UNSIGNED_SHORT; break;
    default: return false;
    }
    bytes = comps * (type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1);
    return true;
}

GLuint sampler(const uint32_t* words, bool compare, bool integer) {
    struct Key {
        uint32_t w0, w1, w2, flags;
        bool operator==(const Key& o) const { return !memcmp(this, &o, sizeof o); }
    };
    struct Hash {
        size_t operator()(const Key& k) const {
            uint64_t a = (uint64_t(k.w0) << 32 | k.w1) * 0x9E3779B97F4A7C15ull, b = (uint64_t(k.w2) << 32 | k.flags);
            return size_t(a ^ (b * 0xFF51AFD7ED558CCDull) ^ (a >> 29));
        }
    };
    static std::unordered_map<Key, GLuint, Hash> cache;
    const Key key{words[0], words[1], words[2], uint32_t(compare) | uint32_t(integer) << 1};
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    LATTE_SQ_TEX_SAMPLER_WORD0_0 w;
    LATTE_SQ_TEX_SAMPLER_WORD1_0 w1;
    memcpy(&w, words, 4);
    memcpy(&w1, words + 1, 4);
    auto linear = [](uint32_t v) { return !(v == 0 || v == 4); };
    auto wrap = [](uint32_t v) -> GLint {
        switch (v) {
        case 0: return GL_REPEAT;
        case 1: return GL_MIRRORED_REPEAT;
        case 2: return GL_CLAMP_TO_EDGE;
        case 3: case 5: case 7: return 0x8743;  // GL_MIRROR_CLAMP_TO_EDGE (ARB_texture_mirror_clamp_to_edge)
        default: return GL_CLAMP_TO_BORDER;
        }
    };
    bool mag = !integer && linear(uint32_t(w.get_XY_MAG_FILTER()));
    bool min = !integer && linear(uint32_t(w.get_XY_MIN_FILTER()));
    uint32_t mip = integer ? 1 : uint32_t(w.get_MIP_FILTER());
    GLint minFilter = mip == 0   ? (min ? GL_LINEAR : GL_NEAREST)
                      : mip == 2 ? (min ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR)
                                 : (min ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST);
    GLuint s = gen_sampler_name();
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, minFilter);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, wrap(uint32_t(w.get_CLAMP_X())));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, wrap(uint32_t(w.get_CLAMP_Y())));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R, wrap(uint32_t(w.get_CLAMP_Z())));
    glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS, float(w1.get_LOD_BIAS()) / 64.f);
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, w1.get_MIN_LOD() / 64.f);
    glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, uint32_t(w.get_MIP_FILTER()) ? w1.get_MAX_LOD() / 64.f : 0.f);
    uint32_t border = uint32_t(w.get_BORDER_COLOR_TYPE());
    static const GLfloat colors[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};
    glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, colors[border < 3 ? border : 0]);
    if (compare) {
        glSamplerParameteri(s, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(s, GL_TEXTURE_COMPARE_FUNC, GL_NEVER + uint32_t(w.get_DEPTH_COMPARE_FUNCTION()));
    }
    cache.emplace(key, s);
    return s;
}

// ---- GL state the draws set, so a draw only issues the calls whose values changed. Mesa runs
// every call (context lookup, vertex flush, dirty flags) even when the value is the same, and on
// the Switch that driver time is most of a draw. Code outside draw() calls forget_gl_state().
enum Cap { kScissor, kDepthClamp, kCull, kPolyOffset, kDepthTest, kStencilTest, kSrgb, kLogicOp, kRestart, kNearClip, kCapCount };
constexpr GLenum kCapEnums[kCapCount] = {GL_SCISSOR_TEST, GL_DEPTH_CLAMP, GL_CULL_FACE, GL_POLYGON_OFFSET_FILL, GL_DEPTH_TEST,
                                         GL_STENCIL_TEST, GL_FRAMEBUFFER_SRGB, GL_COLOR_LOGIC_OP,
                                         GL_PRIMITIVE_RESTART_FIXED_INDEX, GL_CLIP_DISTANCE0};
// a GL state change: the draws recorded for one multi-draw (flush_draws) need the state as it was
#define FLUSHED(f) (flush_draws(), f)
constexpr int kTexUnits = 96, kUboBindings = 64, kAttribs = 32, kVertexBindings = 16;

// Compared field by field: a memcmp of a struct just written on the stack stalls on store
// forwarding. Floats compare by bits, so the cache's "unknown" fill (all-ones: NaN) never matches.
inline bool same(float a, float b) {
    uint32_t x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    return x == y;
}
struct Viewport {
    GLenum origin, depthMode;
    float rect[4], range[2];
    bool operator==(const Viewport& o) const {
        return origin == o.origin && depthMode == o.depthMode && same(rect[0], o.rect[0]) && same(rect[1], o.rect[1]) &&
               same(rect[2], o.rect[2]) && same(rect[3], o.rect[3]) && same(range[0], o.range[0]) && same(range[1], o.range[1]);
    }
};
struct Raster {
    GLenum cullFace, frontFace;
    float offset[2];
    bool operator==(const Raster& o) const {
        return cullFace == o.cullFace && frontFace == o.frontFace && same(offset[0], o.offset[0]) && same(offset[1], o.offset[1]);
    }
};
struct DepthState {
    GLenum func;
    GLboolean mask;
    bool operator==(const DepthState& o) const { return func == o.func && mask == o.mask; }
};
struct StencilState {
    uint32_t v[12];
    bool operator==(const StencilState& o) const {
        for (int i = 0; i < 10; i++)  // entries 10, 11 are unused
            if (v[i] != o.v[i]) return false;
        return true;
    }
};
struct BlendState {
    uint32_t on, src, dst, srcA, dstA, op, opA;
    bool operator==(const BlendState& o) const {
        return on == o.on && src == o.src && dst == o.dst && srcA == o.srcA && dstA == o.dstA && op == o.op && opA == o.opA;
    }
};
struct TexState {
    GLuint tex;
    GLenum target;
    GLuint sampler;
};
struct UboState {
    GLuint buffer;
    GLintptr offset;
    GLsizeiptr size;
    bool operator==(const UboState& o) const { return buffer == o.buffer && offset == o.offset && size == o.size; }
};
struct AttribState {
    GLint comps;
    GLenum type;
    GLuint offset, binding;
    bool operator==(const AttribState& o) const {
        return comps == o.comps && type == o.type && offset == o.offset && binding == o.binding;
    }
};
struct VertexBindingState {
    GLuint buffer;
    GLintptr offset;
    GLsizei stride;
    GLuint divisor;
    bool operator==(const VertexBindingState& o) const {
        return buffer == o.buffer && offset == o.offset && stride == o.stride && divisor == o.divisor;
    }
};
struct StateCache {
    uint64_t epoch = 0;
    int8_t cap[kCapCount];
    GLuint program, elementBuffer;
    uint32_t fbo;
    Viewport viewport;
    GLint scissor[4];
    Raster raster;
    DepthState depth;
    StencilState stencil;
    uint32_t colorMask[8];
    BlendState blend[8];
    float blendColor[4];
    GLenum logicOp;
    TexState tex[kTexUnits];
    UboState ubo[kUboBindings];
    AttribState attrib[kAttribs];
    VertexBindingState binding[kVertexBindings];
} gs;

// every cached value becomes "unknown" (all-ones bytes never equal a real setting)
void sync_cache() {
    static const bool off = getenv("WWHD_GL_NO_STATE_CACHE") != nullptr;
    if (gs.epoch == R.stateEpoch && !off) return;
    memset(&gs, 0xFF, sizeof gs);
    gs.epoch = R.stateEpoch;
}
template <class T> bool changed(T& cached, const T& value) {
    if (cached == value) return false;
    cached = value;
    return true;
}
void cap(Cap c, bool on) {
    if (gs.cap[c] == int8_t(on)) return;
    gs.cap[c] = int8_t(on);
    if (on) FLUSHED(glEnable)(kCapEnums[c]);
    else FLUSHED(glDisable)(kCapEnums[c]);
}

// a snapshot of a surface that this draw also renders to (sampling an attached texture is undefined)
Surface* feedback_copy(Surface* s) {
    static std::unordered_map<Surface*, std::pair<uint64_t, std::unique_ptr<Surface>>> copies;
    auto& [seq, copy] = copies[s];
    if (copy && copy->scale != s->scale) {  // s was rescaled
        rescale_surface(copy.get(), s->scale, false);
        seq = 0;
    }
    if (!copy) {
        copy = std::make_unique<Surface>();
        copy->width = s->width;
        copy->height = s->height;
        copy->slices = s->slices;
        copy->dim = s->dim;
        copy->format = s->format;
        copy->isDepth = s->isDepth;
        copy->fmt = s->fmt;
        copy->mips = 1;
        copy->gpuWritten = true;
        copy->scale = s->scale;
        create_surface_texture(copy.get());
        seq = 0;
    }
    if (seq != s->writeSeq) {
        flush_draws();  // the copy must include what recorded draws render into s
        R.perf.feedbackCopies++;
        if (g_traceFrame) trace_event("feedback copy of %s", trace_name(s).c_str());
        if (s->fmt.depth && depth_copy_by_blit())
            for (uint32_t layer = 0; layer < (s->target == GL_TEXTURE_2D ? 1u : s->layers); layer++)
                blit(s, 0, layer, s->width, s->height, copy.get(), 0, layer, s->width, s->height);
        else
            glCopyImageSubData(s->tex, s->target, 0, 0, 0, 0, copy->tex, copy->target, 0, 0, 0, 0, s->pw, s->ph,
                               s->target == GL_TEXTURE_2D ? 1 : s->layers);
        seq = s->writeSeq;
    }
    return copy.get();
}

struct TextureBinding {
    GLuint unit, texture, sampler;
    GLenum target;
};
struct TextureCacheEntry {
    uint64_t epoch = 0;  // R.surfaceEpoch when filled; 0: not reusable
    uint32_t words[7], sampler[3];
    bool compare = false;
    Surface* s = nullptr;
    GLenum target = 0;
    GLuint view = 0, smp = 0;
};
TextureCacheEntry textureCache[2][LATTE_NUM_MAX_TEX_UNITS];  // pixel, vertex
const bool textureCacheOn = getenv("WWHD_GL_NO_TEXTURE_CACHE") == nullptr;
struct UboBinding {
    GLuint binding;
    StreamSlice slice;
    GLsizeiptr size;
};

// ---- ARB_multi_bind (core in 4.4; glad is generated for 4.3): one call binds a range of texture
// units, samplers, uniform buffers or vertex buffers. Slots inside the range that this draw does not
// use get their cached binding again (or 0 when unknown). WWHD_GL_NO_MULTIBIND=1 binds one by one.
struct MultiBind {
    void(APIENTRYP textures)(GLuint first, GLsizei count, const GLuint* textures) = nullptr;
    void(APIENTRYP samplers)(GLuint first, GLsizei count, const GLuint* samplers) = nullptr;
    void(APIENTRYP buffersRange)(GLenum target, GLuint first, GLsizei count, const GLuint* buffers,
                                 const GLintptr* offsets, const GLsizeiptr* sizes) = nullptr;
    void(APIENTRYP vertexBuffers)(GLuint first, GLsizei count, const GLuint* buffers, const GLintptr* offsets,
                                  const GLsizei* strides) = nullptr;
    bool on = false;
    MultiBind() {
        if (getenv("WWHD_GL_NO_MULTIBIND")) return;
        textures = (decltype(textures))eglGetProcAddress("glBindTextures");
        samplers = (decltype(samplers))eglGetProcAddress("glBindSamplers");
        buffersRange = (decltype(buffersRange))eglGetProcAddress("glBindBuffersRange");
        vertexBuffers = (decltype(vertexBuffers))eglGetProcAddress("glBindVertexBuffers");
        on = textures && samplers && buffersRange && vertexBuffers;
    }
};
const MultiBind& multi_bind() {
    static const MultiBind m;  // first used on the render thread, with the context current
    return m;
}
constexpr uint32_t kUnknown = 0xFFFFFFFFu;  // a state-cache value after forget_gl_state()

// a dirty range [lo, hi] within one group of slots
struct Range {
    uint32_t lo = ~0u, hi = 0;
    void add(uint32_t i) { lo = std::min(lo, i); hi = std::max(hi, i); }
    bool empty() const { return lo > hi; }
};

void bind_textures(const std::vector<TextureBinding>& textures) {
    const auto& mb = multi_bind();
    Range tex[2], smp[2];  // pixel units 0-31, vertex units 32+
    bool texChanged = false, smpChanged = false;
    for (auto& t : textures) {
        if (t.unit >= (GLuint)kTexUnits) {
            FLUSHED(glActiveTexture)(GL_TEXTURE0 + t.unit);
            FLUSHED(glBindTexture)(t.target, t.texture);
            FLUSHED(glBindSampler)(t.unit, t.sampler);
            continue;
        }
        TexState& cached = gs.tex[t.unit];
        const int group = t.unit >= 32;
        if (cached.tex != t.texture || cached.target != t.target) {
            texChanged = true;
            if (mb.on) tex[group].add(t.unit);
            else {
                FLUSHED(glActiveTexture)(GL_TEXTURE0 + t.unit);
                FLUSHED(glBindTexture)(t.target, t.texture);
            }
            cached.tex = t.texture;
            cached.target = t.target;
        }
        if (cached.sampler != t.sampler) {
            smpChanged = true;
            if (mb.on) smp[group].add(t.unit);
            else FLUSHED(glBindSampler)(t.unit, t.sampler);
            cached.sampler = t.sampler;
        }
    }
    R.perf.chgTextures += texChanged;
    R.perf.chgSamplers += smpChanged;
    if (!mb.on) return;
    GLuint names[kTexUnits];
    for (int g = 0; g < 2; g++) {
        if (!tex[g].empty()) {
            for (uint32_t u = tex[g].lo; u <= tex[g].hi; u++) {
                if (gs.tex[u].tex == kUnknown) gs.tex[u].tex = gs.tex[u].target = 0;  // unbinds every target
                names[u - tex[g].lo] = gs.tex[u].tex;
            }
            FLUSHED(mb.textures)(tex[g].lo, GLsizei(tex[g].hi - tex[g].lo + 1), names);
        }
        if (!smp[g].empty()) {
            for (uint32_t u = smp[g].lo; u <= smp[g].hi; u++) {
                if (gs.tex[u].sampler == kUnknown) gs.tex[u].sampler = 0;
                names[u - smp[g].lo] = gs.tex[u].sampler;
            }
            FLUSHED(mb.samplers)(smp[g].lo, GLsizei(smp[g].hi - smp[g].lo + 1), names);
        }
    }
}

void bind_ubos(const std::vector<UboBinding>& ubos) {
    const auto& mb = multi_bind();
    Range dirty[2];  // vertex bindings 0-15, pixel bindings 32-47
    bool any = false;
    for (auto& u : ubos) {
        UboState v = UboState{};
        v.buffer = u.slice.buffer;
        v.offset = u.slice.offset;
        v.size = u.size;
        if (u.binding >= (GLuint)kUboBindings) {
            FLUSHED(glBindBufferRange)(GL_UNIFORM_BUFFER, u.binding, v.buffer, v.offset, v.size);
            continue;
        }
        if (!changed(gs.ubo[u.binding], v)) continue;
        any = true;
        if (mb.on) dirty[u.binding >= 32].add(u.binding);
        else FLUSHED(glBindBufferRange)(GL_UNIFORM_BUFFER, u.binding, v.buffer, v.offset, v.size);
    }
    R.perf.chgUbos += any;
    if (!mb.on) return;
    GLuint buffers[32];
    GLintptr offsets[32];
    GLsizeiptr sizes[32];
    for (auto& d : dirty) {
        if (d.empty()) continue;
        for (uint32_t b = d.lo; b <= d.hi; b++) {
            UboState& c = gs.ubo[b];
            if (c.buffer == kUnknown) c = UboState{};  // unbound
            buffers[b - d.lo] = c.buffer;
            offsets[b - d.lo] = c.offset;
            sizes[b - d.lo] = c.buffer ? c.size : 0;
        }
        FLUSHED(mb.buffersRange)(GL_UNIFORM_BUFFER, d.lo, GLsizei(d.hi - d.lo + 1), buffers, offsets, sizes);
    }
}


// A program's uniform blocks are declared as large as its highest possible index (skinning palettes
// as vec4[4096], 64 KB), while the guest block is usually a fraction of that. Only the guest's bytes
// are copied and bound; reads past a bound range return zero on NVIDIA hardware and in Mesa's
// software rasterizer, as the zero-filled copy did. WWHD_GL_FULL_UBO=1 copies the declared size.
GLuint zero_buffer() {
    static GLuint buffer = [] {
        GLuint b;
        glGenBuffers(1, &b);
        glBindBuffer(GL_COPY_WRITE_BUFFER, b);
        std::vector<uint8_t> zeros(0x10000, 0);
        glBufferData(GL_COPY_WRITE_BUFFER, zeros.size(), zeros.data(), GL_STATIC_DRAW);
        return b;
    }();
    return buffer;
}

// Ambient-occlusion quirks, as in the Metal and Vulkan renderers (WWHD_AO_MODE=0..2 chooses one;
// WWHD_NO_AO_QUIRK=1 means 0). The game downsamples the scene depth to 640x360 and computes ambient
// occlusion from it at 960x540 (vertex shader 44BDF900, pixel shader 44BDFD00), then blurs it into
// the shadow mask:
//   0 = as the hardware renders it: the occlusion pass point-samples its centre depth, and every
//       third row and column lands half a texel off, which shows as screen-fixed lines (and, with
//       the noise below, grainy bands) on sloped ground in shadow
//   1 = that one fetch is bilinear, like the pass's neighbour fetches: the lines go
//   2 = (default) 1, and the 4x4 noise texture is tiled per 960x540 pixel instead of per 640x360
//       pixel, so the game's blur averages it out and no uneven bands remain
constexpr uint32_t kOcclusionVS = 0x44BDF900, kOcclusionPS = 0x44BDFD00;
const int g_aoMode = [] {
    if (const char* e = getenv("WWHD_AO_MODE")) return ((atoi(e) % 3) + 3) % 3;
    return getenv("WWHD_NO_AO_QUIRK") ? 0 : 2;
}();
}  // namespace
int ao_mode() { return g_aoMode; }
namespace {
// The occlusion pass's program in this stage: at its address and with its contents (size and hash of
// the guest program). Another area could load another program at that address; it is left as it is.
constexpr uint32_t kOcclusionSize[2] = {1584, 384};  // pixel, vertex (Outset, US v0)
constexpr uint64_t kOcclusionHash[2] = {0x26870ca3f2e34dfaull, 0x36e37317f62658e9ull};
bool is_occlusion(const uint32_t* r, bool vertex) {
    const uint32_t reg = vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
    const uint32_t addr = r[reg] << 8, size = r[reg + 1] << 3;
    if (addr != (vertex ? kOcclusionVS : kOcclusionPS)) return false;
    static uint64_t frame[2] = {~0ull, ~0ull};
    static uint32_t sizeSeen[2] = {};
    static bool result[2] = {};
    if (frame[vertex] == R.frame && sizeSeen[vertex] == size) return result[vertex];
    const uint64_t h = program_hash_of(program_hash_ref(addr, size), addr, size, R.frame);
    const bool match = size == kOcclusionSize[vertex] && h == kOcclusionHash[vertex];
    if (match != result[vertex] || frame[vertex] == ~0ull)
        LOG("[gl] %s program at %08X (size %u, hash %016llx): %s", vertex ? "vertex" : "pixel", addr, size,
            (unsigned long long)h, match ? "the occlusion pass, AO fix applied" : "not the occlusion pass, AO fix not applied");
    frame[vertex] = R.frame;
    sizeSeen[vertex] = size;
    result[vertex] = match;
    return match;
}
// AO mode 2: the occlusion vertex shader's first remapped constant, .w (the noise tiling) x1.5. src is
// the constant's 16 bytes; the patched copy goes to tmp.
inline const void* ao_noise_constant(const Shader* sh, const uint32_t* r, uint32_t mappedOffset, const void* src,
                                     uint8_t (&tmp)[16]) {
    if (mappedOffset != 0 || g_aoMode != 2 || !sh->vertex || !is_occlusion(r, true)) return src;
    memcpy(tmp, src, 16);
    float w;
    memcpy(&w, tmp + 12, 4);
    w *= 1.5f;
    memcpy(tmp + 12, &w, 4);
    return tmp;
}

// WWHD_GL_TRACE_DRAWS=1 (testing, with WWHD_GL_TRACE_FRAMES): every draw of a traced frame in the log,
// with its shaders' GLSL hashes (sources: shadercache_gl.bin), depth state and textures
const bool g_traceDraws = [] {
    const char* e = getenv("WWHD_GL_TRACE_DRAWS");
    return e && *e && strcmp(e, "0") != 0;
}();
std::string g_traceTextures;  // the textures of the draw being prepared (g_traceDraws)
// uniform blocks and textures of one stage; textures are resolved (and uploaded) before any is bound
bool g_gamepadDrawing = false;  // the draw being prepared renders the GamePad picture (draw_impl)
// internal resolution of the draw being prepared: its render targets' scale, and each texture unit's
// texture pixels per guest pixel (programs with uf_fragCoordScale / uf_texNScale only)
float g_drawScale = 1.0f;
auto g_unitScale = [] {
    std::array<std::array<std::array<float, 2>, LATTE_NUM_MAX_TEX_UNITS>, 2> a;
    for (auto& stage : a)
        for (auto& unit : stage) unit = {1.0f, 1.0f};
    return a;
}();
bool g_drawSamplesRendered = false;  // the draw samples a texture the GPU rendered

void prepare_stage(const uint32_t* r, Shader* sh, Program* p, const std::array<Surface*, 8>& colors, Surface* depth,
                   std::vector<TextureBinding>& textures, std::vector<UboBinding>& ubos) {
    static const bool fullUbo = getenv("WWHD_GL_FULL_UBO") != nullptr;
    auto& m = sh->mapping;
    uint32_t block = sh->vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
    static std::vector<uint8_t> scratch;
    for (int i = 0; i < 16; i++) {
        int binding = m.uniformBuffersBindingPoint[i];
        if (binding < 0 || binding >= (int)p->blockSize.size() || !p->blockSize[binding]) continue;
        uint32_t addr = r[block + i * 7], size = std::min<uint32_t>(r[block + i * 7 + 1] + 1, 0x10000);
        uint32_t need = (uint32_t)p->blockSize[binding];
        StreamSlice slice;
        GLsizeiptr bound = need;
        if (!addr) {
            slice = {zero_buffer(), 0};
            bound = std::min<GLsizeiptr>(need, 0x10000);
        }
        else if (size >= need)
            slice = stream_guest(addr, need, R.uboAlignment);
        else if (!fullUbo) {
            // the guest's bytes, then zeros up to a 256-byte multiple: shaders must read zeros past
            // the guest's data (as with the full zero-filled copy), and nouveau rounds a constant
            // buffer's bound size up to 256 bytes, so unpadded it would read the next upload's data
            bound = std::min<GLsizeiptr>((size + 255) & ~255u, std::max<GLsizeiptr>(need, size));
            slice = stream_guest(addr, size, R.uboAlignment, size_t(bound) - size);
        } else {
            scratch.assign(need, 0);
            memcpy(scratch.data(), mem::ptr(addr), size);
            slice = stream_upload(scratch.data(), need, R.uboAlignment);
        }
        R.perf.uboBytes += bound;
        ubos.push_back({(GLuint)binding, slice, bound});
    }
    uint32_t texbase = sh->vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
    for (int i = 0; i < sh->dec->textureUnitListCount; i++) {
        uint32_t unit = sh->dec->textureUnitList[i];
        if (unit >= LATTE_NUM_MAX_TEX_UNITS) continue;
        int binding = m.textureUnitToBindingPoint[unit];
        uint32_t samplerId = sh->dec->textureUnitSamplerAssignment[unit];
        if (binding < 0 || samplerId >= 18) continue;
        const uint32_t* words = r + texbase + unit * 7;
        const uint32_t* samplerWords = r + REGADDR::SQ_TEX_SAMPLER_WORD0_0 + ((sh->vertex ? 18 : 0) + samplerId) * 3;
        uint32_t aoSampler[3];
        if (g_aoMode >= 1 && unit == 0 && !sh->vertex && is_occlusion(r, false)) {
            // AO quirk 1: the occlusion pass's centre depth fetch, bilinear (XY mag/min filter)
            memcpy(aoSampler, samplerWords, sizeof aoSampler);
            aoSampler[0] = (aoSampler[0] & ~0x7E00u) | (1u << 9) | (1u << 12);
            samplerWords = aoSampler;
        }
        const bool compare = sh->dec->textureUsesDepthCompare[unit];
        // the last lookup for this unit, reused while its words and the surface set are unchanged
        TextureCacheEntry& cached = textureCache[sh->vertex ? 1 : 0][unit];
        R.perf.textureLookups++;
        Surface* s;
        GLenum target;
        GLuint view, smp;
        if (textureCacheOn && cached.epoch == R.surfaceEpoch && cached.compare == compare &&
            !memcmp(cached.words, words, sizeof cached.words) && !memcmp(cached.sampler, samplerWords, sizeof cached.sampler)) {
            R.perf.textureCacheHits++;
            s = cached.s;
            upload_surface(s);  // once per frame: CPU changes to the texture
            target = cached.target;
            view = cached.view;
            smp = cached.smp;
        } else {
            bool unique = false;
            s = sampled_texture(words, compare, &unique);
            if (!s) continue;
            view = sampled_view(s, words, target);
            smp = sampler(samplerWords, compare, s->fmt.kind != FormatInfo::FLOAT);
            cached.epoch = unique ? R.surfaceEpoch : 0;  // several surfaces at one address: chosen by recency
            memcpy(cached.words, words, sizeof cached.words);
            memcpy(cached.sampler, samplerWords, sizeof cached.sampler);
            cached.compare = compare;
            cached.s = s;
            cached.target = target;
            cached.view = view;
            cached.smp = smp;
        }
        if (g_gamepadDrawing) {  // a texture of the GamePad picture
            if (!s->gamepadSource) s->gamepadSourceSince = R.frame;
            s->gamepadSource = true;
        } else if (s->drcScanFrame != ~0ull && gamepad_only(s)) {
            // the GamePad picture itself read by another draw: at an area change the game captures it into
            // a buffer of its own for the GamePad's fade, which only GamePad-picture draws then sample. It
            // counts as a read once something else reads what this draw renders. (Round 21 did this for
            // every GamePad-only surface: a texture the TV also needs could then stay GamePad-only.)
            for (auto* c : colors)
                if (c && c != s) c->derivedFrom = s;
        } else
            note_read(s);
        if (g_traceFrame) {
            trace_draw(s);
            if (g_traceDraws || g_captureDraws) {
                char t[160];
                snprintf(t, sizeof t, " %s%u=%s(smp %08X %08X %08X%s)", sh->vertex ? "vt" : "t", unit, trace_name(s).c_str(),
                         samplerWords[0], samplerWords[1], samplerWords[2], compare ? " cmp" : "");
                g_traceTextures += t;
            }
        }
        bool aliases = depth && s == depth;
        for (auto* c : colors)
            if (c && c == s) aliases = true;
        if (aliases) {
            s = feedback_copy(s);
            view = sampled_view(s, words, target);
            smp = sampler(samplerWords, compare, s->fmt.kind != FormatInfo::FLOAT);
        }
        if (s->gpuWritten) g_drawSamplesRendered = true;
        if (p->scaleUniforms) {
            g_unitScale[sh->vertex][unit][0] = float(s->pw) / float(s->width);
            g_unitScale[sh->vertex][unit][1] = float(s->ph) / float(s->height);
        }
        textures.push_back({(GLuint)binding, view, smp, target});
    }
}

// loose uniforms, skipped when the program already holds the same values (glUniform* writes the
// bound program's storage, which keeps its values across program switches)
// a stage's uniform-variable block (shaders.h UniformVarBlock): the same values set_uniforms gives
// the loose uniforms, written into the block's copy; a changed copy goes to the stream buffer
void set_uniform_block(const uint32_t* r, Shader* sh, UniformVarBlock& b, bool deferred) {
    auto* dec = sh->dec;
    const uint32_t aluBase = mmSQ_ALU_CONSTANT0_0 + (sh->vertex ? 0x400 : 0);
    const uint32_t blockBase = sh->vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
    const size_t size = b.data.size();
    uint8_t* data = b.data.data();
    bool dirty = false;
    // a batched draw's copy is always taken (record_draw), so its values are written without comparing
    auto put = [&](GLint offset, const void* src, size_t bytes) {
        if (offset < 0 || size_t(offset) + bytes > size) return;
        if (!deferred && !memcmp(data + offset, src, bytes)) return;
        memcpy(data + offset, src, bytes);
        dirty = true;
    };
    if (b.remapped >= 0) {
        static const uint8_t zeros[16] = {};
        uint8_t ao[16];
        for (const auto& e : dec->list_remappedUniformEntries_register)
            put(b.remapped + GLint(e.mappedIndexOffset),
                ao_noise_constant(sh, r, e.mappedIndexOffset, r + aluBase + e.indexOffset / 4, ao), 16);
        for (const auto& g : dec->list_remappedUniformEntries_bufferGroups) {
            uint32_t address = r[blockBase + g.kcacheBankIdOffset / 4];
            for (const auto& e : g.entries)
                put(b.remapped + GLint(e.mappedIndexOffset),
                    ao_noise_constant(sh, r, e.mappedIndexOffset, address ? ppc_ptr(address + e.indexOffset) : zeros, ao),
                    16);
        }
    }
    if (b.registers >= 0 && sh->registerCount) put(b.registers, r + aluBase, size_t(sh->registerCount) * 16);
    if (sh->vertex) {
        if (b.pointSize >= 0) {
            float point = float(r[REGADDR::PA_SU_POINT_SIZE] & 0xFFFF) / 8.0f;
            point = (point == 0 ? 0.125f : point) * g_drawScale;
            put(b.pointSize, &point, 4);
        }
        if (b.windowToClip >= 0) {
            float width = 2.0f * f32(r[REGADDR::PA_CL_VPORT_XSCALE]), height = -2.0f * f32(r[REGADDR::PA_CL_VPORT_YSCALE]);
            float v[2] = {width != 0 ? 2.0f / width : 0, height != 0 ? 2.0f / height : 0};
            put(b.windowToClip, v, 8);
        }
    } else if (b.alphaRef >= 0) {
        float ref = f32(r[REGADDR::SX_ALPHA_REF]);
        put(b.alphaRef, &ref, 4);
    }
    if (!sh->vertex && b.fragCoordScale >= 0) {  // gl_FragCoord in guest pixels
        const float v[2] = {1.0f / g_drawScale, 1.0f / g_drawScale};
        put(b.fragCoordScale, v, 8);
    }
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++)
        if (b.texScale[t] >= 0) put(b.texScale[t], g_unitScale[sh->vertex][t].data(), 8);
    if (deferred) return;  // the multi-draw uploads every recorded draw's copy (flush_draws)
    if (dirty || b.gen != R.streamGen) {
        b.slice = stream_upload(data, size, R.uboAlignment);
        b.gen = R.streamGen;
        R.perf.uboBytes += size;
    }
    const GLuint binding = sh->vertex ? kUniformVarBindingVS : kUniformVarBindingPS;
    UboState v = UboState{};
    v.buffer = b.slice.buffer;
    v.offset = b.slice.offset;
    v.size = GLsizeiptr(size);
    if (changed(gs.ubo[binding], v)) {
        glBindBufferRange(GL_UNIFORM_BUFFER, binding, v.buffer, v.offset, v.size);
        R.perf.chgUniformBlocks++;
    }
}

void set_uniforms(const uint32_t* r, Shader* sh, Program* p) {
    if (UniformVarBlock& block = sh->vertex ? p->blockVS : p->blockPS; block.size > 0) {
        set_uniform_block(r, sh, block, p->batchable);
        return;
    }
    static const bool noShadow = getenv("WWHD_GL_NO_UNIFORM_SHADOW") != nullptr;
    if (noShadow) {
        p->shadowVS.clear(); p->shadowPS.clear(); p->registerShadowVS.clear(); p->registerShadowPS.clear();
        p->lastPointSize = p->lastAlphaRef = p->lastWindowToClip[0] = p->lastWindowToClip[1] = NAN;
    }
    auto* dec = sh->dec;
    const uint32_t aluBase = mmSQ_ALU_CONSTANT0_0 + (sh->vertex ? 0x400 : 0);
    const uint32_t blockBase = sh->vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
    auto& shadow = sh->vertex ? p->shadowVS : p->shadowPS;
    GLint remapped = sh->vertex ? p->remappedVS : p->remappedPS;
    if (remapped >= 0 && !dec->list_remappedUniformEntries.empty()) {
        // each entry is compared with the program's copy and written there only if it differs
        const size_t bytes = dec->list_remappedUniformEntries.size() * 16;
        bool dirty = shadow.size() != bytes;
        if (dirty) shadow.assign(bytes, 0);
        auto put = [&](uint32_t offset, const void* src) {
            if (offset + 16 > bytes || !memcmp(shadow.data() + offset, src, 16)) return;
            memcpy(shadow.data() + offset, src, 16);
            dirty = true;
        };
        static const uint8_t zeros[16] = {};
        uint8_t ao[16];
        for (const auto& e : dec->list_remappedUniformEntries_register)
            put(e.mappedIndexOffset, ao_noise_constant(sh, r, e.mappedIndexOffset, r + aluBase + e.indexOffset / 4, ao));
        for (const auto& g : dec->list_remappedUniformEntries_bufferGroups) {
            uint32_t address = r[blockBase + g.kcacheBankIdOffset / 4];
            for (const auto& e : g.entries)
                put(e.mappedIndexOffset,
                    ao_noise_constant(sh, r, e.mappedIndexOffset, address ? ppc_ptr(address + e.indexOffset) : zeros, ao));
        }
        if (dirty) glUniform4iv(remapped, (GLsizei)dec->list_remappedUniformEntries.size(), (const GLint*)shadow.data());
    }
    GLint registers = sh->vertex ? p->registersVS : p->registersPS;
    if (registers >= 0 && sh->registerCount) {
        auto& regs = sh->vertex ? p->registerShadowVS : p->registerShadowPS;
        size_t bytes = size_t(sh->registerCount) * 16;
        if (regs.size() != bytes || memcmp(regs.data(), r + aluBase, bytes)) {
            regs.assign((const uint8_t*)(r + aluBase), (const uint8_t*)(r + aluBase) + bytes);
            glUniform4iv(registers, (GLsizei)sh->registerCount, (const GLint*)(r + aluBase));
        }
    }
    if (sh->vertex) {
        if (p->pointSize >= 0) {
            float point = float(r[REGADDR::PA_SU_POINT_SIZE] & 0xFFFF) / 8.0f;
            point = (point == 0 ? 0.125f : point) * g_drawScale;
            if (point != p->lastPointSize) glUniform1f(p->pointSize, p->lastPointSize = point);
        }
        if (p->windowToClip >= 0) {
            float width = 2.0f * f32(r[REGADDR::PA_CL_VPORT_XSCALE]), height = -2.0f * f32(r[REGADDR::PA_CL_VPORT_YSCALE]);
            float v[2] = {width != 0 ? 2.0f / width : 0, height != 0 ? 2.0f / height : 0};
            if (memcmp(v, p->lastWindowToClip, sizeof v)) {
                memcpy(p->lastWindowToClip, v, sizeof v);
                glUniform2f(p->windowToClip, v[0], v[1]);
            }
        }
    } else if (p->alphaRef >= 0) {
        float ref = f32(r[REGADDR::SX_ALPHA_REF]);
        if (memcmp(&ref, &p->lastAlphaRef, 4)) {
            p->lastAlphaRef = ref;
            glUniform1f(p->alphaRef, ref);
        }
    }
    if (p->scaleUniforms) {  // loose uniforms (rare): set each draw
        if (!sh->vertex && p->fragCoordScale >= 0) glUniform2f(p->fragCoordScale, 1.0f / g_drawScale, 1.0f / g_drawScale);
        if (!sh->vertex)  // (a program's loose uf_texNScale are shared by its stages: the pixel stage's)
            for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++)
                if (p->texScale[t] >= 0) glUniform2f(p->texScale[t], g_unitScale[0][t][0], g_unitScale[0][t][1]);
    }
}

// ---- indices: guest index formats and primitives GL lacks become host-order index lists, 16-bit
// when the values fit. Converted lists are reused within a frame (shadow and reflection passes draw
// the same meshes again) until guest buffers may have changed (R.streamGen).
template <int Type> inline uint32_t read_index(const uint8_t* p, uint32_t i) {
    if constexpr (Type == 0) { uint16_t v; memcpy(&v, p + i * 2, 2); return v; }
    else if constexpr (Type == 1) { uint32_t v; memcpy(&v, p + i * 4, 4); return v; }
    else if constexpr (Type == 4) { uint16_t v; memcpy(&v, p + i * 2, 2); return __builtin_bswap16(v); }
    else if constexpr (Type == 9) { uint32_t v; memcpy(&v, p + i * 4, 4); return __builtin_bswap32(v); }
    else return i;  // not indexed
}

struct IndexList {
    StreamSlice slice;
    GLsizei count = 0;
    GLenum type = 0;  // 0: no index list (glDrawArrays)
    uint32_t minIndex = 0, maxIndex = 0;  // the vertices the list reads (restart index excluded)
};

template <int Type, class Out>
void build_indices(const uint8_t* src, uint32_t count, uint32_t prim, bool restart, uint32_t restartIndex,
                   std::vector<Out>& out, uint32_t& minIndex, uint32_t& maxIndex) {
    constexpr Out kRestart = Out(~Out(0));
    uint32_t m = 0, lo = ~0u;
    auto get = [&](uint32_t i) -> Out {
        uint32_t v = read_index<Type>(src, i);
        if (restart && v == restartIndex) return kRestart;
        m = std::max(m, v);
        lo = std::min(lo, v);
        return Out(v);
    };
    switch (prim) {
    case 5:  // triangle fan
        out.resize(count > 2 ? size_t(count - 2) * 3 : 0);
        for (uint32_t i = 1, o = 0; i + 1 < count; i++, o += 3) {
            out[o] = get(0);
            out[o + 1] = get(i);
            out[o + 2] = get(i + 1);
        }
        break;
    case 0x13:  // quad list
        out.resize(size_t(count / 4) * 6);
        for (uint32_t i = 0, o = 0; i + 3 < count; i += 4, o += 6) {
            Out a = get(i), b = get(i + 1), c = get(i + 2), d = get(i + 3);
            out[o] = a; out[o + 1] = b; out[o + 2] = c; out[o + 3] = a; out[o + 4] = c; out[o + 5] = d;
        }
        break;
    case 0x14:  // quad strip
        out.resize(count >= 4 ? size_t((count - 2) / 2) * 6 : 0);
        for (uint32_t i = 0, o = 0; i + 3 < count; i += 2, o += 6) {
            Out a = get(i), b = get(i + 1), c = get(i + 2), d = get(i + 3);
            out[o] = a; out[o + 1] = b; out[o + 2] = c; out[o + 3] = b; out[o + 4] = d; out[o + 5] = c;
        }
        break;
    case 0x12:  // line loop
        out.resize(size_t(count) + 1);
        for (uint32_t i = 0; i < count; i++) out[i] = get(i);
        out[count] = get(0);
        break;
    default:
        out.resize(count);
        if (!restart) {
            for (uint32_t i = 0; i < count; i++) {
                uint32_t v = read_index<Type>(src, i);
                m = std::max(m, v);
                lo = std::min(lo, v);
                out[i] = Out(v);
            }
        } else
            for (uint32_t i = 0; i < count; i++) out[i] = get(i);
        break;
    }
    maxIndex = m;
    minIndex = lo == ~0u ? 0 : std::min(lo, m);
}

template <int Type> IndexList convert_typed(const uint8_t* src, uint32_t count, uint32_t prim, bool restart, uint32_t restartIndex) {
    IndexList list;
    // 16-bit output keeps the guest's 16-bit lists (and short generated lists) at half the bytes;
    // primitive restart then uses 0xFFFF, so a different restart index needs 32-bit output
    constexpr bool source16 = Type == 0 || Type == 4;
    static const bool wide = getenv("WWHD_GL_WIDE_INDICES") != nullptr;
    bool narrow = !wide && (Type < 0 ? count < 0xFFFF : source16 && (!restart || restartIndex == 0xFFFF));
    if (narrow) {
        static std::vector<uint16_t> out;
        build_indices<Type>(src, count, prim, restart, restartIndex, out, list.minIndex, list.maxIndex);
        list.count = GLsizei(out.size());
        list.type = GL_UNSIGNED_SHORT;
        list.slice = stream_upload(out.data(), out.size() * 2, 4);
        R.perf.indexBytes += out.size() * 2;
    } else {
        static std::vector<uint32_t> out;
        build_indices<Type>(src, count, prim, restart, restartIndex, out, list.minIndex, list.maxIndex);
        list.count = GLsizei(out.size());
        list.type = GL_UNSIGNED_INT;
        list.slice = stream_upload(out.data(), out.size() * 4, 4);
        R.perf.indexBytes += out.size() * 4;
    }
    return list;
}

IndexList index_list(uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, bool restart, uint32_t restartIndex) {
    struct Entry {
        uint64_t key = 0, stamp = ~0ull, gen = 0;
        uint32_t addr = 0, count = 0, type = 0, prim = 0, restartIndex = 0;
        bool restart = false;
        IndexList list;
    };
    static FrameTable<Entry> cache;
    const uint64_t key = (uint64_t(indexAddr) << 32 | count) ^ (uint64_t(prim) << 56 | uint64_t(indexType) << 48) ^
                         (restart ? 0x5bd1e995ull * (restartIndex + 1) : 0);
    static const bool noCache = getenv("WWHD_GL_NO_INDEX_CACHE") != nullptr;
    if (const Entry* e = cache.find(key, R.frame); !noCache && e->stamp == R.frame) {
        if (e->gen == R.streamGen && e->addr == indexAddr && e->count == count && e->type == indexType && e->prim == prim &&
            e->restart == restart && e->restartIndex == restartIndex)
            return e->list;
    }
    const uint8_t* src = indexAddr ? mem::ptr(indexAddr) : nullptr;
    IndexList list;
    switch (indexAddr ? indexType : ~0u) {
    case ~0u: list = convert_typed<-1>(src, count, prim, false, 0); break;
    case 0: list = convert_typed<0>(src, count, prim, restart, restartIndex); break;
    case 1: list = convert_typed<1>(src, count, prim, restart, restartIndex); break;
    case 4: list = convert_typed<4>(src, count, prim, restart, restartIndex); break;
    case 9: list = convert_typed<9>(src, count, prim, restart, restartIndex); break;
    default: list = convert_typed<-1>(src, count, prim, false, 0); break;
    }
    // after the upload: moving to the next stream buffer advances the generation
    cache.put({key, R.frame, R.streamGen, indexAddr, count, indexType, prim, restartIndex, restart, list});
    return list;
}

void log_once(uint64_t key, const char* fmt, const std::string& what) {
    static std::unordered_set<uint64_t> seen;
    if (seen.insert(key).second) LOG(fmt, what.c_str());
}

// the previous draw's shader lookup, reused while no shader-relevant register changed
struct ShaderMemo {
    uint64_t gen = 0, frame = ~0ull, epoch = 0;
    uint32_t prim = ~0u;
    LatteFetchShader* fs = nullptr;
    Shader *vs = nullptr, *ps = nullptr;
    Program* p = nullptr;
} memo;

// ---- multi-draw batching (shaders.h draw_batching_on). The state code above calls flush_draws()
// before any GL state call (FLUSHED), so while draws are recorded the GL state is theirs; what may
// differ between them is in the indirect commands (index range, base vertex) and in each draw's copy
// of the shader-constant blocks. A draw's index is its baseInstance: the vertex shader reads it
// through attribute kDrawIndexAttrib from a buffer of 0, 1, 2... whose divisor is so large that
// instancing never advances it.
struct PendingDraws {
    Program* program = nullptr;
    GLenum mode = 0, indexType = 0;  // indexType 0: glDrawArrays
    uint32_t count = 0, capacity = 1;
    std::vector<uint32_t> commands;  // DrawElementsIndirectCommand (5 words) or DrawArraysIndirectCommand (4)
    std::vector<uint8_t> constantsVS, constantsPS;
} pending;

GLuint draw_index_buffer() {
    static GLuint buffer = [] {
        std::vector<uint32_t> iota(4096);
        for (uint32_t i = 0; i < iota.size(); i++) iota[i] = i;
        GLuint b;
        glGenBuffers(1, &b);
        glBindBuffer(GL_COPY_WRITE_BUFFER, b);
        glBufferData(GL_COPY_WRITE_BUFFER, GLsizeiptr(iota.size() * 4), iota.data(), GL_STATIC_DRAW);
        return b;
    }();
    return buffer;
}

void bind_constants(GLuint binding, const std::vector<uint8_t>& data) {
    StreamSlice slice = stream_upload(data.data(), data.size(), R.uboAlignment);
    UboState v = UboState{};
    v.buffer = slice.buffer;
    v.offset = slice.offset;
    v.size = GLsizeiptr(data.size());
    if (changed(gs.ubo[binding], v)) {
        glBindBufferRange(GL_UNIFORM_BUFFER, binding, v.buffer, v.offset, v.size);
        R.perf.chgUniformBlocks++;
    }
    R.perf.uboBytes += data.size();
}
}  // namespace

void flush_draws() {
    if (!pending.count) return;
    PendingDraws& d = pending;
    const uint32_t n = d.count;
    d.count = 0;  // nothing below records draws
    if (!d.constantsVS.empty()) bind_constants(kUniformVarBindingVS, d.constantsVS);
    if (!d.constantsPS.empty()) bind_constants(kUniformVarBindingPS, d.constantsPS);
    const uint32_t* c = d.commands.data();
    if (n == 1) {  // baseInstance 0: a plain draw
        if (d.indexType)
            glDrawElementsInstancedBaseVertex(d.mode, GLsizei(c[0]), d.indexType,
                                              (const void*)(uintptr_t(c[2]) * (d.indexType == GL_UNSIGNED_INT ? 4 : 2)),
                                              GLsizei(c[1]), GLint(c[3]));
        else
            glDrawArraysInstanced(d.mode, GLint(c[2]), GLsizei(c[0]), GLsizei(c[1]));
    } else {
        StreamSlice slice = stream_upload(d.commands.data(), d.commands.size() * 4, 16);
        static GLuint boundIndirect = 0;
        static uint64_t boundEpoch = 0;
        if (boundIndirect != slice.buffer || boundEpoch != R.stateEpoch) {
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, slice.buffer);
            boundIndirect = slice.buffer;
            boundEpoch = R.stateEpoch;
        }
        if (d.indexType) glMultiDrawElementsIndirect(d.mode, d.indexType, (const void*)slice.offset, GLsizei(n), 0);
        else glMultiDrawArraysIndirect(d.mode, (const void*)slice.offset, GLsizei(n), 0);
    }
    R.perf.batches++;
    R.perf.batchedDraws += n;
    d.commands.clear();
    d.constantsVS.clear();
    d.constantsPS.clear();
}

namespace {
// records a draw of a batchable program; instanced draws go alone (their baseInstance must be 0)
void record_draw(Program* p, GLenum mode, GLenum indexType, uint32_t count, uint32_t first, uint32_t baseVertex,
                 uint32_t instances, bool instanced) {
    PendingDraws& d = pending;
    if (d.count && (d.program != p || d.mode != mode || d.indexType != indexType || d.count >= d.capacity || instanced))
        flush_draws();
    if (!d.count) {
        d.program = p;
        d.mode = mode;
        d.indexType = indexType;
        d.capacity = std::max<uint32_t>(1, p->batchCapacity);
    }
    if (indexType) d.commands.insert(d.commands.end(), {count, instances, first, baseVertex, d.count});
    else d.commands.insert(d.commands.end(), {count, instances, baseVertex, d.count});
    if (p->blockVS.size > 0) d.constantsVS.insert(d.constantsVS.end(), p->blockVS.data.begin(), p->blockVS.data.end());
    if (p->blockPS.size > 0) d.constantsPS.insert(d.constantsPS.end(), p->blockPS.data.begin(), p->blockPS.data.end());
    d.count++;
    if (instanced) flush_draws();
}
}  // namespace

namespace {
void draw_impl(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
               uint32_t baseVertex, uint32_t instances);
}

// The GamePad picture (WWHD_GL_SKIP_GAMEPAD). The game draws a second picture every frame for the
// Wii U GamePad's screen and hands it over with GX2CopyColorBufferToScanBuffer(target 4); the Switch
// shows only the TV picture (copy_to_scan ignores the GamePad copy). A color buffer that went to the
// GamePad within the last 60 frames, was never copied to the TV and was never read by a draw or a
// copy is GamePad-only (so is a texture only GamePad-picture draws sample): its draws and color
// clears are skipped (depth clears are kept: a depth buffer may also serve the TV picture). A buffer
// read once by anything else (a draw samples it, a copy reads it, it reaches the TV) is never
// skipped again. Measured at Outset: 26-48 draws a frame, the TV picture pixel-identical.
bool skip_gamepad() {
    static const bool on = [] {
        const char* e = getenv("WWHD_GL_SKIP_GAMEPAD");
        return !(e && *e == '0');
    }();
    return on;
}
bool gamepad_only(const Surface* s) {
    // a read by another draw or a copy keeps a buffer drawn, for 300 frames: the game reads the GamePad
    // picture now and then for a GamePad-sized capture (a loading transition: one read at the save
    // load), and that read kept all 117 draws a frame of the GamePad picture drawn for the session
    constexpr uint64_t kReadWindow = 300;
    const bool unread = s->readFrame == ~0ull || R.frame - s->readFrame > kReadWindow;
    if (s->tvScanFrame != ~0ull) return false;
    if (s->drcScanFrame != ~0ull && R.frame - s->drcScanFrame <= 60) return unread;
    // A texture of the GamePad picture: reads from before a GamePad-picture draw first sampled it do not
    // count (until the first GamePad copy, at the end of the first frame, the draws that compose the
    // GamePad picture count as the TV's); one read by anything else since then shares it for good
    return s->gamepadSource && !s->tvShared && (unread || s->readFrame < s->gamepadSourceSince);
}

// ---- probe frames (round 28): diagnostics for a pixel shader that goes wrong only on the Switch.
// After a capture (both sticks), each following frame redraws the probed pixel shaders' draws with one
// diagnostic output instead of their color (mode 0: unchanged) and writes the draw's target to
// probe_<first probe frame>_m<mode>_<n>[_before].png right after each such draw (n: the draw's order in
// the frame; mode 0 also before it). The default probe is
// the Forsaken Fortress searchlight beam (the cone drawn into the main picture with alpha blending);
// WWHD_GL_PROBE_PS=hash,... (hex, as the trace prints them) chooses others. Opaque outputs (alpha 1) show
// the value itself where the cone is drawn; NaN shows as magenta, infinity as cyan.
struct ProbeMode {
    const char* name;
    const char* expr;  // "$": the original output
};
const ProbeMode kProbeModes[] = {
    {"original", nullptr},
    {"coverage", "vec4(1.0, 1.0, 1.0, 1.0)"},
    {"normal", "(any(isnan(passParameterSem4.xyz)) ? vec4(1.0, 0.0, 1.0, 1.0) : any(isinf(passParameterSem4.xyz)) ? "
               "vec4(0.0, 1.0, 1.0, 1.0) : vec4(abs(normalize(passParameterSem4.xyz)), 1.0))"},
    {"facing", "vec4(vec3(abs(dot(normalize(passParameterSem5.xyz), normalize(passParameterSem4.xyz)))), 1.0)"},
    {"depth", "vec4(texture(textureUnitPS0, passParameterSem7.xy / passParameterSem7.z).x, "
              "clamp(passParameterSem5.w, 0.0, 1.0), 0.5 + 4.0 * (texture(textureUnitPS0, passParameterSem7.xy / "
              "passParameterSem7.z).x - clamp(passParameterSem5.w, 0.0, 1.0)), 1.0)"},
    {"screen", "vec4(fract(passParameterSem7.xy / passParameterSem7.z), passParameterSem7.z > 0.0 ? 0.0 : 1.0, 1.0)"},
    {"alpha", "(isnan(($).w) ? vec4(1.0, 0.0, 1.0, 1.0) : isinf(($).w) ? vec4(0.0, 1.0, 1.0, 1.0) : "
              "vec4(vec3(clamp(($).w, 0.0, 1.0)), 1.0))"},
    {"color", "vec4(clamp(($).xyz, 0.0, 1.0), 1.0)"},
    {"gradient", "vec4(texture(textureUnitPS3, passParameterSem8.xy).xyz, 1.0)"},
    {"vertex alpha", "vec4(vec3(clamp(passParameterSem1.w, 0.0, 1.0)), 1.0)"},
    // the beam shader's own intermediate values at its output (its registers): facing term (R123f.w),
    // depth fade (R0f.w / R126f.x), and the beam without the depth fade (alpha R126f.x)
    {"game facing", "(isnan(R123f.w) ? vec4(1.0, 0.0, 1.0, 1.0) : vec4(vec3(clamp(R123f.w, 0.0, 1.0)), 1.0))"},
    {"game depth fade", "(isnan(R0f.w / R126f.x) ? vec4(1.0, 0.0, 1.0, 1.0) : vec4(vec3(clamp(R0f.w / R126f.x, 0.0, 1.0)), "
                        "1.0))"},
    {"no depth fade", "vec4(R0f.xyz, R126f.x)"},
    // flags (dark gray where drawn, a channel at 1 where its flag is set)
    {"nan map", "vec4(max(vec3(isnan(R123f.w) ? 1.0 : 0.0, isnan(($).w) ? 1.0 : 0.0, 0.0), vec3(0.2)), 1.0)"},
    {"nan source", "vec4(max(vec3((dot(passParameterSem4.xyz, passParameterSem4.xyz) == 0.0 || "
                   "isinf(inversesqrt(dot(passParameterSem4.xyz, passParameterSem4.xyz)))) ? 1.0 : 0.0, "
                   "any(isnan(passParameterSem4)) ? 1.0 : 0.0, (dot(passParameterSem5.xyz, passParameterSem5.xyz) == "
                   "0.0 || any(isnan(passParameterSem5)) || any(isinf(passParameterSem5))) ? 1.0 : 0.0), vec3(0.2)), 1.0)"},
    {"depth source", "vec4(max(vec3(isinf(texture(textureUnitPS0, passParameterSem7.xy / passParameterSem7.z).x) ? 1.0 : "
                     "0.0, isnan(texture(textureUnitPS0, passParameterSem7.xy / passParameterSem7.z).x) ? 1.0 : 0.0, "
                     "0.0), vec3(0.2)), 1.0)"},
};
constexpr int kProbeModeCount = int(sizeof kProbeModes / sizeof *kProbeModes);
// the probe mode of the frame being built, or -1
int probe_mode() {
    if (g_probeStart == ~0ull || R.frame < g_probeStart || R.frame >= g_probeStart + kProbeModeCount) return -1;
    return int(R.frame - g_probeStart);
}
bool probed_ps(uint64_t hash) {
    static const std::vector<uint64_t> list = [] {
        std::vector<uint64_t> out;
        const char* e = getenv("WWHD_GL_PROBE_PS");
        if (!e) return std::vector<uint64_t>{0xBCB22BAD319DC0BDull};  // the searchlight beam
        for (const char* q = e; *q;) {
            char* end = nullptr;
            const unsigned long long v = strtoull(q, &end, 16);
            if (end == q) break;
            out.push_back(v);
            q = *end ? end + 1 : end;
        }
        return out;
    }();
    return std::find(list.begin(), list.end(), hash) != list.end();
}
void probe_dump(Surface* target, int mode, const char* suffix) {
    static uint64_t frame = ~0ull;
    static int n = 0;
    if (frame != R.frame) {
        frame = R.frame;
        n = 0;
    }
    if (!*suffix) n++;
    char name[96];
    snprintf(name, sizeof name, "probe_%llu_m%d_%d%s.png", (unsigned long long)g_probeStart, mode, n + (*suffix ? 1 : 0),
             suffix);
    flush_draws();
    dump_surface(target, name);
    forget_gl_state();
}

void draw(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, uint32_t baseVertex,
          uint32_t instances) {
    draw_impl(r, prim, count, indexType, indexAddr, baseVertex, instances);
}

namespace {
void draw_impl(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
               uint32_t baseVertex, uint32_t instances) {
    make_current();
    static uint32_t drawSample = 0;
    const bool timed = R.timedDraw = (++drawSample % kDrawTimeSample) == 0;
    SampledTime timer{R.perf.drawNs, timed};
    if (!count || !instances || ((prim == 0x13 || prim == 0x14) && count < 4)) return;
    if (r[REGADDR::PA_CL_CLIP_CNTL] & (1 << 22)) return;  // rasterization disabled
    uint64_t lapAt = timed ? now_ns() : 0;
    const uint64_t drawStart = lapAt;
    auto lap = [&](uint64_t& total) {
        if (!timed) return;
        const uint64_t now = now_ns();
        total += (now - lapAt) * kDrawTimeSample;
        lapAt = now;
    };
    ((uint32_t*)r)[REGADDR::VGT_PRIMITIVE_TYPE] = prim;
    Shader *vs, *ps;
    Program* p;
    LatteFetchShader* fs;
    static const bool noMemo = getenv("WWHD_GL_NO_MEMO") != nullptr;
    if (!noMemo && memo.gen == g_shader_state_gen && memo.frame == R.frame && memo.epoch == R.shaderEpoch && memo.prim == prim) {
        fs = memo.fs;
        vs = memo.vs;
        ps = memo.ps;
        p = memo.p;
        R.perf.memoHits++;
    } else {
        // the register part of the shader keys, kept while only programs change
        static struct { uint64_t gen = 0; uint32_t prim = ~0u; uint64_t vs = 0, ps = 0, vsCore = 0, psCore = 0; } stateHash;
        if (noMemo || stateHash.gen != g_shader_regs_gen || stateHash.prim != prim) {
            stateHash.vs = shader_state_hash(r, true, &stateHash.vsCore);
            stateHash.ps = shader_state_hash(r, false, &stateHash.psCore);
            stateHash.gen = g_shader_regs_gen;
            stateHash.prim = prim;
        }
        // recent (programs, register state) combinations: a draw that goes back to one skips the
        // shader and program maps. Program memory may change between frames, so a combination made in
        // an earlier frame is used again only if its programs still have the hashes they had (checked
        // once per frame, as for the shader lookup itself) and the fetch shader is the same: before,
        // each combination's first draw of every frame (a quarter of all draws) did the full lookup.
        struct Combo {
            uint64_t frame = ~0ull, epoch = 0, vsState = 0, psState = 0;
            uint32_t programs[6] = {};
            LatteFetchShader* fs = nullptr;
            Shader *vs = nullptr, *ps = nullptr;
            Program* p = nullptr;
            void *vsRef = nullptr, *psRef = nullptr;  // program_hash_ref
            uint64_t vsHash = 0, psHash = 0;
        };
        static const bool comboAcrossFrames = [] {
            const char* e = getenv("WWHD_GL_COMBO_FRAMES");
            return !(e && *e == '0');
        }();
        auto stillValid = [&](Combo& c) {
            if (c.frame == R.frame) return true;
            if (!comboAcrossFrames || !c.vsRef || !c.psRef) return false;
            uint64_t fsKey = 0;
            if (get_fetch_shader(r, &fsKey, R.frame) != c.fs ||
                program_hash_of(c.vsRef, c.programs[2] << 8, c.programs[3] << 3, R.frame) != c.vsHash ||
                program_hash_of(c.psRef, c.programs[4] << 8, c.programs[5] << 3, R.frame) != c.psHash)
                return false;
            c.frame = R.frame;
            return true;
        };
        // (4,096: a busy view has several hundred combinations a frame, and 256 slots kept evicting them)
        static Combo combos[4096];
        const uint32_t programs[6] = {r[mmSQ_PGM_START_FS], r[mmSQ_PGM_START_FS + 1], r[mmSQ_PGM_START_VS],
                                      r[mmSQ_PGM_START_VS + 1], r[mmSQ_PGM_START_PS], r[mmSQ_PGM_START_PS + 1]};
        uint64_t h = stateHash.vs * 31 + stateHash.ps;
        for (uint32_t v : programs) h = (h ^ v) * 0x100000001B3ull;
        Combo& c = combos[(h ^ (h >> 29)) & 4095];
        if (!noMemo && c.epoch == R.shaderEpoch && c.vsState == stateHash.vs && c.psState == stateHash.ps &&
            !memcmp(c.programs, programs, sizeof programs) && stillValid(c)) {
            R.perf.comboHits++;
            fs = c.fs;
            vs = c.vs;
            ps = c.ps;
            p = c.p;
        } else {
            uint64_t fsKey = 0;
            fs = get_fetch_shader(r, &fsKey, R.frame);
            vs = fs ? translate(r, true, fs, fsKey, R.frame, stateHash.vsCore) : nullptr;
            ps = fs ? translate(r, false, fs, fsKey, R.frame, stateHash.psCore) : nullptr;
            p = vs && ps && vs->ready() && ps->ready() ? program(vs, ps) : nullptr;
            c.frame = R.frame;
            c.epoch = R.shaderEpoch;
            c.vsState = stateHash.vs;
            c.psState = stateHash.ps;
            memcpy(c.programs, programs, sizeof programs);
            c.fs = fs;
            c.vs = vs;
            c.ps = ps;
            const uint32_t vsAddr = programs[2] << 8, vsSize = programs[3] << 3, psAddr = programs[4] << 8,
                           psSize = programs[5] << 3;
            const bool hashable = vsAddr && vsSize && psAddr && psSize;  // (what translate checks first)
            c.vsRef = hashable ? program_hash_ref(vsAddr, vsSize) : nullptr;
            c.psRef = hashable ? program_hash_ref(psAddr, psSize) : nullptr;
            c.vsHash = hashable ? program_hash_of(c.vsRef, vsAddr, vsSize, R.frame) : 0;
            c.psHash = hashable ? program_hash_of(c.psRef, psAddr, psSize, R.frame) : 0;
            c.p = p;
        }
        memo = {g_shader_state_gen, R.frame, R.shaderEpoch, prim, fs, vs, ps, p};
    }
    if (!fs) return;
    if (!vs || !ps || !vs->ready() || !ps->ready()) {
        R.skippedDraws++;
        if (vs && !vs->ready()) log_once(vs->key, "[gl] vertex shader skipped: %s", vs->error);
        if (ps && !ps->ready()) log_once(ps->key, "[gl] pixel shader skipped: %s", ps->error);
        return;
    }
    if (!p) {
        R.skippedDraws++;
        return;
    }
    lap(R.perf.lookupNs);

    // ---- indices
    const bool stripRestart = indexAddr && (prim == 3 || prim == 6) && (r[REGADDR::VGT_MULTI_PRIM_IB_RESET_EN] & 1);
    const uint32_t restartIndex = r[REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX];
    GLenum mode;
    switch (prim) {
    case 1: mode = GL_POINTS; break;
    case 2: mode = GL_LINES; break;
    case 3: case 0x12: mode = GL_LINE_STRIP; break;
    case 4: case 5: case 0x13: case 0x14: mode = GL_TRIANGLES; break;
    case 6: mode = GL_TRIANGLE_STRIP; break;
    default:
        log_once(0xD0000000u | prim, "[gl] unsupported primitive %s", std::to_string(prim));
        return;
    }
    const bool generated = prim == 5 || prim == 0x12 || prim == 0x13 || prim == 0x14;
    IndexList indices;
    if (indexAddr || generated) indices = index_list(prim, count, indexType, indexAddr, stripRestart, restartIndex);
    uint64_t maxVertex = indices.type ? uint64_t(indices.maxIndex) + baseVertex : uint64_t(baseVertex) + count - 1;
    const uint64_t firstVertex = indices.type ? uint64_t(indices.minIndex) + baseVertex : uint64_t(baseVertex);
    lap(R.perf.indexNs);

    // ---- render targets
    const auto& lcr = *reinterpret_cast<const LatteContextRegister*>(r);
    std::array<Surface*, 8> colors{};
    uint32_t slices[8]{}, depthSlice = 0;
    auto mask = LatteMRT::GetActiveColorBufferMask(ps->dec, lcr);
    for (int i = 0; i < 8; i++)
        if (mask & (1 << i)) colors[i] = color_target(r, i, &slices[i]);
    Surface* depth = LatteMRT::GetActiveDepthBufferMask(lcr) ? depth_target(r, &depthSlice) : nullptr;
    // each render target at the internal resolution it should have now (rescale_surface keeps contents);
    // the TV picture's buffer back from full resolution if 3D drawing follows the HUD's
    fit_scale(depth, true);
    float shared = depth ? depth->scale : 0.0f;
    bool mixed = false;
    for (auto* c : colors)
        if (c) {
            before_write(c);
            if (c->hudFull && depth) c->hudFull = false;
            fit_scale(c, true);
            if (shared == 0.0f) shared = c->scale;
            mixed |= c->scale != shared;
        }
    if (mixed) {  // some took the new scale, some wait for an allocation: all of them now
        fit_scale(depth, true, true);
        for (auto* c : colors) fit_scale(c, true, true);
    }
    Surface* target = depth;
    for (auto* c : colors)
        if (c) {
            target = c;
            break;
        }
    if (!target) {
        R.skippedDraws++;
        return;
    }
    g_drawScale = target->scale;
    if (depth && depth->scale != g_drawScale)
        log_once(0x5CA1E000u, "[gl] render targets with different internal resolutions (depth %s)",
                 std::to_string(depth->scale) + ", color " + std::to_string(g_drawScale));
    // test aid: WWHD_GL_TEST_SKIP=frame:first-last,... leaves out those draws of that frame (counted from 0,
    // in the order the trace lists them): which draws make a part of the picture
    {
        struct SkipRange { uint64_t frame; uint32_t first, last; };
        static const std::vector<SkipRange> skips = [] {
            std::vector<SkipRange> out;
            const char* e = getenv("WWHD_GL_TEST_SKIP");
            for (const char* q = e; q && *q;) {
                unsigned long long f = 0;
                unsigned a = 0, b = 0;
                if (sscanf(q, "%llu:%u-%u", &f, &a, &b) == 3) out.push_back({f, a, b});
                q = strchr(q, ',');
                if (q) q++;
            }
            return out;
        }();
        if (!skips.empty() && target != depth && !gamepad_only(target)) {  // (as traced: color draws)
            static uint64_t frame = ~0ull;
            static uint32_t index = 0;
            if (frame != R.frame) {
                frame = R.frame;
                index = 0;
            }
            const uint32_t i = index++;
            for (const auto& k : skips)
                if (k.frame == R.frame && i >= k.first && i <= k.last) return;
        }
    }
    bool gamepadDraw = target != depth;  // (a draw without a color target is not classified)
    for (auto* c : colors)
        if (c && !gamepad_only(c)) gamepadDraw = false;
    if (gamepadDraw) {
        R.perf.gamepadDraws++;
        if (skip_gamepad()) {
            R.perf.gamepadSkipped++;
            // each surface's first skipped draw is logged: anything but the GamePad picture's own buffers
            // (854x480 and its parts) being skipped is a misclassification to look into
            for (auto* c : colors)
                if (c && !c->skipLogged) {
                    c->skipLogged = true;
                    LOG("[gl] draws into %s skipped from frame %llu: GamePad picture only (WWHD_GL_SKIP_GAMEPAD)",
                        trace_name(c).c_str(), (unsigned long long)R.frame);
                }
            return;
        }
    }
    for (auto* c : colors)
        if (c) upload_surface(c);
    if (depth) upload_surface(depth);

    // ---- textures and uniform blocks (may upload; nothing is bound to draw units yet)
    static std::vector<TextureBinding> textures;
    static std::vector<UboBinding> ubos;
    textures.clear();
    ubos.clear();
    g_gamepadDrawing = gamepadDraw;
    g_drawSamplesRendered = false;
    g_traceTextures.clear();
    // ---- probe frames: the probed pixel shader's draws with a diagnostic output
    const int probeMode = probe_mode();
    bool probed = probeMode >= 0 && colors[0] && probed_ps(ps->glslHash);
    if (probeMode >= 0 && colors[0] && !probed && !getenv("WWHD_GL_PROBE_PS")) {
        // the searchlight beam by its draw state, should its GLSL hash differ: a 234-index triangle list
        // drawn with alpha blending, no culling and no depth writes into a 1280x720 target
        LATTE_DB_DEPTH_CONTROL dc;
        memcpy(&dc, r + REGADDR::DB_DEPTH_CONTROL, 4);
        LATTE_PA_SU_SC_MODE_CNTL pm;
        memcpy(&pm, r + REGADDR::PA_SU_SC_MODE_CNTL, 4);
        probed = prim == 4 && count == 234 && r[REGADDR::CB_BLEND0_CONTROL] == 0x05040504 && !pm.get_CULL_FRONT() &&
                 !pm.get_CULL_BACK() && !dc.get_Z_WRITE_ENABLE() && colors[0]->width == 1280 && colors[0]->height == 720;
        if (probed) {
            char hex[20];
            snprintf(hex, sizeof hex, "%016llx", (unsigned long long)ps->glslHash);
            log_once(0x9B0BE000u, "[gl] probe: the beam found by its draw state, pixel shader %s", hex);
        }
    }
    if (probed) {
        if (probeMode == 0) probe_dump(colors[0], 0, "_before");
        if (const char* expr = kProbeModes[probeMode].expr) {
            if (Program* v = probe_program(vs, ps, probeMode, expr)) p = v;
        }
        static uint64_t logged = ~0ull;
        if (logged != R.frame) {
            logged = R.frame;
            LOG("[gl] probe frame %llu: mode %d (%s), pixel shader %016llx into %s", (unsigned long long)R.frame,
                probeMode, kProbeModes[probeMode].name, (unsigned long long)ps->glslHash, trace_name(colors[0]).c_str());
        }
    }

    prepare_stage(r, vs, p, colors, depth, textures, ubos);
    prepare_stage(r, ps, p, colors, depth, textures, ubos);
    // The HUD at full resolution: the game draws it into the same buffer as the scene, last, after the
    // post-processing that reads the scene. At a lower internal resolution, the first draw into the TV
    // picture's buffer that comes after a read of it this frame, has no depth buffer and samples only the
    // game's own textures (nothing rendered) switches that buffer to its full-size texture, the scene
    // scaled up into it; the next frame's clear (or 3D drawing) switches back.
    if (target->scale < 1.0f && target == R.tvSource && !depth && !g_drawSamplesRendered && colors[0] == target &&
        target->readFrame == R.frame) {
        bool single = true;
        for (int i = 1; i < 8; i++)
            if (colors[i]) single = false;
        if (single) {
            target->hudFull = true;
            fit_scale(target, true, true);
            g_drawScale = target->scale;
            R.perf.hudSwitches++;
        }
    }
    lap(R.perf.resourceNs);

    // ---- framebuffer
    sync_cache();
    if (gs.fbo != R.drawFbo) {
        FLUSHED(glBindFramebuffer)(GL_FRAMEBUFFER, R.drawFbo);
        gs.fbo = R.drawFbo;
    }
    struct Attachment { Surface* s = nullptr; uint32_t slice = 0; };
    static std::array<Attachment, 9> bound;
    static bool dirty = true;
    static uint64_t boundTextures = 0;
    if (boundTextures != R.textureEpoch) {  // a surface got a new texture: attach everything again
        boundTextures = R.textureEpoch;
        for (auto& a : bound) a = {reinterpret_cast<Surface*>(uintptr_t(1)), ~0u};
    }
    for (int i = 0; i < 8; i++)
        if (bound[i].s != colors[i] || bound[i].slice != slices[i]) {
            FLUSHED(attach)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, colors[i], 0, slices[i]);
            bound[i] = {colors[i], slices[i]};
            dirty = true;
        }
    if (bound[8].s != depth || bound[8].slice != depthSlice) {
        FLUSHED(glFramebufferTexture)(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, 0, 0);
        if (depth) FLUSHED(attach)(GL_FRAMEBUFFER, depth_attachment(depth), depth, 0, depthSlice);
        bound[8] = {depth, depthSlice};
        dirty = true;
    }
    if (dirty && g_traceFrame) trace_pass(colors, depth);
    if (dirty) {
        if (g_gpuPassSampling) {
            const Surface* first = nullptr;
            for (auto* c : colors)
                if (c && !first) first = c;
            gpu_pass_mark("draw", first, depth);
        }
        GLenum bufs[8];
        for (int i = 0; i < 8; i++) bufs[i] = colors[i] ? GL_COLOR_ATTACHMENT0 + i : GL_NONE;
        FLUSHED(glDrawBuffers)(8, bufs);
        // attachment sets already found complete are not asked again (a status query waits for
        // Mesa's GL thread, when that is on); surfaces are never freed, so the pointers stay valid
        static std::unordered_set<uint64_t> complete;
        uint64_t setKey = 0xcbf29ce484222325ull;
        for (auto& a : bound) setKey = (setKey ^ (uint64_t(uintptr_t(a.s)) * 31 + a.slice)) * 0x100000001b3ull;
        GLenum status = complete.count(setKey) ? GL_FRAMEBUFFER_COMPLETE : glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status == GL_FRAMEBUFFER_COMPLETE) complete.insert(setKey);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            log_once(0xFB000000u | status, "[gl] incomplete framebuffer (%s)", std::to_string(status));
            bound = {};
            R.skippedDraws++;
            return;
        }
        dirty = false;
    }
    bind_textures(textures);
    bind_ubos(ubos);

    // ---- program and uniforms
    if (gs.program != p->prog) {
        FLUSHED(glUseProgram)(p->prog);
        R.perf.chgProgram++;
        gs.program = p->prog;
    }
    set_uniforms(r, vs, p);
    set_uniforms(r, ps, p);

    // ---- viewport: pick the clip origin that keeps the GL viewport height positive (gl.h)
    float xs = f32(r[REGADDR::PA_CL_VPORT_XSCALE]), xo = f32(r[REGADDR::PA_CL_VPORT_XOFFSET]);
    float ys = f32(r[REGADDR::PA_CL_VPORT_YSCALE]), yo = f32(r[REGADDR::PA_CL_VPORT_YOFFSET]);
    float zs = f32(r[REGADDR::PA_CL_VPORT_ZSCALE]), zo = f32(r[REGADDR::PA_CL_VPORT_ZOFFSET]);
    LATTE_PA_CL_CLIP_CNTL clip;
    memcpy(&clip, r + REGADDR::PA_CL_CLIP_CNTL, 4);
    const bool dxClip = clip.get_DX_CLIP_SPACE_DEF();
    const bool upper = ys < 0;
    {
        Viewport v = Viewport{};
        v.origin = upper ? GL_UPPER_LEFT : GL_LOWER_LEFT;
        v.depthMode = dxClip ? GL_ZERO_TO_ONE : GL_NEGATIVE_ONE_TO_ONE;
        v.rect[0] = (xo - std::fabs(xs)) * g_drawScale;
        v.rect[1] = (upper ? yo + ys : yo - ys) * g_drawScale;
        v.rect[2] = 2 * std::fabs(xs) * g_drawScale;
        v.rect[3] = 2 * std::fabs(ys) * g_drawScale;
        v.range[0] = dxClip ? zo : zo - zs;
        v.range[1] = zo + zs;
        Viewport old = gs.viewport;
        if (changed(gs.viewport, v)) {
            if (old.origin != v.origin || old.depthMode != v.depthMode)
                if (auto cc = clip_control()) FLUSHED(cc)(v.origin, v.depthMode);
            if (memcmp(old.rect, v.rect, sizeof v.rect)) FLUSHED(glViewportIndexedf)(0, v.rect[0], v.rect[1], v.rect[2], v.rect[3]);
            if (memcmp(old.range, v.range, sizeof v.range)) FLUSHED(glDepthRangef)(v.range[0], v.range[1]);
        }
    }
    cap(kDepthClamp, clip.get_ZCLIP_FAR_DISABLE());
    // the vertex shaders' near-plane clip distance (shaders.cpp with_near_clip), unless the game turned
    // near clipping off
    cap(kNearClip, p->nearClip && !clip.get_ZCLIP_NEAR_DISABLE());

    uint32_t width = target->width, height = target->height;
    uint32_t tl = r[REGADDR::PA_SC_GENERIC_SCISSOR_TL], br = r[REGADDR::PA_SC_GENERIC_SCISSOR_BR];
    uint32_t x = std::min(tl & 0x7fff, width), y = std::min((tl >> 16) & 0x7fff, height);
    uint32_t ex = std::min(br & 0x7fff, width), ey = std::min((br >> 16) & 0x7fff, height);
    if (ex <= x || ey <= y) return;
    if (g_drawScale != 1.0f) {  // texture pixels: outward
        x = uint32_t(float(x) * g_drawScale);
        y = uint32_t(float(y) * g_drawScale);
        ex = std::min(scaled_size(ex, g_drawScale), target->pw);
        ey = std::min(scaled_size(ey, g_drawScale), target->ph);
    }
    cap(kScissor, true);
    {
        const GLint sc[4] = {GLint(x), GLint(y), GLint(ex - x), GLint(ey - y)};
        if (gs.scissor[0] != sc[0] || gs.scissor[1] != sc[1] || gs.scissor[2] != sc[2] || gs.scissor[3] != sc[3]) {
            for (int i = 0; i < 4; i++) gs.scissor[i] = sc[i];
            FLUSHED(glScissor)(sc[0], sc[1], sc[2], sc[3]);
        }
    }

    // ---- rasterizer
    LATTE_PA_SU_SC_MODE_CNTL pm;
    memcpy(&pm, r + REGADDR::PA_SU_SC_MODE_CNTL, 4);
    bool cullFront = pm.get_CULL_FRONT(), cullBack = pm.get_CULL_BACK();
    // Latte decides facing in y-down window space; GL with a lower-left origin sees it mirrored
    bool ccw = pm.get_FRONT_FACE() == LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW;
    {
        Raster v = Raster{};
        Raster old = gs.raster;
        v.cullFace = cullFront && cullBack ? GL_FRONT_AND_BACK : cullFront ? GL_FRONT : GL_BACK;
        v.frontFace = (upper ? ccw : !ccw) ? GL_CCW : GL_CW;
        bool offset = pm.get_OFFSET_FRONT_ENABLED();
        if (offset) {
            v.offset[0] = f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE]) / 16;
            v.offset[1] = f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET]);
        } else {  // unused: keep whatever is set
            v.offset[0] = old.offset[0];
            v.offset[1] = old.offset[1];
        }
        if (!(cullFront || cullBack)) v.cullFace = old.cullFace;
        if (changed(gs.raster, v)) {
            if (old.cullFace != v.cullFace) FLUSHED(glCullFace)(v.cullFace);
            if (old.frontFace != v.frontFace) FLUSHED(glFrontFace)(v.frontFace);
            if (offset && memcmp(old.offset, v.offset, sizeof v.offset)) FLUSHED(glPolygonOffset)(v.offset[0], v.offset[1]);
        }
        cap(kCull, cullFront || cullBack);
        cap(kPolyOffset, offset);
    }

    // ---- depth / stencil
    LATTE_DB_DEPTH_CONTROL dc;
    memcpy(&dc, r + REGADDR::DB_DEPTH_CONTROL, 4);
    if (depth && dc.get_Z_ENABLE()) {
        cap(kDepthTest, true);
        DepthState v = DepthState{};
        DepthState old = gs.depth;
        v.func = GL_NEVER + uint32_t(dc.get_Z_FUNC());
        v.mask = dc.get_Z_WRITE_ENABLE() ? GL_TRUE : GL_FALSE;
        if (changed(gs.depth, v)) {
            if (old.func != v.func) FLUSHED(glDepthFunc)(v.func);
            if (old.mask != v.mask) FLUSHED(glDepthMask)(v.mask);
        }
    } else
        cap(kDepthTest, false);
    if (depth && depth->fmt.stencil && dc.get_STENCIL_ENABLE()) {
        cap(kStencilTest, true);
        uint32_t f = r[REGADDR::DB_STENCILREFMASK];
        bool separate = dc.get_BACK_STENCIL_ENABLE();
        uint32_t b = separate ? r[REGADDR::DB_STENCILREFMASK_BF] : f;
        StencilState v = StencilState{};
        v.v[0] = GL_NEVER + uint32_t(dc.get_STENCIL_FUNC_F());
        v.v[1] = f;
        v.v[2] = stencil_op(uint32_t(dc.get_STENCIL_FAIL_F()));
        v.v[3] = stencil_op(uint32_t(dc.get_STENCIL_ZFAIL_F()));
        v.v[4] = stencil_op(uint32_t(dc.get_STENCIL_ZPASS_F()));
        v.v[5] = GL_NEVER + uint32_t(separate ? dc.get_STENCIL_FUNC_B() : dc.get_STENCIL_FUNC_F());
        v.v[6] = b;
        v.v[7] = stencil_op(uint32_t(separate ? dc.get_STENCIL_FAIL_B() : dc.get_STENCIL_FAIL_F()));
        v.v[8] = stencil_op(uint32_t(separate ? dc.get_STENCIL_ZFAIL_B() : dc.get_STENCIL_ZFAIL_F()));
        v.v[9] = stencil_op(uint32_t(separate ? dc.get_STENCIL_ZPASS_B() : dc.get_STENCIL_ZPASS_F()));
        if (changed(gs.stencil, v)) {
            FLUSHED(glStencilFuncSeparate)(GL_FRONT, v.v[0], f & 255, (f >> 8) & 255);
            FLUSHED(glStencilOpSeparate)(GL_FRONT, v.v[2], v.v[3], v.v[4]);
            FLUSHED(glStencilMaskSeparate)(GL_FRONT, (f >> 16) & 255);
            FLUSHED(glStencilFuncSeparate)(GL_BACK, v.v[5], b & 255, (b >> 8) & 255);
            FLUSHED(glStencilOpSeparate)(GL_BACK, v.v[7], v.v[8], v.v[9]);
            FLUSHED(glStencilMaskSeparate)(GL_BACK, (b >> 16) & 255);
        }
    } else
        cap(kStencilTest, false);

    // ---- color output (targets without a color buffer keep whatever state they have)
    cap(kSrgb, true);
    bool constantColor = false;
    for (uint32_t i = 0; i < 8; i++) {
        if (!colors[i]) continue;
        uint32_t m = (r[REGADDR::CB_TARGET_MASK] >> (4 * i)) & 15;
        if (gs.colorMask[i] != m) {
            FLUSHED(glColorMaski)(i, m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1);
            gs.colorMask[i] = m;
        }
        bool on = colors[i]->fmt.kind == FormatInfo::FLOAT && ((r[REGADDR::CB_COLOR_CONTROL] >> (8 + i)) & 1);
        BlendState v = BlendState{};
        BlendState old = gs.blend[i];
        v.on = on;
        if (on) {
            LATTE_CB_BLENDN_CONTROL b;
            memcpy(&b, r + REGADDR::CB_BLEND0_CONTROL + i, 4);
            v.src = blend_factor(uint32_t(b.get_COLOR_SRCBLEND()));
            v.dst = blend_factor(uint32_t(b.get_COLOR_DSTBLEND()));
            v.op = blend_op(uint32_t(b.get_COLOR_COMB_FCN()));
            bool separate = b.get_SEPARATE_ALPHA_BLEND();
            v.srcA = separate ? blend_factor(uint32_t(b.get_ALPHA_SRCBLEND())) : v.src;
            v.dstA = separate ? blend_factor(uint32_t(b.get_ALPHA_DSTBLEND())) : v.dst;
            v.opA = separate ? blend_op(uint32_t(b.get_ALPHA_COMB_FCN())) : v.op;
            for (GLenum f : {v.src, v.dst, v.srcA, v.dstA})
                if (f == GL_CONSTANT_COLOR || f == GL_ONE_MINUS_CONSTANT_COLOR || f == GL_CONSTANT_ALPHA ||
                    f == GL_ONE_MINUS_CONSTANT_ALPHA)
                    constantColor = true;
        } else {
            // disabled: the factors stay as they are
            v.src = old.src; v.dst = old.dst; v.srcA = old.srcA; v.dstA = old.dstA; v.op = old.op; v.opA = old.opA;
        }
        if (changed(gs.blend[i], v)) {
            if (old.on != v.on) {
                if (on) FLUSHED(glEnablei)(GL_BLEND, i);
                else FLUSHED(glDisablei)(GL_BLEND, i);
            }
            if (on && (old.src != v.src || old.dst != v.dst || old.srcA != v.srcA || old.dstA != v.dstA))
                FLUSHED(glBlendFuncSeparatei)(i, v.src, v.dst, v.srcA, v.dstA);
            if (on && (old.op != v.op || old.opA != v.opA)) FLUSHED(glBlendEquationSeparatei)(i, v.op, v.opA);
        }
    }
    if (constantColor) {
        const float* constant = reinterpret_cast<const float*>(r + REGADDR::CB_BLEND_RED);
        if (memcmp(gs.blendColor, constant, sizeof gs.blendColor)) {
            memcpy(gs.blendColor, constant, sizeof gs.blendColor);
            FLUSHED(glBlendColor)(constant[0], constant[1], constant[2], constant[3]);
        }
    }
    uint32_t rop = (r[REGADDR::CB_COLOR_CONTROL] >> 16) & 255;
    if (rop != 0xCC) {
        cap(kLogicOp, true);
        GLenum op = logic_op(rop);
        if (gs.logicOp != op) FLUSHED(glLogicOp)(gs.logicOp = op);
    } else
        cap(kLogicOp, false);

    // ---- vertex buffers (guest bytes as stored)
    static uint32_t enabledAttribs = 0;
    uint32_t wantAttribs = 0;
    bool attribChanged = false, instanceData = false;
    const auto& mb = multi_bind();
    Range vbDirty;
    if (p->batchable) {
        // the draw index (multi-draw batching): attribute kDrawIndexAttrib from binding kDrawIndexBinding
        AttribState v = AttribState{};
        v.comps = 1;
        v.type = GL_UNSIGNED_INT;
        v.offset = 0;
        v.binding = kDrawIndexBinding;
        AttribState old = gs.attrib[kDrawIndexAttrib];
        if (changed(gs.attrib[kDrawIndexAttrib], v)) {
            attribChanged = true;
            if (old.comps != v.comps || old.type != v.type || old.offset != v.offset)
                FLUSHED(glVertexAttribIFormat)(kDrawIndexAttrib, 1, GL_UNSIGNED_INT, 0);
            if (old.binding != v.binding) FLUSHED(glVertexAttribBinding)(kDrawIndexAttrib, kDrawIndexBinding);
        }
        VertexBindingState b = VertexBindingState{};
        b.buffer = draw_index_buffer();
        b.offset = 0;
        b.stride = 4;
        b.divisor = 0x7FFFFFFF;  // instancing never advances it: the value is the draw's baseInstance
        VertexBindingState oldb = gs.binding[kDrawIndexBinding];
        if (changed(gs.binding[kDrawIndexBinding], b)) {
            if (oldb.buffer != b.buffer || oldb.offset != b.offset || oldb.stride != b.stride)
                FLUSHED(glBindVertexBuffer)(kDrawIndexBinding, b.buffer, b.offset, b.stride);
            if (oldb.divisor != b.divisor) FLUSHED(glVertexBindingDivisor)(kDrawIndexBinding, b.divisor);
        }
        wantAttribs |= 1u << kDrawIndexAttrib;
    }
    // Vertex rebasing (WWHD_GL_VERTEX_REBASE, on unless =0, with batching): a draw with one vertex
    // buffer gets its data at a multiple of the stride in the stream buffer and binds the buffer at
    // offset 0; the difference goes into the base vertex. Consecutive draws of different meshes
    // then keep the same binding and can share a multi-draw.
    static const bool rebaseOn = [] {
        const char* e = getenv("WWHD_GL_VERTEX_REBASE");
        return !(e && *e == '0');
    }();
    uint32_t vertexGroups = 0;
    for (auto& g : fs->bufferGroups)
        if (r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7]) vertexGroups++;
    // Vertex range trimming (WWHD_GL_VERTEX_TRIM, on unless =0, rebased draws): only the vertices
    // from the lowest one the draw reads are copied. A model's parts share one vertex buffer and each
    // draws its own range, so copying from vertex 0 copied every part below it again (and the copy
    // of the buffer made for an earlier part was too short to reuse).
    static const bool trimOn = [] {
        const char* e = getenv("WWHD_GL_VERTEX_TRIM");
        return !(e && *e == '0');
    }();
    uint32_t rebase = 0;  // added to the draw's base vertex
    for (auto& g : fs->bufferGroups) {
        if (p->batchable && g.attributeBufferIndex == kDrawIndexBinding)
            log_once(0xD1D00000u, "[gl] vertex buffer %u is also the draw-index binding: batched draws may break",
                     std::to_string(g.attributeBufferIndex));
        uint32_t addr = r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7];
        uint32_t size = r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 1] + 1;
        uint32_t stride = (r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 2] >> 11) & 0xFFFF;
        if (!addr) continue;
        bool instance = false;
        uint64_t attributeEnd = 0;
        for (int j = 0; j < g.attribCount; j++) {
            auto& a = g.attrib[j];
            int loc = vs->mapping.attributeMapping[a.semanticId];
            if (loc < 0 || loc >= kAttribs) continue;
            AttribState v = AttribState{};
            uint32_t bytes;
            if (!vertex_format(a.format, v.comps, v.type, bytes)) continue;
            v.offset = a.offset;
            v.binding = g.attributeBufferIndex;
            AttribState old = gs.attrib[loc];
            if (changed(gs.attrib[loc], v)) {
                attribChanged = true;
                if (old.comps != v.comps || old.type != v.type || old.offset != v.offset)
                    FLUSHED(glVertexAttribIFormat)(loc, v.comps, v.type, v.offset);
                if (old.binding != v.binding) FLUSHED(glVertexAttribBinding)(loc, v.binding);
            }
            wantAttribs |= 1u << loc;
            attributeEnd = std::max<uint64_t>(attributeEnd, uint64_t(a.offset) + bytes);
            if (a.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA) instance = true;
        }
        instanceData |= instance;
        uint64_t last = instance ? instances - 1 : maxVertex;
        uint64_t copied = std::min<uint64_t>(size, last * stride + std::max<uint64_t>(attributeEnd, stride));
        const bool rebased = rebaseOn && p->batchable && vertexGroups == 1 && !instance && stride && stride % 4 == 0;
        // with rebasing the base vertex absorbs the skipped vertices: it becomes slice / stride - first
        // (negative for GL when the first vertex lies past the slice's start; indices >= it keep the
        // vertex numbers non-negative)
        const uint64_t skip = rebased && trimOn && firstVertex * stride < copied ? firstVertex * stride : 0;
        auto slice = stream_guest(addr + uint32_t(skip), (size_t)std::max<uint64_t>(copied - skip, 4), rebased ? stride : 16);
        R.perf.vertexBytes += copied - skip;
        VertexBindingState v = VertexBindingState{};
        v.buffer = slice.buffer;
        v.offset = slice.offset;
        if (rebased) {
            rebase = uint32_t(slice.offset / stride) - uint32_t(skip / stride);
            v.offset = 0;
            R.perf.rebasedDraws++;
        }
        v.stride = GLsizei(stride);
        v.divisor = instance ? 1 : 0;
        uint32_t index = g.attributeBufferIndex;
        if (index >= (uint32_t)kVertexBindings) {
            FLUSHED(glBindVertexBuffer)(index, v.buffer, v.offset, v.stride);
            FLUSHED(glVertexBindingDivisor)(index, v.divisor);
            continue;
        }
        VertexBindingState old = gs.binding[index];
        if (changed(gs.binding[index], v)) {
            if (old.buffer != v.buffer || old.offset != v.offset || old.stride != v.stride) {
                if (mb.on) vbDirty.add(index);
                else FLUSHED(glBindVertexBuffer)(index, v.buffer, v.offset, v.stride);
            }
            if (old.divisor != v.divisor) FLUSHED(glVertexBindingDivisor)(index, v.divisor);
        }
    }
    R.perf.chgAttribFormats += attribChanged || enabledAttribs != wantAttribs;
    R.perf.chgVertexBuffers += !vbDirty.empty();
    if (!vbDirty.empty()) {
        GLuint buffers[kVertexBindings];
        GLintptr offsets[kVertexBindings];
        GLsizei strides[kVertexBindings];
        for (uint32_t b = vbDirty.lo; b <= vbDirty.hi; b++) {
            VertexBindingState& c = gs.binding[b];
            if (c.buffer == kUnknown) {  // unbound; the divisor stays unknown
                c.buffer = 0;
                c.offset = 0;
                c.stride = 16;
            }
            buffers[b - vbDirty.lo] = c.buffer;
            offsets[b - vbDirty.lo] = c.offset;
            strides[b - vbDirty.lo] = c.stride;
        }
        FLUSHED(mb.vertexBuffers)(vbDirty.lo, GLsizei(vbDirty.hi - vbDirty.lo + 1), buffers, offsets, strides);
    }
    for (uint32_t change = enabledAttribs ^ wantAttribs; change; change &= change - 1) {
        uint32_t loc = __builtin_ctz(change);
        if (wantAttribs & (1u << loc)) FLUSHED(glEnableVertexAttribArray)(loc);
        else FLUSHED(glDisableVertexAttribArray)(loc);
    }
    enabledAttribs = wantAttribs;
    lap(R.perf.stateNs);

    // ---- draw
    if (indices.type) {
        cap(kRestart, stripRestart);
        if (gs.elementBuffer != indices.slice.buffer) {
            FLUSHED(glBindBuffer)(GL_ELEMENT_ARRAY_BUFFER, indices.slice.buffer);
            gs.elementBuffer = indices.slice.buffer;
        }
    }
    if (p->batchable) {
        const uint32_t indexSize = indices.type == GL_UNSIGNED_INT ? 4 : 2;
        record_draw(p, mode, indices.type, indices.type ? uint32_t(indices.count) : count,
                    indices.type ? uint32_t(indices.slice.offset / indexSize) : 0, baseVertex + rebase, instances,
                    instances > 1 || instanceData);
    } else if (indices.type) {
        flush_draws();
        glDrawElementsInstancedBaseVertex(mode, indices.count, indices.type, (const void*)indices.slice.offset, instances,
                                          (GLint)baseVertex);
    } else {
        flush_draws();
        glDrawArraysInstanced(mode, (GLint)baseVertex, count, instances);
    }
    lap(R.perf.submitNs);
    if (probed) probe_dump(colors[0], probeMode, "");
    auto tally = [](Surface* s) {
        if (s->drawFrame != R.frame) {
            s->drawFrame = R.frame;
            s->frameDraws = 0;
        }
        s->frameDraws++;
    };
    for (auto* c : colors)
        if (c) {
            mark_gpu_written(c);
            tally(c);
        }
    if (depth) {
        mark_gpu_written(depth);
        tally(depth);
    }
    if (gamepadDraw && timed) R.perf.gamepadDrawNs += (now_ns() - drawStart) * kDrawTimeSample;
    if (g_traceFrame) {
        trace_draw(nullptr);
        if (g_traceDraws || g_captureDraws) {
            LATTE_DB_DEPTH_CONTROL tdc;
            memcpy(&tdc, r + REGADDR::DB_DEPTH_CONTROL, 4);
            LATTE_PA_SU_SC_MODE_CNTL tpm;
            memcpy(&tpm, r + REGADDR::PA_SU_SC_MODE_CNTL, 4);
            LATTE_PA_CL_CLIP_CNTL tcl;
            memcpy(&tcl, r + REGADDR::PA_CL_CLIP_CNTL, 4);
            LOG("[trace]   draw raster: cull %s%s, front %s, viewport y %s, z %g..%g, clip %s, z clip near %s far %s, near clip plane %s",
                tpm.get_CULL_FRONT() ? "F" : "", tpm.get_CULL_BACK() ? "B" : (tpm.get_CULL_FRONT() ? "" : "none"),
                tpm.get_FRONT_FACE() == LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW ? "ccw" : "cw",
                f32(r[REGADDR::PA_CL_VPORT_YSCALE]) < 0 ? "down (upper-left)" : "up (lower-left)",
                f32(r[REGADDR::PA_CL_VPORT_ZOFFSET]) - (tcl.get_DX_CLIP_SPACE_DEF() ? 0.0f : f32(r[REGADDR::PA_CL_VPORT_ZSCALE])),
                f32(r[REGADDR::PA_CL_VPORT_ZOFFSET]) + f32(r[REGADDR::PA_CL_VPORT_ZSCALE]),
                tcl.get_DX_CLIP_SPACE_DEF() ? "0..1" : "-1..1", tcl.get_ZCLIP_NEAR_DISABLE() ? "off" : "on",
                tcl.get_ZCLIP_FAR_DISABLE() ? "off" : "on",
                p->nearClip && !tcl.get_ZCLIP_NEAR_DISABLE() ? "on" : "off");
            LOG("[trace]   draw c0=%s d=%s vs %08X/%016llX ps %08X/%016llX prim %u count %u inst %u; depth %s func %u write %u, "
                "stencil %u, poly offset %u (%g, %g), blend %08X, mask %08X;%s",
                trace_name(colors[0]).c_str(), trace_name(depth).c_str(), r[mmSQ_PGM_START_VS] << 8,
                (unsigned long long)vs->glslHash, r[mmSQ_PGM_START_PS] << 8, (unsigned long long)ps->glslHash, prim, count,
                instances, depth && tdc.get_Z_ENABLE() ? "on" : "off",
                uint32_t(tdc.get_Z_FUNC()), uint32_t(tdc.get_Z_WRITE_ENABLE()), uint32_t(tdc.get_STENCIL_ENABLE()),
                (r[REGADDR::PA_SU_SC_MODE_CNTL] >> 11) & 1, f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE]) / 16,
                f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET]), r[REGADDR::CB_BLEND0_CONTROL],
                r[REGADDR::CB_TARGET_MASK], g_traceTextures.c_str());
#ifndef __SWITCH__
            // test aid (desktop): WWHD_GL_TRACE_PIXELS=1 also gives the pixels each traced draw changed in its
            // first color target (read back after the draw): which draws make a part of the picture
            static const bool tracePixels = getenv("WWHD_GL_TRACE_PIXELS") != nullptr;
            if (tracePixels && colors[0] && colors[0]->tex && !colors[0]->fmt.depth) {
                flush_draws();
                Surface* c = colors[0];
                GLint w = 0, h = 0;
                glGetTextureLevelParameteriv(c->tex, 0, GL_TEXTURE_WIDTH, &w);
                glGetTextureLevelParameteriv(c->tex, 0, GL_TEXTURE_HEIGHT, &h);
                static std::unordered_map<GLuint, std::vector<uint8_t>> before;
                std::vector<uint8_t> now(size_t(w) * h * 4 * std::max<uint32_t>(c->slices, 1));
                glGetTextureImage(c->tex, 0, GL_RGBA, GL_UNSIGNED_BYTE, GLsizei(now.size()), now.data());
                auto& old = before[c->tex];
                if (old.size() == now.size()) {
                    size_t changed = 0;
                    int x0 = w, y0 = h, x1 = -1, y1 = -1;
                    double sum[3] = {};
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < w; x++) {
                            const uint8_t* a = &old[(size_t(y) * w + x) * 4];
                            const uint8_t* b = &now[(size_t(y) * w + x) * 4];
                            if (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) <= 6) continue;
                            changed++;
                            for (int k = 0; k < 3; k++) sum[k] += b[k] - a[k];
                            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
                        }
                    if (changed)
                        LOG("[trace]   draw pixels: %zu of %dx%d changed in %d,%d-%d,%d (upper-left origin flipped: GL rows "
                            "from the bottom), mean change %+.0f %+.0f %+.0f",
                            changed, w, h, x0, y0, x1, y1, sum[0] / changed, sum[1] / changed, sum[2] / changed);
                    else
                        LOG("[trace]   draw pixels: none changed");
                }
                old = std::move(now);
            }
#endif
        }
    }
    g_traceTextures.clear();
    R.drawCount++;
}
}  // namespace

}  // namespace gfxgl
