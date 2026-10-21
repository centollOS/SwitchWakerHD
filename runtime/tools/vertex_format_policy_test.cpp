#include "gfx/vulkan/vertex_format_policy.h"
#include "gfx/vulkan/vertex_format_shader.h"
#include <cassert>
#include <cstring>
#include <vector>
using namespace gfxvk::vk;
// Model Vulkan UINT component fetches independently of the fallback policy.
// The selected VkFormat determines how many bytes are actually read.
static std::array<uint32_t, 4> fetch(VkFormat format, const unsigned char* bytes) {
    unsigned width = 0, lanes = 0;
    switch (format) {
    case VK_FORMAT_R8_UINT: width = 1; lanes = 1; break;
    case VK_FORMAT_R8G8_UINT: width = 1; lanes = 2; break;
    case VK_FORMAT_R16_UINT: width = 2; lanes = 1; break;
    case VK_FORMAT_R16G16_UINT: width = 2; lanes = 2; break;
    case VK_FORMAT_R32_UINT: width = 4; lanes = 1; break;
    case VK_FORMAT_R32G32_UINT: width = 4; lanes = 2; break;
    default: assert(false);
    }
    std::array<uint32_t, 4> result{0, 0, 0, 1};
    for (unsigned lane = 0; lane < lanes; ++lane)
        for (unsigned byte = 0; byte < width; ++byte)
            result[lane] |= uint32_t(bytes[lane * width + byte]) << (8 * byte);
    return result;
}
int main() {
    for (const std::string layout : {"layout(location = 7)", "ATTR_LAYOUT(7)"}) {
        std::string source = layout + " in uvec4 custom;\nvoid main(){uvec4 v=custom;}";
        assert(rewrite_vertex_fallback_input(source, "attrDataSem3", 7, 9));
        assert(source.find("#define custom uvec4(customHead.xy, customTail, 1u)") != std::string::npos);
        assert(source.find("uvec4 v=custom;") != std::string::npos);
        std::string unrelated = layout + " in uvec4 custom;";
        const auto original = unrelated;
        assert(!rewrite_vertex_fallback_input(unrelated, "attrDataSem3", 8, 9));
        assert(unrelated == original);
    }
    std::string generated = "ATTR_LAYOUT(2) in uvec4 attrDataSem7;";
    assert(rewrite_vertex_fallback_input(generated, "attrDataSem7", 2, 3));
    auto all = [](VkFormat) { return true; };
    auto native = vertex_format_policy(all, false);
    assert(native.fallbackMask == 0 && native.shader_salt() == 0);
    auto forced = vertex_format_policy(all, true);
    assert(forced.fallbackMask == 7 && forced.shader_salt() != 0);
    for (auto format : vertex_formats) {
        assert(native.fetch(format) == format);
        assert(forced.fetch(format) == split_vertex_format(format));
        if (!VertexFormatPolicy::bit(format)) continue;
        auto missing = vertex_format_policy([=](VkFormat f) { return f != format; }, false);
        assert(missing.fallbackMask == VertexFormatPolicy::bit(format));
        assert(missing.shader_salt() != native.shader_salt());
        assert(missing.fetch(format) == split_vertex_format(format));
        bool failed = false;
        try { vertex_format_policy([=](VkFormat f) { return f != split_vertex_format(format); }, true); }
        catch (const std::runtime_error&) { failed = true; }
        assert(failed);
    }
    bool failed = false;
    try { vertex_format_policy([](VkFormat f) { return f != VK_FORMAT_R8_UINT; }, false); }
    catch (const std::runtime_error&) { failed = true; }
    assert(failed);
    const int mapping[] = {-1, 0, 3, 1};
    assert(vertex_tail_location(mapping) == 4);
    // Tight and interleaved layouts, unaligned u8/u16 offsets, zero stride,
    // and a last RGB vertex exactly ending at the declared guest buffer end.
    for (unsigned component : {1u, 2u, 4u}) {
        for (unsigned offset : {0u, component, 3 * component}) {
            for (unsigned stride : {0u, 3 * component, 7 * component}) {
                unsigned end = offset + 2 * stride + 3 * component;
                std::vector<unsigned char> guest(end);
                for (unsigned i = 0; i < end; ++i) guest[i] = (i * 53 + 127) & 255;
                const VkFormat format = component == 1 ? VK_FORMAT_R8G8B8_UINT :
                    component == 2 ? VK_FORMAT_R16G16B16_UINT : VK_FORMAT_R32G32B32_UINT;
                for (unsigned vertex = 0; vertex < 3; ++vertex) {
                    unsigned begin = offset + vertex * stride;
                    assert(begin + vertex_tail_offset(format) + component <= guest.size());
                    const auto head = fetch(forced.fetch(format), guest.data() + begin);
                    const auto tail = fetch(vertex_tail_format(format),
                        guest.data() + begin + vertex_tail_offset(format));
                    const std::array<uint32_t, 4> unpacked{head[0], head[1], tail[0], 1};
                    std::array<uint32_t, 4> expected{0, 0, 0, 1};
                    for (unsigned lane = 0; lane < 3; ++lane)
                        for (unsigned byte = 0; byte < component; ++byte)
                            expected[lane] |= uint32_t(guest[begin + lane * component + byte]) << (8 * byte);
                    assert(unpacked == expected);
                }
            }
        }
    }
    assert(vertex_fallback_declaration("attrDataSem7", 8) ==
        "in uvec4 attrDataSem7Head;\nlayout(location = 8) in uint attrDataSem7Tail;\n"
        "#define attrDataSem7 uvec4(attrDataSem7Head.xy, attrDataSem7Tail, 1u)\n");
    assert(vertex_tail_offset(VK_FORMAT_R8G8B8_UINT) == 2);
    assert(vertex_tail_offset(VK_FORMAT_R16G16B16_UINT) == 4);
    assert(vertex_tail_offset(VK_FORMAT_R32G32B32_UINT) == 8);
}
