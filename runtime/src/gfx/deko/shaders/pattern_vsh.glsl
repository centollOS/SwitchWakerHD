// Test pattern (backend.cpp draw_pattern): positions in normalized device coordinates with y down (row 0 at
// y = -1); deko3d's clip-space y points up (dk.h), so y is negated
#version 460
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = inColor;
    gl_Position = vec4(inPos.x, -inPos.y, inPos.z, 1.0);
}
