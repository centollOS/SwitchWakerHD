// The FPS counter's 3x5 pixel font (backend.cpp draw_text): a bit pattern per character, row 0 (top)
// in bits 14-12, column 0 (left) the highest bit of a row. Window coordinates from the top left
// (OriginUpperLeft): with the other origin the text shows mirrored upside down.
#version 460
layout(location = 0) out vec4 outColor;
layout(std140, binding = 0) uniform Text {
    vec4 box;           // left, top (window pixels), window pixels per font pixel, unused
    ivec4 grid;         // columns, rows
    vec4 fg;
    vec4 bg;
    uvec4 glyphs[192];  // up to 768 characters, row by row, four per entry
};
void main() {
    ivec2 p = ivec2(floor((gl_FragCoord.xy - box.xy) / box.z)) - ivec2(1);
    bool on = false;
    if (p.x >= 0 && p.y >= 0) {
        ivec2 cell = p / ivec2(4, 6), sub = p - cell * ivec2(4, 6);
        if (cell.x < grid.x && cell.y < grid.y && sub.x < 3 && sub.y < 5) {
            int i = cell.y * grid.x + cell.x;
            uint g = glyphs[i >> 2][i & 3];
            on = ((g >> uint((4 - sub.y) * 3 + (2 - sub.x))) & 1u) != 0u;
        }
    }
    outColor = on ? fg : bg;
}
