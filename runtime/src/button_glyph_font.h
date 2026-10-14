// Compose shoulder captions from the player's actual HUD font; no font data is shipped.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace button_glyphs {

// The composed button atlas: 128x1024 RGBA, linear, in 16 rows of 64x64 cells. Column 0 is the
// framed/disc version (dialog icons), column 1 the bare one (HUD panes, which have their own
// button pictures). Rows: 0-3 L1 R1 L2 R2, 4-7 LB RB LT RT (for the game's L R ZL ZR),
// 8-11 the PlayStation shapes for A B X Y in colour, 12-15 the same shapes without colour.
constexpr int kAtlasWidth = 128, kAtlasHeight = 1024, kAtlasRows = 16;
constexpr int numbered_row(int shoulder) { return shoulder; }
constexpr int xbox_row(int shoulder) { return 4 + shoulder; }
constexpr int shape_row(int face, bool colour) { return (colour ? 8 : 12) + face; }

// Native nw::font::Glyph records for L, R, 1, 2, B, T (0x18 bytes each). Empty means
// unsupported/not ready.
std::vector<uint8_t> button_atlas(const std::array<uint32_t, 6>& glyphs);

}  // namespace button_glyphs
