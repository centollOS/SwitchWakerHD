// Round-trip check for the layout texture writer: reads a tiled surface with the port's own address
// library (the vendored Cemu LatteAddrLib, the same calls as runtime/src/gfx/vulkan/surfaces.cpp's
// guest upload) and writes the elements back in linear row-major order.
//   gx2_detile GX2FORMAT WIDTH HEIGHT TILEMODE SWIZZLE IN OUT
// prints: pitch height surface-size base-alignment
// Built and run by tools/story_mods/test_lyt.py against a build's libcemu_latte.a.
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 8) { fprintf(stderr, "usage: gx2_detile FORMAT W H TILEMODE SWIZZLE IN OUT\n"); return 2; }
    auto format = (Latte::E_GX2SURFFMT)strtoul(argv[1], nullptr, 0);
    uint32 w = strtoul(argv[2], nullptr, 0), h = strtoul(argv[3], nullptr, 0);
    auto gx2tm = (Latte::E_GX2TILEMODE)strtoul(argv[4], nullptr, 0);
    uint32 swizzle = strtoul(argv[5], nullptr, 0);
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    LatteAddrLib::GX2CalculateSurfaceInfo(format, w, h, 1, Latte::E_DIM::DIM_2D, gx2tm, 0, 0, &info);
    printf("%u %u %u %u\n", info.pitch, info.height, (uint32)info.surfSize, info.baseAlign);
    FILE* f = fopen(argv[6], "rb");
    if (!f) return 1;
    std::vector<uint8_t> src(info.surfSize);
    if (fread(src.data(), 1, src.size(), f) != src.size()) return 1;
    fclose(f);
    bool bc = Latte::IsCompressedFormat(format);
    uint32 bw = bc ? (w + 3) / 4 : w, bh = bc ? (h + 3) / 4 : h;
    uint32 bpp = info.bpp, bytes = bpp / 8;
    auto tm = info.hwTileMode;
    LatteAddrLib::CachedSurfaceAddrInfo ci;
    bool macro = Latte::TM_IsMacroTiled(tm);
    if (macro) LatteAddrLib::SetupCachedSurfaceAddrInfo(&ci, 0, 0, bpp, info.pitch, info.height, 1, 1, tm, false, (swizzle >> 8) & 1, (swizzle >> 9) & 3);
    std::vector<uint8_t> out((size_t)bw * bh * bytes);
    for (uint32 y = 0; y < bh; y++)
        for (uint32 x = 0; x < bw; x++) {
            uint32 off;
            if (tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
                off = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, 0, 0, bpp, info.pitch, info.height, 1);
            else if (!macro)
                off = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, 0, bpp, info.pitch, info.height, tm, false);
            else
                off = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, &ci);
            memcpy(&out[((size_t)y * bw + x) * bytes], &src[off], bytes);
        }
    f = fopen(argv[7], "wb");
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return 0;
}
