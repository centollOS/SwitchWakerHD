// The present pass: the game's picture with the picture adjustments (gfx/switch_renderer.h PictureGrade) and,
// for an sRGB TV format, sRGB encoding, as gfx/gl's present_program()
#version 460
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D scan;
layout(std140, binding = 0) uniform Present {
    vec4 grade;    // exposure, contrast, saturation, gamma
    ivec4 encode;  // x: encode as sRGB; y: the picture's image has an sRGB format (sampling decoded it);
                   // z: FSR 1: 1 EASU upscale (the picture is smaller than the window), 2 RCAS sharpening (same size)
};
vec3 srgb_encode(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }
vec4 srgb_encode(vec4 c) { return mix(c * 12.92, 1.055 * pow(c, vec4(1.0 / 2.4)) - 0.055, step(vec4(0.0031308), c)); }

// ---- FSR 1 EASU (AMD FidelityFX Super Resolution 1.0, MIT; ffx_fsr1.h FsrEasuF, GLSL port): an edge-adaptive
// upscale from 12 taps. It expects perceptual values: when the picture is still linear (encode.x) the taps are
// taken to gamma 2.0 (sqrt) and back, which is cheaper than the sRGB curve on 12 texels.
vec4 easu_in(vec4 v) {
    if (encode.y != 0) v = srgb_encode(clamp(v, 0.0, 1.0));
    return encode.x != 0 ? sqrt(clamp(v, 0.0, 1.0)) : v;
}
void easu_tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len;
    float d2 = min(dot(v, v), clp);
    float wB = 2.0 / 5.0 * d2 - 1.0, wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}
void easu_set(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
    float lenX = max(abs(lD - lC), abs(lC - lB)), dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) / lenX, 0.0, 1.0);
    len += lenX * lenX * w;
    float lenY = max(abs(lE - lC), abs(lC - lA)), dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) / lenY, 0.0, 1.0);
    len += lenY * lenY * w;
}
vec3 easu() {
    vec2 size = vec2(textureSize(scan, 0)), inv = 1.0 / size;
    vec2 pp = uv * size - 0.5;  // the output pixel's centre in the picture's texel coordinates
    vec2 fp = floor(pp);
    pp -= fp;
    vec2 p0 = fp * inv + vec2(inv.x, -inv.y);
    vec2 p1 = p0 + vec2(-inv.x, 2.0 * inv.y), p2 = p0 + vec2(inv.x, 2.0 * inv.y), p3 = p0 + vec2(0.0, 4.0 * inv.y);
    //   b c        gathers: (x y z w) = (upper left, upper right, lower right, lower left)
    // e f g h
    // i j k l
    //   n o
    vec4 bcR = easu_in(textureGather(scan, p0, 0)), bcG = easu_in(textureGather(scan, p0, 1)), bcB = easu_in(textureGather(scan, p0, 2));
    vec4 ijR = easu_in(textureGather(scan, p1, 0)), ijG = easu_in(textureGather(scan, p1, 1)), ijB = easu_in(textureGather(scan, p1, 2));
    vec4 klR = easu_in(textureGather(scan, p2, 0)), klG = easu_in(textureGather(scan, p2, 1)), klB = easu_in(textureGather(scan, p2, 2));
    vec4 noR = easu_in(textureGather(scan, p3, 0)), noG = easu_in(textureGather(scan, p3, 1)), noB = easu_in(textureGather(scan, p3, 2));
    vec4 bcL = bcB * 0.5 + (bcR * 0.5 + bcG), ijL = ijB * 0.5 + (ijR * 0.5 + ijG);
    vec4 klL = klB * 0.5 + (klR * 0.5 + klG), noL = noB * 0.5 + (noR * 0.5 + noG);
    float bL = bcL.x, cL = bcL.y, iL = ijL.x, jL = ijL.y, fL = ijL.z, eL = ijL.w;
    float kL = klL.x, lL = klL.y, hL = klL.z, gL = klL.w, oL = noL.z, nL = noL.w;
    vec2 dir = vec2(0.0);
    float len = 0.0;
    easu_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easu_set(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    easu_set(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    easu_set(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    float dirR = dot(dir, dir);
    bool zro = dirR < 1.0 / 32768.0;
    dirR = zro ? 1.0 : inversesqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len, clp = 1.0 / lob;
    vec3 fC = vec3(ijR.z, ijG.z, ijB.z), gC = vec3(klR.w, klG.w, klB.w), jC = vec3(ijR.y, ijG.y, ijB.y), kC = vec3(klR.x, klG.x, klB.x);
    vec3 min4 = min(min(fC, gC), min(jC, kC)), max4 = max(max(fC, gC), max(jC, kC));
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    easu_tap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, vec3(bcR.x, bcG.x, bcB.x));
    easu_tap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, vec3(bcR.y, bcG.y, bcB.y));
    easu_tap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, vec3(ijR.x, ijG.x, ijB.x));
    easu_tap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);
    easu_tap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);
    easu_tap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, vec3(ijR.w, ijG.w, ijB.w));
    easu_tap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);
    easu_tap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, vec3(klR.y, klG.y, klB.y));
    easu_tap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, vec3(klR.z, klG.z, klB.z));
    easu_tap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);
    easu_tap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, vec3(noR.z, noG.z, noB.z));
    easu_tap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, vec3(noR.w, noG.w, noB.w));
    return clamp(min(max4, max(min4, aC / aW)), 0.0, 1.0);
}

// ---- FSR 1 RCAS (ffx_fsr1.h FsrRcasF, GLSL port): contrast-adaptive sharpening from the pixel and its four
// neighbours, on a picture of the window's size (encode.z == 2). 0.25 stops of sharpness (AMD's default is 0.2).
vec3 rcas() {
    ivec2 ip = ivec2(uv * vec2(textureSize(scan, 0))), hi = textureSize(scan, 0) - 1;
    vec3 b = easu_in(vec4(texelFetch(scan, clamp(ip + ivec2(0, -1), ivec2(0), hi), 0).rgb, 0.0)).rgb;
    vec3 d = easu_in(vec4(texelFetch(scan, clamp(ip + ivec2(-1, 0), ivec2(0), hi), 0).rgb, 0.0)).rgb;
    vec3 e = easu_in(vec4(texelFetch(scan, ip, 0).rgb, 0.0)).rgb;
    vec3 f = easu_in(vec4(texelFetch(scan, clamp(ip + ivec2(1, 0), ivec2(0), hi), 0).rgb, 0.0)).rgb;
    vec3 h = easu_in(vec4(texelFetch(scan, clamp(ip + ivec2(0, 1), ivec2(0), hi), 0).rgb, 0.0)).rgb;
    vec3 mn4 = min(min(min(b, d), f), h), mx4 = max(max(max(b, d), f), h);
    vec3 hitMin = min(mn4, e) / (4.0 * mx4);
    vec3 hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0);
    vec3 lobeC = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(max(lobeC.r, lobeC.g), lobeC.b), 0.0)) * exp2(-0.25);
    return clamp((lobe * (b + d + h + f) + e) / (4.0 * lobe + 1.0), 0.0, 1.0);
}

void main() {
    vec3 c;
    if (encode.z != 0) {
        // EASU returns the picture as it would be after the encoding steps below (perceptual values)
        c = encode.z == 2 ? rcas() : easu();
        if (encode.x != 0) c = srgb_encode(clamp(c * c * grade.x, 0.0, 1.0));  // (exposure scales the linear picture)
        else c = clamp(c * grade.x, 0.0, 1.0);
    } else {
        c = texture(scan, uv).rgb;
        // gfx/gl samples the picture's bytes as stored (GL_SKIP_DECODE_EXT): an sRGB image's decoding is undone
        if (encode.y != 0) c = srgb_encode(clamp(c, 0.0, 1.0));
        c = clamp(c * grade.x, 0.0, 1.0);
        if (encode.x != 0) c = srgb_encode(c);
    }
    if (grade.y != 1.0) c = clamp(mix(c, c * c * (3.0 - 2.0 * c), grade.y - 1.0), 0.0, 1.0);
    if (grade.z != 1.0) {
        float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = clamp(mix(vec3(luma), c, grade.z), 0.0, 1.0);
    }
    if (grade.w != 1.0) c = pow(c, vec3(grade.w));
    color = vec4(c, 1.0);
}
