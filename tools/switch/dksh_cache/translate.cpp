// dksh_cache translate (docs/shader-cache-from-dump-plan.md, step 4): the shader variants of a shader manifest
// (runtime/src/gfx/deko/shader_manifest.cpp) translated again on the computer, with the same Latte decompiler and
// the same inputs as the console's runtime, into a shadercache_gl.bin (WGS1) whose GLSL sources `dksh_cache build`
// then compiles. The programs come from the player's own dump (tools/switch/shader_manifest.py programs).
//
//     dksh_cache translate <shader_manifest.bin> <programs.bin> <shadercache_gl.bin> [<reference shadercache_dksh.bin>]
//
// programs.bin (WSP1): u32 count, then {u64 hash, u32 size, bytes}: each program's bytes as the GPU gets them.
// With a reference WDK1 file (a console's cache), it also counts how many of the GLSL hashes it has: the GLSL
// matches the console's exactly only when the translation is the same.
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "shader_files.h"
#include "util/helpers/StringBuf.h"

LatteDecompilerShader* FinishDecompiledShader(LatteDecompilerOutput_t& output);
LatteFetchShader* LatteShaderRecompiler_createFetchShader(LatteFetchShader::CacheHash hash, uint32* regs, uint32* code,
                                                          uint32 size);

// what runtime/src/gx2/decompiler_glue.cpp gives the decompiler on the console: its OpenGL mode (deko3d's)
std::unique_ptr<Renderer> g_renderer = std::make_unique<OpenGLRenderer>();
void cemu_shim_log(const std::string& msg) { fprintf(stderr, "[decompiler] %s\n", msg.c_str()); }

namespace {

constexpr uint32_t kNumRegs = 0x10000;

// gfx/deko shaders_dk.cpp hash_bytes
uint64_t hash_bytes(const void* bytes, size_t size, uint64_t hash = 0x9E3779B97F4A7C15ull) {
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

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// gx2::build_fetch_shader (runtime/src/gx2/gx2_resources.cpp) over the recorded bytes of the runtime's compact
// fetch program ("WWFS", count, 16 bytes per attribute, big-endian as the game's memory): keep the two in step
LatteConst::VertexFetchEndianMode default_endian(uint32_t fmt) {
    switch (fmt) {
    case 0: case 1: case 4: case 10: return LatteConst::VertexFetchEndianMode::SWAP_NONE;
    case 2: case 3: case 7: case 8: case 14: case 15: return LatteConst::VertexFetchEndianMode::SWAP_U16;
    default: return LatteConst::VertexFetchEndianMode::SWAP_U32;
    }
}
const uint32_t kRawToFetchFormat[] = {1, 2, 5, 6, 7, 0xD, 0xE, 0xF, 0x10, 0x16, 0x1A, 0x19, 0x1D, 0x1E, 0x1F, 0x20, 0x2F, 0x30, 0x22, 0x23};

LatteFetchShader* compact_fetch_shader(const std::vector<uint8_t>& b) {
    if (b.size() < 16 || be32(b.data()) != 0x57574653) return nullptr;
    const uint32_t n = be32(b.data() + 4);
    if (b.size() < 16 + size_t(n) * 16) return nullptr;
    auto* fs = new LatteFetchShader();
    auto* attrs = new LatteParsedFetchShaderAttribute[n];
    std::vector<std::vector<LatteParsedFetchShaderAttribute*>> groups(16);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e = b.data() + 16 + i * 16;
        uint32_t w0 = be32(e), offset = be32(e + 4), w2 = be32(e + 8), destSel = be32(e + 12);
        uint32_t location = w0 & 0xFF, buffer = (w0 >> 8) & 0xFF, indexType = (w0 >> 16) & 0xFF, endian = w0 >> 24;
        uint32_t fmt = w2 & 0xFFFF, divisor = w2 >> 16;
        LatteParsedFetchShaderAttribute& a = attrs[i];
        a = {};
        a.attributeBufferIndex = (uint8)buffer;
        a.semanticId = (uint8)location;
        a.format = (Latte::E_HWFMT)(kRawToFetchFormat[std::min<uint32_t>(fmt & 0x3F, 19)] & 0x3F);
        a.nfa = (fmt & 0x800) ? 2 : (fmt & 0x100) ? 1 : 0;
        a.isSigned = (fmt & 0x200) ? 1 : 0;
        a.endianSwap = endian == 3 ? default_endian(fmt & 0x3F) : (LatteConst::VertexFetchEndianMode)endian;
        a.fetchType = indexType ? LatteConst::VertexFetchType2::INSTANCE_DATA : LatteConst::VertexFetchType2::VERTEX_DATA;
        a.aluDivisor = indexType ? (sint32)std::max<uint32_t>(divisor, 1) : 0;
        a.offset = offset;
        for (int k = 0; k < 4; k++) a.ds[k] = (destSel >> (24 - 8 * k)) & 7;
        if (buffer < 16) groups[buffer].push_back(&a);
    }
    auto* sorted = new LatteParsedFetchShaderAttribute[n];
    uint32_t k = 0;
    for (uint32_t bi = 0; bi < 16; bi++) {
        if (groups[bi].empty()) continue;
        LatteParsedFetchShaderBufferGroup g{};
        g.attributeBufferIndex = (uint8)bi;
        g.attribCount = (sint8)groups[bi].size();
        g.attrib = &sorted[k];
        g.minOffset = 0xFFFFFFFF;
        for (auto* a : groups[bi]) {
            sorted[k++] = *a;
            g.minOffset = std::min(g.minOffset, a->offset);
            if (a->fetchType == LatteConst::VertexFetchType2::VERTEX_DATA) g.hasVtxIndexAccess = true;
            else g.hasInstanceIndexAccess = true;
        }
        fs->bufferGroups.push_back(g);
        fs->attributeBufferMask |= 1u << bi;
    }
    delete[] attrs;
    fs->key = 0;  // (the console's is the program's address: not part of the GLSL)
    return fs;
}

bool read_all(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    out.resize(size_t(ftell(f)));
    fseek(f, 0, SEEK_SET);
    const bool ok = out.empty() || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

template <class T> T get(const uint8_t* p) {
    T v;
    memcpy(&v, p, sizeof v);
    return v;
}

struct Variant {
    bool vertex = false;
    uint64_t programHash = 0;
    uint32_t programSize = 0;
    bool fetchCompact = false;
    std::vector<uint8_t> fetch;
    std::vector<std::pair<uint16_t, uint32_t>> regs;
};

// WSM1 (runtime/src/gfx/deko/shader_manifest.cpp)
bool read_manifest(const char* path, std::vector<Variant>& out) {
    std::vector<uint8_t> d;
    if (!read_all(path, d) || d.size() < 8 || memcmp(d.data(), "WSM1", 4) || get<uint32_t>(d.data() + 4) != 1) return false;
    size_t at = 8;
    while (at + 9 <= d.size() && d[at] == 1) {
        const uint32_t packed = get<uint32_t>(d.data() + at + 1), size = get<uint32_t>(d.data() + at + 5);
        if (at + 9 + packed > d.size()) break;
        std::vector<uint8_t> r(size);
        uLongf outSize = size;
        if (uncompress(r.data(), &outSize, d.data() + at + 9, packed) != Z_OK || outSize != size) break;
        at += 9 + packed;
        const uint8_t* p = r.data();
        Variant v;
        v.vertex = p[0];
        v.programHash = get<uint64_t>(p + 1);
        v.programSize = get<uint32_t>(p + 9);
        v.fetchCompact = p[21];
        const uint32_t fs = get<uint32_t>(p + 22);
        size_t o = 26;
        v.fetch.assign(p + o, p + o + fs);
        o += fs;
        const uint32_t count = get<uint32_t>(p + o);
        o += 4;
        for (uint32_t i = 0; i < count && o + 6 <= r.size(); i++, o += 6) v.regs.emplace_back(get<uint16_t>(p + o), get<uint32_t>(p + o + 2));
        out.push_back(std::move(v));
    }
    return true;
}

// WSP1 (tools/switch/shader_manifest.py programs)
bool read_programs(const char* path, std::map<std::pair<uint64_t, uint32_t>, std::vector<uint8_t>>& out) {
    std::vector<uint8_t> d;
    if (!read_all(path, d) || d.size() < 8 || memcmp(d.data(), "WSP1", 4)) return false;
    const uint32_t count = get<uint32_t>(d.data() + 4);
    size_t at = 8;
    for (uint32_t i = 0; i < count && at + 12 <= d.size(); i++) {
        const uint64_t h = get<uint64_t>(d.data() + at);
        const uint32_t n = get<uint32_t>(d.data() + at + 8);
        if (at + 12 + n > d.size()) return false;
        out[{h, n}].assign(d.data() + at + 12, d.data() + at + 12 + n);
        at += 12 + n;
    }
    return true;
}

}  // namespace

int translate(const char* manifestPath, const char* programsPath, const char* outPath, const char* referencePath) {
    std::vector<Variant> variants;
    if (!read_manifest(manifestPath, variants)) {
        fprintf(stderr, "%s: not a shader manifest (WSM1)\n", manifestPath);
        return 1;
    }
    std::map<std::pair<uint64_t, uint32_t>, std::vector<uint8_t>> programs;
    if (!read_programs(programsPath, programs)) {
        fprintf(stderr, "%s: not a programs file (WSP1)\n", programsPath);
        return 1;
    }
    std::unordered_set<uint64_t> reference;
    if (referencePath) {
        std::vector<uint8_t> d;
        std::vector<gfxdk::DkshRecord> recs;
        uint64_t uamId = 0;
        std::string error;
        if (!read_all(referencePath, d) || !gfxdk::read_wdk1(d, &recs, &uamId, &error)) {
            fprintf(stderr, "%s: %s\n", referencePath, error.empty() ? "cannot read" : error.c_str());
            return 1;
        }
        for (auto& r : recs) reference.insert(r.glslHash);
    }

    FILE* out = fopen(outPath, "wb");
    if (!out) {
        fprintf(stderr, "cannot write %s\n", outPath);
        return 1;
    }
    fwrite("WGS1", 1, 4, out);
    std::unordered_set<uint64_t> written;
    size_t noProgram = 0, failed = 0, translated = 0, inReference = 0, cubeFixed = 0;
    std::vector<uint32_t> regs(kNumRegs);
    for (Variant& v : variants) {
        auto it = programs.find({v.programHash, v.programSize});
        if (it == programs.end()) {
            noProgram++;
            continue;
        }
        std::fill(regs.begin(), regs.end(), 0);
        for (auto& [i, value] : v.regs) regs[i] = value;
        LatteFetchShader* fetch = nullptr;
        if (v.vertex) {
            std::vector<uint8_t> fb = v.fetch;
            fetch = v.fetchCompact ? compact_fetch_shader(fb)
                                   : LatteShaderRecompiler_createFetchShader(0, regs.data(), reinterpret_cast<uint32*>(fb.data()),
                                                                            uint32_t(fb.size()));
            if (!fetch) {
                failed++;
                continue;
            }
        }
        std::vector<uint8_t> program = it->second;  // (the decompiler may read it as words: an aligned copy)
        const uint64_t base = v.programHash ^ (v.vertex ? 0x1111 : 0x2222);
        auto decompile = [&](std::string& glsl) {
            LatteShader_UpdatePSInputs(regs.data());
            LatteDecompilerOptions options;
            LatteDecompilerOutput_t output{};
            if (v.vertex)
                LatteDecompiler_DecompileVertexShader(base, regs.data(), program.data(), v.programSize, fetch, options,
                                                      &output);
            else
                LatteDecompiler_DecompilePixelShader(base, regs.data(), program.data(), v.programSize, options, &output);
            if (!output.shader || output.shader->hasError || !output.shader->strBuf_shaderSource) return false;
            glsl = FinishDecompiledShader(output)->strBuf_shaderSource->c_str();
            return true;
        };
        std::string glsl;
        if (!decompile(glsl)) {
            failed++;
            continue;
        }
        // a guessed state (shader_manifest.py speculate) takes its textures from another program's: a program that
        // reads a unit as a cube map (SET_CUBEMAP_INDEX) may find a 2D texture there, and the GLSL uses a
        // cubeMapArrayIndexN it never declares (uam rejects it). The game binds a cube map to that unit, so the
        // variant it makes has the unit's dimension set to one: translated again that way
        bool cube = false;
        for (uint32_t unit = 0; unit < Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE; unit++) {
            const std::string name = "cubeMapArrayIndex" + std::to_string(unit);
            if (glsl.find(name + " =") == std::string::npos || glsl.find("float " + name + " ") != std::string::npos)
                continue;
            const uint32_t word0 = (v.vertex ? Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS
                                             : Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS) + unit * 7;
            cube = true;
            for (auto& [i, value] : v.regs)
                if (i == word0) value = (value & ~7u) | uint32_t(Latte::E_DIM::DIM_CUBEMAP);
            if (!std::any_of(v.regs.begin(), v.regs.end(), [&](auto& r) { return r.first == word0; }))
                v.regs.emplace_back(word0, uint32_t(Latte::E_DIM::DIM_CUBEMAP));
        }
        if (cube) {
            std::fill(regs.begin(), regs.end(), 0);
            for (auto& [i, value] : v.regs) regs[i] = value;
            if (!decompile(glsl)) {
                failed++;
                continue;
            }
            cubeFixed++;
        }
        const uint64_t hash = hash_bytes(glsl.data(), glsl.size(), v.vertex ? 0x1111 : 0x2222);
        translated++;
        if (!written.insert(hash).second) continue;
        if (reference.count(hash)) inReference++;
        // record 1 of shadercache_gl.bin (gfx/deko shaders_dk.cpp cache_gl_source)
        uLongf packed = compressBound(glsl.size());
        std::vector<uint8_t> record(18 + packed);
        if (compress2(record.data() + 18, &packed, (const Bytef*)glsl.data(), glsl.size(), 6) != Z_OK) continue;
        const uint32_t sizes[2] = {uint32_t(packed), uint32_t(glsl.size())};
        record[0] = 1;
        record[1] = v.vertex;
        memcpy(record.data() + 2, &hash, 8);
        memcpy(record.data() + 10, sizes, 8);
        fwrite(record.data(), 1, 18 + packed, out);
    }
    fclose(out);
    printf("%zu variants: %zu translated (%zu distinct GLSL sources written to %s), %zu without their program in the "
           "dump, %zu failed; %zu guessed with a cube map unit\n",
           variants.size(), translated, written.size(), outPath, noProgram, failed, cubeFixed);
    if (referencePath)
        printf("%zu of the %zu sources are in %s (the console's translation gave the same GLSL)\n", inReference,
               written.size(), referencePath);
    return failed ? 2 : 0;
}
