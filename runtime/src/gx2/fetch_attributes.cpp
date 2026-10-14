// Pure fetch layout construction; no guest memory, renderer or native API access.
#include "../../third_party/cemu/cemu_shim.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "fetch_attributes.h"
#include <cstdlib>
using namespace Latte;
namespace gx2 {
static LatteConst::VertexFetchEndianMode default_endian(uint32 fmt) {
    switch (fmt) {
    case 0: case 1: case 4: case 10: return LatteConst::VertexFetchEndianMode::SWAP_NONE;
    case 2: case 3: case 7: case 8: case 14: case 15: return LatteConst::VertexFetchEndianMode::SWAP_U16;
    default: return LatteConst::VertexFetchEndianMode::SWAP_U32;
    }
}
static const uint32 kRawToFetchFormat[] = {1, 2, 5, 6, 7, 0xD, 0xE, 0xF, 0x10, 0x16, 0x1A, 0x19, 0x1D, 0x1E, 0x1F, 0x20, 0x2F, 0x30, 0x22, 0x23};

static uint32 legacy_attribute_size(uint32 format) {
    switch (format) {
    case 1: return 1;
    case 5: case 6: case 7: return 2;
    case 0xD: case 0xE: case 0xF: case 0x10: case 0x19: case 0x1A: return 4;
    case 0x1D: case 0x1E: case 0x1F: case 0x20: return 8;
    case 0x2F: case 0x30: return 12;
    case 0x22: case 0x23: return 16;
    default: return 0; // Matches the native parser for unsupported formats.
    }
}

void normalize_fetch_shader(LatteFetchShader* fs) {
    if (!fs) return;
    auto normalize = [](auto& groups) {
        for (auto& group : groups) for (sint32 i = 0; i < group.attribCount; ++i) {
            const auto& a = group.attrib[i];
            LatteParsedFetchShaderAttribute clean;
            memset(&clean, 0, sizeof clean);
            clean.attributeBufferIndex = a.attributeBufferIndex; clean.semanticId = a.semanticId;
            clean.format = a.format; clean.fetchType = a.fetchType;
            clean.nfa = a.nfa; clean.isSigned = a.isSigned; clean.endianSwap = a.endianSwap;
            for (int k = 0; k < 4; ++k) clean.ds[k] = a.ds[k];
            clean.aluDivisor = a.aluDivisor; clean.offset = a.offset;
            memcpy(&group.attrib[i], &clean, sizeof clean);
        }
    };
    normalize(fs->bufferGroups); normalize(fs->bufferGroupsInvalid);
    fs->CalculateFetchShaderVkHash();
}

LatteFetchShader* decode_fetch_attributes_bytes(std::span<const uint8_t> bytes, uint64_t* sourceHash) {
    const size_t size=bytes.size();
    if (size < 32 || size > 32 + 64 * 52) return nullptr;
    const uint8_t* data=bytes.data();
    if (memcmp(data, "WWFA0001", 8)) return nullptr;
    auto word = [](const uint8_t* p) {
        return uint32(p[0]) | uint32(p[1]) << 8 | uint32(p[2]) << 16 | uint32(p[3]) << 24;
    };
    const uint32 count = word(data + 16);
    const uint32 compact = word(data + 20);
    if (count > 64 || compact > 1 || size != 32 + count * 52) return nullptr;
    std::vector<LatteParsedFetchShaderAttribute> attrs(count);
    for (uint32 i = 0; i < count; ++i) {
        uint32 w[13];
        for (uint32 k = 0; k < 13; ++k) w[k] = word(data + 32 + i * 52 + k * 4);
        if (w[0] > 255 || w[1] > 255 ||
            std::find(std::begin(kRawToFetchFormat), std::end(kRawToFetchFormat), w[2]) == std::end(kRawToFetchFormat) ||
            w[3] > 1 || w[4] > 2 || w[5] > 1 || w[6] > 2 || w[11] > 65535 ||
            w[7] > 7 || w[8] > 7 || w[9] > 7 || w[10] > 7 ||
            (w[12] > 0x7fffffffu && w[12] != 0xffffffffu) ||
            (w[3] == 1 && (w[12] == 0 || w[12] > 0x7fffffffu))) return nullptr;
        auto& a = attrs[i];
        a = {};
        a.attributeBufferIndex = uint8(w[0]); a.semanticId = uint8(w[1]);
        a.format = E_HWFMT(w[2]); a.fetchType = LatteConst::VertexFetchType2(w[3]);
        a.nfa = uint8(w[4]); a.isSigned = uint8(w[5]);
        a.endianSwap = LatteConst::VertexFetchEndianMode(w[6]);
        for (uint32 k = 0; k < 4; ++k) a.ds[k] = uint8(w[7 + k]);
        a.offset = w[11]; a.aluDivisor = w[12] == 0xffffffffu ? -1 : sint32(w[12]);
    }
    auto* fs = new LatteFetchShader();
    fs->key = uint64_t(word(data + 8)) | uint64_t(word(data + 12)) << 32;
    // Keep first-encounter group order and attribute order, including invalid
    // groups whose constant component selectors still influence translation.
    for (const auto& a : attrs) {
        auto& groups = a.attributeBufferIndex < 16 ? fs->bufferGroups : fs->bufferGroupsInvalid;
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) {
            return g.attributeBufferIndex == a.attributeBufferIndex;
        });
        if (it == groups.end()) {
            groups.emplace_back(); it = groups.end() - 1;
            it->attributeBufferIndex = a.attributeBufferIndex; it->minOffset = a.offset;
        }
        // Parsed fetch shaders live with the renderer's fetch cache, like the
        // existing guest-program parser's attribute allocations.
        auto* replacement = static_cast<LatteParsedFetchShaderAttribute*>(
            realloc(it->attrib, sizeof(a) * (it->attribCount + 1)));
        if (!replacement) {
            for (auto& g : fs->bufferGroups) free(g.attrib);
            for (auto& g : fs->bufferGroupsInvalid) free(g.attrib);
            delete fs; return nullptr;
        }
        it->attrib = replacement; it->attrib[it->attribCount++] = a;
        it->minOffset = std::min(it->minOffset, a.offset);
        if (!compact) it->totalAttribRangeSize = std::max(it->totalAttribRangeSize, a.offset + legacy_attribute_size(uint32(a.format)));
        it->hasVtxIndexAccess |= a.fetchType == LatteConst::VertexFetchType2::VERTEX_DATA;
        it->hasInstanceIndexAccess |= a.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA;
        if (a.attributeBufferIndex < 16) fs->attributeBufferMask |= 1u << a.attributeBufferIndex;
    }
    normalize_fetch_shader(fs);
    // The ordinary HLE compact layout has always used fragment zero. Keep
    // prebuilt and ordinary pipeline keys identical for this representation.
    if (compact) fs->vkPipelineHashFragment = 0;
    if (sourceHash) *sourceHash = fs->key;
    return fs;
}

LatteFetchShader* build_fetch_shader_bytes(std::span<const uint8_t> bytes, uint64_t identity) {
    if(bytes.size()<16) return nullptr;
    const auto word=[&](size_t offset) { const auto* p=bytes.data()+offset;return uint32(p[0])<<24|uint32(p[1])<<16|uint32(p[2])<<8|p[3]; };
    if(word(0)!=0x57574653) return nullptr;
    uint32 n=word(4);
    if(n>64 || bytes.size()!=16+size_t(n)*16) return nullptr;
    auto* fs = new LatteFetchShader();
    auto* attrs = new LatteParsedFetchShaderAttribute[n];
    std::vector<std::vector<LatteParsedFetchShaderAttribute*>> groups(16);
    for (uint32 i = 0; i < n; i++) {
        uint32 e = 16 + i * 16;
        uint32 w0 = word(e), offset = word(e + 4), w2 = word(e + 8), destSel = word(e + 12);
        uint32 location = w0 & 0xFF, buffer = (w0 >> 8) & 0xFF, indexType = (w0 >> 16) & 0xFF, endian = w0 >> 24;
        uint32 fmt = w2 & 0xFFFF, divisor = w2 >> 16;
        LatteParsedFetchShaderAttribute& a = attrs[i];
        a = {};
        a.attributeBufferIndex = (uint8)buffer;
        a.semanticId = (uint8)location;
        a.format = (E_HWFMT)(kRawToFetchFormat[std::min<uint32>(fmt & 0x3F, 19)] & 0x3F);
        a.nfa = (fmt & 0x800) ? 2 : (fmt & 0x100) ? 1 : 0;
        a.isSigned = (fmt & 0x200) ? 1 : 0;
        a.endianSwap = endian == 3 /* SWAP_DEFAULT */ ? default_endian(fmt & 0x3F) : (LatteConst::VertexFetchEndianMode)endian;
        a.fetchType = indexType ? LatteConst::VertexFetchType2::INSTANCE_DATA : LatteConst::VertexFetchType2::VERTEX_DATA;
        a.aluDivisor = indexType ? (sint32)std::max<uint32>(divisor, 1) : 0;
        a.offset = offset;
        for (int k = 0; k < 4; k++) a.ds[k] = (destSel >> (24 - 8 * k)) & 7;
        if (buffer < 16) groups[buffer].push_back(&a);
    }
    // attributes are grouped per buffer, contiguous in the attrs array order
    auto* sorted = new LatteParsedFetchShaderAttribute[n];
    uint32 k = 0;
    for (uint32 b = 0; b < 16; b++) {
        if (groups[b].empty()) continue;
        LatteParsedFetchShaderBufferGroup g{};
        g.attributeBufferIndex = (uint8)b;
        g.attribCount = (sint8)groups[b].size();
        g.attrib = &sorted[k];
        g.minOffset = 0xFFFFFFFF;
        for (auto* a : groups[b]) {
            sorted[k++] = *a;
            g.minOffset = std::min(g.minOffset, a->offset);
            if (a->fetchType == LatteConst::VertexFetchType2::VERTEX_DATA) g.hasVtxIndexAccess = true;
            else g.hasInstanceIndexAccess = true;
        }
        fs->bufferGroups.push_back(g);
        fs->attributeBufferMask |= 1u << b;
    }
    delete[] attrs;
    fs->key = identity;
    return fs;
}
}
