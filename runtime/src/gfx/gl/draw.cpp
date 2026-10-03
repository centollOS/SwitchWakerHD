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
    GLuint s = 0;
    glGenSamplers(1, &s);
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
enum Cap { kScissor, kDepthClamp, kCull, kPolyOffset, kDepthTest, kStencilTest, kSrgb, kLogicOp, kRestart, kCapCount };
constexpr GLenum kCapEnums[kCapCount] = {GL_SCISSOR_TEST, GL_DEPTH_CLAMP, GL_CULL_FACE, GL_POLYGON_OFFSET_FILL, GL_DEPTH_TEST,
                                         GL_STENCIL_TEST, GL_FRAMEBUFFER_SRGB, GL_COLOR_LOGIC_OP,
                                         GL_PRIMITIVE_RESTART_FIXED_INDEX};
constexpr int kTexUnits = 96, kUboBindings = 64, kAttribs = 32, kVertexBindings = 16;

struct Viewport {
    GLenum origin, depthMode;
    float rect[4], range[2];
};
struct Raster {
    GLenum cullFace, frontFace;
    float offset[2];
};
struct DepthState {
    GLenum func;
    GLboolean mask;
};
struct StencilState {
    uint32_t v[12];
};
struct BlendState {
    uint32_t on, src, dst, srcA, dstA, op, opA;
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
};
struct AttribState {
    GLint comps;
    GLenum type;
    GLuint offset, binding;
};
struct VertexBindingState {
    GLuint buffer;
    GLintptr offset;
    GLsizei stride;
    GLuint divisor;
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
    if (!memcmp(&cached, &value, sizeof(T))) return false;
    memcpy(&cached, &value, sizeof(T));
    return true;
}
void cap(Cap c, bool on) {
    if (gs.cap[c] == int8_t(on)) return;
    gs.cap[c] = int8_t(on);
    if (on) glEnable(kCapEnums[c]);
    else glDisable(kCapEnums[c]);
}
// value structs are zeroed first so padding never makes equal states compare different
template <class T> T zeroed() {
    T v;
    memset(&v, 0, sizeof v);
    return v;
}

// a snapshot of a surface that this draw also renders to (sampling an attached texture is undefined)
Surface* feedback_copy(Surface* s) {
    static std::unordered_map<Surface*, std::pair<uint64_t, std::unique_ptr<Surface>>> copies;
    auto& [seq, copy] = copies[s];
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
        create_surface_texture(copy.get());
        seq = 0;
    }
    if (seq != s->writeSeq) {
        glCopyImageSubData(s->tex, s->target, 0, 0, 0, 0, copy->tex, copy->target, 0, 0, 0, 0, s->width, s->height,
                           s->target == GL_TEXTURE_2D ? 1 : s->layers);
        seq = s->writeSeq;
    }
    return copy.get();
}

struct TextureBinding {
    GLuint unit, texture, sampler;
    GLenum target;
};
struct UboBinding {
    GLuint binding;
    StreamSlice slice;
    GLsizeiptr size;
};

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

// uniform blocks and textures of one stage; textures are resolved (and uploaded) before any is bound
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
            bound = (size + 15) & ~15u;
            slice = stream_guest(addr, bound, R.uboAlignment);
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
        Surface* s = sampled_texture(words, sh->dec->textureUsesDepthCompare[unit]);
        if (!s) continue;
        bool aliases = depth && s == depth;
        for (auto* c : colors)
            if (c && c == s) aliases = true;
        if (aliases) s = feedback_copy(s);
        GLenum target;
        GLuint view = sampled_view(s, words, target);
        const uint32_t* samplerWords = r + REGADDR::SQ_TEX_SAMPLER_WORD0_0 + ((sh->vertex ? 18 : 0) + samplerId) * 3;
        GLuint smp = sampler(samplerWords, sh->dec->textureUsesDepthCompare[unit], s->fmt.kind != FormatInfo::FLOAT);
        textures.push_back({(GLuint)binding, view, smp, target});
    }
}

// loose uniforms, skipped when the program already holds the same values (glUniform* writes the
// bound program's storage, which keeps its values across program switches)
void set_uniforms(const uint32_t* r, Shader* sh, Program* p) {
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
        static std::vector<uint8_t> data;
        data.assign(dec->list_remappedUniformEntries.size() * 16, 0);
        auto copy = [&](uint32_t offset, const void* src) {
            if (offset + 16 <= data.size()) memcpy(data.data() + offset, src, 16);
        };
        for (const auto& e : dec->list_remappedUniformEntries_register) copy(e.mappedIndexOffset, r + aluBase + e.indexOffset / 4);
        for (const auto& g : dec->list_remappedUniformEntries_bufferGroups) {
            uint32_t address = r[blockBase + g.kcacheBankIdOffset / 4];
            if (!address) continue;
            for (const auto& e : g.entries) copy(e.mappedIndexOffset, ppc_ptr(address + e.indexOffset));
        }
        if (shadow.size() != data.size() || memcmp(shadow.data(), data.data(), data.size())) {
            shadow = data;
            glUniform4iv(remapped, (GLsizei)dec->list_remappedUniformEntries.size(), (const GLint*)data.data());
        }
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
            point = point == 0 ? 0.125f : point;
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
    uint32_t maxIndex = 0;
};

template <int Type, class Out>
void build_indices(const uint8_t* src, uint32_t count, uint32_t prim, bool restart, uint32_t restartIndex,
                   std::vector<Out>& out, uint32_t& maxIndex) {
    constexpr Out kRestart = Out(~Out(0));
    uint32_t m = 0;
    auto get = [&](uint32_t i) -> Out {
        uint32_t v = read_index<Type>(src, i);
        if (restart && v == restartIndex) return kRestart;
        m = std::max(m, v);
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
                out[i] = Out(v);
            }
        } else
            for (uint32_t i = 0; i < count; i++) out[i] = get(i);
        break;
    }
    maxIndex = m;
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
        build_indices<Type>(src, count, prim, restart, restartIndex, out, list.maxIndex);
        list.count = GLsizei(out.size());
        list.type = GL_UNSIGNED_SHORT;
        list.slice = stream_upload(out.data(), out.size() * 2, 4);
        R.perf.indexBytes += out.size() * 2;
    } else {
        static std::vector<uint32_t> out;
        build_indices<Type>(src, count, prim, restart, restartIndex, out, list.maxIndex);
        list.count = GLsizei(out.size());
        list.type = GL_UNSIGNED_INT;
        list.slice = stream_upload(out.data(), out.size() * 4, 4);
        R.perf.indexBytes += out.size() * 4;
    }
    return list;
}

IndexList index_list(uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, bool restart, uint32_t restartIndex) {
    struct Entry {
        uint64_t gen;
        uint32_t addr, count, type, prim, restartIndex;
        bool restart;
        IndexList list;
    };
    static std::unordered_map<uint64_t, Entry> cache;
    static uint64_t clearedFrame = ~0ull;
    if (clearedFrame != R.frame) {
        cache.clear();
        clearedFrame = R.frame;
    }
    const uint64_t key = (uint64_t(indexAddr) << 32 | count) ^ (uint64_t(prim) << 56 | uint64_t(indexType) << 48) ^
                         (restart ? 0x5bd1e995ull * (restartIndex + 1) : 0);
    static const bool noCache = getenv("WWHD_GL_NO_INDEX_CACHE") != nullptr;
    auto it = noCache ? cache.end() : cache.find(key);
    if (it != cache.end()) {
        const Entry& e = it->second;
        if (e.gen == R.streamGen && e.addr == indexAddr && e.count == count && e.type == indexType && e.prim == prim &&
            e.restart == restart && e.restartIndex == restartIndex)
            return e.list;
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
    cache[key] = {R.streamGen, indexAddr, count, indexType, prim, restartIndex, restart, list};
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
}  // namespace

void draw(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, uint32_t baseVertex,
          uint32_t instances) {
    make_current();
    ScopedTime timer{R.perf.drawNs};
    if (!count || !instances || ((prim == 0x13 || prim == 0x14) && count < 4)) return;
    if (r[REGADDR::PA_CL_CLIP_CNTL] & (1 << 22)) return;  // rasterization disabled
    uint64_t t0 = now_ns();
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
        uint64_t fsKey = 0;
        fs = get_fetch_shader(r, &fsKey, R.frame);
        vs = fs ? translate(r, true, fs, fsKey, R.frame) : nullptr;
        ps = fs ? translate(r, false, fs, fsKey, R.frame) : nullptr;
        p = vs && ps && vs->ready() && ps->ready() ? program(vs, ps) : nullptr;
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
    uint64_t t1 = now_ns();
    R.perf.lookupNs += t1 - t0;

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
    uint64_t t2 = now_ns();
    R.perf.indexNs += t2 - t1;

    // ---- render targets
    const auto& lcr = *reinterpret_cast<const LatteContextRegister*>(r);
    std::array<Surface*, 8> colors{};
    uint32_t slices[8]{}, depthSlice = 0;
    auto mask = LatteMRT::GetActiveColorBufferMask(ps->dec, lcr);
    for (int i = 0; i < 8; i++)
        if (mask & (1 << i)) colors[i] = color_target(r, i, &slices[i]);
    Surface* depth = LatteMRT::GetActiveDepthBufferMask(lcr) ? depth_target(r, &depthSlice) : nullptr;
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
    for (auto* c : colors)
        if (c) upload_surface(c);
    if (depth) upload_surface(depth);

    // ---- textures and uniform blocks (may upload; nothing is bound to draw units yet)
    static std::vector<TextureBinding> textures;
    static std::vector<UboBinding> ubos;
    textures.clear();
    ubos.clear();
    prepare_stage(r, vs, p, colors, depth, textures, ubos);
    prepare_stage(r, ps, p, colors, depth, textures, ubos);
    uint64_t t3 = now_ns();
    R.perf.resourceNs += t3 - t2;

    // ---- framebuffer
    sync_cache();
    if (gs.fbo != R.drawFbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, R.drawFbo);
        gs.fbo = R.drawFbo;
    }
    struct Attachment { Surface* s = nullptr; uint32_t slice = 0; };
    static std::array<Attachment, 9> bound;
    static bool dirty = true;
    for (int i = 0; i < 8; i++)
        if (bound[i].s != colors[i] || bound[i].slice != slices[i]) {
            attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, colors[i], 0, slices[i]);
            bound[i] = {colors[i], slices[i]};
            dirty = true;
        }
    if (bound[8].s != depth || bound[8].slice != depthSlice) {
        glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, 0, 0);
        if (depth) attach(GL_FRAMEBUFFER, depth_attachment(depth), depth, 0, depthSlice);
        bound[8] = {depth, depthSlice};
        dirty = true;
    }
    if (dirty) {
        GLenum bufs[8];
        for (int i = 0; i < 8; i++) bufs[i] = colors[i] ? GL_COLOR_ATTACHMENT0 + i : GL_NONE;
        glDrawBuffers(8, bufs);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            log_once(0xFB000000u | status, "[gl] incomplete framebuffer (%s)", std::to_string(status));
            bound = {};
            R.skippedDraws++;
            return;
        }
        dirty = false;
    }
    for (auto& t : textures) {
        if (t.unit >= (GLuint)kTexUnits) {
            glActiveTexture(GL_TEXTURE0 + t.unit);
            glBindTexture(t.target, t.texture);
            glBindSampler(t.unit, t.sampler);
            continue;
        }
        TexState& cached = gs.tex[t.unit];
        if (cached.tex != t.texture || cached.target != t.target) {
            glActiveTexture(GL_TEXTURE0 + t.unit);
            glBindTexture(t.target, t.texture);
            cached.tex = t.texture;
            cached.target = t.target;
        }
        if (cached.sampler != t.sampler) {
            glBindSampler(t.unit, t.sampler);
            cached.sampler = t.sampler;
        }
    }
    for (auto& u : ubos) {
        UboState v = zeroed<UboState>();
        v.buffer = u.slice.buffer;
        v.offset = u.slice.offset;
        v.size = u.size;
        if (u.binding >= (GLuint)kUboBindings || changed(gs.ubo[u.binding], v))
            glBindBufferRange(GL_UNIFORM_BUFFER, u.binding, v.buffer, v.offset, v.size);
    }

    // ---- program and uniforms
    if (gs.program != p->prog) {
        glUseProgram(p->prog);
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
        Viewport v = zeroed<Viewport>();
        v.origin = upper ? GL_UPPER_LEFT : GL_LOWER_LEFT;
        v.depthMode = dxClip ? GL_ZERO_TO_ONE : GL_NEGATIVE_ONE_TO_ONE;
        v.rect[0] = xo - std::fabs(xs);
        v.rect[1] = upper ? yo + ys : yo - ys;
        v.rect[2] = 2 * std::fabs(xs);
        v.rect[3] = 2 * std::fabs(ys);
        v.range[0] = dxClip ? zo : zo - zs;
        v.range[1] = zo + zs;
        Viewport old = gs.viewport;
        if (changed(gs.viewport, v)) {
            if (old.origin != v.origin || old.depthMode != v.depthMode)
                if (auto cc = clip_control()) cc(v.origin, v.depthMode);
            if (memcmp(old.rect, v.rect, sizeof v.rect)) glViewportIndexedf(0, v.rect[0], v.rect[1], v.rect[2], v.rect[3]);
            if (memcmp(old.range, v.range, sizeof v.range)) glDepthRangef(v.range[0], v.range[1]);
        }
    }
    cap(kDepthClamp, clip.get_ZCLIP_FAR_DISABLE());

    uint32_t width = target->width, height = target->height;
    uint32_t tl = r[REGADDR::PA_SC_GENERIC_SCISSOR_TL], br = r[REGADDR::PA_SC_GENERIC_SCISSOR_BR];
    uint32_t x = std::min(tl & 0x7fff, width), y = std::min((tl >> 16) & 0x7fff, height);
    uint32_t ex = std::min(br & 0x7fff, width), ey = std::min((br >> 16) & 0x7fff, height);
    if (ex <= x || ey <= y) return;
    cap(kScissor, true);
    {
        GLint sc[4] = {GLint(x), GLint(y), GLint(ex - x), GLint(ey - y)};
        if (memcmp(gs.scissor, sc, sizeof sc)) {
            memcpy(gs.scissor, sc, sizeof sc);
            glScissor(sc[0], sc[1], sc[2], sc[3]);
        }
    }

    // ---- rasterizer
    LATTE_PA_SU_SC_MODE_CNTL pm;
    memcpy(&pm, r + REGADDR::PA_SU_SC_MODE_CNTL, 4);
    bool cullFront = pm.get_CULL_FRONT(), cullBack = pm.get_CULL_BACK();
    // Latte decides facing in y-down window space; GL with a lower-left origin sees it mirrored
    bool ccw = pm.get_FRONT_FACE() == LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW;
    {
        Raster v = zeroed<Raster>();
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
            if (old.cullFace != v.cullFace) glCullFace(v.cullFace);
            if (old.frontFace != v.frontFace) glFrontFace(v.frontFace);
            if (offset && memcmp(old.offset, v.offset, sizeof v.offset)) glPolygonOffset(v.offset[0], v.offset[1]);
        }
        cap(kCull, cullFront || cullBack);
        cap(kPolyOffset, offset);
    }

    // ---- depth / stencil
    LATTE_DB_DEPTH_CONTROL dc;
    memcpy(&dc, r + REGADDR::DB_DEPTH_CONTROL, 4);
    if (depth && dc.get_Z_ENABLE()) {
        cap(kDepthTest, true);
        DepthState v = zeroed<DepthState>();
        DepthState old = gs.depth;
        v.func = GL_NEVER + uint32_t(dc.get_Z_FUNC());
        v.mask = dc.get_Z_WRITE_ENABLE() ? GL_TRUE : GL_FALSE;
        if (changed(gs.depth, v)) {
            if (old.func != v.func) glDepthFunc(v.func);
            if (old.mask != v.mask) glDepthMask(v.mask);
        }
    } else
        cap(kDepthTest, false);
    if (depth && depth->fmt.stencil && dc.get_STENCIL_ENABLE()) {
        cap(kStencilTest, true);
        uint32_t f = r[REGADDR::DB_STENCILREFMASK];
        bool separate = dc.get_BACK_STENCIL_ENABLE();
        uint32_t b = separate ? r[REGADDR::DB_STENCILREFMASK_BF] : f;
        StencilState v = zeroed<StencilState>();
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
            glStencilFuncSeparate(GL_FRONT, v.v[0], f & 255, (f >> 8) & 255);
            glStencilOpSeparate(GL_FRONT, v.v[2], v.v[3], v.v[4]);
            glStencilMaskSeparate(GL_FRONT, (f >> 16) & 255);
            glStencilFuncSeparate(GL_BACK, v.v[5], b & 255, (b >> 8) & 255);
            glStencilOpSeparate(GL_BACK, v.v[7], v.v[8], v.v[9]);
            glStencilMaskSeparate(GL_BACK, (b >> 16) & 255);
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
            glColorMaski(i, m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1);
            gs.colorMask[i] = m;
        }
        bool on = colors[i]->fmt.kind == FormatInfo::FLOAT && ((r[REGADDR::CB_COLOR_CONTROL] >> (8 + i)) & 1);
        BlendState v = zeroed<BlendState>();
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
                if (on) glEnablei(GL_BLEND, i);
                else glDisablei(GL_BLEND, i);
            }
            if (on && (old.src != v.src || old.dst != v.dst || old.srcA != v.srcA || old.dstA != v.dstA))
                glBlendFuncSeparatei(i, v.src, v.dst, v.srcA, v.dstA);
            if (on && (old.op != v.op || old.opA != v.opA)) glBlendEquationSeparatei(i, v.op, v.opA);
        }
    }
    if (constantColor) {
        const float* constant = reinterpret_cast<const float*>(r + REGADDR::CB_BLEND_RED);
        if (memcmp(gs.blendColor, constant, sizeof gs.blendColor)) {
            memcpy(gs.blendColor, constant, sizeof gs.blendColor);
            glBlendColor(constant[0], constant[1], constant[2], constant[3]);
        }
    }
    uint32_t rop = (r[REGADDR::CB_COLOR_CONTROL] >> 16) & 255;
    if (rop != 0xCC) {
        cap(kLogicOp, true);
        GLenum op = logic_op(rop);
        if (gs.logicOp != op) glLogicOp(gs.logicOp = op);
    } else
        cap(kLogicOp, false);

    // ---- vertex buffers (guest bytes as stored)
    static uint32_t enabledAttribs = 0;
    uint32_t wantAttribs = 0;
    for (auto& g : fs->bufferGroups) {
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
            AttribState v = zeroed<AttribState>();
            uint32_t bytes;
            if (!vertex_format(a.format, v.comps, v.type, bytes)) continue;
            v.offset = a.offset;
            v.binding = g.attributeBufferIndex;
            AttribState old = gs.attrib[loc];
            if (changed(gs.attrib[loc], v)) {
                if (old.comps != v.comps || old.type != v.type || old.offset != v.offset)
                    glVertexAttribIFormat(loc, v.comps, v.type, v.offset);
                if (old.binding != v.binding) glVertexAttribBinding(loc, v.binding);
            }
            wantAttribs |= 1u << loc;
            attributeEnd = std::max<uint64_t>(attributeEnd, uint64_t(a.offset) + bytes);
            if (a.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA) instance = true;
        }
        uint64_t last = instance ? instances - 1 : maxVertex;
        uint64_t copied = std::min<uint64_t>(size, last * stride + std::max<uint64_t>(attributeEnd, stride));
        auto slice = stream_guest(addr, (size_t)std::max<uint64_t>(copied, 4), 16);
        R.perf.vertexBytes += copied;
        VertexBindingState v = zeroed<VertexBindingState>();
        v.buffer = slice.buffer;
        v.offset = slice.offset;
        v.stride = GLsizei(stride);
        v.divisor = instance ? 1 : 0;
        uint32_t index = g.attributeBufferIndex;
        if (index >= (uint32_t)kVertexBindings) {
            glBindVertexBuffer(index, v.buffer, v.offset, v.stride);
            glVertexBindingDivisor(index, v.divisor);
            continue;
        }
        VertexBindingState old = gs.binding[index];
        if (changed(gs.binding[index], v)) {
            if (old.buffer != v.buffer || old.offset != v.offset || old.stride != v.stride)
                glBindVertexBuffer(index, v.buffer, v.offset, v.stride);
            if (old.divisor != v.divisor) glVertexBindingDivisor(index, v.divisor);
        }
    }
    for (uint32_t change = enabledAttribs ^ wantAttribs; change; change &= change - 1) {
        uint32_t loc = __builtin_ctz(change);
        if (wantAttribs & (1u << loc)) glEnableVertexAttribArray(loc);
        else glDisableVertexAttribArray(loc);
    }
    enabledAttribs = wantAttribs;
    uint64_t t4 = now_ns();
    R.perf.stateNs += t4 - t3;

    // ---- draw
    if (indices.type) {
        cap(kRestart, stripRestart);
        if (gs.elementBuffer != indices.slice.buffer) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices.slice.buffer);
            gs.elementBuffer = indices.slice.buffer;
        }
        glDrawElementsInstancedBaseVertex(mode, indices.count, indices.type, (const void*)indices.slice.offset, instances,
                                          (GLint)baseVertex);
    } else
        glDrawArraysInstanced(mode, (GLint)baseVertex, count, instances);
    R.perf.submitNs += now_ns() - t4;
    for (auto* c : colors)
        if (c) mark_gpu_written(c);
    if (depth) mark_gpu_written(depth);
    R.drawCount++;
}

}  // namespace gfxgl
