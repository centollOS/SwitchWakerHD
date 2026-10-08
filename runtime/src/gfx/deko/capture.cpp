// Captures of the deko3d renderer (dk_capture.h): the GPU's images copied back to the CPU after a captured
// frame is presented, converted to RGBA8 and written as PNG files by a worker thread, as gfx/gl's
// frame_<n>.png / target_*.png dumps (gfx/gl/dump.cpp's PNG writer, here without the GL readback).
#include <sys/stat.h>
#include <zlib.h>

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "dk_capture.h"
#include "dk_draw.h"
#include "dk_surfaces.h"
#include "runtime.h"

Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N&,
                                                     const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N&);
namespace gfxdk {

uint32_t g_capture = 0;

namespace {
// ---------------------------------------------------------------- PNG (gfx/gl/dump.cpp's writer)
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; i--) v.push_back(uint8_t(x >> (i * 8)));
}
void chunk(FILE* f, const char* type, const uint8_t* data, size_t size) {
    std::vector<uint8_t> head;
    put32(head, (uint32_t)size);
    head.insert(head.end(), type, type + 4);
    uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
    if (size) crc = crc32(crc, data, (uInt)size);
    std::vector<uint8_t> tail;
    put32(tail, (uint32_t)crc);
    fwrite(head.data(), 1, head.size(), f);
    if (size) fwrite(data, 1, size, f);
    fwrite(tail.data(), 1, tail.size(), f);
}
void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) { chunk(f, type, data.data(), data.size()); }
// rgba: rows top to bottom. zlib level 1 (Z_BEST_SPEED): a capture writes dozens of files on the console's CPU;
// written through a 512 KiB stdio buffer (the SD card is slow with small writes).
bool write_png(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (w * 4 + 1));
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);
        const uint8_t* row = rgba.data() + size_t(y) * w * 4;
        raw.insert(raw.end(), row, row + size_t(w) * 4);
    }
    uLongf size = compressBound(raw.size());
    std::vector<uint8_t> z(size);
    compress2(z.data(), &size, raw.data(), raw.size(), Z_BEST_SPEED);
    z.resize(size);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    static char buffer[512u << 10];  // (the worker thread only)
    setvbuf(f, buffer, _IOFBF, sizeof buffer);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, w);
    put32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------- deko3d formats on the CPU
enum class Kind : uint8_t { UNORM, SNORM, UINT, SINT, FLOAT, DEPTH, SRGB, PACKED, BC };
struct DkFmt {
    const char* name;
    uint8_t bpb;       // bytes per texel or 4x4 block
    uint8_t block;     // 1 or 4
    uint8_t channels;  // per texel (packed and BC formats: what the decoder makes)
    uint8_t bits;      // per channel (8, 16, 32)
    Kind kind;
};
DkFmt dk_fmt(DkImageFormat f) {
    switch (f) {
    case DkImageFormat_R8_Unorm: return {"R8_Unorm", 1, 1, 1, 8, Kind::UNORM};
    case DkImageFormat_R8_Snorm: return {"R8_Snorm", 1, 1, 1, 8, Kind::SNORM};
    case DkImageFormat_R8_Uint: return {"R8_Uint", 1, 1, 1, 8, Kind::UINT};
    case DkImageFormat_R8_Sint: return {"R8_Sint", 1, 1, 1, 8, Kind::SINT};
    case DkImageFormat_R16_Float: return {"R16_Float", 2, 1, 1, 16, Kind::FLOAT};
    case DkImageFormat_R16_Unorm: return {"R16_Unorm", 2, 1, 1, 16, Kind::UNORM};
    case DkImageFormat_R16_Snorm: return {"R16_Snorm", 2, 1, 1, 16, Kind::SNORM};
    case DkImageFormat_R16_Uint: return {"R16_Uint", 2, 1, 1, 16, Kind::UINT};
    case DkImageFormat_R16_Sint: return {"R16_Sint", 2, 1, 1, 16, Kind::SINT};
    case DkImageFormat_R32_Float: return {"R32_Float", 4, 1, 1, 32, Kind::FLOAT};
    case DkImageFormat_R32_Uint: return {"R32_Uint", 4, 1, 1, 32, Kind::UINT};
    case DkImageFormat_R32_Sint: return {"R32_Sint", 4, 1, 1, 32, Kind::SINT};
    case DkImageFormat_RG8_Unorm: return {"RG8_Unorm", 2, 1, 2, 8, Kind::UNORM};
    case DkImageFormat_RG8_Snorm: return {"RG8_Snorm", 2, 1, 2, 8, Kind::SNORM};
    case DkImageFormat_RG8_Uint: return {"RG8_Uint", 2, 1, 2, 8, Kind::UINT};
    case DkImageFormat_RG8_Sint: return {"RG8_Sint", 2, 1, 2, 8, Kind::SINT};
    case DkImageFormat_RG16_Float: return {"RG16_Float", 4, 1, 2, 16, Kind::FLOAT};
    case DkImageFormat_RG16_Unorm: return {"RG16_Unorm", 4, 1, 2, 16, Kind::UNORM};
    case DkImageFormat_RG16_Snorm: return {"RG16_Snorm", 4, 1, 2, 16, Kind::SNORM};
    case DkImageFormat_RG16_Uint: return {"RG16_Uint", 4, 1, 2, 16, Kind::UINT};
    case DkImageFormat_RG16_Sint: return {"RG16_Sint", 4, 1, 2, 16, Kind::SINT};
    case DkImageFormat_RG32_Float: return {"RG32_Float", 8, 1, 2, 32, Kind::FLOAT};
    case DkImageFormat_RG32_Uint: return {"RG32_Uint", 8, 1, 2, 32, Kind::UINT};
    case DkImageFormat_RG32_Sint: return {"RG32_Sint", 8, 1, 2, 32, Kind::SINT};
    case DkImageFormat_RGBA8_Unorm: return {"RGBA8_Unorm", 4, 1, 4, 8, Kind::UNORM};
    case DkImageFormat_RGBA8_Snorm: return {"RGBA8_Snorm", 4, 1, 4, 8, Kind::SNORM};
    case DkImageFormat_RGBA8_Uint: return {"RGBA8_Uint", 4, 1, 4, 8, Kind::UINT};
    case DkImageFormat_RGBA8_Sint: return {"RGBA8_Sint", 4, 1, 4, 8, Kind::SINT};
    case DkImageFormat_RGBA8_Unorm_sRGB: return {"RGBA8_Unorm_sRGB", 4, 1, 4, 8, Kind::SRGB};
    case DkImageFormat_RGBA16_Float: return {"RGBA16_Float", 8, 1, 4, 16, Kind::FLOAT};
    case DkImageFormat_RGBA16_Unorm: return {"RGBA16_Unorm", 8, 1, 4, 16, Kind::UNORM};
    case DkImageFormat_RGBA16_Snorm: return {"RGBA16_Snorm", 8, 1, 4, 16, Kind::SNORM};
    case DkImageFormat_RGBA16_Uint: return {"RGBA16_Uint", 8, 1, 4, 16, Kind::UINT};
    case DkImageFormat_RGBA16_Sint: return {"RGBA16_Sint", 8, 1, 4, 16, Kind::SINT};
    case DkImageFormat_RGBA32_Float: return {"RGBA32_Float", 16, 1, 4, 32, Kind::FLOAT};
    case DkImageFormat_RGBA32_Uint: return {"RGBA32_Uint", 16, 1, 4, 32, Kind::UINT};
    case DkImageFormat_RGBA32_Sint: return {"RGBA32_Sint", 16, 1, 4, 32, Kind::SINT};
    case DkImageFormat_Z16: return {"Z16", 2, 1, 1, 16, Kind::DEPTH};
    case DkImageFormat_ZF32: return {"ZF32", 4, 1, 1, 32, Kind::DEPTH};
    case DkImageFormat_Z24S8: return {"Z24S8", 4, 1, 1, 32, Kind::DEPTH};
    case DkImageFormat_ZF32_X24S8: return {"ZF32_X24S8", 8, 1, 1, 32, Kind::DEPTH};
    case DkImageFormat_RGB10A2_Unorm: return {"RGB10A2_Unorm", 4, 1, 4, 0, Kind::PACKED};
    case DkImageFormat_RGB10A2_Uint: return {"RGB10A2_Uint", 4, 1, 4, 0, Kind::PACKED};
    case DkImageFormat_RG11B10_Float: return {"RG11B10_Float", 4, 1, 3, 0, Kind::PACKED};
    case DkImageFormat_RGBA_BC1: return {"RGBA_BC1", 8, 4, 4, 0, Kind::BC};
    case DkImageFormat_RGBA_BC1_sRGB: return {"RGBA_BC1_sRGB", 8, 4, 4, 0, Kind::BC};
    case DkImageFormat_RGBA_BC2: return {"RGBA_BC2", 16, 4, 4, 0, Kind::BC};
    case DkImageFormat_RGBA_BC2_sRGB: return {"RGBA_BC2_sRGB", 16, 4, 4, 0, Kind::BC};
    case DkImageFormat_RGBA_BC3: return {"RGBA_BC3", 16, 4, 4, 0, Kind::BC};
    case DkImageFormat_RGBA_BC3_sRGB: return {"RGBA_BC3_sRGB", 16, 4, 4, 0, Kind::BC};
    case DkImageFormat_R_BC4_Unorm: return {"R_BC4_Unorm", 8, 4, 1, 0, Kind::BC};
    case DkImageFormat_R_BC4_Snorm: return {"R_BC4_Snorm", 8, 4, 1, 0, Kind::BC};
    case DkImageFormat_RG_BC5_Unorm: return {"RG_BC5_Unorm", 16, 4, 2, 0, Kind::BC};
    case DkImageFormat_RG_BC5_Snorm: return {"RG_BC5_Snorm", 16, 4, 2, 0, Kind::BC};
    default: return {nullptr, 0, 0, 0, 0, Kind::UNORM};
    }
}
std::string dk_name(DkImageFormat f) {
    const DkFmt d = dk_fmt(f);
    return d.name ? d.name : "DkImageFormat " + std::to_string(int(f));
}
// bytes of a w x h image (one layer) in the copy engine's linear layout: rows of blocks without padding
size_t linear_bytes(DkImageFormat f, uint32_t w, uint32_t h) {
    const DkFmt d = dk_fmt(f);
    if (!d.name) return 0;
    return size_t((w + d.block - 1) / d.block) * ((h + d.block - 1) / d.block) * d.bpb;
}

float half_to_float(uint16_t h) {
    const uint32_t s = uint32_t(h >> 15) << 31, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    uint32_t bits;
    if (e == 0) {
        if (!m) bits = s;
        else {  // subnormal
            float v = std::ldexp(float(m), -24);
            return s ? -v : v;
        }
    } else if (e == 31) bits = s | 0x7F800000u | (m << 13);
    else bits = s | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
// unsigned small floats of RG11B10 (no sign; 5-bit exponent)
float small_float(uint32_t v, int mantBits) {
    const uint32_t e = v >> mantBits, m = v & ((1u << mantBits) - 1);
    if (e == 0) return std::ldexp(float(m), -14 - mantBits);
    if (e == 31) return m ? NAN : INFINITY;
    return std::ldexp(1.0f + float(m) / float(1u << mantBits), int(e) - 15);
}
uint8_t unit8(float v) {  // 0..1 -> 0..255 (NaN: 0)
    if (!(v > 0.0f)) return 0;
    return v >= 1.0f ? 255 : uint8_t(std::lround(v * 255.0f));
}
uint8_t snorm8(float v) { return unit8(std::clamp(v, -1.0f, 1.0f) * 0.5f + 0.5f); }

// ---- BC1-BC5 blocks -> 16 texels
void rgb565(uint16_t c, int out[3]) {
    out[0] = ((c >> 11) & 31) * 255 / 31;
    out[1] = ((c >> 5) & 63) * 255 / 63;
    out[2] = (c & 31) * 255 / 31;
}
// color block (8 bytes); bc1: the 3-color mode with transparent black when c0 <= c1 (BC2/BC3 always 4 colors)
void bc_color(const uint8_t* b, uint8_t out[16][4], bool bc1) {
    const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    int p[4][4];
    rgb565(c0, p[0]);
    rgb565(c1, p[1]);
    p[0][3] = p[1][3] = 255;
    if (!bc1 || c0 > c1) {
        for (int c = 0; c < 3; c++) {
            p[2][c] = (2 * p[0][c] + p[1][c]) / 3;
            p[3][c] = (p[0][c] + 2 * p[1][c]) / 3;
        }
        p[2][3] = p[3][3] = 255;
    } else {
        for (int c = 0; c < 3; c++) {
            p[2][c] = (p[0][c] + p[1][c]) / 2;
            p[3][c] = 0;
        }
        p[2][3] = 255;
        p[3][3] = 0;
    }
    const uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) {
        const int k = (idx >> (2 * i)) & 3;
        for (int c = 0; c < 4; c++) out[i][c] = uint8_t(p[k][c]);
    }
}
// 8-byte interpolated channel block (BC3 alpha, BC4, BC5 halves) -> 16 values in 0..255 (signed: snorm shown
// as (v + 1) / 2)
void bc_channel(const uint8_t* b, uint8_t out[16], bool isSigned) {
    float v[8];
    if (!isSigned) {
        const float a0 = b[0], a1 = b[1];
        v[0] = a0;
        v[1] = a1;
        if (b[0] > b[1])
            for (int i = 2; i < 8; i++) v[i] = ((8 - i) * a0 + (i - 1) * a1) / 7.0f;
        else {
            for (int i = 2; i < 6; i++) v[i] = ((6 - i) * a0 + (i - 1) * a1) / 5.0f;
            v[6] = 0;
            v[7] = 255;
        }
        for (float& x : v) x /= 255.0f;
    } else {
        const float a0 = std::max(-127.0f, float(int8_t(b[0]))), a1 = std::max(-127.0f, float(int8_t(b[1])));
        v[0] = a0;
        v[1] = a1;
        if (int8_t(b[0]) > int8_t(b[1]))
            for (int i = 2; i < 8; i++) v[i] = ((8 - i) * a0 + (i - 1) * a1) / 7.0f;
        else {
            for (int i = 2; i < 6; i++) v[i] = ((6 - i) * a0 + (i - 1) * a1) / 5.0f;
            v[6] = -127;
            v[7] = 127;
        }
        for (float& x : v) x = x / 254.0f + 0.5f;
    }
    uint64_t idx = 0;
    for (int i = 0; i < 6; i++) idx |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = unit8(v[(idx >> (3 * i)) & 7]);
}

// the image's bytes (rows of blocks, no padding) -> RGBA8 rows. Channels a format lacks: as OpenGL's
// glReadPixels gives them (gfx/gl dumps): green and blue 0, alpha 1. note: what the conversion did.
bool to_rgba8(DkImageFormat f, const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out, std::string& note) {
    const DkFmt d = dk_fmt(f);
    if (!d.name) return false;
    out.assign(size_t(w) * h * 4, 0);
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    if (d.kind == Kind::BC) {
        const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        uint8_t texels[16][4];
        uint8_t ch[2][16];
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t* b = src + (size_t(by) * bw + bx) * d.bpb;
                switch (f) {
                case DkImageFormat_RGBA_BC1: case DkImageFormat_RGBA_BC1_sRGB: bc_color(b, texels, true); break;
                case DkImageFormat_RGBA_BC2: case DkImageFormat_RGBA_BC2_sRGB:
                    bc_color(b + 8, texels, false);
                    for (int i = 0; i < 16; i++) texels[i][3] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
                    break;
                case DkImageFormat_RGBA_BC3: case DkImageFormat_RGBA_BC3_sRGB:
                    bc_color(b + 8, texels, false);
                    bc_channel(b, ch[0], false);
                    for (int i = 0; i < 16; i++) texels[i][3] = ch[0][i];
                    break;
                case DkImageFormat_R_BC4_Unorm: case DkImageFormat_R_BC4_Snorm:
                    bc_channel(b, ch[0], f == DkImageFormat_R_BC4_Snorm);
                    for (int i = 0; i < 16; i++) texels[i][0] = ch[0][i], texels[i][1] = texels[i][2] = 0, texels[i][3] = 255;
                    break;
                default:  // BC5
                    bc_channel(b, ch[0], f == DkImageFormat_RG_BC5_Snorm);
                    bc_channel(b + 8, ch[1], f == DkImageFormat_RG_BC5_Snorm);
                    for (int i = 0; i < 16; i++) texels[i][0] = ch[0][i], texels[i][1] = ch[1][i], texels[i][2] = 0, texels[i][3] = 255;
                    break;
                }
                for (int i = 0; i < 16; i++) {
                    const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < w && y < h) memcpy(&out[(size_t(y) * w + x) * 4], texels[i], 4);
                }
            }
        if (f == DkImageFormat_R_BC4_Snorm || f == DkImageFormat_RG_BC5_Snorm) note = "; snorm shown as (v + 1) / 2";
        return true;
    }
    const size_t n = size_t(w) * h;
    if (d.kind == Kind::DEPTH) {  // stretched from the smallest to the largest depth, gray
        std::vector<float> z(n);
        for (size_t i = 0; i < n; i++) {
            const uint8_t* p = src + i * d.bpb;
            if (f == DkImageFormat_Z16) z[i] = float(p[0] | p[1] << 8) / 65535.0f;
            else if (f == DkImageFormat_Z24S8) {
                uint32_t v;
                memcpy(&v, p, 4);
                z[i] = float(v >> 8) / 16777215.0f;
            } else memcpy(&z[i], p, 4);  // ZF32, ZF32_X24S8 (the float first)
        }
        float lo = INFINITY, hi = -INFINITY;
        for (float v : z)
            if (std::isfinite(v)) lo = std::min(lo, v), hi = std::max(hi, v);
        const float range = hi > lo ? hi - lo : 1.0f;
        for (size_t i = 0; i < n; i++) {
            const uint8_t g = std::isfinite(z[i]) && lo <= hi ? unit8((z[i] - lo) / range) : 0;
            out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = g;
        }
        char b[96];
        snprintf(b, sizeof b, "; depth %.6f..%.6f stretched to black..white", lo <= hi ? lo : 0.0f, lo <= hi ? hi : 0.0f);
        note = b;
        return true;
    }
    if (d.kind == Kind::PACKED) {
        for (size_t i = 0; i < n; i++) {
            uint32_t v;
            memcpy(&v, src + i * 4, 4);
            uint8_t* o = &out[i * 4];
            if (f == DkImageFormat_RG11B10_Float) {
                o[0] = unit8(small_float(v & 0x7FF, 6));
                o[1] = unit8(small_float((v >> 11) & 0x7FF, 6));
                o[2] = unit8(small_float(v >> 22, 5));
            } else {  // RGB10A2: R in the low bits (the Uint one shown as if normalized)
                o[0] = uint8_t((v & 0x3FF) >> 2);
                o[1] = uint8_t(((v >> 10) & 0x3FF) >> 2);
                o[2] = uint8_t(((v >> 20) & 0x3FF) >> 2);
                o[3] = uint8_t((v >> 30) * 85);
            }
        }
        if (f == DkImageFormat_RG11B10_Float) note = "; floats clamped to 0..1";
        return true;
    }
    // plain channels of 8, 16 or 32 bits
    float maxValue = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* p = src + i * d.bpb;
        uint8_t* o = &out[i * 4];
        for (int c = 0; c < d.channels; c++) {
            const uint8_t* q = p + c * (d.bits / 8);
            uint8_t v = 0;
            switch (d.kind) {
            case Kind::UNORM: case Kind::SRGB:
                v = d.bits == 8 ? q[0] : q[d.bits / 8 - 1];  // the high byte
                break;
            case Kind::SNORM:
                v = d.bits == 8 ? snorm8(float(int8_t(q[0])) / 127.0f) : snorm8(float(int16_t(q[0] | q[1] << 8)) / 32767.0f);
                break;
            case Kind::UINT: case Kind::SINT: v = q[0]; break;  // the low byte
            default: {  // FLOAT
                float x;
                if (d.bits == 16) x = half_to_float(uint16_t(q[0] | q[1] << 8));
                else memcpy(&x, q, 4);
                if (c < 3 && x > maxValue) maxValue = x;
                v = unit8(x);
                break;
            }
            }
            o[c] = v;
        }
    }
    switch (d.kind) {
    case Kind::SNORM: note = "; snorm shown as (v + 1) / 2"; break;
    case Kind::UINT: case Kind::SINT: note = "; integers: the low byte of each channel"; break;
    case Kind::FLOAT:
        if (maxValue > 1.0f) {
            char b[64];
            snprintf(b, sizeof b, "; floats clamped to 0..1 (largest color value %g)", maxValue);
            note = b;
        }
        break;
    default: break;
    }
    return true;
}

void srgb_encode(std::vector<uint8_t>& rgba) {
    static uint8_t table[256];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 256; i++) {
            double c = i / 255.0;
            c = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
            table[i] = uint8_t(std::lround(std::clamp(c, 0.0, 1.0) * 255.0));
        }
        built = true;
    }
    for (size_t i = 0; i < rgba.size(); i++)
        if ((i & 3) != 3) rgba[i] = table[rgba[i]];
}

// ---------------------------------------------------------------- the PNG worker
struct Job {
    std::string path, meta;
    uint32_t w = 0, h = 0;
    DkImageFormat format = DkImageFormat_None;
    bool encodeSrgb = false;
    uint64_t frame = 0;
    std::vector<uint8_t> data;
};
std::mutex g_jobMutex;
std::condition_variable g_jobCv;
std::deque<Job> g_jobs;
size_t g_jobBytes = 0;  // data bytes queued or being written
// the capture being written: files queued, done (written or failed), written, dropped (queue full), textures
// not written because the GPU holds exactly their upload data; final once capture_present has queued all
uint64_t g_filesFrame = ~0ull;
uint32_t g_filesQueued = 0, g_filesDone = 0, g_filesWritten = 0, g_filesDropped = 0, g_texturesIdentical = 0;
uint64_t g_filesStart = 0;  // now_ns() when the capture began (the render thread's readback included)
bool g_filesFinal = false;
bool g_workerStarted = false;
Thread g_worker;
// Data bytes the writer may have queued. The render thread never waits for the writer: a file that does not
// fit is dropped with a log line (the console has ~1.4 GiB of heap left after the setup; RGBA conversions of
// the worker come on top of this).
constexpr size_t kMaxQueuedBytes = 128u << 20;

std::atomic<uint64_t> g_doneFrame{0};  // the last capture whose files are all written (capture_done_frame)

void log_capture_done(uint64_t frame) {  // (g_jobMutex held)
    g_doneFrame.store(frame, std::memory_order_release);
    LOG("[dk] capture of frame %llu done in %.1f s: %u files written to captures/%llu/, %u failed, %u dropped (writer "
        "queue full), %u sampled textures identical to their upload data not written",
        (unsigned long long)frame, double(now_ns() - g_filesStart) / 1e9, g_filesWritten, (unsigned long long)frame,
        g_filesDone - g_filesWritten, g_filesDropped, g_texturesIdentical);
}

void worker_main(void*) {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(g_jobMutex);
            g_jobCv.wait(lk, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        const uint64_t t0 = now_ns();
        std::vector<uint8_t> rgba;
        std::string note;
        const size_t bytes = job.data.size();
        bool ok = false;
        if (!to_rgba8(job.format, job.data.data(), job.w, job.h, rgba, note))
            LOG("[dk] capture %s NOT written: no CPU conversion for %s; %s", job.path.c_str(), dk_name(job.format).c_str(),
                job.meta.c_str());
        else {
            job.data = std::vector<uint8_t>();
            if (job.encodeSrgb) {
                srgb_encode(rgba);
                note += "; sRGB-encoded as the display shows it";
            }
            ok = write_png(job.path, job.w, job.h, rgba);
            LOG("[dk] capture %s%s: %s%s (%.0f ms)", job.path.c_str(), ok ? "" : " NOT WRITTEN (cannot write the file)",
                job.meta.c_str(), note.c_str(), double(now_ns() - t0) / 1e6);
        }
        {
            std::lock_guard<std::mutex> lk(g_jobMutex);
            g_jobBytes -= bytes;
            if (job.frame == g_filesFrame) {
                g_filesWritten += ok;
                if (++g_filesDone == g_filesQueued && g_filesFinal) log_capture_done(job.frame);
            }
        }
    }
}

// false: dropped (no writer, or its queue full)
bool queue_job(Job&& job) {
    if (!g_workerStarted) {
        // priority 0x3B (the game's own; an application may not go lower, shaders_dk.cpp), core 2
        const Result rc = threadCreate(&g_worker, worker_main, nullptr, nullptr, 256u << 10, 0x3B, 2);
        if (R_FAILED(rc) || R_FAILED(threadStart(&g_worker))) {
            LOG("[dk] capture: cannot start the PNG writer thread (result 0x%X): nothing written", unsigned(rc));
            return false;
        }
        g_workerStarted = true;
    }
    std::unique_lock<std::mutex> lk(g_jobMutex);
    if (g_jobBytes && g_jobBytes + job.data.size() > kMaxQueuedBytes) {
        if (job.frame == g_filesFrame) g_filesDropped++;
        LOG("[dk] capture %s dropped: the PNG writer has %.1f MiB queued (at most %u MiB; the render thread does not "
            "wait for it)", job.path.c_str(), double(g_jobBytes) / 1048576.0, unsigned(kMaxQueuedBytes >> 20));
        return false;
    }
    g_jobBytes += job.data.size();
    if (job.frame == g_filesFrame) g_filesQueued++;
    g_jobs.push_back(std::move(job));
    g_jobCv.notify_one();
    return true;
}

// ---------------------------------------------------------------- what the frame's draws used
struct Use {
    std::string what;              // unit, view, texture words
    std::vector<uint32_t> draws;   // executed draws (frame numbering)
};
struct Noted {
    Surface* s = nullptr;
    bool target = false, sampled = false;
    uint32_t layer = 0;            // the layer read back (a target's first rendered layer)
    std::vector<uint32_t> targetDraws;
    std::vector<Use> uses;
};
std::map<Surface*, Noted> g_noted;  // by surface: deterministic enough, and the log is sorted below
std::vector<Noted*> g_order;        // first-noted order
struct Pending {
    Surface* s;
    std::string what;
};
std::vector<Pending> g_pending;     // the textures of the draw being prepared
uint64_t g_armedFrame = ~0ull;

Noted& note(Surface* s) {
    auto [it, fresh] = g_noted.try_emplace(s);
    if (fresh) {
        it->second.s = s;
        g_order.push_back(&it->second);
    }
    return it->second;
}

char sel_name(uint32_t sel) { return "xyzw01??"[sel & 7]; }

// "#3-5,#9": sorted draw numbers as ranges (the first 12: a log line holds 2 KiB)
std::string ranges(std::vector<uint32_t> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    std::string s;
    int n = 0;
    for (size_t i = 0; i < v.size();) {
        size_t j = i;
        while (j + 1 < v.size() && v[j + 1] == v[j] + 1) j++;
        if (n++ == 12) {
            s += ",...";
            break;
        }
        char b[32];
        if (j > i) snprintf(b, sizeof b, "%s#%u-%u", s.empty() ? "" : ",", v[i], v[j]);
        else snprintf(b, sizeof b, "%s#%u", s.empty() ? "" : ",", v[i]);
        s += b;
        i = j + 1;
    }
    return s.empty() ? "none" : s;
}

const char* dim_name(uint32_t dim) {
    static const char* const kNames[8] = {"1d", "2d", "3d", "cube", "1darray", "2darray", "2dmsaa", "2darraymsaa"};
    return dim < 8 ? kNames[dim] : "?";
}

std::string surface_meta(const Surface* s) {
    char b[320];
    snprintf(b, sizeof b, "surface %08X %ux%u%s%s GX2 format 0x%03X%s -> deko3d %s (image %ux%u, %u mips, %u layers, "
             "type %d), %s, tile mode %u, swizzle 0x%X, %s", s->addr, s->width, s->height,
             s->slices > 1 ? " x" : "", s->slices > 1 ? std::to_string(s->slices).c_str() : "", s->format,
             s->isDepth ? " (depth)" : "", dk_name(s->fmt.image).c_str(), s->img.pw, s->img.ph, s->mips, s->img.layers,
             int(s->img.type), dim_name(s->dim), s->tileMode, s->swizzle,
             s->gpuWritten ? "GPU-written" : "CPU data");
    return b;
}

void make_dir(const std::string& d) {
    if (mkdir(d.c_str(), 0777) != 0 && errno != EEXIST) LOG("[dk] capture: cannot create %s (errno %d)", d.c_str(), errno);
}

// ---------------------------------------------------------------- GPU readback
// One image layer to copy back: what it is and where its PNG goes
struct Readback {
    const DkImage* image;
    DkImageFormat format;
    uint32_t w, h, layer;
    Job job;  // path, meta, size, format (data filled from the block)
    // a sampled texture of CPU data: compared with its upload data after the readback; both written when they
    // differ (or always with WWHD_DK_CAPTURE_ALL_TEXTURES=1), neither when the GPU holds exactly that data
    Surface* uploadOf = nullptr;
    std::string uploadPath, uploadMeta;
};
// WWHD_DK_CAPTURE_ALL_TEXTURES=1: every sampled texture and its upload picture, as before (slow: ~200 files)
bool all_textures() {
    static const bool all = [] {
        const char* e = getenv("WWHD_DK_CAPTURE_ALL_TEXTURES");
        return e && *e && *e != '0';
    }();
    return all;
}
// units (texels, or 4x4 blocks) of a w x h image whose bytes differ; first: the first one's unit coordinates
uint32_t count_differences(DkImageFormat f, uint32_t w, uint32_t h, const uint8_t* a, const uint8_t* b, uint32_t& units,
                           uint32_t first[2]) {
    const DkFmt d = dk_fmt(f);
    const uint32_t uw = (w + d.block - 1) / d.block, uh = (h + d.block - 1) / d.block;
    units = uw * uh;
    uint32_t n = 0;
    for (uint32_t y = 0; y < uh; y++) {
        const size_t row = size_t(y) * uw * d.bpb;
        if (!memcmp(a + row, b + row, size_t(uw) * d.bpb)) continue;
        for (uint32_t x = 0; x < uw; x++)
            if (memcmp(a + row + size_t(x) * d.bpb, b + row + size_t(x) * d.bpb, d.bpb)) {
                if (!n) first[0] = x, first[1] = y;
                n++;
            }
    }
    return n;
}

struct ReadbackBlock {
    // kMaxCopies: dkCmdBufCopyImageToBuffer records 28 words per layer; the command memory is not grown
    static constexpr uint32_t kSize = 16u << 20, kCmdSize = 256u << 10, kMaxCopies = 1024;
    DkMemBlock mem = nullptr, cmdMem = nullptr;
    DkCmdBuf cmd = nullptr;
    void* storage = nullptr;  // the readback block's memory, from the heap here: no heap, no capture (not an abort)
    bool create() {
        storage = aligned_alloc(DK_MEMBLOCK_ALIGNMENT, kSize);
        if (!storage) {
            LOG("[dk] capture: no %u MiB of heap for the readback block: the GPU's images are not written", kSize >> 20);
            return false;
        }
        LOG("[dk] creating memory blocks: capture readback (%u MiB, CPU-cached) and its commands", kSize >> 20);
        log_flush();  // deko3d aborts when a creation fails
        DkMemBlockMaker m;
        dkMemBlockMakerDefaults(&m, R.device, kSize);
        m.flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuUncached;
        m.storage = storage;
        mem = dkMemBlockCreate(&m);
        dkMemBlockFlushCpuCache(mem, 0, kSize);  // no CPU line of it left to be written back over the GPU's data
        dkMemBlockMakerDefaults(&m, R.device, kCmdSize);
        m.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        cmdMem = dkMemBlockCreate(&m);
        DkCmdBufMaker cm;
        dkCmdBufMakerDefaults(&cm, R.device);
        cmd = dkCmdBufCreate(&cm);
        return true;
    }
    void destroy() {
        if (!storage) return;
        dkCmdBufDestroy(cmd);
        dkMemBlockDestroy(cmdMem);
        dkMemBlockDestroy(mem);
        free(storage);
        *this = ReadbackBlock{};
    }
};

// copies the batch's images into the block, waits for the GPU, hands each to the worker
bool run_batch(ReadbackBlock& rb, std::vector<Readback*>& batch, uint64_t& bytesRead) {
    if (batch.empty()) return true;
    if (dkQueueIsInErrorState(R.queue)) {
        LOG("[dk] capture: the deko3d queue is in an error state: no readback");
        return false;
    }
    dkCmdBufClear(rb.cmd);
    dkCmdBufAddMemory(rb.cmd, rb.cmdMem, 0, ReadbackBlock::kCmdSize);
    dkCmdBufBarrier(rb.cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    std::vector<uint32_t> offsets;
    uint32_t at = 0;
    for (Readback* r : batch) {
        offsets.push_back(at);
        DkImageView v;
        dkImageViewDefaults(&v, r->image);
        const DkImageRect rect = {0, 0, r->layer, r->w, r->h, 1};
        const DkCopyBuf dst = {dkMemBlockGetGpuAddr(rb.mem) + at, 0, 0};
        dkCmdBufCopyImageToBuffer(rb.cmd, &v, &rect, &dst, 0);
        at += uint32_t((linear_bytes(r->format, r->w, r->h) + 255) & ~size_t(255));
    }
    // the copies' writes out of the GPU's L2 into memory before the CPU reads them
    dkCmdBufBarrier(rb.cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
    {
        Stage stage("deko3d: capture readback");
        dkQueueSubmitCommands(R.queue, dkCmdBufFinishList(rb.cmd));
        dkQueueWaitIdle(R.queue);
    }
    if (dkQueueIsInErrorState(R.queue)) {
        LOG("[dk] capture: the deko3d queue went into an error state during the readback copies");
        return false;
    }
    dkMemBlockFlushCpuCache(rb.mem, 0, at);  // the CPU's view of the block: what the GPU wrote
    const uint8_t* cpu = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(rb.mem));
    for (size_t i = 0; i < batch.size(); i++) {
        Readback* r = batch[i];
        const size_t n = linear_bytes(r->format, r->w, r->h);
        const uint8_t* gpu = cpu + offsets[i];
        bytesRead += n;
        if (r->uploadOf) {
            Job up;
            bool changed = false;
            if (!capture_upload_data(r->uploadOf, up.data, changed) || up.data.size() != n) {
                LOG("[dk] capture %s: no upload data to compare (%zu bytes decoded, %zu expected)", r->uploadPath.c_str(),
                    up.data.size(), n);
            } else {
                uint32_t units = 0, first[2] = {0, 0};
                const uint32_t diff = count_differences(r->format, r->w, r->h, gpu, up.data.data(), units, first);
                const bool block = dk_fmt(r->format).block > 1;
                if (!diff && !all_textures()) {
                    std::lock_guard<std::mutex> lk(g_jobMutex);
                    g_texturesIdentical++;
                    continue;
                }
                char b[200];
                if (diff)
                    snprintf(b, sizeof b, "; DIFFERS from its upload data in %u of %u %s (the first at %s %u,%u)%s", diff,
                             units, block ? "4x4 blocks" : "texels", block ? "block" : "texel", first[0], first[1],
                             changed ? "; the guest data changed since the last upload" : "");
                else
                    snprintf(b, sizeof b, "; identical to its upload data");
                r->job.meta += b;
                up.path = r->uploadPath;
                up.meta = r->uploadMeta + (changed ? "; the guest data CHANGED since the last upload (the GPU has older data)"
                                                   : "; guest data as last uploaded") + b;
                up.w = r->job.w;
                up.h = r->job.h;
                up.format = r->format;
                up.frame = r->job.frame;
                r->job.data.assign(gpu, gpu + n);
                queue_job(std::move(r->job));
                queue_job(std::move(up));
                continue;
            }
        }
        r->job.data.assign(gpu, gpu + n);
        queue_job(std::move(r->job));
    }
    batch.clear();
    return true;
}
}  // namespace

// ---------------------------------------------------------------- the draw path's notes
void capture_arm(uint32_t what, const char* why) {
    g_capture = what;
    g_noted.clear();
    g_order.clear();
    g_pending.clear();
    g_armedFrame = what ? R.frame + 1 : ~0ull;
    if (what)
        LOG("[dk] capture of frame %llu armed (%s): %s%s%s to captures/%llu/", (unsigned long long)(R.frame + 1), why,
            what & kCapturePictures ? "pictures " : "", what & kCaptureTargets ? "render targets " : "",
            what & kCaptureTextures ? "sampled textures and their upload data " : "", (unsigned long long)(R.frame + 1));
}

void capture_draw_begin() { g_pending.clear(); }

void capture_note_texture(Surface* s, bool vertex, uint32_t unit, const uint32_t* w, const uint32_t* smp, bool compare,
                          bool feedback) {
    if (!s) return;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N w0;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N w1;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N w4;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD5_N w5;
    memcpy(static_cast<void*>(&w0), &w[0], 4);
    memcpy(static_cast<void*>(&w1), &w[1], 4);
    memcpy(static_cast<void*>(&w4), &w[4], 4);
    memcpy(static_cast<void*>(&w5), &w[5], 4);
    char b[320];
    snprintf(b, sizeof b,
             "%s%u view %c%c%c%c %s, words fmt 0x%03X %ux%u mips %u-%u layers %u-%u num %u comp %u%u%u%u srf %u "
             "degamma %u endian %u%s%s (smp %08X %08X %08X)",
             vertex ? "vt" : "t", unit, sel_name(uint32_t(w4.get_DST_SEL_X())), sel_name(uint32_t(w4.get_DST_SEL_Y())),
             sel_name(uint32_t(w4.get_DST_SEL_Z())), sel_name(uint32_t(w4.get_DST_SEL_W())), dim_name(uint32_t(w0.get_DIM())),
             uint32_t(LatteTexture_ReconstructGX2Format(w1, w4)), uint32_t(w0.get_WIDTH()) + 1, uint32_t(w1.get_HEIGHT()) + 1,
             uint32_t(w4.get_BASE_LEVEL()), uint32_t(w5.get_LAST_LEVEL()), uint32_t(w5.get_BASE_ARRAY()),
             uint32_t(w5.get_LAST_ARRAY()), uint32_t(w4.get_NUM_FORM_ALL()), uint32_t(w4.get_FORMAT_COMP_X()),
             uint32_t(w4.get_FORMAT_COMP_Y()), uint32_t(w4.get_FORMAT_COMP_Z()), uint32_t(w4.get_FORMAT_COMP_W()),
             uint32_t(w4.get_SRF_MODE_ALL()), uint32_t(w4.get_FORCE_DEGAMMA()), uint32_t(w4.get_ENDIAN_SWAP()),
             compare ? " compare" : "", feedback ? " feedback copy" : "", smp[0], smp[1], smp[2]);
    g_pending.push_back({s, b});
}

void capture_note_draw(uint32_t index, const std::array<Surface*, 8>& colors, const uint32_t* slices, Surface* depth,
                       uint32_t depthSlice) {
    for (Pending& p : g_pending) {
        Noted& n = note(p.s);
        n.sampled = true;
        auto it = std::find_if(n.uses.begin(), n.uses.end(), [&](const Use& u) { return u.what == p.what; });
        if (it == n.uses.end()) n.uses.push_back({p.what, {index}});
        else if (it->draws.back() != index) it->draws.push_back(index);
    }
    g_pending.clear();
    auto target = [&](Surface* s, uint32_t layer) {
        Noted& n = note(s);
        if (!n.target) n.layer = layer;
        n.target = true;
        if (n.targetDraws.empty() || n.targetDraws.back() != index) n.targetDraws.push_back(index);
    };
    for (int i = 0; i < 8; i++)
        if (colors[i]) target(colors[i], slices[i]);
    if (depth) target(depth, depthSlice);
}

// ---------------------------------------------------------------- after the present
void capture_present(const DkImage& window, uint32_t ww, uint32_t wh, const PresentSource& src) {
    const uint32_t what = g_capture;
    const uint64_t frame = R.frame + 1;
    if (!what || g_armedFrame != frame) return;
    g_capture = 0;
    const uint64_t t0 = now_ns();
    {
        std::lock_guard<std::mutex> lk(g_jobMutex);
        g_filesFrame = frame;
        g_filesQueued = g_filesDone = g_filesWritten = g_filesDropped = g_texturesIdentical = 0;
        g_filesFinal = false;
        g_filesStart = t0;
    }
    const std::string dir = "captures/" + std::to_string(frame);
    make_dir("captures");
    make_dir(dir);
    const std::string n = std::to_string(frame);
    std::deque<Readback> reads;  // (stable addresses)
    uint32_t compared = 0, skipped = 0;
    auto add = [&](const DkImage* image, DkImageFormat f, uint32_t w, uint32_t h, uint32_t layer, std::string path,
                   std::string meta, bool encode) {
        if (!dk_fmt(f).name) {
            LOG("[dk] capture %s skipped: no CPU conversion for deko3d format %s; %s", path.c_str(), dk_name(f).c_str(),
                meta.c_str());
            skipped++;
            return static_cast<Readback*>(nullptr);
        }
        if (linear_bytes(f, w, h) > ReadbackBlock::kSize) {
            LOG("[dk] capture %s skipped: %ux%u %s is larger than the %u MiB readback block; %s", path.c_str(), w, h,
                dk_name(f).c_str(), ReadbackBlock::kSize >> 20, meta.c_str());
            skipped++;
            return static_cast<Readback*>(nullptr);
        }
        Readback r{image, f, w, h, layer, {}};
        r.job.path = dir + "/" + path;
        r.job.meta = std::move(meta);
        r.job.w = w;
        r.job.h = h;
        r.job.format = f;
        r.job.encodeSrgb = encode;
        r.job.frame = frame;
        reads.push_back(std::move(r));
        return &reads.back();
    };
    // ---- the pictures
    if (what & kCapturePictures) {
        char meta[160];
        snprintf(meta, sizeof meta, "the swapchain image presented, %ux%u RGBA8_Unorm (the window: picture, bars, FPS "
                 "counter, overlay)", ww, wh);
        add(&window, DkImageFormat_RGBA8_Unorm, ww, wh, 0, "frame_" + n + "_window.png", meta, false);
        if (Surface* s = src.surface; s && s->img.valid) {
            const bool encode = R.tvSrgb.load(std::memory_order_relaxed);
            add(&s->img.image, s->fmt.image, s->img.pw, s->img.ph, 0, "frame_" + n + ".png",
                std::string("the TV picture presented (") + (s == S.tvScan.get() ? "the TV scan copy" : "the TV buffer itself") +
                    ", stored bytes" + (encode ? " then sRGB-encoded: sRGB TV format" : ": TV format not sRGB") + "), " +
                    surface_meta(s),
                encode);
        } else
            LOG("[dk] capture frame_%s.png skipped: no TV picture presented yet", n.c_str());
        if (Surface* t = S.tvSource; t && t != src.surface && t->img.valid)
            add(&t->img.image, t->fmt.image, t->img.pw, t->img.ph, 0, "frame_" + n + "_tv_source.png",
                "the buffer the game copied to the TV scan buffer (stored bytes), " + surface_meta(t), false);
    }
    // ---- the frame's targets and textures, in the order the draws first used them
    for (Noted* nt : g_order) {
        Surface* s = nt->s;
        if (!s->img.valid) continue;
        const bool asTarget = nt->target && (what & kCaptureTargets);
        const bool asTexture = nt->sampled && (what & kCaptureTextures);
        if (!asTarget && !asTexture) continue;
        std::string uses;
        if (nt->target) uses += "; rendered by draws " + ranges(nt->targetDraws);
        for (size_t i = 0; i < nt->uses.size() && i < 4; i++)
            uses += "; sampled as " + nt->uses[i].what + " by draws " + ranges(nt->uses[i].draws);
        if (nt->uses.size() > 4) uses += "; " + std::to_string(nt->uses.size() - 4) + " more uses";
        char name[96];
        const uint32_t layer = std::min(nt->target ? nt->layer : 0u, s->img.layers - 1);
        if (nt->target)
            snprintf(name, sizeof name, "target_%08X_%ux%u_f%03X%s%s", s->addr, s->width, s->height, s->format,
                     layer ? ("_s" + std::to_string(layer)).c_str() : "", s->isDepth ? "_depth" : "");
        else
            snprintf(name, sizeof name, "tex_%08X_%ux%u_f%03X%s%s", s->addr, s->width, s->height, s->format,
                     s->dim != 1 ? "_" : "", s->dim != 1 ? dim_name(s->dim) : "");
        const std::string base = name;
        Readback* r = add(&s->img.image, s->fmt.image, s->img.pw, s->img.ph, layer, base + ".png",
                          "GPU image level 0 layer " + std::to_string(layer) + ", " + surface_meta(s) + uses, false);
        // a texture the CPU wrote: compared after the readback with its guest data as the upload decodes it
        // (at the guest size: a texture of CPU data is not scaled)
        if (r && asTexture && !s->gpuWritten && s->img.pw == s->width && s->img.ph == s->height) {
            r->uploadOf = s;
            r->uploadPath = dir + "/" + base + "_upload.png";
            r->uploadMeta = "CPU-decoded upload data (detiled, converted; before the GPU) level 0 layer 0, " + surface_meta(s) + uses;
            compared++;
        }
    }
    // ---- the GPU's images, in batches through the readback block
    ReadbackBlock rb;
    uint64_t bytesRead = 0;
    int batches = 0;
    bool ok = true;
    if (!reads.empty() && !rb.create()) ok = false;
    else if (!reads.empty()) {
        std::vector<Readback*> batch;
        uint64_t used = 0;
        auto flush = [&] {
            if (ok) ok = run_batch(rb, batch, bytesRead);
            batch.clear();
            batches++;
            used = 0;
        };
        for (Readback& r : reads) {
            const uint64_t size = (linear_bytes(r.format, r.w, r.h) + 255) & ~uint64_t(255);
            if (used + size > ReadbackBlock::kSize || batch.size() == ReadbackBlock::kMaxCopies) flush();
            batch.push_back(&r);
            used += size;
        }
        if (!batch.empty()) flush();
        rb.destroy();
    }
    forget_state();  // (another command buffer ran on the queue)
    uint32_t queued = 0, identical = 0, dropped = 0;
    {
        std::lock_guard<std::mutex> lk(g_jobMutex);
        queued = g_filesQueued;
        identical = g_texturesIdentical;
        dropped = g_filesDropped;
    }
    LOG("[dk] capture of frame %llu: %zu GPU images read back (%.1f MiB in %d readback batches%s), %u textures compared "
        "with their upload data (%u identical, not written%s), %u files queued to %s/, %u dropped, %u skipped; %zu render "
        "targets, %zu sampled surfaces noted; render thread %.0f ms (the PNG writer goes on)",
        (unsigned long long)frame, reads.size(), double(bytesRead) / 1048576.0, batches, ok ? "" : ", FAILED", compared,
        identical, all_textures() ? "" : "; WWHD_DK_CAPTURE_ALL_TEXTURES=1 writes them", queued, dir.c_str(), dropped,
        skipped,
        size_t(std::count_if(g_order.begin(), g_order.end(), [](Noted* x) { return x->target; })),
        size_t(std::count_if(g_order.begin(), g_order.end(), [](Noted* x) { return x->sampled; })),
        double(now_ns() - t0) / 1e6);
    g_noted.clear();
    g_order.clear();
    {
        std::lock_guard<std::mutex> lk(g_jobMutex);
        if (g_filesFrame == frame) {
            g_filesFinal = true;
            if (g_filesDone == g_filesQueued) log_capture_done(frame);
        }
    }
}

uint64_t capture_done_frame() { return g_doneFrame.load(std::memory_order_acquire); }

}  // namespace gfxdk
