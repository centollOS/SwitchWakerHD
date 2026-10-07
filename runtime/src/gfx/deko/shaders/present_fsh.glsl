// The present pass: the game's picture with the picture adjustments (gfx/switch_renderer.h PictureGrade) and,
// for an sRGB TV format, sRGB encoding, as gfx/gl's present_program()
#version 460
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D scan;
layout(std140, binding = 0) uniform Present {
    vec4 grade;    // exposure, contrast, saturation, gamma
    ivec4 encode;  // x: encode as sRGB; y: the picture's image has an sRGB format (sampling decoded it)
};
vec3 srgb_encode(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }
void main() {
    vec3 c = texture(scan, uv).rgb;
    // gfx/gl samples the picture's bytes as stored (GL_SKIP_DECODE_EXT): an sRGB image's decoding is undone
    if (encode.y != 0) c = srgb_encode(clamp(c, 0.0, 1.0));
    c = clamp(c * grade.x, 0.0, 1.0);
    if (encode.x != 0) c = srgb_encode(c);
    if (grade.y != 1.0) c = clamp(mix(c, c * c * (3.0 - 2.0 * c), grade.y - 1.0), 0.0, 1.0);
    if (grade.z != 1.0) {
        float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = clamp(mix(vec3(luma), c, grade.z), 0.0, 1.0);
    }
    if (grade.w != 1.0) c = pow(c, vec3(grade.w));
    color = vec4(c, 1.0);
}
