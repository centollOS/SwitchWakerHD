#version 460
// the pixel stage of depth-only draws (draw.cpp, WWHD_DK_DEPTH_ONLY): no outputs, no discard, so depth and
// stencil come from the rasterizer alone and early depth testing applies
void main() {}
