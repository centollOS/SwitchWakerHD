// Page art that is not the game's: the notebook's parchment frame and its quest-row banner, painted
// for the port (user artwork, loaded at run time from files outside the repository while its licence
// is being settled: WWHD_NOTEBOOK_FRAME, WWHD_NOTEBOOK_BANNER). Pure code: the layout of the two
// pictures (where their plates, slots and medallion are, as shares of the picture), the clean-up
// of their soft edges and the stretching of a row. Unit-tested in runtime/tools/story_notebook_test.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#include "../game_glyph.h"
#include "guest_hud.h"

namespace story_notebook::art {

using game_glyph::Bitmap;

struct Rect {
    float x0, y0, x1, y1;  // shares of the picture (0..1)
};

// The frame (a torn parchment with a green border, waves along the top and the bottom): measured
// on its 1448 x 1086 picture. Its parts, as shares of the whole picture.
namespace frame {
inline constexpr float kAspect = 1448.0f / 1086.0f;
inline constexpr Rect kTitle{112 / 1448.0f, 92 / 1086.0f, 735 / 1448.0f, 160 / 1086.0f};  // the band left of the tabs
inline constexpr Rect kTab0{748 / 1448.0f, 123 / 1086.0f, 1031 / 1448.0f, 197 / 1086.0f};
inline constexpr Rect kTab1{1043 / 1448.0f, 123 / 1086.0f, 1311 / 1448.0f, 197 / 1086.0f};
inline constexpr Rect kSlot{1327 / 1448.0f, 94 / 1086.0f, 1395 / 1448.0f, 162 / 1086.0f};  // the round slot
inline constexpr Rect kContent{105 / 1448.0f, 228 / 1086.0f, 1365 / 1448.0f, 905 / 1086.0f};
inline constexpr Rect kPlateLeft{80 / 1448.0f, 935 / 1086.0f, 464 / 1448.0f, 1023 / 1086.0f};
inline constexpr Rect kPlateRight{948 / 1448.0f, 937 / 1086.0f, 1390 / 1448.0f, 1024 / 1086.0f};
}  // namespace frame

// The detail page as the user's composite lays it out (the same 1448 x 1086 space as the frame):
// the parchment sheet, the title's wave line, the three section bands, the tilted chart card, the
// header band and the two bottom plates.
namespace detail {
inline constexpr Rect kSheet{120 / 1448.0f, 250 / 1086.0f, 1385 / 1448.0f, 905 / 1086.0f};
inline constexpr Rect kTitle{160 / 1448.0f, 268 / 1086.0f, 914 / 1448.0f, 338 / 1086.0f};
inline constexpr Rect kUnderline{155 / 1448.0f, 342 / 1086.0f, 914 / 1448.0f, 371 / 1086.0f};
inline constexpr Rect kBands[3] = {{198 / 1448.0f, 527 / 1086.0f, 905 / 1448.0f, 613 / 1086.0f},
                                   {202 / 1448.0f, 667 / 1086.0f, 898 / 1448.0f, 740 / 1086.0f},
                                   {202 / 1448.0f, 793 / 1086.0f, 903 / 1448.0f, 866 / 1086.0f}};
inline constexpr Rect kCard{925 / 1448.0f, 405 / 1086.0f, 1412 / 1448.0f, 815 / 1086.0f};
inline constexpr Rect kHeader{770 / 1448.0f, 112 / 1086.0f, 1410 / 1448.0f, 268 / 1086.0f};
inline constexpr Rect kPlateLeft{91 / 1448.0f, 921 / 1086.0f, 430 / 1448.0f, 1008 / 1086.0f};
inline constexpr Rect kPlateRight{930 / 1448.0f, 921 / 1086.0f, 1393 / 1448.0f, 1008 / 1086.0f};
// the frame's tab plates and round slot, painted over for the detail page with the band's green
inline constexpr Rect kCover{745 / 1448.0f, 92 / 1086.0f, 1402 / 1448.0f, 205 / 1086.0f};
inline constexpr Rect kCoverSource{700 / 1448.0f, 92 / 1086.0f, 740 / 1448.0f, 205 / 1086.0f};
}  // namespace detail

// The chart card (its picture without the transparent margin): the corners of its blue sea, as
// shares of the card (it is drawn tilted)
namespace card {
inline constexpr float kTL[2] = {0.124f, 0.063f}, kTR[2] = {0.960f, 0.193f}, kBL[2] = {0.044f, 0.783f}, kBR[2] = {0.898f, 0.942f};
}

// The quest-row banner (a long torn parchment strip, a medallion on the left, a status pill on the
// right): measured on its picture without the transparent margin (2103 x 311 there).
namespace banner {
inline constexpr float kAspect = 2103.0f / 311.0f;
inline constexpr float kLeftEnd = 524 / 2103.0f, kRightStart = 1484 / 2103.0f;  // the plain middle, stretched
inline constexpr float kMedallionX = 199 / 2103.0f, kMedallionY = 164 / 311.0f, kMedallionR = 97 / 311.0f;
inline constexpr Rect kPill{1552 / 2103.0f, 106 / 311.0f, 1919 / 2103.0f, 201 / 311.0f};
inline constexpr Rect kPillArt{1507 / 2103.0f, 81 / 311.0f, 1964 / 2103.0f, 221 / 311.0f};  // with its swirls
inline constexpr Rect kText{340 / 2103.0f, 30 / 311.0f, 1490 / 2103.0f, 215 / 311.0f};
}  // namespace banner

// Painted art often keeps a soft, nearly transparent fringe outside its edge, which shows as a glow
// over the dark menu. Texels under `min_alpha` are cleared; the picture is cropped to what is left
// (`crop`), or keeps its size (the frame: its layout is measured on the whole picture).
inline Bitmap clean_edges(const Bitmap& in, uint8_t min_alpha = 96, bool crop = true) {
    int x0 = in.width, y0 = in.height, x1 = 0, y1 = 0;
    for (int y = 0; y < in.height; y++)
        for (int x = 0; x < in.width; x++)
            if (in.rgba[(size_t(y) * in.width + x) * 4 + 3] >= min_alpha)
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
    Bitmap out;
    if (x1 <= x0 || y1 <= y0) return out;
    if (!crop) x0 = 0, y0 = 0, x1 = in.width, y1 = in.height;
    out.width = x1 - x0, out.height = y1 - y0;
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (int y = 0; y < out.height; y++)
        for (int x = 0; x < out.width; x++) {
            const uint8_t* s = &in.rgba[(size_t(y0 + y) * in.width + x0 + x) * 4];
            uint8_t* d = &out.rgba[(size_t(y) * out.width + x) * 4];
            std::copy_n(s, 4, d);
            if (d[3] < min_alpha) d[0] = d[1] = d[2] = d[3] = 0;
        }
    return out;
}

// A picture resized (box filter when it shrinks, bilinear when it grows), straight RGBA
inline Bitmap resized(const Bitmap& in, int w, int h) {
    Bitmap out;
    if (in.width <= 0 || in.height <= 0 || w <= 0 || h <= 0) return out;
    out.width = w, out.height = h;
    out.rgba.assign(size_t(w) * h * 4, 0);
    const float sx = float(in.width) / w, sy = float(in.height) / h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            // the source texels under this pixel (at least the 2 x 2 around its centre)
            const float fx0 = x * sx, fx1 = std::max((x + 1) * sx, fx0 + 1.0f), fy0 = y * sy, fy1 = std::max((y + 1) * sy, fy0 + 1.0f);
            const int ix0 = std::clamp(int(fx0), 0, in.width - 1), ix1 = std::clamp(int(std::ceil(fx1)) - 1, ix0, in.width - 1);
            const int iy0 = std::clamp(int(fy0), 0, in.height - 1), iy1 = std::clamp(int(std::ceil(fy1)) - 1, iy0, in.height - 1);
            float acc[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = iy0; yy <= iy1; yy++)
                for (int xx = ix0; xx <= ix1; xx++) {
                    const uint8_t* s = &in.rgba[(size_t(yy) * in.width + xx) * 4];
                    const float a = s[3] / 255.0f;  // premultiplied, so transparent texels don't bleed colour
                    for (int c = 0; c < 3; c++) acc[c] += s[c] * a;
                    acc[3] += s[3], n += 1;
                }
            uint8_t* d = &out.rgba[(size_t(y) * w + x) * 4];
            if (acc[3] <= 0) continue;
            for (int c = 0; c < 3; c++) d[c] = uint8_t(std::clamp(acc[c] * 255.0f / acc[3], 0.0f, 255.0f));
            d[3] = uint8_t(std::clamp(acc[3] / n + 0.5f, 0.0f, 255.0f));
        }
    return out;
}

// The part (x0..x1, y0..y1 as shares) of a picture
inline Bitmap part(const Bitmap& in, const Rect& r) {
    Bitmap out;
    const int x0 = std::clamp(int(std::lround(r.x0 * in.width)), 0, in.width), x1 = std::clamp(int(std::lround(r.x1 * in.width)), x0, in.width);
    const int y0 = std::clamp(int(std::lround(r.y0 * in.height)), 0, in.height), y1 = std::clamp(int(std::lround(r.y1 * in.height)), y0, in.height);
    out.width = x1 - x0, out.height = y1 - y0;
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (int y = 0; y < out.height; y++)
        std::copy_n(&in.rgba[(size_t(y0 + y) * in.width + x0) * 4], size_t(out.width) * 4, &out.rgba[size_t(y) * out.width * 4]);
    return out;
}

// The banner as a row of w x h pixels: its ends keep their shape (scaled to the height), its plain
// middle stretches to the width. Narrower rows squeeze the whole banner.
inline Bitmap row(const Bitmap& banner, int w, int h) {
    if (banner.width <= 0 || w <= 0 || h <= 0) return {};
    const float k = float(h) / banner.height;
    const int left = int(std::lround(banner.width * banner::kLeftEnd * k)), right = int(std::lround(banner.width * (1 - banner::kRightStart) * k));
    if (left + right >= w) return resized(banner, w, h);
    const Bitmap l = resized(part(banner, {0, 0, banner::kLeftEnd, 1}), left, h);
    const Bitmap m = resized(part(banner, {banner::kLeftEnd, 0, banner::kRightStart, 1}), w - left - right, h);
    const Bitmap r = resized(part(banner, {banner::kRightStart, 0, 1, 1}), right, h);
    Bitmap out;
    out.width = w, out.height = h;
    out.rgba.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) {
        std::copy_n(&l.rgba[size_t(y) * l.width * 4], size_t(l.width) * 4, &out.rgba[size_t(y) * w * 4]);
        std::copy_n(&m.rgba[size_t(y) * m.width * 4], size_t(m.width) * 4, &out.rgba[(size_t(y) * w + left) * 4]);
        std::copy_n(&r.rgba[size_t(y) * r.width * 4], size_t(r.width) * 4, &out.rgba[(size_t(y) * w + left + m.width) * 4]);
    }
    return out;
}

// A picture stretched to w x h with its ends kept (left and right shares of its width, scaled to
// the height), the middle stretched: the section bands and the plates
inline Bitmap stretch3(const Bitmap& in, int w, int h, float left_share, float right_share) {
    if (in.width <= 0 || w <= 0 || h <= 0) return {};
    const float k = float(h) / in.height;
    const int left = int(std::lround(in.width * left_share * k)), right = int(std::lround(in.width * right_share * k));
    if (left + right >= w) return resized(in, w, h);
    const Bitmap l = resized(part(in, {0, 0, left_share, 1}), left, h);
    const Bitmap m = resized(part(in, {left_share, 0, 1 - right_share, 1}), w - left - right, h);
    const Bitmap r = resized(part(in, {1 - right_share, 0, 1, 1}), right, h);
    Bitmap out;
    out.width = w, out.height = h;
    out.rgba.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) {
        std::copy_n(&l.rgba[size_t(y) * l.width * 4], size_t(l.width) * 4, &out.rgba[size_t(y) * w * 4]);
        std::copy_n(&m.rgba[size_t(y) * m.width * 4], size_t(m.width) * 4, &out.rgba[(size_t(y) * w + left) * 4]);
        std::copy_n(&r.rgba[size_t(y) * r.width * 4], size_t(r.width) * 4, &out.rgba[(size_t(y) * w + left + m.width) * 4]);
    }
    return out;
}

// A picture fitted into w x h with its aspect kept (transparent around it), pushed to the right
// and the bottom
inline Bitmap fit_into(const Bitmap& in, int w, int h) {
    if (in.width <= 0 || w <= 0 || h <= 0) return {};
    const float k = std::min(float(w) / in.width, float(h) / in.height);
    const Bitmap r = resized(in, std::max(1, int(std::lround(in.width * k))), std::max(1, int(std::lround(in.height * k))));
    Bitmap out;
    out.width = w, out.height = h;
    out.rgba.assign(size_t(w) * h * 4, 0);
    const int ox = w - r.width, oy = h - r.height;
    for (int y = 0; y < r.height; y++) std::copy_n(&r.rgba[size_t(y) * r.width * 4], size_t(r.width) * 4, &out.rgba[(size_t(oy + y) * w + ox) * 4]);
    return out;
}

// The frame for the detail page: its tab plates and round slot painted over with the band's own
// green (a plain strip of it, stretched), where the header band goes
inline Bitmap detail_frame(const Bitmap& frame) {
    Bitmap out = frame;
    if (frame.width <= 0) return out;
    const Bitmap src = part(frame, detail::kCoverSource);
    const int x0 = int(std::lround(detail::kCover.x0 * frame.width)), x1 = int(std::lround(detail::kCover.x1 * frame.width));
    const int y0 = int(std::lround(detail::kCover.y0 * frame.height)), y1 = int(std::lround(detail::kCover.y1 * frame.height));
    const Bitmap fill = resized(src, x1 - x0, y1 - y0);
    for (int y = 0; y < fill.height; y++)
        for (int x = 0; x < fill.width; x++) {
            const uint8_t* s = &fill.rgba[(size_t(y) * fill.width + x) * 4];
            if (s[3] < 128) continue;
            std::copy_n(s, 4, &out.rgba[(size_t(y0 + y) * out.width + x0 + x) * 4]);
        }
    return out;
}

// Where a part of a row (as shares of the banner) lands in a row of w x h at (x, y)
inline float row_x(float share, float x, float w, float h) {
    const float k = h / 311.0f * 2103.0f;  // the banner's width at this height
    const float left = banner::kLeftEnd * k, right = (1 - banner::kRightStart) * k;
    if (left + right >= w) return x + share * w;
    if (share <= banner::kLeftEnd) return x + share * k;
    if (share >= banner::kRightStart) return x + w - (1 - share) * k;
    return x + left + (share - banner::kLeftEnd) / (banner::kRightStart - banner::kLeftEnd) * (w - left - right);
}

inline std::shared_ptr<guestmods::hud::Image> image(const Bitmap& b) {
    if (b.width <= 0) return nullptr;
    auto img = std::make_shared<guestmods::hud::Image>();
    img->width = uint32_t(b.width), img->height = uint32_t(b.height), img->rgba = b.rgba;
    return img;
}

}  // namespace story_notebook::art
