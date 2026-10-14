// Pictures read from the game's own resources in guest memory at run time (nothing of the game is
// stored by the port): one glyph of a loaded font (a nw::font::Glyph record that the game's
// Font::GetGlyph filled) or a whole texture of a loaded layout (a GX2Texture), decoded on the CPU
// into host RGBA. Used by the story notebook's page (mods/story_notebook_font.h), which draws its
// text in the game's menu font and its windows with the game's own window textures.
#pragma once
#include <cstdint>
#include <vector>

namespace game_glyph {

// RGBA8, top-down rows, width * height * 4 bytes
struct Bitmap {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

// One glyph of a font, as the game's CharWriter would draw it.
struct Glyph {
    Bitmap bitmap;     // the glyph's cell: width = glyph width, height = the font's cell height
    int left = 0;      // the space before the glyph (pixels, may be negative)
    int advance = 0;   // the character's width: where the next glyph starts
    bool colour = false;  // an RGBA glyph (the icon font's button pictures); else white coverage in alpha
};

// A nw::font::Glyph record (0x18 bytes, filled by Font::GetGlyph): false if its sheet can't be read.
bool read_glyph(uint32_t record, Glyph& out);

// The first mip level and slice of a GX2Texture in guest memory: false if its format isn't one the
// decoder knows (RGBA8, R8, R8G8, RGBA4, BC1-BC5) or its addresses don't look right.
bool read_texture(uint32_t texture, Bitmap& out);

}  // namespace game_glyph
