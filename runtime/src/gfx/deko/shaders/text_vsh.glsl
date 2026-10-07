// One triangle covering the viewport (the text box): text_fsh.glsl draws from gl_FragCoord
#version 460
void main() {
    vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));
    gl_Position = vec4(p, 0.0, 1.0);
}
