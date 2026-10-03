// OpenGL draw submission: GX2 register state -> GL state (the translation of gfx/vulkan/draw.cpp).
#include <algorithm>
#include <array>
#include <cmath>
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
    static std::unordered_map<std::string, GLuint> cache;
    std::string key(reinterpret_cast<const char*>(words), 12);
    key.push_back(compare);
    key.push_back(integer);
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
    cache.emplace(std::move(key), s);
    return s;
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

// uniform blocks and textures of one stage; textures are resolved (and uploaded) before any is bound
void prepare_stage(const uint32_t* r, Shader* sh, Program* p, const std::array<Surface*, 8>& colors, Surface* depth,
                   std::vector<TextureBinding>& textures) {
    auto& m = sh->mapping;
    uint32_t block = sh->vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
    static std::vector<uint8_t> scratch;
    for (int i = 0; i < 16; i++) {
        int binding = m.uniformBuffersBindingPoint[i];
        if (binding < 0 || binding >= (int)p->blockSize.size() || !p->blockSize[binding]) continue;
        uint32_t addr = r[block + i * 7], size = std::min<uint32_t>(r[block + i * 7 + 1] + 1, 0x10000);
        uint32_t need = (uint32_t)p->blockSize[binding];
        StreamSlice slice;
        if (addr && size >= need)
            slice = stream_guest(addr, need, R.uboAlignment);
        else {
            scratch.assign(need, 0);
            if (addr) memcpy(scratch.data(), mem::ptr(addr), std::min(size, need));
            slice = stream_upload(scratch.data(), need, R.uboAlignment);
        }
        glBindBufferRange(GL_UNIFORM_BUFFER, binding, slice.buffer, slice.offset, need);
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

void set_uniforms(const uint32_t* r, Shader* sh, Program* p) {
    auto* dec = sh->dec;
    const uint32_t aluBase = mmSQ_ALU_CONSTANT0_0 + (sh->vertex ? 0x400 : 0);
    const uint32_t blockBase = sh->vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
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
        glUniform4iv(remapped, (GLsizei)dec->list_remappedUniformEntries.size(), (const GLint*)data.data());
    }
    GLint registers = sh->vertex ? p->registersVS : p->registersPS;
    if (registers >= 0 && sh->registerCount) glUniform4iv(registers, (GLsizei)sh->registerCount, (const GLint*)(r + aluBase));
    if (sh->vertex) {
        if (p->pointSize >= 0) {
            float point = float(r[REGADDR::PA_SU_POINT_SIZE] & 0xFFFF) / 8.0f;
            glUniform1f(p->pointSize, point == 0 ? 0.125f : point);
        }
        if (p->windowToClip >= 0) {
            float width = 2.0f * f32(r[REGADDR::PA_CL_VPORT_XSCALE]), height = -2.0f * f32(r[REGADDR::PA_CL_VPORT_YSCALE]);
            glUniform2f(p->windowToClip, width != 0 ? 2.0f / width : 0, height != 0 ? 2.0f / height : 0);
        }
    } else if (p->alphaRef >= 0)
        glUniform1f(p->alphaRef, f32(r[REGADDR::SX_ALPHA_REF]));
}


void log_once(uint64_t key, const char* fmt, const std::string& what) {
    static std::unordered_set<uint64_t> seen;
    if (seen.insert(key).second) LOG(fmt, what.c_str());
}
}  // namespace

void draw(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, uint32_t baseVertex,
          uint32_t instances) {
    make_current();
    ScopedTime timer{R.perf.drawNs};
    if (!count || !instances || ((prim == 0x13 || prim == 0x14) && count < 4)) return;
    if (r[REGADDR::PA_CL_CLIP_CNTL] & (1 << 22)) return;  // rasterization disabled
    ((uint32_t*)r)[REGADDR::VGT_PRIMITIVE_TYPE] = prim;
    uint64_t fsKey = 0;
    auto* fs = get_fetch_shader(r, &fsKey, R.frame);
    if (!fs) return;
    auto* vs = translate(r, true, fs, fsKey, R.frame);
    auto* ps = translate(r, false, fs, fsKey, R.frame);
    if (!vs || !ps || !vs->ready() || !ps->ready()) {
        R.skippedDraws++;
        if (vs && !vs->ready()) log_once(vs->key, "[gl] vertex shader skipped: %s", vs->error);
        if (ps && !ps->ready()) log_once(ps->key, "[gl] pixel shader skipped: %s", ps->error);
        return;
    }
    Program* p = program(vs, ps);
    if (!p) {
        R.skippedDraws++;
        return;
    }

    // ---- indices: guest index formats and primitives GL lacks become 32-bit triangle/line lists
    const bool stripRestart = indexAddr && (prim == 3 || prim == 6) && (r[REGADDR::VGT_MULTI_PRIM_IB_RESET_EN] & 1);
    const uint32_t restartIndex = r[REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX];
    auto idx = [&](uint32_t i) -> uint32_t {
        if (!indexAddr) return i;
        uint32_t value;
        switch (indexType) {
        case 0: value = ((uint16_t*)mem::ptr(indexAddr))[i]; break;
        case 1: value = ((uint32_t*)mem::ptr(indexAddr))[i]; break;
        case 4: value = ld16(indexAddr + i * 2); break;
        case 9: value = ld32(indexAddr + i * 4); break;
        default: value = i; break;
        }
        return stripRestart && value == restartIndex ? UINT32_MAX : value;
    };
    static std::vector<uint32_t> indices;
    indices.clear();
    GLenum mode;
    switch (prim) {
    case 1: mode = GL_POINTS; break;
    case 2: mode = GL_LINES; break;
    case 3: mode = GL_LINE_STRIP; break;
    case 4: mode = GL_TRIANGLES; break;
    case 6: mode = GL_TRIANGLE_STRIP; break;
    case 5:
        mode = GL_TRIANGLES;
        for (uint32_t i = 1; i + 1 < count; i++) indices.insert(indices.end(), {idx(0), idx(i), idx(i + 1)});
        break;
    case 0x13:
        mode = GL_TRIANGLES;
        for (uint32_t i = 0; i + 3 < count; i += 4)
            indices.insert(indices.end(), {idx(i), idx(i + 1), idx(i + 2), idx(i), idx(i + 2), idx(i + 3)});
        break;
    case 0x14:
        mode = GL_TRIANGLES;
        for (uint32_t i = 0; i + 3 < count; i += 2)
            indices.insert(indices.end(), {idx(i), idx(i + 1), idx(i + 2), idx(i + 1), idx(i + 3), idx(i + 2)});
        break;
    case 0x12:
        mode = GL_LINE_STRIP;
        for (uint32_t i = 0; i < count; i++) indices.push_back(idx(i));
        indices.push_back(idx(0));
        break;
    default:
        log_once(0xD0000000u | prim, "[gl] unsupported primitive %s", std::to_string(prim));
        return;
    }
    if (indices.empty() && indexAddr) {
        indices.resize(count);
        for (uint32_t i = 0; i < count; i++) indices[i] = idx(i);
    }
    uint64_t maxVertex = uint64_t(baseVertex) + count - 1;
    if (!indices.empty()) {
        uint32_t m = 0;
        for (uint32_t v : indices)
            if (v != UINT32_MAX) m = std::max(m, v);
        maxVertex = uint64_t(m) + baseVertex;
    }

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
    textures.clear();
    prepare_stage(r, vs, p, colors, depth, textures);
    prepare_stage(r, ps, p, colors, depth, textures);

    // ---- framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, R.drawFbo);
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
        glActiveTexture(GL_TEXTURE0 + t.unit);
        glBindTexture(t.target, t.texture);
        glBindSampler(t.unit, t.sampler);
    }

    // ---- program and uniforms
    glUseProgram(p->prog);
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
    if (auto cc = clip_control()) cc(upper ? GL_UPPER_LEFT : GL_LOWER_LEFT, dxClip ? GL_ZERO_TO_ONE : GL_NEGATIVE_ONE_TO_ONE);
    glViewportIndexedf(0, xo - std::fabs(xs), upper ? yo + ys : yo - ys, 2 * std::fabs(xs), 2 * std::fabs(ys));
    glDepthRangef(dxClip ? zo : zo - zs, zo + zs);
    if (clip.get_ZCLIP_FAR_DISABLE()) glEnable(GL_DEPTH_CLAMP);
    else glDisable(GL_DEPTH_CLAMP);

    uint32_t width = target->width, height = target->height;
    uint32_t tl = r[REGADDR::PA_SC_GENERIC_SCISSOR_TL], br = r[REGADDR::PA_SC_GENERIC_SCISSOR_BR];
    uint32_t x = std::min(tl & 0x7fff, width), y = std::min((tl >> 16) & 0x7fff, height);
    uint32_t ex = std::min(br & 0x7fff, width), ey = std::min((br >> 16) & 0x7fff, height);
    if (ex <= x || ey <= y) return;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, ex - x, ey - y);

    // ---- rasterizer
    LATTE_PA_SU_SC_MODE_CNTL pm;
    memcpy(&pm, r + REGADDR::PA_SU_SC_MODE_CNTL, 4);
    bool cullFront = pm.get_CULL_FRONT(), cullBack = pm.get_CULL_BACK();
    if (cullFront || cullBack) {
        glEnable(GL_CULL_FACE);
        glCullFace(cullFront && cullBack ? GL_FRONT_AND_BACK : cullFront ? GL_FRONT : GL_BACK);
    } else
        glDisable(GL_CULL_FACE);
    // Latte decides facing in y-down window space; GL with a lower-left origin sees it mirrored
    bool ccw = pm.get_FRONT_FACE() == LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW;
    glFrontFace((upper ? ccw : !ccw) ? GL_CCW : GL_CW);
    if (pm.get_OFFSET_FRONT_ENABLED()) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE]) / 16, f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET]));
    } else
        glDisable(GL_POLYGON_OFFSET_FILL);

    // ---- depth / stencil
    LATTE_DB_DEPTH_CONTROL dc;
    memcpy(&dc, r + REGADDR::DB_DEPTH_CONTROL, 4);
    if (depth && dc.get_Z_ENABLE()) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_NEVER + uint32_t(dc.get_Z_FUNC()));
        glDepthMask(dc.get_Z_WRITE_ENABLE() ? GL_TRUE : GL_FALSE);
    } else
        glDisable(GL_DEPTH_TEST);
    if (depth && depth->fmt.stencil && dc.get_STENCIL_ENABLE()) {
        glEnable(GL_STENCIL_TEST);
        uint32_t f = r[REGADDR::DB_STENCILREFMASK];
        glStencilFuncSeparate(GL_FRONT, GL_NEVER + uint32_t(dc.get_STENCIL_FUNC_F()), f & 255, (f >> 8) & 255);
        glStencilOpSeparate(GL_FRONT, stencil_op(uint32_t(dc.get_STENCIL_FAIL_F())),
                            stencil_op(uint32_t(dc.get_STENCIL_ZFAIL_F())), stencil_op(uint32_t(dc.get_STENCIL_ZPASS_F())));
        glStencilMaskSeparate(GL_FRONT, (f >> 16) & 255);
        if (dc.get_BACK_STENCIL_ENABLE()) {
            uint32_t b = r[REGADDR::DB_STENCILREFMASK_BF];
            glStencilFuncSeparate(GL_BACK, GL_NEVER + uint32_t(dc.get_STENCIL_FUNC_B()), b & 255, (b >> 8) & 255);
            glStencilOpSeparate(GL_BACK, stencil_op(uint32_t(dc.get_STENCIL_FAIL_B())),
                                stencil_op(uint32_t(dc.get_STENCIL_ZFAIL_B())), stencil_op(uint32_t(dc.get_STENCIL_ZPASS_B())));
            glStencilMaskSeparate(GL_BACK, (b >> 16) & 255);
        } else {
            glStencilFuncSeparate(GL_BACK, GL_NEVER + uint32_t(dc.get_STENCIL_FUNC_F()), f & 255, (f >> 8) & 255);
            glStencilOpSeparate(GL_BACK, stencil_op(uint32_t(dc.get_STENCIL_FAIL_F())),
                                stencil_op(uint32_t(dc.get_STENCIL_ZFAIL_F())), stencil_op(uint32_t(dc.get_STENCIL_ZPASS_F())));
            glStencilMaskSeparate(GL_BACK, (f >> 16) & 255);
        }
    } else
        glDisable(GL_STENCIL_TEST);

    // ---- color output
    glEnable(GL_FRAMEBUFFER_SRGB);
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t m = (r[REGADDR::CB_TARGET_MASK] >> (4 * i)) & 15;
        glColorMaski(i, m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1);
        bool blend = colors[i] && colors[i]->fmt.kind == FormatInfo::FLOAT && ((r[REGADDR::CB_COLOR_CONTROL] >> (8 + i)) & 1);
        if (!blend) {
            glDisablei(GL_BLEND, i);
            continue;
        }
        LATTE_CB_BLENDN_CONTROL b;
        memcpy(&b, r + REGADDR::CB_BLEND0_CONTROL + i, 4);
        GLenum src = blend_factor(uint32_t(b.get_COLOR_SRCBLEND())), dst = blend_factor(uint32_t(b.get_COLOR_DSTBLEND()));
        GLenum op = blend_op(uint32_t(b.get_COLOR_COMB_FCN()));
        bool separate = b.get_SEPARATE_ALPHA_BLEND();
        glEnablei(GL_BLEND, i);
        glBlendFuncSeparatei(i, src, dst, separate ? blend_factor(uint32_t(b.get_ALPHA_SRCBLEND())) : src,
                             separate ? blend_factor(uint32_t(b.get_ALPHA_DSTBLEND())) : dst);
        glBlendEquationSeparatei(i, op, separate ? blend_op(uint32_t(b.get_ALPHA_COMB_FCN())) : op);
    }
    const float* constant = reinterpret_cast<const float*>(r + REGADDR::CB_BLEND_RED);
    glBlendColor(constant[0], constant[1], constant[2], constant[3]);
    uint32_t rop = (r[REGADDR::CB_COLOR_CONTROL] >> 16) & 255;
    if (rop != 0xCC) {
        glEnable(GL_COLOR_LOGIC_OP);
        glLogicOp(logic_op(rop));
    } else
        glDisable(GL_COLOR_LOGIC_OP);

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
            if (loc < 0 || loc >= 32) continue;
            GLint comps;
            GLenum type;
            uint32_t bytes;
            if (!vertex_format(a.format, comps, type, bytes)) continue;
            glVertexAttribIFormat(loc, comps, type, a.offset);
            glVertexAttribBinding(loc, g.attributeBufferIndex);
            wantAttribs |= 1u << loc;
            attributeEnd = std::max<uint64_t>(attributeEnd, uint64_t(a.offset) + bytes);
            if (a.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA) instance = true;
        }
        uint64_t last = instance ? instances - 1 : maxVertex;
        uint64_t copied = std::min<uint64_t>(size, last * stride + std::max<uint64_t>(attributeEnd, stride));
        auto slice = stream_guest(addr, (size_t)std::max<uint64_t>(copied, 4), 16);
        glBindVertexBuffer(g.attributeBufferIndex, slice.buffer, slice.offset, stride);
        glVertexBindingDivisor(g.attributeBufferIndex, instance ? 1 : 0);
    }
    for (uint32_t changed = enabledAttribs ^ wantAttribs; changed; changed &= changed - 1) {
        uint32_t loc = __builtin_ctz(changed);
        if (wantAttribs & (1u << loc)) glEnableVertexAttribArray(loc);
        else glDisableVertexAttribArray(loc);
    }
    enabledAttribs = wantAttribs;

    // ---- draw
    if (!indices.empty()) {
        if (stripRestart) glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
        else glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
        auto slice = stream_upload(indices.data(), indices.size() * 4, 4);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, slice.buffer);
        glDrawElementsInstancedBaseVertex(mode, (GLsizei)indices.size(), GL_UNSIGNED_INT, (const void*)slice.offset,
                                          instances, (GLint)baseVertex);
    } else
        glDrawArraysInstanced(mode, (GLint)baseVertex, count, instances);
    for (auto* c : colors)
        if (c) mark_gpu_written(c);
    if (depth) mark_gpu_written(depth);
    R.drawCount++;
}

}  // namespace gfxgl
