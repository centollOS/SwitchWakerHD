// Button art drawn by the port itself. Shoulder lettering is sampled at run time from the game's
// HUD font (button_glyph_font.cpp); the frame and the PlayStation shapes below are plain geometry
// (a rounded box, a disc, two strokes, a ring, a square, a triangle): no image files, fonts or
// third-party artwork are shipped or copied.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace button_glyphs {

// Original button frame only. Lettering is sampled at run time from the game's HUD font.
constexpr uint32_t shoulder_background(int x, int y) {
    if (x < 2 || x > 61 || y < 10 || y > 53) return 0;
    const int cx = x < 8 ? 8 : x > 55 ? 55 : x;
    const int cy = y < 16 ? 16 : y > 47 ? 47 : y;
    const int distance = (x-cx)*(x-cx) + (y-cy)*(y-cy);
    if (distance > 36) return 0;
    const uint32_t shade = distance > 16 || x < 4 || x > 59 || y < 12 || y > 51 ? 112 : 218 + (53-y)/3;
    return (shade << 24) | (shade << 16) | (shade << 8) | 255;
}

// PlayStation-style face shapes for the Wii U A, B, X, Y (by label: Cross is A, Circle B,
// Square X, Triangle Y). Colours follow the common PlayStation convention. Without colours the
// dialog icon's shape is dark grey on its disc, and the HUD's bare shape is white coverage: the
// game draws it with its own letter colours (button_glyphs.cpp, coverage_sheet).
constexpr uint32_t kShapeColours[4] = {0x3C7DD9FF, 0xD9413CFF, 0xD05FB0FF, 0x2AA67DFF};
constexpr uint32_t kShapeInk = 0x3A3A3AFF;

namespace art {
inline float segment(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx-ax, dy = by-ay;
    const float t = std::clamp(((px-ax)*dx + (py-ay)*dy) / (dx*dx + dy*dy), 0.0f, 1.0f);
    return std::hypot(px - (ax + t*dx), py - (ay + t*dy));
}
// Distance from (x, y) (relative to the shape's centre, y down) to the outline of shape `face`
// with half-size s.
inline float shape_distance(int face, float x, float y, float s) {
    switch (face) {
    case 0: return std::min(segment(x, y, -s, -s, s, s), segment(x, y, -s, s, s, -s));  // cross
    case 1: return std::fabs(std::hypot(x, y) - s);                                   // circle
    case 2: return std::fabs(std::max(std::fabs(x), std::fabs(y)) - s*0.9f);          // square
    default: {                                                                        // triangle
        const float top = -s*1.0f, base = s*0.68f, half = s*1.02f;
        return std::min({segment(x, y, 0, top, -half, base), segment(x, y, -half, base, half, base),
                         segment(x, y, half, base, 0, top)});
    }
    }
}
}  // namespace art

// One 64x64 cell, RGBA (0xRRGGBBAA), top-down. `disc`: the dialog icon, a light disc with a rim
// sized like the game's icon font glyphs (centre 32,29, radius 21); otherwise the bare shape for
// the HUD letter panes, which sit on the game's own button pictures.
constexpr float kDiscStroke = 4.2f, kBareStroke = 8.0f;  // stroke widths in cell pixels
inline uint32_t face_shape(int face, bool colour, bool disc, int x, int y) {
    const float cx = 32, cy = disc ? 29 : 32, s = disc ? 10.5f : 21.0f, width = disc ? kDiscStroke : kBareStroke;
    const uint32_t ink = colour ? kShapeColours[face & 3] : disc ? kShapeInk : 0xFFFFFFFFu;
    float r = 0, g = 0, b = 0, a = 0;
    constexpr int kSamples = 4;  // 4x4 supersampling for smooth edges
    for (int sy = 0; sy < kSamples; sy++) for (int sx = 0; sx < kSamples; sx++) {
        const float px = x + (sx + 0.5f)/kSamples - cx, py = y + (sy + 0.5f)/kSamples - cy;
        uint32_t c = 0;
        if (art::shape_distance(face, px, py, s) <= width*0.5f) c = ink;
        else if (disc) {
            const float d = std::hypot(px, py);
            if (d <= 19.2f) c = 0xF2F2F2FF;
            else if (d <= 21.0f) c = 0xA8A8A8FF;
        }
        if (!c) continue;
        r += (c >> 24) & 255; g += (c >> 16) & 255; b += (c >> 8) & 255; a += 1;
    }
    if (a == 0) return 0;
    const auto channel = [&](float v) { return (uint32_t)std::lround(v / a); };  // straight colour
    const uint32_t alpha = (uint32_t)std::lround(255.0f * a / (kSamples*kSamples));
    return channel(r) << 24 | channel(g) << 16 | channel(b) << 8 | alpha;
}

}  // namespace button_glyphs
