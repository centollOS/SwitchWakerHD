// Edge smoothing (FXAA, issue #21; backend.cpp fxaa_pass): the game's 3D picture before its HUD, into another
// image of the TV buffer, at the source's grid (it may also scale up). The same filter as gfx/vulkan's present
// pass and Metal's. Drawn with present_vsh.glsl
#version 460
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D image;
layout(std140, binding = 0) uniform Fxaa {
    ivec4 mode;  // x: the samples are linear light (luma is taken on their square root, close to sRGB)
};
float luma(vec3 c) {
    if (mode.x != 0) c = sqrt(max(c, vec3(0.0)));
    return dot(c, vec3(0.299, 0.587, 0.114));
}
void main() {
    vec2 rcp = 1.0 / vec2(textureSize(image, 0));
    vec3 nw = texture(image, uv + vec2(-1, -1) * rcp).rgb, ne = texture(image, uv + vec2(1, -1) * rcp).rgb;
    vec3 sw = texture(image, uv + vec2(-1, 1) * rcp).rgb, se = texture(image, uv + vec2(1, 1) * rcp).rgb;
    vec4 m = texture(image, uv);
    float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se), lm = luma(m.rgb);
    float lmin = min(lm, min(min(lnw, lne), min(lsw, lse))), lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
    if (lmax - lmin < max(0.0312, lmax * 0.125)) {
        color = m;
        return;
    }
    vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
    float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), vec2(-8.0), vec2(8.0)) * rcp;
    vec3 a = 0.5 * (texture(image, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(image, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(image, uv - dir * 0.5).rgb + texture(image, uv + dir * 0.5).rgb);
    float lb = luma(b);
    color = vec4((lb < lmin || lb > lmax) ? a : b, m.a);
}
