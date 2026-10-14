// The story notebook page's boxes (the style markers of story_notebook_page.h) drawn with the game's
// own menu art, or with plain shapes when that art can't be read. Pure code; the art comes from the
// caller (story_notebook_game.cpp reads it from the item menu's layout at run time: nothing of the
// game is stored by the port); unit-tested in runtime/tools/story_notebook_test.cpp.
//
// The art (all from the item menu's layout, which is open whenever the notebook is):
//   - the torn-paper window of the menu's background (W_BaseEdgeShadow_00: its eight frame pieces,
//     the white torn edge with its shadow; W_Base_00: the paper fibre of its content and its colour),
//   - the menu's button plate (L_MenuSave_00: P_BtnShadow_00 the plate's light rim, P_Btn_00 the
//     plate with its cloth hatch and its gloss).
// The page's boxes are composed from these pieces (nine-slice / three-slice, as the game's window and
// picture panes stretch them) and coloured like the item menu's mini windows (the green "Would you
// like to save?" window), since the notebook is a window of its own over the menu.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../game_glyph.h"
#include "guest_hud.h"
#include "story_notebook_art.h"

namespace story_notebook::style {

using guestmods::hud::Command;
using game_glyph::Bitmap;

// The game's art the boxes are made of (empty bitmaps: not read)
struct Art {
    Bitmap edge[8];        // the window frame pieces: left-top, right-top, left-bottom, right-bottom, left, right, top, bottom
    Bitmap fibre;          // the window content's paper (luminance)
    uint32_t paper = 0;    // the menu window's paper colour (RGBA), as the game draws its background
    Bitmap plate_rim;      // the button plate's rim (luminance + alpha)
    Bitmap plate;          // the button plate with its cloth (luminance + alpha)
    Bitmap gloss;          // the plate's gloss (RGBA)
    Bitmap info_top, info_bottom;  // the item menu's Info box (W_TextBase_00): its torn top and bottom edges
    Bitmap swirl;          // its wind swirls (P_Deco_00)
    // the sea chart's own icons (the MAP screen's layout, once it was shown this session)
    Bitmap link_face, ship;
    // the page's painted frame and quest-row banner (user artwork, story_notebook_art.h), cleaned
    Bitmap frame, banner;
    Bitmap detail_frame;                       // the frame with the header band's place cleared
    Bitmap sheet, card, header, underline, uplate, bands[3];  // the detail page's pieces (story_notebook_art.h)
    bool window() const {
        for (const Bitmap& b : edge)
            if (b.width <= 0) return false;
        return fibre.width > 0;
    }
    bool plates() const { return plate_rim.width > 0 && plate.width > 0; }
    bool info() const { return info_top.width > 0 && info_bottom.width > 0 && swirl.width > 0; }
    bool framed() const { return frame.width > 0 && banner.width > 0; }
};

// The part of a bitmap inside (x0, y0)-(x1, y1)
inline Bitmap crop(const Bitmap& b, int x0, int y0, int x1, int y1) {
    Bitmap out;
    x0 = std::clamp(x0, 0, b.width), x1 = std::clamp(x1, x0, b.width), y0 = std::clamp(y0, 0, b.height), y1 = std::clamp(y1, y0, b.height);
    out.width = x1 - x0, out.height = y1 - y0;
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (int y = 0; y < out.height; y++)
        std::copy_n(&b.rgba[(size_t(y0 + y) * b.width + x0) * 4], size_t(out.width) * 4, &out.rgba[size_t(y) * out.width * 4]);
    return out;
}
// The plate pictures without their margins (the plate body's extent), so a plate fills its box
inline void trim_plates(Art& art) {
    if (!art.plates() || art.plate.width != art.plate_rim.width || art.plate.height != art.plate_rim.height) return;
    int x0 = art.plate_rim.width, y0 = art.plate_rim.height, x1 = 0, y1 = 0;
    for (int y = 0; y < art.plate.height; y++)  // the plate's own body (the rim's soft shadow reaches further)
        for (int x = 0; x < art.plate.width; x++)
            if (art.plate.rgba[(size_t(y) * art.plate.width + x) * 4 + 3] > 128)
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
    if (x1 - x0 < 16 || y1 - y0 < 8) return;
    art.plate_rim = crop(art.plate_rim, x0, y0, x1, y1);
    art.plate = crop(art.plate, x0, y0, x1, y1);
    if (art.gloss.width >= x1 && art.gloss.height >= y1) art.gloss = crop(art.gloss, x0, y0, x1, y1);
}

// colours of the notebook's window (the mini window's greens)
inline constexpr uint32_t kWindowPaper = 0xB4DC8CFF, kPanelPaper = 0xE8F6C6FF, kPlateDark = 0x2F7A16FF, kPlateLight = 0xB8EC8EFF,
                          kPlateOnDark = 0x6DB443FF, kPlateOnLight = 0xEAFFCCFF, kTabDark = 0x5E8F45FF, kTabLight = 0xCBE6AEFF,
                          kChartPaper = 0xE9D9A8FF, kUnderline = 0x2C7FB8FF,
                          kInfoPaper = 0xEDEBBCFF, kInfoEdge = 0xA3B2A3FF, kInfoSwirl = 0xE0DDA6FF,
                          kRimTop = 0xF4FFE4FF, kRimBottom = 0xCFE6B4FF, kCursorInk = 0x5A3A1CFF, kCursorFill = 0xFFFFFFFF;

inline uint8_t ch(uint32_t rgba, int i) { return uint8_t(rgba >> (24 - 8 * i)); }
inline float lum(const Bitmap& b, int x, int y) { return b.rgba[(size_t(y) * b.width + x) * 4] / 255.0f; }

// a bitmap sampled bilinearly at (u, v) in its own pixels
inline void sample(const Bitmap& b, float u, float v, float out[4]) {
    u = std::clamp(u - 0.5f, 0.0f, float(b.width - 1)), v = std::clamp(v - 0.5f, 0.0f, float(b.height - 1));
    const int x0 = int(u), y0 = int(v), x1 = std::min(x0 + 1, b.width - 1), y1 = std::min(y0 + 1, b.height - 1);
    const float fx = u - x0, fy = v - y0;
    for (int c = 0; c < 4; c++) {
        auto p = [&](int x, int y) { return float(b.rgba[(size_t(y) * b.width + x) * 4 + c]); };
        out[c] = (p(x0, y0) * (1 - fx) + p(x1, y0) * fx) * (1 - fy) + (p(x0, y1) * (1 - fx) + p(x1, y1) * fx) * fy;
    }
}

struct Canvas {
    int w = 0, h = 0;
    std::vector<float> px;  // straight RGBA, 0..255
    Canvas(int width, int height) : w(width), h(height), px(size_t(width) * height * 4, 0.0f) {}
    float* at(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
    // source-over of a straight colour
    void over(int x, int y, const float c[4]) {
        if (x < 0 || y < 0 || x >= w || y >= h || c[3] <= 0) return;
        float* d = at(x, y);
        const float sa = c[3] / 255.0f, da = d[3] / 255.0f, oa = sa + da * (1 - sa);
        if (oa <= 0) return;
        for (int i = 0; i < 3; i++) d[i] = (c[i] * sa + d[i] * da * (1 - sa)) / oa;
        d[3] = oa * 255.0f;
    }
    std::shared_ptr<guestmods::hud::Image> image() const {
        auto img = std::make_shared<guestmods::hud::Image>();
        img->width = uint32_t(w), img->height = uint32_t(h);
        img->rgba.resize(size_t(w) * h * 4);
        for (size_t i = 0; i < img->rgba.size(); i++) img->rgba[i] = uint8_t(std::clamp(px[i] + 0.5f, 0.0f, 255.0f));
        return img;
    }
};

// One piece of the frame drawn into the canvas at (x, y, w, h), its texels stretched (corners keep
// their size) or repeated along an edge (`tile_x` / `tile_y`), as the game's window frames are.
inline void piece(Canvas& cv, const Bitmap& b, int x, int y, int w, int h, bool tile_x, bool tile_y, float scale) {
    for (int ty = 0; ty < h; ty++)
        for (int tx = 0; tx < w; tx++) {
            float u = tile_x ? std::fmod(tx / scale, float(b.width)) : (tx + 0.5f) * b.width / w;
            float v = tile_y ? std::fmod(ty / scale, float(b.height)) : (ty + 0.5f) * b.height / h;
            float c[4];
            sample(b, u, v, c);
            cv.over(x + tx, y + ty, c);
        }
}

// The game's torn-paper window at w x h (page pixels): the paper (fibre in the given colour) inside
// the frame's torn edge, the edge's white line and its shadow over it. `corner`: the frame pieces'
// size on the page.
inline std::shared_ptr<guestmods::hud::Image> torn_paper(const Art& art, int w, int h, uint32_t colour, float corner = 40.0f) {
    w = std::clamp(w, 8, 2048), h = std::clamp(h, 8, 2048);
    const int c = std::min({int(corner), w / 3, h / 3});
    const float scale = float(c) / float(art.edge[0].width);
    Canvas edge(w, h);
    piece(edge, art.edge[0], 0, 0, c, c, false, false, scale);
    piece(edge, art.edge[1], w - c, 0, c, c, false, false, scale);
    piece(edge, art.edge[2], 0, h - c, c, c, false, false, scale);
    piece(edge, art.edge[3], w - c, h - c, c, c, false, false, scale);
    piece(edge, art.edge[4], 0, c, c, h - 2 * c, false, true, scale);
    piece(edge, art.edge[5], w - c, c, c, h - 2 * c, false, true, scale);
    piece(edge, art.edge[6], c, 0, w - 2 * c, c, true, false, scale);
    piece(edge, art.edge[7], c, h - c, w - 2 * c, c, true, false, scale);
    // the paper: everything the edge's line encloses (filled from the middle)
    std::vector<uint8_t> inside(size_t(w) * h, 0);
    std::vector<int> todo = {(h / 2) * w + w / 2};
    bool leak = false;
    while (!todo.empty()) {
        const int i = todo.back();
        todo.pop_back();
        if (inside[size_t(i)]) continue;
        const int x = i % w, y = i / w;
        if (edge.px[size_t(i) * 4 + 3] >= 110) continue;  // the line
        inside[size_t(i)] = 1;
        if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
            leak = true;
            break;
        }
        todo.push_back(i - 1), todo.push_back(i + 1), todo.push_back(i - w), todo.push_back(i + w);
    }
    if (leak) {  // the line has a gap at this size: the paper up to the line's middle
        const int in = std::max(2, int(c * 0.3f));
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) inside[size_t(y) * w + x] = x >= in && y >= in && x < w - in && y < h - in;
    }
    Canvas out(w, h);
    const float fs = std::max(0.5f, scale * 1.4f);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (!inside[size_t(y) * w + x]) continue;
            float f[4];
            sample(art.fibre, std::fmod(x / fs, float(art.fibre.width)), std::fmod(y / fs, float(art.fibre.height)), f);
            const float k = 0.82f + 0.18f * (f[0] / 255.0f);  // the fibre, lightly
            const float p[4] = {ch(colour, 0) * k, ch(colour, 1) * k, ch(colour, 2) * k, 255.0f};
            out.over(x, y, p);
        }
    for (size_t i = 0; i < size_t(w) * h; i++) out.over(int(i % w), int(i / w), &edge.px[i * 4]);
    return out.image();
}

// The game's button plate at w x h: its rim, the plate's cloth coloured from `dark` (the texture's
// black) to `light` (its white), lighter to the top, and its gloss.
inline std::shared_ptr<guestmods::hud::Image> plate(const Art& art, int w, int h, uint32_t dark, uint32_t light) {
    w = std::clamp(w, 8, 2048), h = std::clamp(h, 8, 2048);
    Canvas out(w, h);
    // three-slice: the plate's ends keep their shape, its middle stretches
    auto layer = [&](const Bitmap& b, auto&& colour) {
        const float sy = float(b.height) / h, cap = std::min(float(w) / 2, b.width * 0.18f / sy), cap_src = b.width * 0.18f;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float u;
                if (x < cap) u = x * cap_src / cap;
                else if (x >= w - cap) u = b.width - (w - x) * cap_src / cap;
                else u = cap_src + (x - cap) * (b.width - 2 * cap_src) / std::max(1.0f, w - 2 * cap);
                float s[4];
                sample(b, u + 0.5f, (y + 0.5f) * sy, s);
                float c[4];
                colour(s, float(y) / h, c);
                out.over(x, y, c);
            }
    };
    layer(art.plate_rim, [&](const float s[4], float t, float c[4]) {
        for (int i = 0; i < 3; i++) c[i] = s[0] / 255.0f * (ch(kRimTop, i) * (1 - t) + ch(kRimBottom, i) * t);
        c[3] = s[3];
    });
    layer(art.plate, [&](const float s[4], float t, float c[4]) {
        const float l = s[0] / 255.0f, shade = 1.0f - 0.18f * t;
        for (int i = 0; i < 3; i++) c[i] = (ch(dark, i) * (1 - l) + ch(light, i) * l) * (l > 0.2f ? shade : 1.0f);
        c[3] = s[3];
    });
    if (art.gloss.width > 0)
        layer(art.gloss, [&](const float s[4], float, float c[4]) {
            c[0] = c[1] = c[2] = 255.0f;
            c[3] = s[3] * (s[0] / 255.0f) * 0.45f;
        });
    return out.image();
}

// The item menu's Info box (what A shows for an item on the TV) at w x h: the cream paper with the
// game's wind swirls, its torn top and bottom edges
inline std::shared_ptr<guestmods::hud::Image> info_box(const Art& art, int w, int h) {
    w = std::clamp(w, 16, 2048), h = std::clamp(h, 16, 2048);
    Canvas out(w, h);
    const int edge = std::max(4, h / 40);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const bool rim = x < edge || x >= w - edge || y < edge || y >= h - edge;
            float c[4] = {float(ch(rim ? kInfoEdge : kInfoPaper, 0)), float(ch(rim ? kInfoEdge : kInfoPaper, 1)),
                          float(ch(rim ? kInfoEdge : kInfoPaper, 2)), 255.0f};
            out.over(x, y, c);
            if (rim) continue;
            float sw[4];  // the swirls, at the size the game draws them (its 348 x 144 picture over 740 x 260)
            sample(art.swirl, std::fmod(x * 0.47f, float(art.swirl.width)), std::fmod(y * 0.55f, float(art.swirl.height)), sw);
            const float a = sw[3] / 255.0f * 0.7f;
            const float s2[4] = {float(ch(kInfoSwirl, 0)), float(ch(kInfoSwirl, 1)), float(ch(kInfoSwirl, 2)), 255.0f * a};
            out.over(x, y, s2);
        }
    // the torn edges along the top and the bottom
    auto strip = [&](const Bitmap& b, int y0) {
        const int sh = std::max(6, h / 16);
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < w; x++) {
                float t[4];
                sample(b, (x + 0.5f) * b.width / w, (y + 0.5f) * b.height / sh, t);
                const float l = t[0] / 255.0f;
                const float c[4] = {ch(kInfoEdge, 0) + (ch(kInfoPaper, 0) - ch(kInfoEdge, 0)) * l, ch(kInfoEdge, 1) + (ch(kInfoPaper, 1) - ch(kInfoEdge, 1)) * l,
                                    ch(kInfoEdge, 2) + (ch(kInfoPaper, 2) - ch(kInfoEdge, 2)) * l, t[3]};
                out.over(x, y0 + y, c);
            }
    };
    strip(art.info_top, 0);
    strip(art.info_bottom, h - std::max(6, h / 16));
    return out.image();
}

// The wind swirls as a wavy line w x h (the quest title's underline), in a colour
inline std::shared_ptr<guestmods::hud::Image> swirl_line(const Art& art, int w, int h, uint32_t colour) {
    w = std::clamp(w, 8, 2048), h = std::clamp(h, 4, 512);
    Canvas out(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float s[4];  // the upper row of the swirls, repeated along the line
            sample(art.swirl, std::fmod(x * float(art.swirl.height) * 0.5f / h, float(art.swirl.width)), (y + 0.5f) * art.swirl.height * 0.5f / h, s);
            const float edge = std::min({1.0f, x / 24.0f, (w - x) / 24.0f});  // fading at both ends
            const float c[4] = {float(ch(colour, 0)), float(ch(colour, 1)), float(ch(colour, 2)), s[3] * edge};
            out.over(x, y, c);
        }
    return out.image();
}

// The box a marker names, as the game's art (null when there is no art for it)
inline std::shared_ptr<guestmods::hud::Image> compose(const Art& art, const std::string& name, int w, int h) {
    if (name == "nb:frame" && art.framed()) return art::image(art::resized(art.frame, w, h));
    if (name == "nb:dframe" && art.detail_frame.width > 0) return art::image(art::resized(art.detail_frame, w, h));
    if (name == "nb:sheet" && art.sheet.width > 0) return art::image(art::stretch3(art.sheet, w, h, 0.15f, 0.15f));
    if (name == "nb:card" && art.card.width > 0) return art::image(art::resized(art.card, w, h));
    if (name == "nb:header" && art.header.width > 0) return art::image(art::fit_into(art.header, w, h));
    if (name == "nb:uline" && art.underline.width > 0) return art::image(art::stretch3(art.underline, w, h, 0.05f, 0.05f));
    if (name == "nb:uplate" && art.uplate.width > 0) return art::image(art::stretch3(art.uplate, w, h, 0.12f, 0.12f));
    for (int i = 0; i < 3; i++)
        if (name == "nb:band" + std::to_string(i) && art.bands[i].width > 0) return art::image(art::stretch3(art.bands[i], w, h, 0.04f, 0.04f));
    if (name == "nb:row" && art.framed()) return art::image(art::row(art.banner, w, h));
    if (name == "nb:pill" && art.framed()) return art::image(art::resized(art::part(art.banner, art::banner::kPill), w, h));
    if (name == "nb:chart" && art.window()) return torn_paper(art, w, h, kChartPaper, 26);
    if (name == "nb:underline" && art.info()) return swirl_line(art, w, h, kUnderline);
    if (name == "nb:linkface" && art.link_face.width > 0) return art::image(art::resized(art.link_face, w, h));
    if (name == "nb:ship" && art.ship.width > 0) return art::image(art::resized(art.ship, w, h));
    if (name == "nb:window" && art.window()) return torn_paper(art, w, h, kWindowPaper, 46);
    if (name == "nb:panel" && art.window()) return torn_paper(art, w, h, kPanelPaper, 34);
    if (name == "nb:info" && art.info()) return info_box(art, w, h);
    if (name == "nb:plate" && art.plates()) return plate(art, w, h, kPlateDark, kPlateLight);
    if (name == "nb:plate-on" && art.plates()) return plate(art, w, h, kPlateOnDark, kPlateOnLight);
    if (name == "nb:tab" && art.plates()) return plate(art, w, h, kTabDark, kTabLight);
    if (name == "nb:tab-on" && art.plates()) return plate(art, w, h, kPlateOnDark, kPlateOnLight);
    return nullptr;
}
// whether compose() has art for a box name
inline bool has_art(const Art& art, const std::string& n) {
    if (n == "nb:frame" || n == "nb:row" || n == "nb:pill") return art.framed();
    if (n == "nb:dframe") return art.detail_frame.width > 0;
    if (n == "nb:sheet") return art.sheet.width > 0;
    if (n == "nb:card") return art.card.width > 0;
    if (n == "nb:header") return art.header.width > 0;
    if (n == "nb:uline") return art.underline.width > 0;
    if (n == "nb:uplate") return art.uplate.width > 0;
    if (n.rfind("nb:band", 0) == 0 && n.size() == 8 && n[7] >= '0' && n[7] <= '2') return art.bands[n[7] - '0'].width > 0;
    if (n == "nb:window" || n == "nb:panel" || n == "nb:chart") return art.window();
    if (n == "nb:info" || n == "nb:underline") return art.info();
    if (n == "nb:linkface") return art.link_face.width > 0;
    if (n == "nb:ship") return art.ship.width > 0;
    if (n.rfind("nb:plate", 0) == 0 || n.rfind("nb:tab", 0) == 0) return art.plates();
    return false;
}

inline bool marker(const Command& c) { return c.kind == Command::Picture && !c.image && c.text.rfind("nb:", 0) == 0; }
// a marker's box name ("nb:plate") and the size it was laid out at
inline std::string box_name(const std::string& text) { return text.substr(0, text.find('@')); }
inline void box_size(const std::string& text, int& w, int& h) {
    w = h = 0;
    const size_t at = text.find('@');
    if (at != std::string::npos) std::sscanf(text.c_str() + at + 1, "%dx%d", &w, &h);
}

// The plain shapes of a box (no art, or the cursor, which is always drawn by the port: four
// corner brackets as the game's menu cursor shows them)
inline void shapes(const Command& m, std::vector<Command>& out) {
    const uint32_t a = m.rgba & 0xFF;
    auto rect = [&](float x, float y, float w, float h, uint32_t rgba, bool outline = false, float t = 1) {
        Command c;
        c.kind = outline ? Command::RectOutline : Command::Rect, c.x = x, c.y = y, c.w = std::max(0.0f, w), c.h = std::max(0.0f, h);
        c.rgba = (rgba & 0xFFFFFF00u) | ((rgba & 0xFF) * a / 255), c.thickness = t, c.anchor = m.anchor;
        out.push_back(c);
    };
    const float x = m.x, y = m.y, w = m.w, h = m.h;
    const std::string n = box_name(m.text);
    if (n == "nb:window") {
        rect(x + 8, y + 10, w, h, 0x00000060);
        rect(x, y, w, h, 0xF4FFE4FF);
        rect(x + 5, y + 5, w - 10, h - 10, kWindowPaper);
        rect(x + 5, y + 5, w - 10, h - 10, 0x7FB55AFF, true, 2);
    } else if (n == "nb:frame") {
        rect(x + 8, y + 10, w, h, 0x00000060);
        rect(x, y, w, h, 0xF4E9C8FF);
        rect(x + 10, y + 10, w - 20, h - 20, 0x6DB443FF, true, 6);
    } else if (n == "nb:row") {
        rect(x, y + h * 0.1f, w, h * 0.8f, 0xF2EBC9FF);
        rect(x + 4, y + h * 0.1f + 4, w - 8, h * 0.8f - 8, 0x3E9420FF, true, 3);
    } else if (n == "nb:pill") {
        rect(x + w * 0.1f, y + h * 0.18f, w * 0.8f, h * 0.64f, 0x2C7FD0FF);
    } else if (n == "nb:chart") {
        rect(x, y, w, h, kChartPaper);
        rect(x, y, w, h, 0xA88B55FF, true, 2);
    } else if (n == "nb:linkface") {
        Command c;
        c.kind = Command::Circle, c.x = x + w / 2, c.y = y + h / 2, c.size = std::min(w, h) * 0.32f, c.rgba = 0x2E8A2EFF & (0xFFFFFF00u | a), c.anchor = m.anchor;
        out.push_back(c);
    } else if (n == "nb:info") {
        rect(x, y, w, h, kInfoEdge);
        rect(x + 5, y + 5, w - 10, h - 10, kInfoPaper);
    } else if (n == "nb:panel") {
        rect(x, y, w, h, kPanelPaper);
        rect(x, y, w, h, 0xFFFFFFFF, true, 3);
    } else if (n == "nb:plate" || n == "nb:plate-on" || n == "nb:tab" || n == "nb:tab-on") {
        const bool on = n == "nb:plate-on" || n == "nb:tab-on", tab = n == "nb:tab";
        rect(x + 2, y + 4, w, h, 0x00000040);
        rect(x, y, w, h, 0xF4FFE4FF);
        rect(x + 4, y + 4, w - 8, h - 8, on ? kPlateOnLight : tab ? kTabLight : kPlateLight);
        rect(x + 4, y + 4 + (h - 8) * 0.55f, w - 8, (h - 8) * 0.45f, on ? 0xB4EC86FF : tab ? 0xB2D394FF : 0x9AD870FF);
        rect(x + 4, y + 4, w - 8, h - 8, on ? kPlateOnDark : tab ? kTabDark : kPlateDark, true, 2);
    } else if (n == "nb:cursor") {
        const float len = std::min({22.0f, w / 3, h / 3}), t = 7;
        const float xs[2] = {x, x + w}, ys[2] = {y, y + h};
        for (int cx = 0; cx < 2; cx++)
            for (int cy = 0; cy < 2; cy++) {
                const float px = xs[cx], py = ys[cy], dx = cx ? -1.0f : 1.0f, dy = cy ? -1.0f : 1.0f;
                // the bracket: a dark outline, white inside
                auto bracket = [&](float bx, float by, float l, float th, uint32_t col) {
                    rect(std::min(bx, bx + dx * l), std::min(by, by + dy * th), l, th, col);
                    rect(std::min(bx, bx + dx * th), std::min(by, by + dy * l), th, l, col);
                };
                bracket(px, py, len, t, kCursorInk);
                bracket(px + dx * 2, py + dy * 2, len - 4, t - 4, kCursorFill);
            }
    }
}

// The page's commands with every marker resolved: boxes with art become pictures keyed "art:<name>"
// (story_notebook.cpp hands the composed image to the HUD store), the others plain shapes.
inline std::vector<Command> resolve(const std::vector<Command>& in, const Art* art) {
    std::vector<Command> out;
    out.reserve(in.size() + 16);
    for (const Command& c : in) {
        if (!marker(c)) {
            out.push_back(c);
            continue;
        }
        const std::string n = box_name(c.text);
        const bool has = art && has_art(*art, n);
        if (has) {
            Command p = c;
            p.text = "art:" + c.text.substr(3);
            out.push_back(p);
        } else {
            shapes(c, out);
        }
    }
    return out;
}

}  // namespace story_notebook::style
