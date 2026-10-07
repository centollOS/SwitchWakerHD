// GX2 surface formats -> deko3d image formats (dk_surfaces.h): the mapping and texel conversions of
// gfx/vulkan/formats.cpp (and gfx/gl/formats.cpp), with the CPU conversions kept as they are there (RGB565,
// 5551, 4444, RG4 -> RGBA8 / RG8; D24S8 -> ZF32_X24S8; D24 sampled as a color -> R32F).
#include <cstring>

#include "dk_surfaces.h"
#include "runtime.h"
#include "surf_internal.h"

namespace gfxdk {
namespace {
FormatInfo make(DkImageFormat image, uint32_t bpb, FormatInfo::Kind kind = FormatInfo::FLOAT, Convert cv = Convert::NONE,
                uint32_t hostBpb = 0) {
    FormatInfo f;
    f.image = image;
    f.bytesPerBlock = bpb;
    f.hostBytesPerBlock = hostBpb ? hostBpb : bpb;
    f.convert = cv;
    f.kind = kind;
    return f;
}
FormatInfo compressed(DkImageFormat image, uint32_t bpb, bool srgb = false) {
    FormatInfo f = make(image, bpb);
    f.compressed = true;
    f.srgb = srgb;
    return f;
}
}  // namespace

FormatInfo format_lookup(uint32_t fmt, bool isDepth) {
    const uint32_t hw = fmt & 0x3F;
    const bool isInt = fmt & 0x100, isSigned = fmt & 0x200, isSrgb = fmt & 0x400;
    const auto kind = isInt ? (isSigned ? FormatInfo::SINT : FormatInfo::UINT) : FormatInfo::FLOAT;
    // unorm, snorm, uint, sint variants of one layout
    auto pick = [&](DkImageFormat unorm, DkImageFormat snorm, DkImageFormat uint, DkImageFormat sint) {
        if (isInt) return isSigned ? sint : uint;
        return isSigned ? snorm : unorm;
    };
    if (isDepth) {
        FormatInfo f;
        switch (hw) {
        case 0x05: f = make(DkImageFormat_Z16, 2); break;
        case 0x0E: f = make(DkImageFormat_ZF32, 4); break;
        // 24-bit depth: ZF32_X24S8 with a CPU conversion, as gfx/gl and gfx/vulkan (plan section 3)
        case 0x11: case 0x12: case 0x13: case 0x14:
            f = make(DkImageFormat_ZF32_X24S8, 4, FormatInfo::FLOAT, Convert::D24S8, 8);
            f.stencil = true;
            break;
        case 0x1C:
            f = make(DkImageFormat_ZF32_X24S8, 8, FormatInfo::FLOAT, Convert::X24_8_32F, 8);
            f.stencil = true;
            break;
        default: return {};
        }
        f.depth = true;
        return f;
    }
    // packed normalized expansions have no integer or signed equivalent in this mapping
    if ((isInt || isSigned) && (hw == 0x02 || hw == 0x08 || hw == 0x0A || hw == 0x0B || hw == 0x0C || hw == 0x1B)) return {};
    if (isSigned && hw == 0x19) return {};
    switch (hw) {
    case 0x01:
        return make(pick(DkImageFormat_R8_Unorm, DkImageFormat_R8_Snorm, DkImageFormat_R8_Uint, DkImageFormat_R8_Sint), 1, kind);
    case 0x02: return make(DkImageFormat_RG8_Unorm, 1, kind, Convert::RG4, 2);
    case 0x05:
        return make(pick(DkImageFormat_R16_Unorm, DkImageFormat_R16_Snorm, DkImageFormat_R16_Uint, DkImageFormat_R16_Sint), 2,
                    kind);
    case 0x06: return make(DkImageFormat_R16_Float, 2);
    case 0x07:
        return make(pick(DkImageFormat_RG8_Unorm, DkImageFormat_RG8_Snorm, DkImageFormat_RG8_Uint, DkImageFormat_RG8_Sint), 2,
                    kind);
    case 0x08: return make(DkImageFormat_RGBA8_Unorm, 2, kind, Convert::RGB565, 4);
    case 0x0A: return make(DkImageFormat_RGBA8_Unorm, 2, kind, Convert::RGBA5551, 4);
    case 0x0B: return make(DkImageFormat_RGBA8_Unorm, 2, kind, Convert::RGBA4, 4);
    case 0x0C: return make(DkImageFormat_RGBA8_Unorm, 2, kind, Convert::ABGR1555, 4);
    case 0x0D:
        return isSigned ? make(DkImageFormat_R32_Sint, 4, FormatInfo::SINT) : make(DkImageFormat_R32_Uint, 4, FormatInfo::UINT);
    case 0x0E: return make(DkImageFormat_R32_Float, 4);
    case 0x0F:
        return make(pick(DkImageFormat_RG16_Unorm, DkImageFormat_RG16_Snorm, DkImageFormat_RG16_Uint, DkImageFormat_RG16_Sint),
                    4, kind);
    case 0x10: return make(DkImageFormat_RG16_Float, 4);
    case 0x15: case 0x16: return make(DkImageFormat_RG11B10_Float, 4);
    case 0x19:
        // (Latte 2_10_10_10: R in the low bits, as Vulkan's A2B10G10R10 and deko3d's RGB10A2)
        return make(isInt ? DkImageFormat_RGB10A2_Uint : DkImageFormat_RGB10A2_Unorm, 4, kind);
    case 0x1A: {
        if (isSrgb) {
            FormatInfo f = make(DkImageFormat_RGBA8_Unorm_sRGB, 4, kind);
            f.srgb = true;
            return f;
        }
        return make(pick(DkImageFormat_RGBA8_Unorm, DkImageFormat_RGBA8_Snorm, DkImageFormat_RGBA8_Uint, DkImageFormat_RGBA8_Sint),
                    4, kind);
    }
    case 0x1B: return make(DkImageFormat_RGB10A2_Unorm, 4, kind);
    case 0x1D:
        return isSigned ? make(DkImageFormat_RG32_Sint, 8, FormatInfo::SINT) : make(DkImageFormat_RG32_Uint, 8, FormatInfo::UINT);
    case 0x1E: return make(DkImageFormat_RG32_Float, 8);
    case 0x1F:
        return make(pick(DkImageFormat_RGBA16_Unorm, DkImageFormat_RGBA16_Snorm, DkImageFormat_RGBA16_Uint,
                         DkImageFormat_RGBA16_Sint), 8, kind);
    case 0x20: return make(DkImageFormat_RGBA16_Float, 8);
    case 0x22:
        return isSigned ? make(DkImageFormat_RGBA32_Sint, 16, FormatInfo::SINT)
                        : make(DkImageFormat_RGBA32_Uint, 16, FormatInfo::UINT);
    case 0x23: return make(DkImageFormat_RGBA32_Float, 16);
    case 0x31: return compressed(isSrgb ? DkImageFormat_RGBA_BC1_sRGB : DkImageFormat_RGBA_BC1, 8, isSrgb);
    case 0x32: return compressed(isSrgb ? DkImageFormat_RGBA_BC2_sRGB : DkImageFormat_RGBA_BC2, 16, isSrgb);
    case 0x33: return compressed(isSrgb ? DkImageFormat_RGBA_BC3_sRGB : DkImageFormat_RGBA_BC3, 16, isSrgb);
    case 0x34: return compressed(isSigned ? DkImageFormat_R_BC4_Snorm : DkImageFormat_R_BC4_Unorm, 8);
    case 0x35: return compressed(isSigned ? DkImageFormat_RG_BC5_Snorm : DkImageFormat_RG_BC5_Unorm, 16);
    // depth formats sampled as color textures
    case 0x11: case 0x12: case 0x13: case 0x14:
        return make(DkImageFormat_R32_Float, 4, FormatInfo::FLOAT, Convert::D24_R32F, 4);
    default: return {};
    }
}

FormatInfo format_info(uint32_t fmt, bool isDepth) {
    FormatInfo f = format_lookup(fmt, isDepth);
    if (f.image == DkImageFormat_None) {
        // logged once per format: a surface of it is not drawn (create_surface_image refuses it)
        static uint64_t logged[2][64] = {};  // by depth, by format bits 0-5: a mask of bits 8-11
        const uint64_t bit = 1ull << ((fmt >> 8) & 0xF);
        uint64_t& seen = logged[isDepth][fmt & 0x3F];
        if (!(seen & bit)) {
            seen |= bit;
            LOG("[dk] unsupported GX2 surface format 0x%X%s: its surfaces are not drawn", fmt, isDepth ? " (depth)" : "");
        }
    }
    return f;
}

// What deko3d allows each image format (its formatTraits, deko3d 0.5.0 source/maxwell/format_traits.inc):
// rendering (DkImageFlags_UsageRender) and the 2D engine (DkImageFlags_Usage2DEngine, dkCmdBufBlitImage). The
// debug library rejects an image whose flags its format does not support.
bool format_can_render(DkImageFormat f) {
    switch (f) {
    case DkImageFormat_RGBA4_Unorm: case DkImageFormat_RGB5_Unorm: case DkImageFormat_RGB5A1_Unorm:
    case DkImageFormat_RGB565_Unorm: case DkImageFormat_E5BGR9_Float:
    case DkImageFormat_RGB32_Float: case DkImageFormat_RGB32_Uint: case DkImageFormat_RGB32_Sint:
        return false;
    default: return f != DkImageFormat_None && f < DkImageFormat_RGB_BC1;
    }
}
bool format_can_2d(DkImageFormat f) {
    switch (f) {
    // integer formats (format_traits.inc flags 0x0701 / 0x0501) and 96-bit ones have no 2D-engine format
    case DkImageFormat_R8_Uint: case DkImageFormat_R8_Sint: case DkImageFormat_R16_Uint: case DkImageFormat_R16_Sint:
    case DkImageFormat_R32_Uint: case DkImageFormat_R32_Sint: case DkImageFormat_RG8_Uint: case DkImageFormat_RG8_Sint:
    case DkImageFormat_RG16_Uint: case DkImageFormat_RG16_Sint: case DkImageFormat_RG32_Uint: case DkImageFormat_RG32_Sint:
    case DkImageFormat_RGB32_Float: case DkImageFormat_RGB32_Uint: case DkImageFormat_RGB32_Sint:
    case DkImageFormat_RGBA8_Uint: case DkImageFormat_RGBA8_Sint: case DkImageFormat_RGBA16_Uint:
    case DkImageFormat_RGBA16_Sint: case DkImageFormat_RGBA32_Uint: case DkImageFormat_RGBA32_Sint:
    case DkImageFormat_RGB10A2_Uint:
        return false;
    default: return f != DkImageFormat_None && f < DkImageFormat_RGB_BC1;
    }
}

static inline uint8_t ex5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t ex6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static inline uint8_t ex4(uint32_t v) { return (uint8_t)((v << 4) | v); }

void convert_row(Convert c, const uint8_t* src, uint8_t* dst, uint32_t n) {
    switch (c) {
    case Convert::NONE: break;  // (callers copy such rows themselves)
    case Convert::RGB565:
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v; memcpy(&v, src + 2 * i, sizeof(v));
            dst[4 * i + 0] = ex5(v & 0x1F);
            dst[4 * i + 1] = ex6((v >> 5) & 0x3F);
            dst[4 * i + 2] = ex5((v >> 11) & 0x1F);
            dst[4 * i + 3] = 255;
        }
        break;
    case Convert::RGBA5551:
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v; memcpy(&v, src + 2 * i, sizeof(v));
            dst[4 * i + 0] = ex5(v & 0x1F);
            dst[4 * i + 1] = ex5((v >> 5) & 0x1F);
            dst[4 * i + 2] = ex5((v >> 10) & 0x1F);
            dst[4 * i + 3] = (v >> 15) ? 255 : 0;
        }
        break;
    case Convert::ABGR1555:
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v; memcpy(&v, src + 2 * i, sizeof(v));
            dst[4 * i + 0] = ex5((v >> 11) & 0x1F);
            dst[4 * i + 1] = ex5((v >> 6) & 0x1F);
            dst[4 * i + 2] = ex5((v >> 1) & 0x1F);
            dst[4 * i + 3] = (v & 1) ? 255 : 0;
        }
        break;
    case Convert::RGBA4:
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v; memcpy(&v, src + 2 * i, sizeof(v));
            dst[4 * i + 0] = ex4(v & 0xF);
            dst[4 * i + 1] = ex4((v >> 4) & 0xF);
            dst[4 * i + 2] = ex4((v >> 8) & 0xF);
            dst[4 * i + 3] = ex4((v >> 12) & 0xF);
        }
        break;
    case Convert::RG4:
        for (uint32_t i = 0; i < n; i++) {
            uint8_t v = src[i];
            dst[2 * i + 0] = ex4(v >> 4);
            dst[2 * i + 1] = ex4(v & 0xF);
        }
        break;
    case Convert::D24S8:
        // ZF32_X24S8 in memory: the float depth, then the stencil in the low byte of the next word
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v; memcpy(&v, src + 4 * i, sizeof(v));
            float d = (float)(v & 0xFFFFFF) / 16777215.0f;
            memcpy(dst + 8 * i, &d, 4);
            dst[8 * i + 4] = (uint8_t)(v >> 24);
            dst[8 * i + 5] = dst[8 * i + 6] = dst[8 * i + 7] = 0;
        }
        break;
    case Convert::D24_R32F:
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v; memcpy(&v, src + 4 * i, sizeof(v));
            float d = (float)(v & 0xFFFFFF) / 16777215.0f;
            memcpy(dst + 4 * i, &d, 4);
        }
        break;
    case Convert::X24_8_32F:
        memcpy(dst, src, n * 8);
        break;
    }
}

}  // namespace gfxdk
