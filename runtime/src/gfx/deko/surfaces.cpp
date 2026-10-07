// The deko3d renderer's surfaces (dk_surfaces.h): guest surfaces as DkImages in the image heap. A structural
// port of gfx/gl/surfaces.cpp (lookup, LatteAddrLib detiling, sparse hash change detection, GuestRanges, the
// TV scan copy, internal resolution hooks) with images, views and uploads modelled on gfx/vulkan/surfaces.cpp:
// - images: DkImageLayout + DkImage in memory.cpp's image heap; every image whose format can render has
//   DkImageFlags_UsageRender (a texture may later be rendered to), Usage2DEngine where the format allows it,
//   HwCompression on color render targets (WWHD_DK_RT_COMPRESSION=0 turns it off; depth: WWHD_DK_DEPTH_COMPRESSION=1)
// - 1D surfaces are 2D images one row high (deko3d's transfers reject 1D images); their views say 1D
// - uploads: detiled rows into an upload staging ring (32 MiB, reused by frame fence) and
//   dkCmdBufCopyBufferToImage per level (all layers at once)
// - transfers (uploads, copies, blits) sit between full barriers and invalidate the texture cache after
//   (conservative, plan section 6.3); clears leave their result visible to sampling the same way
// - views: each sampled (type, swizzle) view gets an image descriptor slot (descriptors.cpp)
// Render thread only.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "dk_draw.h"
#include "dk_surfaces.h"
#include "gx2/gx2.h"
#include "gx2_texture_regs.h"
#include "runtime.h"
#include "surf_internal.h"

Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N&,
                                                     const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N&);
namespace gfxdk {

SurfaceSet S;

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

uint64_t recording() { return R.frame + 1; }  // the frame begin_commands opened
bool frame_retired(uint64_t f) { return f <= R.frame && frame_done(f); }

bool env_flag(const char* name, bool def) {
    const char* e = getenv(name);
    return e && *e ? *e != '0' : def;
}
bool color_compression() {
    static const bool on = [] {
        const bool v = env_flag("WWHD_DK_RT_COMPRESSION", true);
        LOG("[dk] color render targets: %s (WWHD_DK_RT_COMPRESSION)", v ? "hardware compression" : "no compression");
        return v;
    }();
    return on;
}
bool depth_compression() {
    static const bool on = [] {
        const bool v = env_flag("WWHD_DK_DEPTH_COMPRESSION", false);
        LOG("[dk] depth buffers: %s (WWHD_DK_DEPTH_COMPRESSION)", v ? "hardware compression" : "no compression");
        return v;
    }();
    return on;
}

std::string describe(const Surface* s) {
    char b[160];
    snprintf(b, sizeof b, "%08X %ux%u%s%u fmt 0x%X dim %u tile %u mips %u%s", s->addr, s->width, s->height,
             s->slices > 1 ? "x" : "", s->slices > 1 ? s->slices : 0, s->format, s->dim, s->tileMode, s->mips,
             s->isDepth ? " depth" : "");
    return b;
}

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

// four independent lanes: one multiply chain is latency-bound (the A57 hashes ~4x faster this way)
uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
    size_t i = 0;
    for (; n - i >= 32; i += 32)
        for (int l = 0; l < 4; l++) {
            uint64_t word;
            memcpy(&word, p + i + l * 8, sizeof(word));
            h[l] = (h[l] ^ word) * 0xFF51AFD7ED558CCDull;
            h[l] ^= h[l] >> 32;
        }
    uint64_t r = h[0] ^ (h[1] * 31) ^ (h[2] * 1009) ^ (h[3] * 65537);
    for (; i < n; ++i) r = (r ^ p[i]) * 0x100000001B3ull;
    return r ^ (r >> 29) ^ uint64_t(n);
}

// 64 words sampled per level (each is usually a cache miss): CPU changes show up within a few
// frames, periodic full checks catch the rest
uint64_t sparse_hash(Surface* s) {
    auto& g = layout(s);
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t level = 0; level < s->mips; ++level) {
        if (level && !s->mipAddr) break;
        const auto* bytes = mem::ptr(g.address[level]);
        size_t size = size_t(g.info[level].surfSize), step = std::max<size_t>((size / 64) & ~size_t(7), 8);
        size_t offset = 0;
        for (; offset < size && size - offset >= 8; offset += step) {
            uint64_t value;
            memcpy(&value, bytes + offset, 8);
            h = (h ^ value) * 0x100000001b3ull;
        }
    }
    return h;
}

// the host size of one level: texels, layers (or 3D slices) and blocks
struct LevelGeom {
    uint32_t w, h, slices, bw, bh;
    size_t rowBytes, bytes;
};
LevelGeom level_geom(const Surface* s, uint32_t level) {
    LevelGeom g;
    const FormatInfo& f = s->fmt;
    g.w = std::max(s->width >> level, 1u);
    g.h = std::max(s->height >> level, 1u);
    g.slices = s->dim == (uint32_t)Latte::E_DIM::DIM_3D ? std::max(s->slices >> level, 1u) : s->slices;
    g.bw = f.compressed ? (g.w + 3) / 4 : g.w;
    g.bh = f.compressed ? (g.h + 3) / 4 : g.h;
    g.rowBytes = size_t(g.bw) * f.hostBytesPerBlock;
    g.bytes = g.rowBytes * g.bh * g.slices;
    return g;
}

// guest level -> host rows of bw blocks (converted), written row by row to dst (write-only memory)
void decode_level(Surface* s, uint32_t level, uint8_t* dst) {
    const FormatInfo& f = s->fmt;
    auto& g = layout(s);
    const LevelGeom lg = level_geom(s, level);
    const auto& info = g.info[level];
    uint32_t pitch = level == 0 && s->pitch ? s->pitch : info.pitch, height = info.height;
    auto tm = (Latte::E_HWTILEMODE)info.hwTileMode;
    uint32_t bpp = f.bytesPerBlock * 8;
    bool depthData = s->isDepth || f.convert == Convert::D24_R32F;
    uint32_t pipeSwizzle = (s->swizzle >> 8) & 1, bankSwizzle = (s->swizzle >> 9) & 3;
    static std::vector<uint8_t> row, converted;
    row.resize(size_t(lg.bw) * f.bytesPerBlock);
    converted.resize(lg.rowBytes);
    const uint8_t* src = mem::ptr(g.address[level]);
    const bool linear = tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED;
    const bool macro = Latte::TM_IsMacroTiled(tm);
    for (uint32_t z = 0; z < lg.slices; z++) {
        LatteAddrLib::CachedSurfaceAddrInfo ci;
        if (macro)
            LatteAddrLib::SetupCachedSurfaceAddrInfo(&ci, z, 0, bpp, pitch, height, lg.slices, 1, tm, depthData, pipeSwizzle,
                                                     bankSwizzle);
        for (uint32_t y = 0; y < lg.bh; y++) {
            for (uint32_t x = 0; x < lg.bw; x++) {
                uint32_t off;
                if (linear)
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, z, 0, bpp, pitch, height, lg.slices);
                else if (!macro)
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, z, bpp, pitch, height, tm, depthData);
                else
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, &ci);
                memcpy(&row[x * f.bytesPerBlock], src + off, f.bytesPerBlock);
            }
            uint8_t* out = dst + (size_t(z) * lg.bh + y) * lg.rowBytes;
            if (f.convert == Convert::NONE) memcpy(out, row.data(), lg.rowBytes);
            else {
                convert_row(f.convert, row.data(), converted.data(), lg.bw);
                memcpy(out, converted.data(), lg.rowBytes);
            }
        }
    }
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

// ---------------------------------------------------------------- upload staging
// Detiled uploads go through their own ring (not the frame's stream slice, which the draws need): 32 MiB
// of CPU-uncached memory, each range tagged with the frame that recorded its copy and reused once the GPU
// has finished that frame. A frame that needs more than the ring gets a one-off block for the rest
// (destroyed when the GPU is done with it).
constexpr uint32_t kStagingSize = 32u << 20;
struct StagingRange {
    uint32_t begin, end;
    uint64_t frame;
};
struct OneOff {
    DkMemBlock block;
    uint64_t frame;
    uint32_t size;
};
DkMemBlock g_staging = nullptr;
uint8_t* g_stagingCpu = nullptr;
DkGpuAddr g_stagingGpu = 0;
uint32_t g_stagingHead = 0;
std::deque<StagingRange> g_stagingUsed;
std::vector<OneOff> g_oneOff;
struct StagingStats {
    uint64_t bytes = 0, oneOffs = 0, oneOffBytes = 0, waits = 0, waitNs = 0;
} g_stagingStats;

struct Staging {
    uint8_t* cpu = nullptr;
    DkGpuAddr gpu = 0;
};

void staging_retire() {
    while (!g_stagingUsed.empty() && frame_retired(g_stagingUsed.front().frame)) g_stagingUsed.pop_front();
    if (g_stagingUsed.empty()) g_stagingHead = 0;
    for (size_t i = 0; i < g_oneOff.size();)
        if (frame_retired(g_oneOff[i].frame)) {
            dkMemBlockDestroy(g_oneOff[i].block);
            g_oneOff[i] = g_oneOff.back();
            g_oneOff.pop_back();
        } else
            i++;
}

bool staging_fits(uint32_t size, uint32_t& at) {
    if (g_stagingUsed.empty()) {
        at = 0;
        return size <= kStagingSize;
    }
    const uint32_t tail = g_stagingUsed.front().begin;
    // head == tail with ranges in flight would read as an empty, unwrapped ring: a wrapped head stays
    // strictly below the tail (one byte short of full), so head >= tail always means "not wrapped"
    if (g_stagingHead >= tail) {
        if (g_stagingHead + size <= kStagingSize) {
            at = g_stagingHead;
            return true;
        }
        if (size < tail) {  // wrap: the end of the ring stays unused until the ranges before it retire
            at = 0;
            return true;
        }
        return false;
    }
    if (g_stagingHead + size < tail) {
        at = g_stagingHead;
        return true;
    }
    return false;
}

Staging staging_alloc(uint32_t size) {
    size = std::max((size + 255) & ~255u, 256u);  // never empty: a zero-size range would make head == tail
    if (!g_staging) {
        LOG("[dk] creating a memory block: upload staging ring, %u KiB", kStagingSize >> 10);
        log_flush();  // deko3d aborts when a creation fails
        DkMemBlockMaker m;
        dkMemBlockMakerDefaults(&m, R.device, kStagingSize);
        m.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        g_staging = dkMemBlockCreate(&m);
        g_stagingCpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(g_staging));
        g_stagingGpu = dkMemBlockGetGpuAddr(g_staging);
    }
    g_stagingStats.bytes += size;
    if (size <= kStagingSize / 2) {
        const uint64_t start = now_ns();
        for (int waited = 0;; waited++) {
            staging_retire();
            uint32_t at;
            if (staging_fits(size, at)) {
                g_stagingUsed.push_back({at, at + size, recording()});
                g_stagingHead = at + size;
                if (waited) g_stagingStats.waitNs += now_ns() - start;
                return {g_stagingCpu + at, g_stagingGpu + at};
            }
            // this frame's own uploads fill the ring: a one-off block (below)
            if (g_stagingUsed.front().frame >= recording()) break;
            // an earlier frame's uploads are still being copied by the GPU: wait for it
            if (!waited) g_stagingStats.waits++;
            if (waited == 25000)  // 5 s
                fatal("[dk] upload staging: the GPU has not finished frame %llu after 5 s (frame %llu waits to upload %u "
                      "bytes); deko3d queue %s", (unsigned long long)g_stagingUsed.front().frame,
                      (unsigned long long)recording(), size, dkQueueIsInErrorState(R.queue) ? "in an error state" : "fine");
            Stage stage("deko3d: waiting for the GPU (upload staging)");
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    const uint32_t blockSize = (size + DK_MEMBLOCK_ALIGNMENT - 1) & ~uint32_t(DK_MEMBLOCK_ALIGNMENT - 1);
    static int logged = 0;
    if (logged++ < 20)
        LOG("[dk] upload staging: frame %llu needs more than the %u MiB ring; a one-off %u KiB block",
            (unsigned long long)recording(), kStagingSize >> 20, blockSize >> 10);
    log_flush();
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, R.device, blockSize);
    m.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock b = dkMemBlockCreate(&m);
    g_oneOff.push_back({b, recording(), blockSize});
    g_stagingStats.oneOffs++;
    g_stagingStats.oneOffBytes += blockSize;
    return {static_cast<uint8_t*>(dkMemBlockGetCpuAddr(b)), dkMemBlockGetGpuAddr(b)};
}

// ---------------------------------------------------------------- transfer ordering
// The copy and 2D engines run beside the 3D engine: a transfer waits for everything recorded before it
// (draws may read or write the images involved) and everything after it waits for the transfer, with the
// texture cache invalidated. The barrier before is left out while nothing but transfers was recorded since
// the last one (draws and clears change the work counter; each frame starts with it unknown).
uint64_t g_clearWork = 0;
uint64_t g_transferWork = ~0ull;
uint64_t work_counter() { return R.drawCount + R.counts.draws + g_clearWork + R.stateEpoch; }
void transfer_begin() {
    if (work_counter() != g_transferWork) dkCmdBufBarrier(R.cmd, DkBarrier_Full, 0);
}
void transfer_end() {
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    g_transferWork = work_counter();
}

// ---------------------------------------------------------------- images
DkImageType image_type(const Surface* s, DkImageType* viewType) {
    const auto dim = static_cast<Latte::E_DIM>(s->dim);
    const bool oneD = dim == Latte::E_DIM::DIM_1D || dim == Latte::E_DIM::DIM_1D_ARRAY;
    const bool cube = dim == Latte::E_DIM::DIM_CUBEMAP && s->slices % 6 == 0 && s->width == s->height;
    DkImageType type, view = DkImageType_None;
    if (dim == Latte::E_DIM::DIM_3D) type = DkImageType_3D;
    else if (cube) type = s->slices > 6 ? DkImageType_CubemapArray : DkImageType_Cubemap;
    else type = s->slices > 1 ? DkImageType_2DArray : DkImageType_2D;
    if (oneD) view = s->slices > 1 ? DkImageType_1DArray : DkImageType_1D;
    if (viewType) *viewType = view;
    return type;
}

uint32_t image_flags(const FormatInfo& f, DkImageType type, bool forRendering) {
    uint32_t flags = 0;
    if (type != DkImageType_3D && format_can_render(f.image)) flags |= DkImageFlags_UsageRender;
    if (type != DkImageType_3D && format_can_2d(f.image)) flags |= DkImageFlags_Usage2DEngine;
    if (forRendering && (flags & DkImageFlags_UsageRender) && (f.depth ? depth_compression() : color_compression()))
        flags |= DkImageFlags_HwCompression;
    return flags;
}

bool is_layered(DkImageType t) {
    return t == DkImageType_2DArray || t == DkImageType_Cubemap || t == DkImageType_CubemapArray;
}

// the image of s at pw x ph pixels (its mips and layers) and its default descriptor
void make_image(Surface* s, SurfaceImage& img, uint32_t pw, uint32_t ph) {
    if (s->fmt.image == DkImageFormat_None)
        throw std::runtime_error("[dk] unsupported GX2 surface format 0x" + std::to_string(s->format) + " (" + describe(s) + ")");
    DkImageType viewType;
    img.type = image_type(s, &viewType);
    img.pw = pw;
    img.ph = ph;
    img.layers = img.type == DkImageType_3D ? 1 : s->slices;
    if (!pw || !ph || !s->slices || !s->mips || pw > 16384 || ph > 16384)
        throw std::runtime_error("[dk] surface with impossible dimensions: " + describe(s));
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, R.device);
    m.type = img.type;
    m.flags = image_flags(s->fmt, img.type, s->renderTarget);
    m.format = s->fmt.image;
    m.dimensions[0] = pw;
    m.dimensions[1] = ph;
    m.dimensions[2] = img.type == DkImageType_3D        ? s->slices
                      : img.type == DkImageType_CubemapArray ? s->slices / 6
                      : img.type == DkImageType_Cubemap      ? 6
                                                             : s->slices;
    m.mipLevels = s->mips;
    dkImageLayoutInitialize(&img.layout, &m);
    const uint64_t size = dkImageLayoutGetSize(&img.layout);
    if (size >= (1ull << 31)) throw std::runtime_error("[dk] surface too large for the image heap: " + describe(s));
    img.mem = image_alloc(uint32_t(size), dkImageLayoutGetAlignment(&img.layout));
    dkImageInitialize(&img.image, &img.layout, img.mem.block, img.mem.offset);
    img.valid = true;
    DkImageView v;
    dkImageViewDefaults(&v, &img.image);
    v.type = viewType;
    img.imageId = image_descriptor_alloc();
    image_descriptor_write(img.imageId, v);
}

void free_image(SurfaceImage& img) {
    if (!img.valid) return;
    image_descriptor_free_later(img.imageId);
    for (auto& [key, id] : img.views) image_descriptor_free_later(id);
    img.views.clear();
    image_free_later(img.mem);
    img = SurfaceImage{};
}

// a view of one level of the image for transfers: layers (or 3D slices) addressed by the rectangle's z
DkImageView transfer_view(SurfaceImage& img, uint32_t level) {
    DkImageView v;
    dkImageViewDefaults(&v, &img.image);
    v.mipLevelOffset = uint8_t(level);
    return v;
}

// copy engine: same bytes per texel, no scaling (w, h: pixels of the images; compressed: texels)
void copy_image(Surface* src, uint32_t srcLevel, uint32_t srcLayer, Surface* dst, uint32_t dstLevel, uint32_t dstLayer,
                uint32_t w, uint32_t h, uint32_t layers) {
    DkImageView sv = transfer_view(src->img, srcLevel), dv = transfer_view(dst->img, dstLevel);
    // (dkCmdBufCopyImage takes compressed rectangles in blocks)
    if (src->fmt.compressed) {
        w = (w + 3) / 4;
        h = (h + 3) / 4;
    }
    // deko3d 0.5.0's dkCmdBufCopyImage ignores srcRect->z (source/dk_image.cpp: `srcZ = z`, only the
    // destination adds its rectangle's z), so the source layer is selected by the view's layerOffset and
    // the source rectangle starts at z 0. Only layered images take a layerOffset (deko3d rejects it on 3D
    // images), so a 3D image's slices > 0 cannot be read here.
    if (srcLayer && !is_layered(src->img.type)) {
        static int logged = 0;
        if (logged++ < 20)
            LOG("[dk] copy_image %s slice %u level %u -> %s skipped: the copy engine reads slice > 0 only from a layered "
                "image (this one is type %d)", describe(src).c_str(), srcLayer, srcLevel, describe(dst).c_str(),
                int(src->img.type));
        return;
    }
    if (srcLayer) sv.layerOffset = uint16_t(srcLayer);
    const DkImageRect sr = {0, 0, 0, w, h, layers}, dr = {0, 0, dstLayer, w, h, layers};
    dkCmdBufCopyImage(R.cmd, &sv, &sr, &dv, &dr, 0);
}
}  // namespace

void create_surface_image(Surface* s) {
    const auto dim = static_cast<Latte::E_DIM>(s->dim);
    const bool threeD = dim == Latte::E_DIM::DIM_3D;
    const bool oneD = dim == Latte::E_DIM::DIM_1D || dim == Latte::E_DIM::DIM_1D_ARRAY;
    uint32_t maxDim = std::max({s->width, oneD ? 1u : s->height, threeD ? s->slices : 1u}), maxMips = 1;
    while (maxDim > 1) {
        maxDim >>= 1;
        ++maxMips;
    }
    s->mips = std::min(s->mips, maxMips);
    if (oneD) s->height = 1;
    if (threeD || s->mips != 1 || s->slices != 1 || oneD) s->scale = 1.0f;
    make_image(s, s->img, scaled_size(s->width, s->scale), scaled_size(s->height, s->scale));
}

void destroy_surface_image(Surface* s) {
    forget_state();     // its views may be bound as render targets
    R.surfaceEpoch++;   // and its descriptors may be in the draw caches
    R.textureEpoch++;
    before_write(s);
    free_image(s->img);
    free_image(s->twin);
}

uint32_t sampled_view_id(Surface* s, const uint32_t* texWords) {
    if (!s || !s->img.valid) return null_image_id();
    Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N w0;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N w4;
    memcpy(static_cast<void*>(&w0), texWords, 4);
    memcpy(static_cast<void*>(&w4), texWords + 4, 4);
    DkImageType own;
    const DkImageType imageType = image_type(s, &own);
    if (own == DkImageType_None) own = imageType;  // the view type the default descriptor has
    const bool oneD = own == DkImageType_1D || own == DkImageType_1DArray, threeD = own == DkImageType_3D;
    DkImageType type = own;
    switch (w0.get_DIM()) {
    case Latte::E_DIM::DIM_1D: type = oneD ? DkImageType_1D : type; break;
    case Latte::E_DIM::DIM_1D_ARRAY: type = DkImageType_1DArray; break;
    case Latte::E_DIM::DIM_2D: case Latte::E_DIM::DIM_2D_MSAA:
        if (!threeD && !oneD) type = DkImageType_2D;
        break;
    case Latte::E_DIM::DIM_2D_ARRAY: case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
        if (!threeD && !oneD) type = DkImageType_2DArray;
        break;
    case Latte::E_DIM::DIM_CUBEMAP:
        if (own == DkImageType_Cubemap || own == DkImageType_CubemapArray) type = own;
        break;
    default: break;
    }
    const uint32_t sel[4] = {uint32_t(w4.get_DST_SEL_X()), uint32_t(w4.get_DST_SEL_Y()), uint32_t(w4.get_DST_SEL_Z()),
                             uint32_t(w4.get_DST_SEL_W())};
    const bool identity = s->fmt.depth || (sel[0] == 0 && sel[1] == 1 && sel[2] == 2 && sel[3] == 3);
    if (identity && type == own) return s->img.imageId;
    // key: view type, swizzle; (format, first mip and mip count: the image's own, as gfx/gl's views)
    uint64_t key = uint64_t(type) << 12;
    for (unsigned i = 0; i < 4; ++i) key |= uint64_t(s->fmt.depth ? i : sel[i] & 7) << (i * 3);
    if (auto it = s->img.views.find(key); it != s->img.views.end()) return it->second;
    static const DkImageSwizzle map[8] = {DkImageSwizzle_Red,  DkImageSwizzle_Green, DkImageSwizzle_Blue,
                                          DkImageSwizzle_Alpha, DkImageSwizzle_Zero, DkImageSwizzle_One,
                                          DkImageSwizzle_Zero, DkImageSwizzle_Zero};
    DkImageView v;
    dkImageViewDefaults(&v, &s->img.image);
    v.type = type;
    if (!s->fmt.depth)
        for (int i = 0; i < 4; i++) v.swizzle[i] = map[sel[i] & 7];
    const uint32_t id = image_descriptor_alloc();
    image_descriptor_write(id, v);
    s->img.views.emplace(key, id);
    return id;
}

void target_view(Surface* s, uint32_t level, uint32_t layer, DkImageView* out) {
    if (!s || !s->img.valid) throw std::runtime_error("[dk] render target without an image");
    if (!format_can_render(s->fmt.image))
        throw std::runtime_error("[dk] render target format cannot render: " + describe(s));
    dkImageViewDefaults(out, &s->img.image);
    out->mipLevelOffset = uint8_t(std::min(level, s->mips - 1));
    if (s->img.type == DkImageType_3D) {
        if (layer) throw std::runtime_error("[dk] rendering into slice " + std::to_string(layer) + " of a 3D surface: " + describe(s));
    } else if (is_layered(s->img.type)) {
        out->type = DkImageType_2D;  // one layer
        out->layerOffset = uint16_t(std::min(layer, s->img.layers - 1));
    }
}

// ---------------------------------------------------------------- uploads
void upload_surface(Surface* s) {
    if (!s || !s->img.valid || s->gpuWritten) return;
    if (s->lastCheckedFrame == R.frame) return;
    s->lastCheckedFrame = R.frame;
    // Textures the game changes without GX2Invalidate are found by sampling: every frame for one that
    // changed in the last 64 frames, else every 4th frame. Invalidated ones (dirty) are checked fully
    // at once, and every texture fully every 256 frames. The schedule is staggered by a hash of the
    // address (gfx/gl: staggering by address bits made many come due in the same frame).
    const uint64_t phase = R.frame + ((uint32_t(s->addr) * 0x9E3779B1u) >> 20);
    bool full = s->dirty || !s->dataSize || (phase & 255) == 0;
    if (!full && R.frame - s->changedFrame >= 64 && (phase & 3) != 0) return;
    ScopedTime timer{R.perf.uploadNs};
    uint64_t sparse = sparse_hash(s);
    if (!full && sparse == s->sparseHash) return;
    auto& g = layout(s);
    const uint32_t levels = s->mipAddr ? s->mips : 1;
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t level = 0; level < levels; ++level) {
        if (!level) s->dataSize = uint32_t(g.info[level].surfSize);
        hash = (hash ^ fnv(mem::ptr(g.address[level]), size_t(g.info[level].surfSize))) * 1099511628211ull;
    }
    s->sparseHash = sparse;
    if (!s->dirty && hash == s->contentHash) return;
    R.perf.uploads++;
    before_write(s);
    // (internal resolution, P3: a scaled render target gets the guest data at the guest size, then
    // resampled; until then every surface has scale 1)
    transfer_begin();
    for (uint32_t level = 0; level < levels; ++level) {
        const LevelGeom lg = level_geom(s, level);
        if (!lg.bytes) continue;
        const Staging st = staging_alloc(uint32_t(lg.bytes));
        decode_level(s, level, st.cpu);
        DkImageView v = transfer_view(s->img, level);
        const DkCopyBuf src = {st.gpu, uint32_t(lg.rowBytes), uint32_t(lg.rowBytes * lg.bh)};
        const DkImageRect rect = {0, 0, 0, lg.w, lg.h, lg.slices};
        dkCmdBufCopyBufferToImage(R.cmd, &src, &v, &rect, 0);
        R.perf.uploadBytes += lg.bytes;
    }
    transfer_end();
    s->contentHash = hash;
    s->writeSeq = next_write_seq();
    s->dirty = false;
    s->changedFrame = R.frame;
}

// ---------------------------------------------------------------- internal resolution (P3)
// Scale stays 1 in P2: render targets keep their guest size. The fields (scale, scalable, hudFull, twin)
// and these hooks are in place for the port of gfx/gl's dynamic resolution.
namespace {
bool screen_shaped(const Surface* s) {
    if (s->fmt.compressed || s->mips > 1 || s->slices > 1 || s->width < 32 || s->img.type != DkImageType_2D) return false;
    for (uint32_t w = 854, h = 480; w >= 32; w >>= 1, h >>= 1)
        if ((s->width == w || s->width == w + 1) && s->height == h) return false;
    float r = float(s->width) * 9.0f / (float(s->height) * 16.0f);
    return r > 0.97f && r < 1.03f;
}
}  // namespace
float res_scale() { return 1.0f; }
void set_res_scale(float scale) {
    static bool logged = false;
    if (scale != 1.0f && !logged) {
        logged = true;
        LOG("[dk] internal resolution %.2f requested: not implemented before P3 (render targets stay at 1.0)", scale);
    }
}
void latch_res_scale() {}
void rescale_surface(Surface*, float, bool, bool) {}  // P3 (gfx/gl rescale_surface: twin image, pool, blit)
bool fit_scale(Surface*, bool, bool) { return true; }

// ---------------------------------------------------------------- lookup
Surface* find_or_create_surface(const SurfaceDesc& d, bool forRendering) {
    if (!d.addr) return nullptr;
    auto range = S.byAddr.equal_range(d.addr);
    Surface *exact = nullptr, *rendered = nullptr;
    auto score = [&](Surface* s) { return std::make_tuple(s->width == d.width && s->height == d.height, s->slices == d.slices, s->writeSeq); };
    auto consider = [&](Surface* s) { if (!rendered || score(s) > score(rendered)) rendered = s; };
    for (auto it = range.first; it != range.second; ++it) {
        auto* s = it->second.get();
        if (!forRendering && s->isDepth && !d.isDepth && s->gpuWritten && s->width == d.width && s->height == d.height) consider(s);
        if (s->isDepth != d.isDepth) continue;
        if (s->width == d.width && s->height == d.height && s->format == d.format && s->slices == d.slices &&
            (forRendering || s->mips >= d.mips || s->gpuWritten)) {
            if (forRendering) {
                s->renderTarget = true;
                return s;
            }
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
    s->renderTarget = forRendering;
    s->scale = 1.0f;
    create_surface_image(s.get());  // (throws for an unsupported format: the GX2 command is skipped and logged)
    s->scalable = screen_shaped(s.get());
    auto* raw = s.get();
    S.byAddr.emplace(d.addr, std::move(s));
    S.list.push_back(raw);
    R.surfaceEpoch++;
    return raw;
}

namespace {
// the last render-target lookup of each slot, reused while its registers and the surface set are unchanged
struct TargetCache {
    uint64_t epoch = 0;
    uint32_t regs[6] = {};
    Surface* s = nullptr;
    uint32_t slice = 0;
    bool hit(const uint32_t* r, uint32_t* outSlice) const {
        if (epoch != R.surfaceEpoch || memcmp(regs, r, sizeof regs)) return false;
        if (outSlice) *outSlice = slice;
        return true;
    }
    Surface* fill(const uint32_t* r, Surface* surface, const uint32_t* outSlice) {
        epoch = R.surfaceEpoch;  // after the lookup, which may have created the surface
        memcpy(regs, r, sizeof regs);
        s = surface;
        slice = outSlice ? *outSlice : 0;
        return surface;
    }
};
TargetCache colorCache[8], depthCache;
}  // namespace

Surface* color_target(const uint32_t* regs, int i, uint32_t* slice) {
    uint32_t base = regs[mmCB_COLOR0_BASE + i];
    if (!base) return nullptr;
    const uint32_t key[6] = {base, regs[mmCB_COLOR0_SIZE + i], regs[mmCB_COLOR0_INFO + i], regs[mmCB_COLOR0_TILE + i],
                             regs[mmCB_COLOR0_FRAG + i], regs[mmCB_COLOR0_VIEW + i]};
    if (colorCache[i].hit(key, slice)) return colorCache[i].s;
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
    return colorCache[i].fill(key, find_or_create_surface(d, true), slice);
}

Surface* depth_target(const uint32_t* regs, uint32_t* slice) {
    uint32_t base = regs[mmDB_DEPTH_BASE];
    if (!base) return nullptr;
    const uint32_t key[6] = {base, regs[gx2::kDepthSlicesReg], regs[mmDB_DEPTH_VIEW], regs[mmDB_DEPTH_SIZE],
                             regs[mmDB_DEPTH_INFO], regs[mmDB_HTILE_DATA_BASE]};
    if (depthCache.hit(key, slice)) return depthCache.s;
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
    return depthCache.fill(key, find_or_create_surface(d, true), slice);
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

Surface* sampled_texture(const uint32_t* w, bool isDepthSampler, bool* unique) {
    Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N w0;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N w1;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N w4;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD5_N w5;
    memcpy(static_cast<void*>(&w0), &w[0], 4);
    memcpy(static_cast<void*>(&w1), &w[1], 4);
    memcpy(static_cast<void*>(&w4), &w[4], 4);
    memcpy(static_cast<void*>(&w5), &w[5], 4);
    if (unique) *unique = false;
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
    if (unique) *unique = S.byAddr.count(d.addr) == 1;
    upload_surface(s);
    return s;
}

// ---------------------------------------------------------------- copies
namespace {
bool same_2d_class(const Surface* a, const Surface* b) {
    return format_can_2d(a->fmt.image) && format_can_2d(b->fmt.image) && a->img.type != DkImageType_3D &&
           b->img.type != DkImageType_3D && a->fmt.depth == b->fmt.depth &&
           (!a->fmt.depth || a->fmt.image == b->fmt.image);
}
}  // namespace

void blit(Surface* src, uint32_t srcLevel, uint32_t srcLayer, uint32_t sw, uint32_t sh, Surface* dst, uint32_t dstLevel,
          uint32_t dstLayer, uint32_t dw, uint32_t dh) {
    if (!src || !dst || !src->img.valid || !dst->img.valid) return;
    // guest sizes -> image pixels (scaled surfaces have one level)
    sw = scaled_size(sw, src->scale);
    sh = scaled_size(sh, src->scale);
    dw = scaled_size(dw, dst->scale);
    dh = scaled_size(dh, dst->scale);
    sw = std::min(sw, std::max(src->img.pw >> srcLevel, 1u));
    sh = std::min(sh, std::max(src->img.ph >> srcLevel, 1u));
    dw = std::min(dw, std::max(dst->img.pw >> dstLevel, 1u));
    dh = std::min(dh, std::max(dst->img.ph >> dstLevel, 1u));
    if (!sw || !sh || !dw || !dh) return;
    transfer_begin();
    if (same_2d_class(src, dst) && !src->fmt.compressed) {
        // 2D engine (depth always: plan section 3)
        DkImageView sv = transfer_view(src->img, srcLevel), dv = transfer_view(dst->img, dstLevel);
        const DkImageRect sr = {0, 0, src->img.type == DkImageType_2D ? 0 : srcLayer, sw, sh, 1};
        const DkImageRect dr = {0, 0, dst->img.type == DkImageType_2D ? 0 : dstLayer, dw, dh, 1};
        const bool linear = (sw != dw || sh != dh) && !src->fmt.depth && src->fmt.kind == FormatInfo::FLOAT;
        dkCmdBufBlitImage(R.cmd, &sv, &sr, &dv, &dr, linear ? DkBlitFlag_FilterLinear : DkBlitFlag_FilterNearest, 0);
    } else if (sw == dw && sh == dh && src->fmt.hostBytesPerBlock == dst->fmt.hostBytesPerBlock &&
               src->fmt.compressed == dst->fmt.compressed && src->fmt.depth == dst->fmt.depth) {
        copy_image(src, srcLevel, srcLayer, dst, dstLevel, dstLayer, sw, sh, 1);
    } else {
        static int logged = 0;
        if (logged++ < 20)
            LOG("[dk] blit %s (%ux%u) -> %s (%ux%u) skipped: formats 0x%X -> 0x%X have no 2D-engine path", describe(src).c_str(),
                sw, sh, describe(dst).c_str(), dw, dh, src->format, dst->format);
    }
    transfer_end();
}

Surface* feedback_copy(Surface* s) {
    static std::unordered_map<Surface*, std::pair<uint64_t, std::unique_ptr<Surface>>> copies;
    if (!s || !s->img.valid) return s;
    auto& [seq, copy] = copies[s];
    if (copy && (copy->scale != s->scale || copy->img.pw != s->img.pw || copy->img.ph != s->img.ph)) {  // s was rescaled
        destroy_surface_image(copy.get());
        copy.reset();
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
        create_surface_image(copy.get());
        seq = 0;
    }
    if (seq != s->writeSeq) {
        R.perf.feedbackCopies++;
        if (g_traceFrame) trace_event("feedback copy of %s", trace_name(s).c_str());
        const uint32_t layers = s->img.type == DkImageType_3D ? 1 : s->img.layers;
        if (s->fmt.depth) {
            for (uint32_t layer = 0; layer < layers; layer++)
                blit(s, 0, layer, s->width, s->height, copy.get(), 0, layer, s->width, s->height);
        } else {
            transfer_begin();
            copy_image(s, 0, 0, copy.get(), 0, 0, s->img.pw, s->img.ph,
                       s->img.type == DkImageType_3D ? std::max(s->slices, 1u) : layers);
            transfer_end();
        }
        seq = s->writeSeq;
    }
    return copy.get();
}

// ---------------------------------------------------------------- clears
namespace {
void bind_whole(uint32_t pw, uint32_t ph) {
    const DkViewport vp = {0.0f, 0.0f, float(pw), float(ph), 0.0f, 1.0f};
    const DkScissor sc = {0, 0, pw, ph};
    dkCmdBufSetViewports(R.cmd, 0, &vp, 1);
    dkCmdBufSetScissors(R.cmd, 0, &sc, 1);
}
// what a clear wrote is visible to the commands after it (sampling, copies)
void clear_done() {
    dkCmdBufBarrier(R.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
    g_clearWork++;
    forget_state();
}
}  // namespace

void clear_color(const uint32_t*, uint32_t cb, const float rgba[4]) {
    ScopedTime timer{R.perf.clearNs};
    R.perf.clears++;
    R.counts.clears++;
    uint32_t first, num;
    auto* s = surface_from_color_buffer(cb, &first, &num);
    if (!s || s->fmt.depth || s->fmt.compressed || !s->img.valid) return;
    if (!format_can_render(s->fmt.image)) {
        static int logged = 0;
        if (logged++ < 10) LOG("[dk] color clear of %s skipped: its format cannot render", describe(s).c_str());
        return;
    }
    if (skip_gamepad() && gamepad_only(s)) {
        R.perf.gamepadClearsSkipped++;
        return;
    }
    before_write(s);
    s->hudFull = false;
    s->derivedFrom = nullptr;
    fit_scale(s, false);  // cleared whole: nothing to keep
    gpu_pass_mark("clear", s, nullptr);
    if (g_traceFrame) trace_event("clear %s", trace_name(s).c_str());
    bind_whole(s->img.pw, s->img.ph);
    for (uint32_t slice = first; slice < first + num; slice++) {
        DkImageView v;
        target_view(s, 0, slice, &v);
        const DkImageView* colors[] = {&v};
        dkCmdBufBindRenderTargets(R.cmd, colors, 1, nullptr);
        // like a Vulkan clear: the value is linear, sRGB targets encode it
        if (s->fmt.kind == FormatInfo::UINT) {
            uint32_t u[4];
            for (int i = 0; i < 4; i++) u[i] = std::isnan(rgba[i]) ? 0 : (uint32_t)std::clamp(double(rgba[i]), 0.0, 4294967295.0);
            dkCmdBufClearColorUint(R.cmd, 0, DkColorMask_RGBA, u[0], u[1], u[2], u[3]);
        } else if (s->fmt.kind == FormatInfo::SINT) {
            int32_t v4[4];
            for (int i = 0; i < 4; i++)
                v4[i] = std::isnan(rgba[i]) ? 0 : (int32_t)std::clamp(double(rgba[i]), -2147483648.0, 2147483647.0);
            dkCmdBufClearColorSint(R.cmd, 0, DkColorMask_RGBA, v4[0], v4[1], v4[2], v4[3]);
        } else
            dkCmdBufClearColorFloat(R.cmd, 0, DkColorMask_RGBA, rgba[0], rgba[1], rgba[2], rgba[3]);
    }
    clear_done();
    mark_gpu_written(s);
}

void clear_depth_stencil(const uint32_t*, uint32_t db, float depth, uint32_t stencil, uint32_t flags) {
    ScopedTime timer{R.perf.clearNs};
    R.perf.clears++;
    R.counts.clears++;
    uint32_t first, num;
    auto* s = surface_from_depth_buffer(db, &first, &num);
    if (!s || !s->img.valid || !s->fmt.depth) return;
    bool d = flags & 1, st = (flags & 2) && s->fmt.stencil;
    if (!d && !st) return;
    before_write(s);
    fit_scale(s, !(d && (st || !s->fmt.stencil)));  // what the clear leaves is kept
    gpu_pass_mark("clear", nullptr, s);
    if (g_traceFrame) trace_event("clear %s", trace_name(s).c_str());
    bind_whole(s->img.pw, s->img.ph);
    for (uint32_t slice = first; slice < first + num; slice++) {
        DkImageView v;
        target_view(s, 0, slice, &v);
        dkCmdBufBindRenderTargets(R.cmd, nullptr, 0, &v);
        dkCmdBufClearDepthStencil(R.cmd, d, std::clamp(depth, 0.0f, 1.0f), st ? 0xFF : 0, uint8_t(stencil));
    }
    clear_done();
    mark_gpu_written(s);
}

void copy_surface(uint32_t srcAddr, uint32_t srcMip, uint32_t srcSlice, uint32_t dstAddr, uint32_t dstMip, uint32_t dstSlice) {
    ScopedTime timer{R.perf.surfaceCopyNs};
    R.perf.surfaceCopies++;
    R.counts.copies++;
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
    for (Surface* image : S.list) {
        if (!image->gpuWritten) continue;
        uint32_t addr = image->addr;
        if (addr == sbase && image->width == w && image->height == h) {
            if (!gpuSrc || image->writeSeq > gpuSrc->writeSeq) { gpuSrc = image; gpuLevel = 0; }
        } else if (addr == uint32_t(s->imagePtr) && image->mips > srcMip && std::max(image->width >> srcMip, 1u) == w &&
                   std::max(image->height >> srcMip, 1u) == h)
            if (!gpuSrc || image->writeSeq > gpuSrc->writeSeq) { gpuSrc = image; gpuLevel = srcMip; }
    }
    if (gpuSrc) {
        const bool fromGamepad = gpuSrc->drcScanFrame != ~0ull && gamepad_only(gpuSrc);
        if (!fromGamepad) note_read(gpuSrc);
        gpu_pass_mark("copy", gpuSrc, nullptr);
        if (gpuSrc->img.type == DkImageType_3D || !gpuSrc->img.valid) return;
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
        if (!dst || !dst->img.valid) return;
        before_write(dst);
        fit_scale(dst, true);
        if (dst->fmt.image != gpuSrc->fmt.image) {
            static int logged = 0;
            if (logged++ < 20)
                LOG("[dk] GX2CopySurface with format conversion is not supported (0x%X -> 0x%X)", gpuSrc->format, dd.format);
            return;
        }
        if (srcSlice >= gpuSrc->img.layers || dstSlice >= dst->img.layers) return;
        if (gpuSrc == dst && gpuLevel == 0 && srcSlice == dstSlice) return;
        if (fromGamepad) dst->derivedFrom = gpuSrc;
        if (g_traceFrame) trace_event("copy %s level %u -> %s", trace_name(gpuSrc).c_str(), gpuLevel, trace_name(dst).c_str());
        if (gpuSrc != dst && gpuSrc->scale == dst->scale && !gpuSrc->fmt.depth) {
            const uint32_t pw = std::min(scaled_size(cw, dst->scale), std::max(gpuSrc->img.pw >> gpuLevel, 1u));
            const uint32_t ph = std::min(scaled_size(ch, dst->scale), std::max(gpuSrc->img.ph >> gpuLevel, 1u));
            transfer_begin();
            copy_image(gpuSrc, gpuLevel, gpuSrc->img.type == DkImageType_2D ? 0 : srcSlice, dst, 0,
                       dst->img.type == DkImageType_2D ? 0 : dstSlice, std::min(pw, dst->img.pw), std::min(ph, dst->img.ph), 1);
            transfer_end();
        } else  // depth (2D engine), the same image, or resampled between internal resolutions
            blit(gpuSrc, gpuLevel, srcSlice, cw, ch, dst, 0, dstSlice, cw, ch);
        mark_gpu_written(dst);
        return;
    }
    // CPU copy between guest layouts
    R.perf.cpuSurfaceCopies++;
    if (g_traceFrame) trace_event("CPU copy %08X -> %08X", sbase, dbase);
    auto sf = format_info(uint32_t(s->format.value()), bool(uint32_t(s->format.value()) & 0x800));
    auto df = format_info(uint32_t(d->format.value()), bool(uint32_t(d->format.value()) & 0x800));
    if (sf.image == DkImageFormat_None || df.image == DkImageFormat_None || sf.bytesPerBlock != df.bytesPerBlock ||
        sf.compressed != df.compressed) {
        LOG("[dk] unsupported CPU GX2CopySurface format layout (0x%X -> 0x%X)", uint32_t(s->format.value()),
            uint32_t(d->format.value()));
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
    for (Surface* image : S.list)
        if (image->addr == dbase) {
            if (image->gpuWritten) R.surfaceEpoch++;
            image->gpuWritten = false;
            image->dirty = true;
            image->lastCheckedFrame = ~0ull;
        }
}

// ---------------------------------------------------------------- the TV picture
void copy_to_scan(uint32_t cb, uint32_t target) {
    ScopedTime timer{R.perf.scanNs};
    R.perf.scans++;
    R.counts.scans++;
    Surface* src = surface_from_color_buffer(cb);
    if (g_traceFrame && target != 1) trace_event("GamePad scan copy of %s", trace_name(src).c_str());
    if (src && target != 1) {  // the GamePad picture has no screen on the Switch yet
        if (src->drcScanFrame == ~0ull)
            LOG("[dk] GamePad picture: buffer %08X, %ux%u, format %X", src->addr, src->width, src->height, src->format);
        src->drcScanFrame = R.frame;
    }
    if (target != 1) return;
    if (!src || src->fmt.depth || !src->img.valid) return;
    if (src->tvScanFrame == ~0ull)
        LOG("[dk] TV picture: buffer %08X, %ux%u, format %X (%s)", src->addr, src->width, src->height, src->format,
            src->fmt.srgb ? "sRGB" : "linear");
    src->tvScanFrame = R.frame;
    if (src->derivedFrom) src->derivedFrom->readFrame = R.frame;  // the TV shows what it was made from
    R.scanCopies++;
    if (g_traceFrame) trace_event("TV scan copy of %s", trace_name(src).c_str());
    // noted only: presenting reads src itself unless something writes to it first (scan_flush)
    S.tvSource = S.scanSrc = src;
}

// the noted TV picture copied after all: src is about to change before the picture is shown
void scan_flush() {
    Surface* src = S.scanSrc;
    if (!src) return;
    S.scanSrc = nullptr;
    R.perf.scanBlits++;
    auto& scan = S.tvScan;
    if (!scan || scan->width != src->width || scan->height != src->height || scan->img.pw != src->img.pw ||
        scan->img.ph != src->img.ph || scan->fmt.image != src->fmt.image) {
        if (scan) destroy_surface_image(scan.get());
        scan = std::make_unique<Surface>();
        scan->width = src->width;
        scan->height = src->height;
        scan->format = src->format;
        scan->fmt = src->fmt;
        scan->gpuWritten = true;
        scan->renderTarget = true;
        scan->scale = src->scale;  // presenting scales it to the window
        create_surface_image(scan.get());
    }
    if (g_traceFrame) trace_event("TV picture copied from %s", trace_name(src).c_str());
    transfer_begin();
    copy_image(src, 0, 0, scan.get(), 0, 0, std::min(src->img.pw, scan->img.pw), std::min(src->img.ph, scan->img.ph), 1);
    transfer_end();
    mark_gpu_written(scan.get());
}

PresentSource present_source() {
    PresentSource p;
    Surface* const s = S.scanSrc ? S.scanSrc : S.tvScan.get();
    if (!s || !s->img.valid) return p;
    p.surface = s;
    p.pw = s->img.pw;
    p.ph = s->img.ph;
    p.width = s->width;
    p.height = s->height;
    p.srgb = s->fmt.srgb;
    // the reserved slot gets the picture's default view when it changes; the previous frame's present
    // pass may still read the slot: its draws finish first. The image is told apart by its address, size
    // and format: a rescaled picture (internal resolution) or a new image in freed heap memory can have the
    // address of the one written before
    static DkGpuAddr written = DK_GPU_ADDR_INVALID;
    static DkImageFormat writtenFormat = DkImageFormat_None;
    static uint32_t writtenW = 0, writtenH = 0;
    const DkGpuAddr addr = dkImageGetGpuAddr(&s->img.image);
    if (addr != written || s->fmt.image != writtenFormat || s->img.pw != writtenW || s->img.ph != writtenH) {
        dkCmdBufBarrier(R.cmd, DkBarrier_Primitives, 0);
        DkImageView v;
        dkImageViewDefaults(&v, &s->img.image);
        image_descriptor_write(kPresentImageId, v);
        written = addr;
        writtenFormat = s->fmt.image;
        writtenW = s->img.pw;
        writtenH = s->img.ph;
        static int logged = 0;  // (internal resolution changes repeat it)
        if (logged++ < 40)
            LOG("[dk] presenting %s: %ux%u pixels, %s", s == S.tvScan.get() ? "the TV scan copy" : "the TV buffer itself",
                p.pw, p.ph, describe(s).c_str());
    }
    // the picture's rendering finished and visible to the present pass's sampling
    dkCmdBufBarrier(R.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
    commit_descriptors();
    return p;
}

// ---------------------------------------------------------------- invalidation
namespace {
// Guest byte ranges of every surface (base level, and the mip chain when it is elsewhere), sorted by
// start, with the running maximum of the ends: an invalidated range only visits the surfaces it can
// touch. Rebuilt when surfaces are added.
struct GuestRanges {
    struct Range {
        uint64_t start, end;
        Surface* s;
    };
    std::vector<Range> ranges;
    std::vector<uint64_t> maxEnd;  // maxEnd[i] = max(ranges[0..i].end)
    size_t built = ~size_t(0);
    void update() {
        if (built == S.list.size()) return;
        built = S.list.size();
        ranges.clear();
        for (Surface* s : S.list) {
            if (s->addr >= 0xF4000000 && s->addr < 0xF6000000) continue;  // render targets in MEM1
            auto& g = layout(s);
            uint64_t bytes = std::max<uint64_t>(g.info[0].surfSize, uint64_t(s->pitch) * s->height * s->fmt.bytesPerBlock);
            ranges.push_back({s->addr, uint64_t(s->addr) + bytes, s});
            if (s->mipAddr && s->mips > 1) {
                uint64_t lo = ~0ull, hi = 0;
                for (uint32_t level = 1; level < s->mips; ++level) {
                    lo = std::min<uint64_t>(lo, g.address[level]);
                    hi = std::max<uint64_t>(hi, uint64_t(g.address[level]) + g.info[level].surfSize);
                }
                if (lo < hi) ranges.push_back({lo, hi, s});
            }
        }
        std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.start < b.start; });
        maxEnd.resize(ranges.size());
        uint64_t m = 0;
        for (size_t i = 0; i < ranges.size(); i++) maxEnd[i] = m = std::max(m, ranges[i].end);
    }
} guestRanges;
}  // namespace

void invalidate(uint32_t flags, uint32_t addr, uint32_t size) {
    ScopedTime timer{R.perf.invalidateNs};
    R.perf.invalidates++;
    R.counts.invalidates++;
    if (flags & 0x5) R.streamGen++;  // attribute buffers or uniform blocks
    if (!(flags & 2) || size >= 0x10000000) return;
    const uint64_t begin = addr, end = uint64_t(addr) + size;
    guestRanges.update();
    auto& rs = guestRanges.ranges;
    // first range whose running maximum end passes the start; stop at ranges starting past the end
    size_t i = size_t(std::upper_bound(guestRanges.maxEnd.begin(), guestRanges.maxEnd.end(), begin) - guestRanges.maxEnd.begin());
    for (; i < rs.size() && rs[i].start < end; i++) {
        if (rs[i].end <= begin) continue;
        Surface* s = rs[i].s;
        if (s->gpuWritten || (s->dirty && s->lastCheckedFrame == ~0ull)) continue;
        s->dirty = true;
        s->lastCheckedFrame = ~0ull;
    }
}

void ss_reset_surfaces() {
    for (Surface* s : S.list) {
        s->dirty = true;
        s->lastCheckedFrame = ~0ull;
    }
}

// ---------------------------------------------------------------- the null texture, frame start, stats
uint32_t null_image_id() {
    static Surface* null = nullptr;
    if (!null) {
        static Surface n;
        n.width = n.height = 1;
        n.format = 0x1A;
        n.fmt = format_info(0x1A, false);
        n.gpuWritten = true;
        make_image(&n, n.img, 1, 1);
        // transparent black: 4 zero bytes copied in
        const Staging st = staging_alloc(4);
        memset(st.cpu, 0, 4);
        transfer_begin();
        DkImageView v = transfer_view(n.img, 0);
        const DkCopyBuf src = {st.gpu, 4, 4};
        const DkImageRect rect = {0, 0, 0, 1, 1, 1};
        dkCmdBufCopyBufferToImage(R.cmd, &src, &v, &rect, 0);
        transfer_end();
        null = &n;
        LOG("[dk] null texture: image descriptor %u (transparent black, 1x1)", n.img.imageId);
    }
    return null->img.imageId;
}

namespace {
// Every supported GX2 format's image layouts with the flags the surfaces would use (render target and
// sampled), once at the first frame: deko3d's debug library ends the game right here, readably, when the
// format table gives a format a flag deko3d does not allow, instead of at the first surface of that format.
void self_check() {
    struct Seen {
        DkImageFormat f;
        bool depth;
    };
    std::vector<Seen> seen;
    int layouts = 0;
    LOG("[dk] surfaces self-check: image layouts of every supported GX2 format (a deko3d error right after this line: "
        "gfx/deko/formats.cpp gives a format a flag deko3d does not allow)");
    log_flush();
    static const uint32_t variants[] = {0, 0x100, 0x200, 0x300, 0x400};
    for (int depth = 0; depth < 2; depth++)
        for (uint32_t hw = 0; hw < 0x40; hw++)
            for (uint32_t v : variants) {
                if (depth && v) continue;
                const uint32_t fmt = hw | v | (depth ? 0x800 : 0);
                const FormatInfo f = format_lookup(fmt, depth);  // (quiet: no line per unsupported format)
                if (f.image == DkImageFormat_None) continue;
                bool dup = false;
                for (auto& s : seen) dup |= s.f == f.image && s.depth == bool(depth);
                if (dup) continue;
                seen.push_back({f.image, bool(depth)});
                for (int rt = 0; rt < 2; rt++)
                    for (DkImageType type : {DkImageType_2D, DkImageType_2DArray, DkImageType_Cubemap, DkImageType_3D}) {
                        if (f.depth && type == DkImageType_3D) continue;
                        DkImageLayoutMaker m;
                        dkImageLayoutMakerDefaults(&m, R.device);
                        m.type = type;
                        m.flags = image_flags(f, type, rt);
                        m.format = f.image;
                        m.dimensions[0] = m.dimensions[1] = 64;
                        m.dimensions[2] = type == DkImageType_2D ? 1 : type == DkImageType_Cubemap ? 6 : 4;
                        m.mipLevels = 2;
                        DkImageLayout l;
                        dkImageLayoutInitialize(&l, &m);
                        layouts++;
                    }
            }
    LOG("[dk] surfaces self-check passed: %zu image formats, %d layouts; upload staging %u MiB", seen.size(), layouts,
        kStagingSize >> 20);
}

void surface_stats() {
    static auto lastAt = std::chrono::steady_clock::now();
    static Renderer::Perf last;
    static StagingStats lastStaging;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastAt < std::chrono::seconds(5)) return;
    const double secs = std::chrono::duration<double>(now - lastAt).count();
    lastAt = now;
    const Renderer::Perf& p = R.perf;
    size_t total, targets, count;
    surface_memory(total, targets, count);
    uint32_t images = 0, samplers = 0;
    descriptor_usage(images, samplers);
    const StagingStats& st = g_stagingStats;
    auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / secs; };
    LOG("[dk] surfaces: %zu (%.1f MiB, render targets %.1f MiB); uploads %llu (%.1f MiB, %.1f ms/s), staging %.1f MiB/s, "
        "%llu waits (%.1f ms), %llu one-off blocks; clears %llu (%.1f ms/s), copies %llu (%llu by the CPU, %.1f ms/s), "
        "scan copies %llu (%llu made), feedback copies %llu, invalidates %llu (%.1f ms/s); descriptors: %u images, "
        "%u samplers",
        count, double(total) / 1048576.0, double(targets) / 1048576.0, (unsigned long long)(p.uploads - last.uploads),
        double(p.uploadBytes - last.uploadBytes) / 1048576.0, ms(p.uploadNs - last.uploadNs),
        double(st.bytes - lastStaging.bytes) / 1048576.0 / secs, (unsigned long long)(st.waits - lastStaging.waits),
        double(st.waitNs - lastStaging.waitNs) / 1e6, (unsigned long long)(st.oneOffs - lastStaging.oneOffs),
        (unsigned long long)(p.clears - last.clears), ms(p.clearNs - last.clearNs),
        (unsigned long long)(p.surfaceCopies - last.surfaceCopies), (unsigned long long)(p.cpuSurfaceCopies - last.cpuSurfaceCopies),
        ms(p.surfaceCopyNs - last.surfaceCopyNs), (unsigned long long)(p.scans - last.scans),
        (unsigned long long)(p.scanBlits - last.scanBlits), (unsigned long long)(p.feedbackCopies - last.feedbackCopies),
        (unsigned long long)(p.invalidates - last.invalidates), ms(p.invalidateNs - last.invalidateNs), images, samplers);
    last = p;
    lastStaging = st;
}
}  // namespace

void surfaces_frame_start() {
    static bool checked = false;
    if (!checked) {
        checked = true;
        self_check();
    }
    descriptors_frame_start();
    staging_retire();
    g_transferWork = ~0ull;  // the previous frame's present pass may still read what a transfer writes
    // the previous frame was presented: its TV buffer gets a copy only if the game copies it again
    S.scanSrc = nullptr;
    if (S.tvSource) S.tvSource->hudFull = false;
    surface_stats();
}

void surface_memory(size_t& total, size_t& targets, size_t& count) {
    total = targets = count = 0;
    for (const Surface* s : S.list) {
        if (!s->img.valid) continue;
        size_t bytes = s->img.mem.size + (s->twin.valid ? s->twin.mem.size : 0);
        total += bytes;
        if (s->renderTarget || s->gpuWritten) targets += bytes;
        count++;
    }
}

}  // namespace gfxdk
