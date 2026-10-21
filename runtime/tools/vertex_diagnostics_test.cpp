// Issue #207 driver troubleshooting helpers: the CPU vertex decoder (vertex_cpu_decode.h) against
// values written out by hand from Cemu's GLSL decoder, and the RelaxedPrecision strip.
#include "gfx/vulkan/vertex_cpu_decode.h"
#include "gfx/vulkan/spirv_precision.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
using namespace gfxvk::vk;
void cemu_shim_log(const std::string& line) { fprintf(stderr, "%s\n", line.c_str()); }  // vertex_formats.h
static uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
static LatteParsedFetchShaderAttribute attr(Latte::E_HWFMT format, uint8_t nfa, bool sig, LatteConst::VertexFetchEndianMode endian) {
    LatteParsedFetchShaderAttribute a{};
    a.format = format; a.nfa = nfa; a.isSigned = sig; a.endianSwap = endian;
    return a;
}
int main() {
    using F = Latte::E_HWFMT;
    using E = LatteConst::VertexFetchEndianMode;
    uint32_t out[4];
    // big-endian float3 position (the reporter's R32G32B32 + swap32): bytes swapped, w = 0
    const uint8_t pos[12] = {0x3F, 0x80, 0, 0, 0xC0, 0, 0, 0, 0x42, 0x28, 0, 0};
    assert(cpu_decode_attribute(attr(F::HWFMT_32_32_32_FLOAT, 2, false, E::SWAP_U32), pos, out));
    assert(out[0] == bits(1.0f) && out[1] == bits(-2.0f) && out[2] == bits(42.0f) && out[3] == 0);
    assert(cpu_vertex_format(attr(F::HWFMT_32_32_32_FLOAT, 2, false, E::SWAP_U32)) == VK_FORMAT_R32G32B32A32_SFLOAT);
    // big-endian SNORM16 x2: max(float(int)/32767, -1)
    const uint8_t sn[4] = {0x80, 0x00, 0x7F, 0xFF};
    assert(cpu_decode_attribute(attr(F::HWFMT_16_16, 0, true, E::SWAP_U16), sn, out));
    assert(out[0] == bits(-1.0f) && out[1] == bits(1.0f) && out[2] == 0 && out[3] == 0);
    // big-endian half4
    const uint8_t h[8] = {0x3C, 0x00, 0xC0, 0x00, 0x00, 0x01, 0x7C, 0x00};
    assert(cpu_decode_attribute(attr(F::HWFMT_16_16_16_16_FLOAT, 2, false, E::SWAP_U16), h, out));
    assert(out[0] == bits(1.0f) && out[1] == bits(-2.0f) && out[2] == bits(5.9604645e-08f) && out[3] == 0x7F800000u);
    // integer data stays integer bits (UINT stream), sign-extended where the shader does
    const uint8_t i16[4] = {0xFF, 0xFE, 0x00, 0x05};
    assert(cpu_decode_attribute(attr(F::HWFMT_16_16, 1, true, E::SWAP_U16), i16, out));
    assert(out[0] == 0xFFFFFFFEu && out[1] == 5 && cpu_vertex_format(attr(F::HWFMT_16_16, 1, true, E::SWAP_U16)) == VK_FORMAT_R32G32B32A32_UINT);
    // little-endian UNORM8 colour and the swapped (wzyx) big-endian variant
    const uint8_t c[4] = {0, 255, 51, 102};
    assert(cpu_decode_attribute(attr(F::HWFMT_8_8_8_8, 0, false, E::SWAP_NONE), c, out));
    assert(out[0] == bits(0.0f) && out[1] == bits(1.0f) && out[2] == bits(51.0f / 255.0f));
    assert(cpu_decode_attribute(attr(F::HWFMT_8_8_8_8, 0, false, E::SWAP_U32), c, out));
    assert(out[0] == bits(102.0f / 255.0f) && out[3] == bits(0.0f));
    // 2_10_10_10 signed, swap32
    const uint32_t packed = (0x1FFu) | (0x201u << 10) | (0u << 20) | (3u << 30);
    const uint8_t p10[4] = {uint8_t(packed >> 24), uint8_t(packed >> 16), uint8_t(packed >> 8), uint8_t(packed)};
    assert(cpu_decode_attribute(attr(F::HWFMT_2_10_10_10, 0, true, E::SWAP_U32), p10, out));
    assert(out[0] == bits(1.0f) && out[1] == bits(-1.0f) && out[2] == bits(0.0f) && out[3] == bits(3.0f * (1.0f / 3.0f)));
    // a combination the shader does not decode either
    assert(!cpu_decode_attribute(attr(F::HWFMT_8_8_8, 0, false, E::SWAP_NONE), c, out) && !out[0]);

    // SPIR-V: header + OpDecorate %5 RelaxedPrecision + OpDecorate %6 Location 0 + OpMemberDecorate %7 1 RelaxedPrecision
    std::vector<uint32_t> words = {0x07230203u, 0x00010300u, 0, 8, 0,
                                   (3u << 16) | 71, 5, 0,
                                   (4u << 16) | 71, 6, 30, 0,
                                   (4u << 16) | 72, 7, 1, 0};
    assert(strip_relaxed_precision(words) == 2);
    assert(words.size() == 9 && words[5] == ((4u << 16) | 71) && words[7] == 30);
    assert(strip_relaxed_precision(words) == 0);
    std::vector<uint32_t> broken = {0x07230203u, 0, 0, 0, 0, (9u << 16) | 71, 5};
    assert(strip_relaxed_precision(broken) == 0 && broken.size() == 7);
    puts("vertex_diagnostics_test: ok");
}
