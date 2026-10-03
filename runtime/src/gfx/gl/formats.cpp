// GX2 surface formats -> OpenGL formats (the same mapping and texel conversions as gfx/vulkan/formats.cpp).
#include <cstring>

#include "gl.h"

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT 0x8C4D
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT 0x8C4E
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif

namespace gfxgl {
namespace {
struct Gl { GLenum internal, format, type; };
FormatInfo make(Gl g, uint32_t bpb, FormatInfo::Kind kind = FormatInfo::FLOAT, Convert cv = Convert::NONE,
                uint32_t hostBpb = 0) {
    FormatInfo f;
    f.internal = g.internal;
    f.format = g.format;
    f.type = g.type;
    f.bytesPerBlock = bpb;
    f.hostBytesPerBlock = hostBpb ? hostBpb : bpb;
    f.convert = cv;
    f.kind = kind;
    return f;
}
FormatInfo compressed(GLenum internal, uint32_t bpb) {
    FormatInfo f = make({internal, 0, 0}, bpb);
    f.compressed = true;
    return f;
}
}  // namespace

FormatInfo format_info(uint32_t fmt, bool isDepth) {
    const uint32_t hw = fmt & 0x3F;
    const bool isInt = fmt & 0x100, isSigned = fmt & 0x200, isSrgb = fmt & 0x400;
    const auto kind = isInt ? (isSigned ? FormatInfo::SINT : FormatInfo::UINT) : FormatInfo::FLOAT;
    // unorm, snorm, uint, sint variants of one layout
    auto pick = [&](Gl unorm, Gl snorm, Gl uint, Gl sint) {
        if (isInt) return isSigned ? sint : uint;
        return isSigned ? snorm : unorm;
    };
    if (isDepth) {
        FormatInfo f;
        switch (hw) {
        case 0x05:
            f = make({GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT}, 2);
            break;
        case 0x0E:
            f = make({GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT}, 4);
            break;
        case 0x11: case 0x12: case 0x13: case 0x14:
            f = make({GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV}, 4, FormatInfo::FLOAT,
                     Convert::D24S8, 8);
            f.stencil = true;
            break;
        case 0x1C:
            f = make({GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV}, 8, FormatInfo::FLOAT,
                     Convert::X24_8_32F, 8);
            f.stencil = true;
            break;
        default: return {};
        }
        f.depth = true;
        return f;
    }
    if ((isInt || isSigned) && (hw == 0x02 || hw == 0x08 || hw == 0x0A || hw == 0x0B || hw == 0x0C || hw == 0x1B)) return {};
    if (isSigned && hw == 0x19) return {};
    const Gl rgba8{GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
    switch (hw) {
    case 0x01:
        return make(pick({GL_R8, GL_RED, GL_UNSIGNED_BYTE}, {GL_R8_SNORM, GL_RED, GL_BYTE},
                         {GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE}, {GL_R8I, GL_RED_INTEGER, GL_BYTE}), 1, kind);
    case 0x02: return make({GL_RG8, GL_RG, GL_UNSIGNED_BYTE}, 1, kind, Convert::RG4, 2);
    case 0x05:
        return make(pick({GL_R16, GL_RED, GL_UNSIGNED_SHORT}, {GL_R16_SNORM, GL_RED, GL_SHORT},
                         {GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT}, {GL_R16I, GL_RED_INTEGER, GL_SHORT}), 2, kind);
    case 0x06: return make({GL_R16F, GL_RED, GL_HALF_FLOAT}, 2);
    case 0x07:
        return make(pick({GL_RG8, GL_RG, GL_UNSIGNED_BYTE}, {GL_RG8_SNORM, GL_RG, GL_BYTE},
                         {GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE}, {GL_RG8I, GL_RG_INTEGER, GL_BYTE}), 2, kind);
    case 0x08: return make(rgba8, 2, kind, Convert::RGB565, 4);
    case 0x0A: return make(rgba8, 2, kind, Convert::RGBA5551, 4);
    case 0x0B: return make(rgba8, 2, kind, Convert::RGBA4, 4);
    case 0x0C: return make(rgba8, 2, kind, Convert::ABGR1555, 4);
    case 0x0D:
        return isSigned ? make({GL_R32I, GL_RED_INTEGER, GL_INT}, 4, FormatInfo::SINT)
                        : make({GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT}, 4, FormatInfo::UINT);
    case 0x0E: return make({GL_R32F, GL_RED, GL_FLOAT}, 4);
    case 0x0F:
        return make(pick({GL_RG16, GL_RG, GL_UNSIGNED_SHORT}, {GL_RG16_SNORM, GL_RG, GL_SHORT},
                         {GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT}, {GL_RG16I, GL_RG_INTEGER, GL_SHORT}), 4, kind);
    case 0x10: return make({GL_RG16F, GL_RG, GL_HALF_FLOAT}, 4);
    case 0x15: case 0x16: return make({GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV}, 4);
    case 0x19:
        return isInt ? make({GL_RGB10_A2UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV}, 4, kind)
                     : make({GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV}, 4, kind);
    case 0x1A:
        if (isSrgb) return make({GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE}, 4, kind);
        return make(pick(rgba8, {GL_RGBA8_SNORM, GL_RGBA, GL_BYTE}, {GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE},
                         {GL_RGBA8I, GL_RGBA_INTEGER, GL_BYTE}), 4, kind);
    case 0x1B: return make({GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV}, 4, kind);
    case 0x1D:
        return isSigned ? make({GL_RG32I, GL_RG_INTEGER, GL_INT}, 8, FormatInfo::SINT)
                        : make({GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT}, 8, FormatInfo::UINT);
    case 0x1E: return make({GL_RG32F, GL_RG, GL_FLOAT}, 8);
    case 0x1F:
        return make(pick({GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT}, {GL_RGBA16_SNORM, GL_RGBA, GL_SHORT},
                         {GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT}, {GL_RGBA16I, GL_RGBA_INTEGER, GL_SHORT}),
                    8, kind);
    case 0x20: return make({GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT}, 8);
    case 0x22:
        return isSigned ? make({GL_RGBA32I, GL_RGBA_INTEGER, GL_INT}, 16, FormatInfo::SINT)
                        : make({GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT}, 16, FormatInfo::UINT);
    case 0x23: return make({GL_RGBA32F, GL_RGBA, GL_FLOAT}, 16);
    case 0x31: return compressed(isSrgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 8);
    case 0x32: return compressed(isSrgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 16);
    case 0x33: return compressed(isSrgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 16);
    case 0x34: return compressed(isSigned ? GL_COMPRESSED_SIGNED_RED_RGTC1 : GL_COMPRESSED_RED_RGTC1, 8);
    case 0x35: return compressed(isSigned ? GL_COMPRESSED_SIGNED_RG_RGTC2 : GL_COMPRESSED_RG_RGTC2, 16);
    // depth formats sampled as color textures
    case 0x11: case 0x12: case 0x13: case 0x14:
        return make({GL_R32F, GL_RED, GL_FLOAT}, 4, FormatInfo::FLOAT, Convert::D24_R32F, 4);
    default: return {};
    }
}

static inline uint8_t ex5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t ex6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static inline uint8_t ex4(uint32_t v) { return (uint8_t)((v << 4) | v); }

void convert_row(Convert c, const uint8_t* src, uint8_t* dst, uint32_t n) {
    switch (c) {
    case Convert::NONE: break;
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
        // GL_FLOAT_32_UNSIGNED_INT_24_8_REV: float depth, then the stencil in the low byte of the next word
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

}  // namespace gfxgl
