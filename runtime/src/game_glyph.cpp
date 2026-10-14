// Glyphs and textures from the game's memory, decoded on the CPU (game_glyph.h). The texel
// addressing follows button_glyph_font.cpp (the HUD shoulder captions), generalised to RGBA sheets
// and whole textures.
#include "game_glyph.h"
#include "runtime.h"
#include "gx2_texture_regs.h"
#include "gfx/vulkan/bc_reference.h"  // backend-independent CPU BC decoder

#include <algorithm>
#include <array>
#include <cstring>

namespace game_glyph {
namespace {

// One mip-0 slice of a GX2 surface: texel fetches in logical pixels, after the texture's
// component selection.
class Surface {
public:
    bool open(uint32_t texture_addr, uint32_t slice_index) {
        texture_ = (const GX2::GX2Texture*)mem::ptr(texture_addr);
        const auto& s = texture_->surface;
        width_ = s.width, height_ = s.height;
        const uint32_t depth = s.depth;
        if (!width_ || !height_ || width_ > 4096 || height_ > 4096 || !depth || depth > 64 || !s.imagePtr) return false;
        slice_ = slice_index;
        if (slice_ >= depth) return false;
        format_ = (uint32_t)s.format.value() & 0x3F;
        bc_ = format_ >= 0x31 && format_ <= 0x35 ? int(format_ - 0x30) : 0;
        bytes_ = bc_ ? gfxvk::bc::block_bytes(bc_) : format_ == 1 || format_ == 2 ? 1 : format_ == 5 || format_ == 7 || format_ == 11 ? 2
                                                                                     : format_ == 0x1A ? 4 : 0;
        if (!bytes_) return false;
        LatteAddrLib::AddrSurfaceInfo_OUT geometry{};
        LatteAddrLib::GX2CalculateSurfaceInfo(s.format, width_, height_, depth, s.dim, s.tileMode, s.aa, 0, &geometry);
        tile_ = geometry.hwTileMode;
        pitch_ = s.pitch ? (uint32_t)s.pitch : geometry.pitch;
        geometry_height_ = geometry.height, geometry_depth_ = geometry.depth;
        image_ = s.imagePtr, image_size_ = s.imageSize;
        if (Latte::TM_IsMacroTiled(tile_))
            LatteAddrLib::SetupCachedSurfaceAddrInfo(&cached_, slice_, 0, bytes_ * 8, pitch_, geometry.height, geometry.depth, 1, tile_, false,
                                                     (s.swizzle >> 8) & 1, (s.swizzle >> 9) & 3);
        select_ = texture_->compSel;
        return true;
    }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t format() const { return format_; }
    uint32_t select() const { return select_; }
    // the raw channels of a texel (R, G, B, A before the component selection); false: out of the image
    bool raw(uint32_t px, uint32_t py, std::array<uint8_t, 4>& channels) const {
        const uint32_t ex = bc_ ? px / 4 : px, ey = bc_ ? py / 4 : py;
        uint32_t offset;
        if (tile_ == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tile_ == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
            offset = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(ex, ey, slice_, 0, bytes_ * 8, pitch_, geometry_height_, geometry_depth_);
        else if (!Latte::TM_IsMacroTiled(tile_))
            offset = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(ex, ey, slice_, bytes_ * 8, pitch_, geometry_height_, tile_, false);
        else
            offset = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(ex, ey, &cached_);
        if (uint64_t(offset) + bytes_ > image_size_) return false;
        const uint8_t* p = mem::ptr(image_) + offset;
        channels = {0, 0, 0, 255};
        uint16_t packed = 0;
        if (bytes_ == 2 && !bc_) std::memcpy(&packed, p, 2);
        if (bc_) channels = gfxvk::bc::pixel(p, unsigned(bc_), false, (py % 4) * 4 + px % 4);
        else if (format_ == 1) channels[0] = p[0];
        else if (format_ == 2) channels[0] = (p[0] >> 4) * 17, channels[1] = (p[0] & 15) * 17;
        else if (format_ == 5) channels[0] = packed >> 8;
        else if (format_ == 7) channels[0] = p[0], channels[1] = p[1];
        else if (format_ == 11) for (int i = 0; i < 4; i++) channels[i] = ((packed >> (i * 4)) & 15) * 17;
        else std::copy(p, p + 4, channels.begin());
        return true;
    }
    // output component c (0 R .. 3 A) of a texel through the component selection
    static uint8_t selected(const std::array<uint8_t, 4>& channels, uint32_t select, int c) {
        const uint32_t m = (select >> (24 - 8 * c)) & 255;
        return m < 4 ? channels[m] : m == 5 ? 255 : 0;
    }

private:
    const GX2::GX2Texture* texture_ = nullptr;
    uint32_t width_ = 0, height_ = 0, slice_ = 0, format_ = 0, bytes_ = 0, pitch_ = 0, geometry_height_ = 0, geometry_depth_ = 0;
    uint32_t image_ = 0, image_size_ = 0, select_ = 0;
    int bc_ = 0;
    Latte::E_HWTILEMODE tile_{};
    mutable LatteAddrLib::CachedSurfaceAddrInfo cached_{};
};

}  // namespace

bool read_glyph(uint32_t record, Glyph& out) {
    const uint32_t sheet = ld32(record + 0x14);
    if (!sheet) return false;
    Surface surface;
    const auto& texture = *(const GX2::GX2Texture*)mem::ptr(sheet + 0x10);
    if (!surface.open(sheet + 0x10, ld8(record + 0x13) + (uint32_t)texture.viewFirstSlice)) return false;
    const uint32_t width = surface.width(), height = surface.height();
    const uint32_t logical_w = ld16(record + 8), logical_h = ld16(record + 10);
    if (!logical_w || !logical_h || logical_w % width || logical_h % height) return false;
    const uint32_t rx = logical_w / width, ry = logical_h / height;
    if (!rx || !ry || rx * ry > 4) return false;
    const int gw = ld8(record + 5), gh = ld8(record + 7);
    const uint32_t x0 = ld16(record + 12), cell_y = ld16(record + 14);
    if (!gh || x0 + gw > logical_w || cell_y + gh > logical_h) return false;
    out = {};
    out.left = int(int8_t(ld8(record + 4)));
    out.advance = ld8(record + 6);
    if (!gw) {  // a space: only its advance
        out.bitmap.height = gh;
        return true;
    }
    // sheet byte +0x0E: 1 a coverage sheet (an alpha font coloured by the text), 0 an RGBA picture sheet
    out.colour = ld8(sheet + 0x0E) == 0 && rx * ry == 1 && surface.format() != 1;
    out.bitmap.width = gw, out.bitmap.height = gh;
    out.bitmap.rgba.assign(size_t(gw) * gh * 4, 0);
    // The game's CharWriter converts cell Y to 1-(Y+height)/sheetHeight for the top vertex: the
    // sheet is bottom-up; the bitmap is top-down.
    const uint32_t y0 = logical_h - cell_y - gh;
    const uint32_t select = surface.select();
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++) {
            const uint32_t lx = x0 + x, ly = y0 + y;
            std::array<uint8_t, 4> channels;
            if (!surface.raw(lx / rx, ly / ry, channels)) return false;
            uint8_t* p = &out.bitmap.rgba[(size_t(gh - 1 - y) * gw + x) * 4];
            if (out.colour) {
                for (int c = 0; c < 4; c++) p[c] = Surface::selected(channels, select, c);
                continue;
            }
            const uint32_t component = rx * ry > 1 ? (ly % ry) * rx + (lx % rx) : 3;
            uint32_t mapped = (select >> (24 - 8 * component)) & 255;
            // single-channel alpha fonts keep coverage in R; packed sheets show several logical
            // pixels through the sampler's component mapping (as button_glyph_font.cpp)
            if (rx * ry == 1 && surface.format() == 1) mapped = 0;
            if (rx * ry == 1 && surface.format() == 7 && mapped > 1) mapped = 1;
            p[0] = p[1] = p[2] = 255;
            p[3] = mapped < 4 ? channels[mapped] : mapped == 5 ? 255 : 0;
        }
    return true;
}

bool read_texture(uint32_t texture, Bitmap& out) {
    Surface surface;
    if (!texture || !surface.open(texture, 0)) return false;
    out.width = int(surface.width()), out.height = int(surface.height());
    out.rgba.assign(size_t(out.width) * out.height * 4, 0);
    const uint32_t select = surface.select();
    for (uint32_t y = 0; y < surface.height(); y++)
        for (uint32_t x = 0; x < surface.width(); x++) {
            std::array<uint8_t, 4> channels;
            if (!surface.raw(x, y, channels)) return false;
            uint8_t* p = &out.rgba[(size_t(y) * out.width + x) * 4];
            for (int c = 0; c < 4; c++) p[c] = Surface::selected(channels, select, c);
        }
    return true;
}

}  // namespace game_glyph
