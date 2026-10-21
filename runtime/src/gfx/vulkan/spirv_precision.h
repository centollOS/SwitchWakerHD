// Diagnostic "Full precision in shaders" (issue #207, WWHD_VK_FULL_PRECISION=1): removes every
// RelaxedPrecision decoration (OpDecorate / OpMemberDecorate ... RelaxedPrecision) from a SPIR-V
// module, so a driver cannot evaluate any value at reduced (mediump) precision.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gfxvk::vk {
// returns the number of decorations removed; a malformed module is left unchanged
inline size_t strip_relaxed_precision(std::vector<uint32_t>& words) {
    constexpr uint32_t kOpDecorate = 71, kOpMemberDecorate = 72, kRelaxedPrecision = 0;
    if (words.size() < 5 || words[0] != 0x07230203u) return 0;
    std::vector<uint32_t> out;
    size_t removed = 0, i = 5;
    out.reserve(words.size());
    out.insert(out.end(), words.begin(), words.begin() + 5);
    while (i < words.size()) {
        const uint32_t count = words[i] >> 16, opcode = words[i] & 0xFFFF;
        if (!count || i + count > words.size()) return 0;
        const bool relaxed = (opcode == kOpDecorate && count >= 3 && words[i + 2] == kRelaxedPrecision) ||
                             (opcode == kOpMemberDecorate && count >= 4 && words[i + 3] == kRelaxedPrecision);
        if (relaxed) ++removed;
        else out.insert(out.end(), words.begin() + i, words.begin() + i + count);
        i += count;
    }
    if (removed) words.swap(out);
    return removed;
}
}
