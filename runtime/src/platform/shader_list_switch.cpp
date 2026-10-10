// The console's own shader list (shader_list_switch.h): tools/switch/shader_manifest.py read_manifest_full and
// speculate, in C++.
#include "shader_list_switch.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

namespace shader_list {
namespace {

// tools/shaderprep.py's register indices
constexpr uint16_t mmSQ_PGM_START_PS = 0xA210, mmSQ_PGM_RESOURCES_PS = 0xA214, mmSQ_PGM_START_VS = 0xA216,
                   mmSQ_PGM_RESOURCES_VS = 0xA21A, mmVGT_PRIMITIVEID_EN = 0xA2A1, mmSPI_VS_OUT_CONFIG = 0xA1B1,
                   mmPA_CL_VS_OUT_CNTL = 0xA207, mmSPI_VS_OUT_ID_0 = 0xA185, mmSQ_VTX_SEMANTIC_0 = 0xA0E0,
                   mmSQ_VTX_SEMANTIC_CLEAR = 0xA238, mmSPI_PS_IN_CONTROL_0 = 0xA1B3, mmSPI_PS_INPUT_CNTL_0 = 0xA191,
                   mmCB_SHADER_MASK = 0xA08F, mmCB_SHADER_CONTROL = 0xA1E8, mmDB_SHADER_CONTROL = 0xA203,
                   mmSPI_INPUT_Z = 0xA1B6;

uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | p[2] << 16 | p[1] << 8 | p[0]; }

// shaderprep.own_block, with SQ_VTX_SEMANTIC_CLEAR as this runtime's GX2SetVertexShader writes it (shader_manifest.py)
Regs own_block(bool vertex, const std::vector<uint32_t>& w) {
    std::map<uint16_t, uint32_t> r;
    if (vertex) {
        r[mmSQ_PGM_RESOURCES_VS] = w[0];
        r[mmVGT_PRIMITIVEID_EN] = w[1];
        r[mmSPI_VS_OUT_CONFIG] = w[2];
        r[mmPA_CL_VS_OUT_CNTL] = w[0x38 / 4];
        for (uint32_t i = 0; i < std::min<uint32_t>(w[3], 10); i++) r[uint16_t(mmSPI_VS_OUT_ID_0 + i)] = w[4 + i];
        const uint32_t nsem = std::min<uint32_t>(w[0x40 / 4], 32);
        if (nsem) {
            r[mmSQ_VTX_SEMANTIC_CLEAR] = nsem >= 32 ? 0u : uint32_t(0xFFFFFFFFull << nsem);
            for (uint32_t i = 0; i < nsem; i++) r[uint16_t(mmSQ_VTX_SEMANTIC_0 + i)] = w[0x44 / 4 + i];
        }
    } else {
        r[mmSQ_PGM_RESOURCES_PS] = w[0];
        r[mmSPI_PS_IN_CONTROL_0] = w[2];
        r[mmSPI_PS_IN_CONTROL_0 + 1] = w[3];
        for (uint32_t i = 0; i < std::min<uint32_t>(w[4], 0x20); i++) r[uint16_t(mmSPI_PS_INPUT_CNTL_0 + i)] = w[5 + i];
        r[mmCB_SHADER_MASK] = w[37];
        r[mmCB_SHADER_CONTROL] = w[38];
        r[mmDB_SHADER_CONTROL] = w[39];
        r[mmSPI_INPUT_Z] = w[40];
    }
    return Regs(r.begin(), r.end());
}

uint32_t reg_value(const Regs& regs, uint16_t index) {  // regs sorted by index
    auto it = std::lower_bound(regs.begin(), regs.end(), index, [](const auto& p, uint16_t i) { return p.first < i; });
    return it != regs.end() && it->first == index ? it->second : 0;
}

std::string family_key(bool vertex, const Regs& block) {
    std::string k(1, char(vertex));
    k.append(reinterpret_cast<const char*>(block.data()), block.size() * sizeof block[0]);
    return k;
}

}  // namespace

uint64_t hash_bytes(const void* bytes, size_t size, uint64_t hash) {
    const auto* p = static_cast<const uint8_t*>(bytes);
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        memcpy(&word, p + i, 8);
        hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
        hash ^= hash >> 32;
    }
    for (; i < size; ++i) hash = (hash ^ p[i]) * 0x100000001B3ull;
    return hash ^ (hash >> 29);
}

std::vector<Recorded> read_manifest(const std::string& path) {
    std::vector<Recorded> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    std::vector<uint8_t> d(size_t(std::max(0L, ftell(f))));
    fseek(f, 0, SEEK_SET);
    const bool ok = d.empty() || fread(d.data(), 1, d.size(), f) == d.size();
    fclose(f);
    if (!ok || d.size() < 8 || memcmp(d.data(), "WSM1", 4) || le32(&d[4]) != 1) return out;
    size_t at = 8;
    std::vector<uint8_t> rec;
    while (at + 9 <= d.size() && d[at] == 1) {
        const uint32_t packed = le32(&d[at + 1]), size = le32(&d[at + 5]);
        if (at + 9 + packed > d.size()) break;
        rec.resize(size);
        uLongf n = size;
        if (uncompress(rec.data(), &n, &d[at + 9], packed) != Z_OK || n != size || size < 26) break;
        at += 9 + packed;
        Recorded r;
        r.vertex = rec[0];
        memcpy(&r.programHash, &rec[1], 8);
        memcpy(&r.programSize, &rec[9], 4);
        r.fetchCompact = rec[21];
        const uint32_t fs = le32(&rec[22]);
        if (26 + size_t(fs) + 4 > size) continue;
        r.fetch.assign(rec.begin() + 26, rec.begin() + 26 + fs);
        const uint32_t count = le32(&rec[26 + fs]);
        size_t o = 26 + fs + 4;
        if (o + size_t(count) * 6 > size) continue;
        r.regs.reserve(count);
        for (uint32_t k = 0; k < count; k++, o += 6) {
            uint16_t i;
            memcpy(&i, &rec[o], 2);
            r.regs.emplace_back(i, le32(&rec[o + 2]));
        }
        std::sort(r.regs.begin(), r.regs.end());
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<Variant> speculate(const std::vector<shader_scan::Program>& programs, const std::vector<Recorded>& recorded,
                               size_t limit) {
    // the programs by (hash, size), in shader_manifest.py's order (sorted), and the ones recorded
    struct Key {
        uint64_t hash;
        uint32_t size;
        bool operator<(const Key& o) const { return hash != o.hash ? hash < o.hash : size < o.size; }
    };
    std::map<Key, size_t> byKey;
    for (size_t i = 0; i < programs.size(); i++) {
        const auto& c = programs[i].code;
        byKey.emplace(Key{hash_bytes(c.data(), c.size()), uint32_t(c.size())}, i);
    }
    std::map<Key, bool> seen;
    for (const Recorded& r : recorded) seen[Key{r.programHash, r.programSize}] = true;
    // the families' register sets, then each recorded state under every family of its stage whose registers it holds
    std::vector<Regs> blocks(programs.size());
    std::map<std::pair<bool, std::vector<uint16_t>>, bool> regsets;
    for (size_t i = 0; i < programs.size(); i++) {
        blocks[i] = own_block(programs[i].vertex, programs[i].words);
        std::vector<uint16_t> set;
        for (const auto& [a, v] : blocks[i]) set.push_back(a);
        regsets[{programs[i].vertex, std::move(set)}] = true;
    }
    std::unordered_map<std::string, std::vector<const Recorded*>> byFamily;
    for (const Recorded& r : recorded)
        for (const auto& [rs, unused] : regsets) {
            if (rs.first != r.vertex) continue;
            Regs fam;
            fam.reserve(rs.second.size());
            for (uint16_t a : rs.second) fam.emplace_back(a, reg_value(r.regs, a));
            auto& list = byFamily[family_key(r.vertex, fam)];
            if (list.size() < limit) list.push_back(&r);
        }
    std::vector<Variant> out;
    for (const auto& [key, i] : byKey) {
        if (seen.count(key)) continue;
        const shader_scan::Program& p = programs[i];
        auto it = byFamily.find(family_key(p.vertex, blocks[i]));
        if (it == byFamily.end()) continue;
        for (const Recorded* t : it->second) {
            std::map<uint16_t, uint32_t> regs(t->regs.begin(), t->regs.end());
            for (const auto& [a, v] : blocks[i]) regs[a] = v;
            regs[uint16_t((p.vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS) + 1)] = uint32_t(p.code.size() >> 3);
            Variant v;
            v.program = i;
            for (const auto& [a, val] : regs)
                if (val && !(a >= 0xC000 && a < 0xD000)) v.regs.emplace_back(a, val);
            v.fetch = t->fetch;
            v.fetchCompact = t->fetchCompact;
            out.push_back(std::move(v));
        }
    }
    return out;
}

}  // namespace shader_list
