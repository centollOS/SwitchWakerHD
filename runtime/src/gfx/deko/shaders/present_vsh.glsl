// The present pass (backend.cpp draw_picture): one triangle covering the viewport, the game's picture with
// its row 0 at the top. p is in normalized device coordinates with y down (y = -1 at the top); deko3d's
// clip-space y points up (dk.h), so y is negated.
#version 460
layout(location = 0) out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));
    uv = p * 0.5 + 0.5;
    gl_Position = vec4(p.x, -p.y, 0.0, 1.0);
}
