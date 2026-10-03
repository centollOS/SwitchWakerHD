// Guest surfaces backed by OpenGL textures (the surface cache of gfx/vulkan/surfaces.cpp: same lookup,
// change detection and LatteAddrLib detiling, at the guest's resolution).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <tuple>

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "gl.h"
#include "gx2/gx2.h"
#include "gx2_texture_regs.h"
#include "runtime.h"

Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N&,
                                                     const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N&);
namespace gfxgl {

struct GuestLayout {
    std::vector<LatteAddrLib::AddrSurfaceInfo_OUT> info;
    std::vector<uint32_t> address;
};

uint64_t next_write_seq() {
    static uint64_t seq = 0;
    return ++seq;
}

namespace {
constexpr uint32_t kDim2D = 1, kDim2DArray = 5;

GuestLayout& layout(Surface* s) {
    if (!s->guest) {
        auto g = std::make_shared<GuestLayout>();
        g->info.resize(s->mips);
        g->address.resize(s->mips);
        for (uint32_t level = 0; level < s->mips; level++) {
            LatteAddrLib::GX2CalculateSurfaceInfo(static_cast<Latte::E_GX2SURFFMT>(s->format), s->width, s->height,
                                                  s->slices, static_cast<Latte::E_DIM>(s->dim),
                                                  Latte::MakeGX2TileMode(static_cast<Latte::E_HWTILEMODE>(s->tileMode)),
                                                  0, level, &g->info[level]);
            if (level == 0) g->address[0] = s->addr;
            else if (level == 1) g->address[1] = s->mipAddr;
            else {
                uint32_t address = 0, size = 0;
                sint32 sub = 0;
                LatteAddrLib::CalculateMipAndSliceAddr(s->addr, s->mipAddr, static_cast<Latte::E_GX2SURFFMT>(s->format),
                                                       s->width, s->height, s->slices, static_cast<Latte::E_DIM>(s->dim),
                                                       static_cast<Latte::E_HWTILEMODE>(s->tileMode), s->swizzle, 0, level,
                                                       0, &address, &size, &sub);
                g->address[level] = address;
            }
        }
        s->guest = std::move(g);
    }
    return *s->guest;
}

uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    for (; n - i >= sizeof(uint64_t); i += sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, p + i, sizeof(word));
        h = (h ^ word) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 29) ^ uint64_t(n);
}

// 256 words sampled per level: CPU changes show up immediately, periodic full checks catch the rest
uint64_t sparse_hash(Surface* s) {
    auto& g = layout(s);
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t level = 0; level < s->mips; ++level) {
        if (level && !s->mipAddr) break;
        const auto* bytes = mem::ptr(g.address[level]);
        size_t size = size_t(g.info[level].surfSize), step = std::max<size_t>((size / 256) & ~size_t(7), 8);
        size_t offset = 0;
        for (; offset < size && size - offset >= 8; offset += step) {
            uint64_t value;
            memcpy(&value, bytes + offset, 8);
            h = (h ^ value) * 0x100000001b3ull;
        }
    }
    return h;
}

void decode_level(Surface* s, uint32_t level, std::vector<uint8_t>& out, uint32_t& outW, uint32_t& outH,
                  uint32_t& outSlices) {
    const FormatInfo& f = s->fmt;
    auto& g = layout(s);
    uint32_t w = std::max(s->width >> level, 1u), h = std::max(s->height >> level, 1u);
    uint32_t slices = s->dim == (uint32_t)Latte::E_DIM::DIM_3D ? std::max(s->slices >> level, 1u) : s->slices;
    uint32_t bw = f.compressed ? (w + 3) / 4 : w, bh = f.compressed ? (h + 3) / 4 : h;
    outW = w;
    outH = h;
    outSlices = slices;
    const auto& info = g.info[level];
    uint32_t pitch = level == 0 && s->pitch ? s->pitch : info.pitch, height = info.height;
    auto tm = (Latte::E_HWTILEMODE)info.hwTileMode;
    uint32_t bpp = f.bytesPerBlock * 8;
    bool depthData = s->isDepth || f.convert == Convert::D24_R32F;
    uint32_t pipeSwizzle = (s->swizzle >> 8) & 1, bankSwizzle = (s->swizzle >> 9) & 3;
    out.assign((size_t)bw * bh * slices * f.hostBytesPerBlock, 0);
    std::vector<uint8_t> row(bw * f.bytesPerBlock);
    const uint8_t* src = mem::ptr(g.address[level]);
    for (uint32_t z = 0; z < slices; z++) {
        LatteAddrLib::CachedSurfaceAddrInfo ci;
        bool macro = Latte::TM_IsMacroTiled(tm);
        if (macro)
            LatteAddrLib::SetupCachedSurfaceAddrInfo(&ci, z, 0, bpp, pitch, height, slices, 1, tm, depthData, pipeSwizzle,
                                                     bankSwizzle);
        for (uint32_t y = 0; y < bh; y++) {
            for (uint32_t x = 0; x < bw; x++) {
                uint32_t off;
                if (tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, z, 0, bpp, pitch, height, slices);
                else if (!macro)
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, z, bpp, pitch, height, tm, depthData);
                else
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, &ci);
                memcpy(&row[x * f.bytesPerBlock], src + off, f.bytesPerBlock);
            }
            uint8_t* dst = &out[((size_t)z * bh + y) * bw * f.hostBytesPerBlock];
            if (f.convert == Convert::NONE) memcpy(dst, row.data(), row.size());
            else convert_row(f.convert, row.data(), dst, bw);
        }
    }
}

void bind_scratch(Surface* s) {
    glActiveTexture(GL_TEXTURE0 + R.scratchUnit);
    glBindTexture(s->target, s->tex);
}

uint32_t level_address(GX2Surface* s, uint32_t level) {
    if (level == 0) return s->imagePtr;
    if (level == 1) return s->mipPtr;
    return s->mipPtr + s->mipOffset[level - 1];
}

uint32_t element_offset(const LatteAddrLib::AddrSurfaceInfo_OUT& info, Latte::E_HWTILEMODE tm, uint32_t x, uint32_t y,
                        uint32_t slice, uint32_t bpp, LatteAddrLib::CachedSurfaceAddrInfo* ci, bool depth) {
    if (tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
        return LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, slice, 0, bpp, info.pitch, info.height, info.depth);
    if (!Latte::TM_IsMacroTiled(tm))
        return LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, slice, bpp, info.pitch, info.height, tm, depth);
    return LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, ci);
}
}  // namespace

// ---------------------------------------------------------------- textures
void create_surface_texture(Surface* s) {
    if (!s->fmt.internal) throw std::runtime_error("unsupported GX2 surface format " + std::to_string(s->format));
    auto dim = static_cast<Latte::E_DIM>(s->dim);
    bool oneD = dim == Latte::E_DIM::DIM_1D || dim == Latte::E_DIM::DIM_1D_ARRAY;
    bool threeD = dim == Latte::E_DIM::DIM_3D;
    bool cube = dim == Latte::E_DIM::DIM_CUBEMAP && s->slices % 6 == 0 && s->width == s->height;
    s->layers = threeD ? 1 : s->slices;
    if (threeD) s->target = GL_TEXTURE_3D;
    else if (oneD) s->target = s->slices > 1 ? GL_TEXTURE_1D_ARRAY : GL_TEXTURE_1D;
    else if (cube) s->target = s->slices > 6 ? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_CUBE_MAP;
    else s->target = s->slices > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    uint32_t maxDim = std::max({s->width, oneD ? 1u : s->height, threeD ? s->slices : 1u}), maxMips = 1;
    while (maxDim > 1) { maxDim >>= 1; ++maxMips; }
    s->mips = std::min(s->mips, maxMips);
    glGenTextures(1, &s->tex);
    bind_scratch(s);
    while (glGetError() != GL_NO_ERROR) {}  // report only this allocation's failure below
    switch (s->target) {
    case GL_TEXTURE_1D: glTexStorage1D(s->target, s->mips, s->fmt.internal, s->width); break;
    case GL_TEXTURE_1D_ARRAY: glTexStorage2D(s->target, s->mips, s->fmt.internal, s->width, s->slices); break;
    case GL_TEXTURE_2D:
    case GL_TEXTURE_CUBE_MAP: glTexStorage2D(s->target, s->mips, s->fmt.internal, s->width, s->height); break;
    default: glTexStorage3D(s->target, s->mips, s->fmt.internal, s->width, s->height, s->slices); break;
    }
    glTexParameteri(s->target, GL_TEXTURE_MAX_LEVEL, (GLint)s->mips - 1);
    if (GLenum e = glGetError())
        throw std::runtime_error("texture storage failed (GL error " + std::to_string(e) + ", format " +
                                 std::to_string(s->format) + ")");
}

void destroy_surface_texture(Surface* s) {
    forget_gl_state();  // a deleted texture may be bound to a draw unit
    for (auto& [key, view] : s->views) glDeleteTextures(1, &view);
    s->views.clear();
    if (s->tex) glDeleteTextures(1, &s->tex);
    s->tex = 0;
}

GLuint sampled_view(Surface* s, const uint32_t* texWords, GLenum& outTarget) {
    Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N w0;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N w4;
    memcpy(&w0, texWords, 4);
    memcpy(&w4, texWords + 4, 4);
    GLenum target = s->target;
    switch (w0.get_DIM()) {
    case Latte::E_DIM::DIM_1D: target = s->target == GL_TEXTURE_1D_ARRAY || s->target == GL_TEXTURE_1D ? GL_TEXTURE_1D : target; break;
    case Latte::E_DIM::DIM_1D_ARRAY: target = GL_TEXTURE_1D_ARRAY; break;
    case Latte::E_DIM::DIM_2D: case Latte::E_DIM::DIM_2D_MSAA:
        if (s->target != GL_TEXTURE_3D && s->target != GL_TEXTURE_1D && s->target != GL_TEXTURE_1D_ARRAY) target = GL_TEXTURE_2D;
        break;
    case Latte::E_DIM::DIM_2D_ARRAY: case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
        if (s->target != GL_TEXTURE_3D && s->target != GL_TEXTURE_1D && s->target != GL_TEXTURE_1D_ARRAY) target = GL_TEXTURE_2D_ARRAY;
        break;
    case Latte::E_DIM::DIM_CUBEMAP:
        if (s->target == GL_TEXTURE_CUBE_MAP_ARRAY || s->target == GL_TEXTURE_CUBE_MAP) target = s->target;
        break;
    default: break;
    }
    uint32_t sel[4] = {uint32_t(w4.get_DST_SEL_X()), uint32_t(w4.get_DST_SEL_Y()), uint32_t(w4.get_DST_SEL_Z()),
                       uint32_t(w4.get_DST_SEL_W())};
    bool identity = s->fmt.depth || (sel[0] == 0 && sel[1] == 1 && sel[2] == 2 && sel[3] == 3);
    outTarget = target;
    if (identity && target == s->target) return s->tex;
    uint32_t key = target << 12;
    for (unsigned i = 0; i < 4; ++i) key |= (s->fmt.depth ? i : sel[i]) << (i * 3);
    if (auto it = s->views.find(key); it != s->views.end()) return it->second;
    GLuint view = 0;
    glGenTextures(1, &view);
    uint32_t layers = target == GL_TEXTURE_2D || target == GL_TEXTURE_1D || target == GL_TEXTURE_3D ? 1
                      : target == GL_TEXTURE_CUBE_MAP                                                ? 6
                                                                                                      : s->layers;
    glTextureView(view, target, s->tex, s->fmt.internal, 0, s->mips, 0, layers);
    if (!s->fmt.depth) {
        static const GLint map[8] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA, GL_ZERO, GL_ONE, GL_ZERO, GL_ZERO};
        GLint swz[4] = {map[sel[0] & 7], map[sel[1] & 7], map[sel[2] & 7], map[sel[3] & 7]};
        glActiveTexture(GL_TEXTURE0 + R.scratchUnit);
        glBindTexture(target, view);
        glTexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, swz);
    }
    s->views.emplace(key, view);
    return view;
}

void upload_surface(Surface* s) {
    if (!s || !s->tex || s->gpuWritten) return;
    if (s->lastCheckedFrame == R.frame) return;
    s->lastCheckedFrame = R.frame;
    ScopedTime timer{R.perf.uploadNs};
    bool full = s->dirty || !s->dataSize || ((R.frame + (s->addr >> 12)) & 63) == 0;
    uint64_t sparse = sparse_hash(s);
    if (!full && sparse == s->sparseHash) return;
    auto& g = layout(s);
    uint32_t levels = s->mipAddr ? s->mips : 1;
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t level = 0; level < levels; ++level) {
        if (!level) s->dataSize = uint32_t(g.info[level].surfSize);
        hash = (hash ^ fnv(mem::ptr(g.address[level]), size_t(g.info[level].surfSize))) * 1099511628211ull;
    }
    s->sparseHash = sparse;
    if (!s->dirty && hash == s->contentHash) return;
    R.perf.uploads++;
    bind_scratch(s);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    for (uint32_t level = 0; level < levels; ++level) {
        std::vector<uint8_t> data;
        uint32_t w, h, slices;
        decode_level(s, level, data, w, h, slices);
        const FormatInfo& f = s->fmt;
        GLsizei size = (GLsizei)data.size();
        auto sub2d = [&](GLenum target, GLsizei width, GLsizei height, const uint8_t* p, GLsizei bytes) {
            if (f.compressed) glCompressedTexSubImage2D(target, level, 0, 0, width, height, f.internal, bytes, p);
            else glTexSubImage2D(target, level, 0, 0, width, height, f.format, f.type, p);
        };
        switch (s->target) {
        case GL_TEXTURE_1D: glTexSubImage1D(s->target, level, 0, w, f.format, f.type, data.data()); break;
        case GL_TEXTURE_1D_ARRAY: sub2d(s->target, w, slices, data.data(), size); break;
        case GL_TEXTURE_2D: sub2d(s->target, w, h, data.data(), size); break;
        case GL_TEXTURE_CUBE_MAP: {
            GLsizei face = size / 6;
            for (uint32_t i = 0; i < 6; i++) sub2d(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, w, h, data.data() + i * face, face);
            break;
        }
        default:
            if (f.compressed)
                glCompressedTexSubImage3D(s->target, level, 0, 0, 0, w, h, slices, f.internal, size, data.data());
            else
                glTexSubImage3D(s->target, level, 0, 0, 0, w, h, slices, f.format, f.type, data.data());
            break;
        }
    }
    s->contentHash = hash;
    s->writeSeq = next_write_seq();
    s->dirty = false;
}

// ---------------------------------------------------------------- lookup
Surface* find_or_create_surface(const SurfaceDesc& d, bool forRendering) {
    if (!d.addr) return nullptr;
    auto range = R.surfaces.equal_range(d.addr);
    Surface *exact = nullptr, *rendered = nullptr;
    auto score = [&](Surface* s) { return std::make_tuple(s->width == d.width && s->height == d.height, s->slices == d.slices, s->writeSeq); };
    auto consider = [&](Surface* s) { if (!rendered || score(s) > score(rendered)) rendered = s; };
    for (auto it = range.first; it != range.second; ++it) {
        auto* s = it->second.get();
        if (!forRendering && s->isDepth && !d.isDepth && s->gpuWritten && s->width == d.width && s->height == d.height) consider(s);
        if (s->isDepth != d.isDepth) continue;
        if (s->width == d.width && s->height == d.height && s->format == d.format && s->slices == d.slices &&
            (forRendering || s->mips >= d.mips || s->gpuWritten)) {
            if (forRendering) return s;
            if (!exact || s->writeSeq > exact->writeSeq) exact = s;
        } else if (!forRendering && s->gpuWritten && (s->format & 0x3f) == (d.format & 0x3f))
            consider(s);
    }
    if (exact && (exact->gpuWritten || !rendered || exact->writeSeq > rendered->writeSeq)) return exact;
    if (rendered) return rendered;
    if (exact) return exact;
    auto s = std::make_unique<Surface>();
    s->addr = d.addr;
    s->mipAddr = d.mipAddr;
    s->width = std::max(d.width, 1u);
    s->height = std::max(d.height, 1u);
    s->slices = std::max(d.slices, 1u);
    s->pitch = d.pitch;
    s->mips = forRendering ? 1 : std::max(d.mips, 1u);
    s->format = d.format;
    s->dim = d.dim;
    s->tileMode = d.tileMode;
    s->swizzle = d.swizzle;
    s->isDepth = d.isDepth;
    s->fmt = format_info(d.format, d.isDepth);
    create_surface_texture(s.get());
    auto* raw = s.get();
    R.surfaces.emplace(d.addr, std::move(s));
    return raw;
}

Surface* color_target(const uint32_t* regs, int i, uint32_t* slice) {
    uint32_t base = regs[mmCB_COLOR0_BASE + i];
    if (!base) return nullptr;
    uint32_t size = regs[mmCB_COLOR0_SIZE + i], info = regs[mmCB_COLOR0_INFO + i];
    uint32_t pitch = ((size & 0x3FF) + 1) * 8;
    uint32_t height = (((size >> 10) & 0xFFFFF) + 1) * 64 / pitch;
    // our convention (GX2SetColorBuffer): TILE = width | array slices << 16, FRAG = height
    uint32_t w = regs[mmCB_COLOR0_TILE + i] & 0xFFFF, h = regs[mmCB_COLOR0_FRAG + i];
    uint32_t slices = std::max<uint32_t>(regs[mmCB_COLOR0_TILE + i] >> 16, 1);
    if (slice) *slice = slices > 1 ? std::min<uint32_t>(regs[mmCB_COLOR0_VIEW + i] & 0x7FF, slices - 1) : 0;
    static const uint32_t numberBits[8] = {0, 0x200, 0, 0, 0x100, 0x300, 0x400, 0x800};
    SurfaceDesc d;
    d.addr = base;
    d.width = w ? w : pitch;
    d.height = h ? h : height;
    d.pitch = pitch;
    d.format = ((info >> 2) & 0x3F) | numberBits[(info >> 12) & 7];
    d.tileMode = (info >> 8) & 0xF;
    d.slices = slices;
    d.dim = slices > 1 ? kDim2DArray : kDim2D;
    return find_or_create_surface(d, true);
}

Surface* depth_target(const uint32_t* regs, uint32_t* slice) {
    uint32_t base = regs[mmDB_DEPTH_BASE];
    if (!base) return nullptr;
    uint32_t slices = std::max<uint32_t>(regs[gx2::kDepthSlicesReg], 1);
    if (slice) *slice = slices > 1 ? std::min<uint32_t>(regs[mmDB_DEPTH_VIEW] & 0x7FF, slices - 1) : 0;
    uint32_t size = regs[mmDB_DEPTH_SIZE], info = regs[mmDB_DEPTH_INFO];
    uint32_t pitch = ((size & 0x3FF) + 1) * 8;
    uint32_t height = (((size >> 10) & 0xFFFFF) + 1) * 64 / pitch;
    uint32_t wh = regs[mmDB_HTILE_DATA_BASE];  // our convention: width << 16 | height
    static const uint32_t fmts[8] = {0, 0x005, 0, 0x011, 0, 0x811, 0x80E, 0x81C};
    SurfaceDesc d;
    d.addr = base;
    d.width = wh ? (wh >> 16) : pitch;
    d.height = wh ? (wh & 0xFFFF) : height;
    d.pitch = pitch;
    d.format = fmts[info & 7];
    d.isDepth = true;
    d.slices = slices;
    d.dim = slices > 1 ? kDim2DArray : kDim2D;
    return find_or_create_surface(d, true);
}

Surface* surface_from_color_buffer(uint32_t addr, uint32_t* firstSlice, uint32_t* numSlices) {
    auto* cb = (GX2::GX2ColorBuffer*)mem::ptr(addr);
    SurfaceDesc d;
    uint32_t slices = cb->surface.dim.value() == Latte::E_DIM::DIM_2D_ARRAY ? std::max<uint32_t>(cb->surface.depth, 1) : 1;
    d.slices = slices;
    d.dim = slices > 1 ? kDim2DArray : kDim2D;
    if (firstSlice) *firstSlice = std::min<uint32_t>(cb->viewFirstSlice, slices - 1);
    if (numSlices)
        *numSlices = std::clamp<uint32_t>(cb->viewNumSlices, 1, slices - std::min<uint32_t>(cb->viewFirstSlice, slices - 1));
    d.addr = gx2::color_buffer_address(cb);
    d.width = std::max<uint32_t>(cb->surface.width >> cb->viewMip, 1);
    d.height = std::max<uint32_t>(cb->surface.height >> cb->viewMip, 1);
    d.pitch = cb->surface.pitch;
    d.format = (uint32_t)cb->surface.format.value();
    d.tileMode = (uint32_t)cb->surface.tileMode.value();
    return find_or_create_surface(d, true);
}

Surface* surface_from_depth_buffer(uint32_t addr, uint32_t* firstSlice, uint32_t* numSlices) {
    auto* db = (GX2::GX2DepthBuffer*)mem::ptr(addr);
    SurfaceDesc d;
    uint32_t slices = db->surface.dim.value() == Latte::E_DIM::DIM_2D_ARRAY ? std::max<uint32_t>(db->surface.depth, 1) : 1;
    d.slices = slices;
    d.dim = slices > 1 ? kDim2DArray : kDim2D;
    if (firstSlice) *firstSlice = std::min<uint32_t>(db->viewFirstSlice, slices - 1);
    if (numSlices)
        *numSlices = std::clamp<uint32_t>(db->viewNumSlices, 1, slices - std::min<uint32_t>(db->viewFirstSlice, slices - 1));
    d.addr = db->surface.imagePtr;
    d.width = db->surface.width;
    d.height = db->surface.height;
    d.pitch = db->surface.pitch;
    d.format = (uint32_t)db->surface.format.value();
    d.tileMode = (uint32_t)db->surface.tileMode.value();
    d.isDepth = true;
    return find_or_create_surface(d, true);
}

Surface* sampled_texture(const uint32_t* w, bool isDepthSampler) {
    Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N w0;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N w1;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N w4;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD5_N w5;
    memcpy(&w0, &w[0], 4);
    memcpy(&w1, &w[1], 4);
    memcpy(&w4, &w[4], 4);
    memcpy(&w5, &w[5], 4);
    uint32_t addr = w[2] << 8, mipAddr = w[3] << 8;
    if (!addr) return nullptr;
    auto dim = w0.get_DIM();
    uint32_t pitch = (w0.get_PITCH() + 1) << 3;
    uint32_t width = w0.get_WIDTH() + 1;
    uint32_t height = w1.get_HEIGHT() + 1;
    uint32_t depth = w1.get_DEPTH();
    if (dim == Latte::E_DIM::DIM_2D_ARRAY || dim == Latte::E_DIM::DIM_3D || dim == Latte::E_DIM::DIM_2D_ARRAY_MSAA ||
        dim == Latte::E_DIM::DIM_1D_ARRAY)
        depth += 1;
    else {
        if (dim == Latte::E_DIM::DIM_CUBEMAP) depth = 6 * (depth + 1);
        if (depth == 0) depth = 1;
    }
    if (dim == Latte::E_DIM::DIM_1D || dim == Latte::E_DIM::DIM_1D_ARRAY) height = 1;
    auto tileMode = w0.get_TILE_MODE();
    if (Latte::IsCompressedFormat(w1.get_DATA_FORMAT())) pitch /= 4;
    uint32_t swizzle = 0;
    if (Latte::TM_IsMacroTiled(tileMode)) {
        swizzle = addr & 0x700;
        addr &= ~0x700u;
    }
    SurfaceDesc d;
    d.addr = addr;
    d.mipAddr = mipAddr;
    d.width = width;
    d.height = height;
    d.slices = depth;
    d.pitch = pitch;
    d.mips = w5.get_LAST_LEVEL() + 1;
    d.format = (uint32_t)LatteTexture_ReconstructGX2Format(w1, w4);
    d.dim = (uint32_t)dim;
    d.tileMode = (uint32_t)tileMode;
    d.swizzle = swizzle;
    d.isDepth = isDepthSampler;
    Surface* s = find_or_create_surface(d, false);
    upload_surface(s);
    return s;
}

// ---------------------------------------------------------------- framebuffer helpers
GLenum depth_attachment(const Surface* s) { return s->fmt.stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT; }

void attach(GLenum fbTarget, GLenum attachment, Surface* s, uint32_t level, uint32_t layer) {
    if (!s) glFramebufferTexture(fbTarget, attachment, 0, 0);
    else if (s->target == GL_TEXTURE_2D || s->target == GL_TEXTURE_1D)
        glFramebufferTexture(fbTarget, attachment, s->tex, level);
    else if (s->target == GL_TEXTURE_CUBE_MAP)
        glFramebufferTexture2D(fbTarget, attachment, GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, s->tex, level);
    else
        glFramebufferTextureLayer(fbTarget, attachment, s->tex, level, layer);
}

void blit(Surface* src, uint32_t srcLevel, uint32_t srcLayer, uint32_t sw, uint32_t sh, Surface* dst, uint32_t dstLevel,
          uint32_t dstLayer, uint32_t dw, uint32_t dh) {
    forget_gl_state();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, R.readFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, R.blitFbo);
    GLbitfield mask;
    GLenum filter = GL_NEAREST;
    if (src->fmt.depth) {
        attach(GL_READ_FRAMEBUFFER, depth_attachment(src), src, srcLevel, srcLayer);
        attach(GL_DRAW_FRAMEBUFFER, depth_attachment(dst), dst, dstLevel, dstLayer);
        mask = GL_DEPTH_BUFFER_BIT | (src->fmt.stencil && dst->fmt.stencil ? GL_STENCIL_BUFFER_BIT : 0);
    } else {
        attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, src, srcLevel, srcLayer);
        attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dst, dstLevel, dstLayer);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        mask = GL_COLOR_BUFFER_BIT;
        if ((sw != dw || sh != dh) && src->fmt.kind == FormatInfo::FLOAT) filter = GL_LINEAR;
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBlitFramebuffer(0, 0, sw, sh, 0, 0, dw, dh, mask, filter);
    // detach so the surfaces can be destroyed or attached elsewhere
    if (src->fmt.depth) {
        attach(GL_READ_FRAMEBUFFER, depth_attachment(src), nullptr, 0, 0);
        attach(GL_DRAW_FRAMEBUFFER, depth_attachment(dst), nullptr, 0, 0);
    } else {
        attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, nullptr, 0, 0);
        attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, nullptr, 0, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, R.drawFbo);
}

// ---------------------------------------------------------------- clears and copies
void clear_color(const uint32_t*, uint32_t cb, const float rgba[4]) {
    make_current();
    uint32_t first, num;
    auto* s = surface_from_color_buffer(cb, &first, &num);
    if (!s || s->fmt.depth || s->fmt.compressed) return;
    forget_gl_state();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, R.blitFbo);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_FRAMEBUFFER_SRGB);  // like a Vulkan clear: the value is linear, sRGB targets encode it
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    for (uint32_t slice = first; slice < first + num; slice++) {
        attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s, 0, slice);
        if (s->fmt.kind == FormatInfo::UINT) {
            GLuint v[4];
            for (int i = 0; i < 4; i++) v[i] = std::isnan(rgba[i]) ? 0 : (GLuint)std::clamp(double(rgba[i]), 0.0, 4294967295.0);
            glClearBufferuiv(GL_COLOR, 0, v);
        } else if (s->fmt.kind == FormatInfo::SINT) {
            GLint v[4];
            for (int i = 0; i < 4; i++)
                v[i] = std::isnan(rgba[i]) ? 0 : (GLint)std::clamp(double(rgba[i]), -2147483648.0, 2147483647.0);
            glClearBufferiv(GL_COLOR, 0, v);
        } else
            glClearBufferfv(GL_COLOR, 0, rgba);
    }
    attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, nullptr, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, R.drawFbo);
    mark_gpu_written(s);
}

void clear_depth_stencil(const uint32_t*, uint32_t db, float depth, uint32_t stencil, uint32_t flags) {
    make_current();
    uint32_t first, num;
    auto* s = surface_from_depth_buffer(db, &first, &num);
    if (!s) return;
    bool d = flags & 1, st = (flags & 2) && s->fmt.stencil;
    if (!d && !st) return;
    forget_gl_state();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, R.blitFbo);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    for (uint32_t slice = first; slice < first + num; slice++) {
        attach(GL_DRAW_FRAMEBUFFER, depth_attachment(s), s, 0, slice);
        if (d && st) glClearBufferfi(GL_DEPTH_STENCIL, 0, depth, (GLint)stencil);
        else if (d) glClearBufferfv(GL_DEPTH, 0, &depth);
        else {
            GLint v = (GLint)stencil;
            glClearBufferiv(GL_STENCIL, 0, &v);
        }
    }
    attach(GL_DRAW_FRAMEBUFFER, depth_attachment(s), nullptr, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, R.drawFbo);
    mark_gpu_written(s);
}

void copy_surface(uint32_t srcAddr, uint32_t srcMip, uint32_t srcSlice, uint32_t dstAddr, uint32_t dstMip, uint32_t dstSlice) {
    make_current();
    auto* s = reinterpret_cast<GX2Surface*>(mem::ptr(srcAddr));
    auto* d = reinterpret_cast<GX2Surface*>(mem::ptr(dstAddr));
    if (srcMip >= uint32_t(s->numLevels) || dstMip >= uint32_t(d->numLevels)) return;
    uint32_t sbase = level_address(s, srcMip), dbase = level_address(d, dstMip);
    uint32_t w = std::max<uint32_t>(uint32_t(s->width) >> srcMip, 1), h = std::max<uint32_t>(uint32_t(s->height) >> srcMip, 1);
    uint32_t dw = std::max<uint32_t>(uint32_t(d->width) >> dstMip, 1), dh = std::max<uint32_t>(uint32_t(d->height) >> dstMip, 1);
    uint32_t cw = std::min(w, dw), ch = std::min(h, dh);
    // GPU-written source: copy on the GPU
    Surface* gpuSrc = nullptr;
    uint32_t gpuLevel = 0;
    for (auto& [addr, image] : R.surfaces) {
        if (!image->gpuWritten) continue;
        if (addr == sbase && image->width == w && image->height == h) {
            if (!gpuSrc || image->writeSeq > gpuSrc->writeSeq) { gpuSrc = image.get(); gpuLevel = 0; }
        } else if (addr == uint32_t(s->imagePtr) && image->mips > srcMip && std::max(image->width >> srcMip, 1u) == w &&
                   std::max(image->height >> srcMip, 1u) == h)
            if (!gpuSrc || image->writeSeq > gpuSrc->writeSeq) { gpuSrc = image.get(); gpuLevel = srcMip; }
    }
    if (gpuSrc) {
        if (gpuSrc->target == GL_TEXTURE_3D) return;
        SurfaceDesc dd;
        dd.addr = dbase;
        dd.width = dw;
        dd.height = dh;
        dd.pitch = d->pitch;
        dd.format = uint32_t(d->format.value());
        dd.tileMode = uint32_t(d->tileMode.value());
        dd.swizzle = d->swizzle;
        dd.isDepth = gpuSrc->isDepth;
        dd.dim = uint32_t(d->dim.value());
        dd.slices = std::max<uint32_t>(d->depth, 1);
        if (dd.dim == uint32_t(Latte::E_DIM::DIM_2D) || dd.dim == uint32_t(Latte::E_DIM::DIM_1D)) dd.slices = 1;
        auto* dst = find_or_create_surface(dd, true);
        if (!dst || dst->fmt.internal != gpuSrc->fmt.internal) {
            LOG("[gl] GX2CopySurface with format conversion is not supported (%u -> %u)", gpuSrc->format, dd.format);
            return;
        }
        if (srcSlice >= gpuSrc->layers || dstSlice >= dst->layers) return;
        if (gpuSrc == dst && gpuLevel == 0 && srcSlice == dstSlice) return;
        if (gpuSrc != dst)
            glCopyImageSubData(gpuSrc->tex, gpuSrc->target, gpuLevel, 0, 0, gpuSrc->target == GL_TEXTURE_2D ? 0 : srcSlice,
                               dst->tex, dst->target, 0, 0, 0, dst->target == GL_TEXTURE_2D ? 0 : dstSlice, cw, ch, 1);
        else
            blit(gpuSrc, gpuLevel, srcSlice, cw, ch, dst, 0, dstSlice, cw, ch);
        mark_gpu_written(dst);
        return;
    }
    // CPU copy between guest layouts
    auto sf = format_info(uint32_t(s->format.value()), bool(uint32_t(s->format.value()) & 0x800));
    auto df = format_info(uint32_t(d->format.value()), bool(uint32_t(d->format.value()) & 0x800));
    if (!sf.internal || !df.internal || sf.bytesPerBlock != df.bytesPerBlock || sf.compressed != df.compressed) {
        LOG("[gl] unsupported CPU GX2CopySurface format layout");
        return;
    }
    LatteAddrLib::AddrSurfaceInfo_OUT si{}, di{};
    LatteAddrLib::GX2CalculateSurfaceInfo(s->format, s->width, s->height, s->depth, s->dim, s->tileMode, s->aa, srcMip, &si);
    LatteAddrLib::GX2CalculateSurfaceInfo(d->format, d->width, d->height, d->depth, d->dim, d->tileMode, d->aa, dstMip, &di);
    if (srcSlice >= si.depth || dstSlice >= di.depth) return;
    auto stm = static_cast<Latte::E_HWTILEMODE>(si.hwTileMode), dtm = static_cast<Latte::E_HWTILEMODE>(di.hwTileMode);
    uint32_t bpp = sf.bytesPerBlock * 8, bw = sf.compressed ? (cw + 3) / 4 : cw, bh = sf.compressed ? (ch + 3) / 4 : ch;
    uint32_t sswz = s->swizzle, dswz = d->swizzle;
    bool sdepth = sf.depth || sf.convert == Convert::D24_R32F, ddepth = df.depth || df.convert == Convert::D24_R32F;
    LatteAddrLib::CachedSurfaceAddrInfo sci{}, dci{};
    if (Latte::TM_IsMacroTiled(stm))
        LatteAddrLib::SetupCachedSurfaceAddrInfo(&sci, srcSlice, 0, bpp, si.pitch, si.height, si.depth, 1, stm, sdepth,
                                                 (sswz >> 8) & 1, (sswz >> 9) & 3);
    if (Latte::TM_IsMacroTiled(dtm))
        LatteAddrLib::SetupCachedSurfaceAddrInfo(&dci, dstSlice, 0, bpp, di.pitch, di.height, di.depth, 1, dtm, ddepth,
                                                 (dswz >> 8) & 1, (dswz >> 9) & 3);
    std::vector<uint8_t> rows(size_t(bw) * bh * sf.bytesPerBlock);
    for (uint32_t y = 0; y < bh; ++y)
        for (uint32_t x = 0; x < bw; ++x)
            memcpy(rows.data() + (size_t(y) * bw + x) * sf.bytesPerBlock,
                   mem::ptr(sbase + element_offset(si, stm, x, y, srcSlice, bpp, &sci, sdepth)), sf.bytesPerBlock);
    for (uint32_t y = 0; y < bh; ++y)
        for (uint32_t x = 0; x < bw; ++x)
            memcpy(mem::ptr(dbase + element_offset(di, dtm, x, y, dstSlice, bpp, &dci, ddepth)),
                   rows.data() + (size_t(y) * bw + x) * sf.bytesPerBlock, sf.bytesPerBlock);
    for (auto& [address, image] : R.surfaces)
        if (address == dbase) {
            image->gpuWritten = false;
            image->dirty = true;
            image->lastCheckedFrame = ~0ull;
        }
}

void invalidate(uint32_t flags, uint32_t addr, uint32_t size) {
    if (flags & 0x5) R.streamGen++;  // attribute buffers or uniform blocks
    if (!(flags & 2) || size >= 0x10000000) return;
    uint64_t end = uint64_t(addr) + size;
    for (auto& [base, s] : R.surfaces) {
        if (s->gpuWritten || (base >= 0xF4000000 && base < 0xF6000000)) continue;
        if (s->dirty && s->lastCheckedFrame == ~0ull) continue;
        uint64_t bytes = std::max<uint64_t>(s->dataSize, uint64_t(s->pitch) * s->height * s->fmt.bytesPerBlock);
        bool hit = uint64_t(base) < end && uint64_t(addr) < uint64_t(base) + bytes;
        if (!hit && s->mipAddr && s->mips > 1) {
            auto& g = layout(s.get());
            for (uint32_t level = 1; level < s->mips && !hit; ++level)
                hit = uint64_t(g.address[level]) < end && uint64_t(addr) < uint64_t(g.address[level]) + g.info[level].surfSize;
        }
        if (hit) {
            s->dirty = true;
            s->lastCheckedFrame = ~0ull;
        }
    }
}

void ss_reset_surfaces() {
    for (auto& [addr, s] : R.surfaces) {
        s->dirty = true;
        s->lastCheckedFrame = ~0ull;
    }
}

}  // namespace gfxgl
