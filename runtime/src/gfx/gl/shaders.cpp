// Latte microcode -> Cemu GLSL (OpenGL flavour) -> GL shader objects and linked programs.
// Shader variants are keyed exactly as in gfx/vulkan/shaders.cpp.
#include "shaders.h"

#include <zlib.h>
#include <malloc.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>
#include <vector>

#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "gx2/gx2.h"
#include "platform/host.h"
#include "ppc.h"
#include "runtime.h"
#include "util/helpers/StringBuf.h"
#include "varyings.h"

LatteDecompilerShader* FinishDecompiledShader(LatteDecompilerOutput_t& output);
LatteFetchShader* LatteShaderRecompiler_createFetchShader(LatteFetchShader::CacheHash hash, uint32* regs, uint32* code,
                                                          uint32 size);

namespace gfxgl {
using Latte::REGADDR;
namespace {
std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders;
std::unordered_map<uint64_t, LatteFetchShader*> fetchShaders;
std::unordered_map<uint64_t, std::unique_ptr<Program>> linked;  // vertex and pixel GLSL hashes -> program
// Shader sources and objects (round 25). Mesa keeps a compiled shader object's whole intermediate code
// until the object is deleted: ~460 KB each for these shaders (measured on desktop Mesa with the
// game's GLSL), and the renderer kept one per source for the session so that new pairs could be linked.
// With the shader cache at ~1,900 sources that was ~870 MB of the Switch's heap: 89 MiB were left after
// startup and the forest ran out (round 24 crash report). Now each source is kept compressed (~2 KB),
// its object is compiled when a link needs it and deleted after (a few recent ones are kept), and a
// link detaches its shaders, so deleting them frees the memory.
struct Source {
    std::vector<uint8_t> packed;  // zlib GLSL, as in the cache file
    uint32_t size = 0;
    bool vertex = false;
};
std::unordered_map<uint64_t, Source> sources;  // GLSL hash -> source
struct LiveObject {
    GLuint obj = 0;
    uint64_t used = 0;
};
std::unordered_map<uint64_t, LiveObject> live;  // GLSL hash -> compiled object, the most recently used
uint64_t liveClock = 0;
size_t liveLimit = 256;   // during startup's links; then kLiveObjects
// (round 26: 96, was 32: a new pair of known shaders recompiled both, and doors stalled for it)
constexpr size_t kLiveObjects = 96;
size_t objectCompiles = 0;  // compiles of sources into objects (startup and links)
struct ProgramHash { uint64_t hash = 0, frame = ~uint64_t{0}, sample = 0; };
std::unordered_map<uint64_t, ProgramHash> programHashes;
std::unordered_map<uint64_t, uint32_t> textureUnits;  // program hash -> texture units it samples (bit t: unit t)

// shader cache file: magic, then records {1, vertex, hash, packed size, size, zlib GLSL} and
// {2, vertex hash, pixel hash}; a record cut short by a crash ends the file
constexpr char kCachePath[] = "shadercache_gl.bin";
constexpr uint8_t kCacheMagic[4] = {'W', 'G', 'S', '1'};
FILE* cacheFile = nullptr;

uint64_t pair_key(uint64_t vs, uint64_t ps) { return vs * 0x9E3779B97F4A7C15ull ^ ps; }

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

// 32 words spread over the program
uint64_t sample_words(const uint8_t* p, uint32_t size) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    const uint32_t words = size / 4, step = std::max<uint32_t>(words / 32, 1);
    for (uint32_t i = 0; i < words; i += step) {
        uint32_t w;
        memcpy(&w, p + size_t(i) * 4, 4);
        h = (h ^ w) * 0x100000001B3ull;
    }
    return h;
}

// Program memory can be reused for another program between frames, so a program's hash is checked
// once per frame: by 32 sampled words, and fully every 64 frames (staggered by address)
uint64_t program_hash_at(ProgramHash& entry, uint32_t address, uint32_t size, uint64_t frame) {
    if (entry.frame != frame) {
        const uint8_t* bytes = ppc_ptr(address);
        uint64_t sample = sample_words(bytes, size);
        if (entry.frame == ~uint64_t{0} || sample != entry.sample || ((frame + ((address * 0x9E3779B1u) >> 20)) & 63) == 0)
            entry.hash = hash_bytes(bytes, size);
        entry.sample = sample;
        entry.frame = frame;
    }
    return entry.hash;
}
uint64_t program_hash(uint32_t address, uint32_t size, uint64_t frame) {
    return program_hash_at(programHashes[(uint64_t(address) << 32) | size], address, size, frame);
}

// four independent multiply chains (one chain is latency-bound); for in-memory keys only (the shader
// cache file keys sources by hash_bytes, which must not change)
uint64_t hash_words4(const uint32_t* w, size_t count, uint64_t seed) {
    uint64_t h[4] = {seed, seed ^ 0xC2B2AE3D27D4EB4Full, seed ^ 0x165667B19E3779F9ull, seed ^ 0x27D4EB2F165667C5ull};
    size_t i = 0;
    for (; i + 8 <= count; i += 8)
        for (int l = 0; l < 4; l++) {
            uint64_t v = uint64_t(w[i + l * 2]) | uint64_t(w[i + l * 2 + 1]) << 32;
            h[l] = (h[l] ^ v) * 0xFF51AFD7ED558CCDull;
            h[l] ^= h[l] >> 32;
        }
    uint64_t r = h[0] ^ (h[1] * 31) ^ (h[2] * 1009) ^ (h[3] * 65537);
    for (; i < count; i++) r = (r ^ w[i]) * 0x100000001B3ull;
    return r ^ (r >> 29) ^ count;
}

// the registers the GLSL translation of one stage reads, other than the texture units'. The Vulkan
// renderer keys both stages on the union (gfx/vulkan/shaders.cpp state_hash); keyed that way, a
// pixel shader was translated again for every vertex format it was drawn with, and both stages for
// the stale input slots past the active count: thousands of identical translations in new areas.
// Readers (Cemu's decompiler and latte_support.cpp):
// - both: the PS input table (SPI_PS_IN_CONTROL_0/1, and SPI_PS_INPUT_CNTL_n for the
//   SPI_PS_IN_CONTROL_0 & 0x3F active inputs only), the primitive type, SPI_INTERP_CONTROL_0,
//   streamout, VGT_GS_MODE, SQ_CONFIG, PA_CL_VTE_CNTL, PA_CL_CLIP_CNTL
// - vertex only: SQ_VTX_SEMANTIC_n (attribute import), SPI_VS_OUT_ID_n (parameter exports)
// - pixel only: CB_SHADER_MASK/CONTROL (color exports), SX_ALPHA_TEST_CONTROL, and the color
//   buffer state (Pixel branch of the analyzer)
uint64_t state_hash(const uint32_t* regs, uint64_t hash, bool vertex) {
    std::array<uint32_t, 112> state;
    size_t count = 0;
    auto append = [&](const uint32_t* words, size_t length) {
        memcpy(state.data() + count, words, length * sizeof(uint32_t));
        count += length;
    };
    auto put = [&](uint32_t first, uint32_t length) { append(regs + first, length); };
    put(mmSPI_PS_IN_CONTROL_0, 2);
    put(mmSPI_PS_INPUT_CNTL_0, std::min<uint32_t>(regs[mmSPI_PS_IN_CONTROL_0] & 0x3F, 32));
    uint32_t primitiveState[] = {regs[REGADDR::VGT_PRIMITIVE_TYPE] & 0x3F, regs[mmSPI_INTERP_CONTROL_0] & (1u << 1),
                                 regs[mmVGT_STRMOUT_EN]};
    append(primitiveState, 3);
    if (regs[mmVGT_STRMOUT_EN])
        for (uint32_t buffer = 0; buffer < 4; ++buffer) put(mmVGT_STRMOUT_VTX_STRIDE_0 + buffer * 4, 1);
    put(REGADDR::VGT_GS_MODE, 1);
    put(REGADDR::SQ_CONFIG, 1);
    uint32_t transformState[] = {regs[REGADDR::PA_CL_VTE_CNTL] & 0x3F, regs[REGADDR::PA_CL_CLIP_CNTL] & (1u << 19)};
    append(transformState, 2);
    if (vertex) {
        put(mmSQ_VTX_SEMANTIC_0, 32);
        put(mmSPI_VS_OUT_ID_0, 10);
        put(mmSPI_VS_OUT_CONFIG, 1);
        put(mmPA_CL_VS_OUT_CNTL, 1);
    } else {
        put(mmCB_SHADER_MASK, 1);
        put(mmCB_SHADER_CONTROL, 1);
        put(mmDB_SHADER_CONTROL, 1);
        put(mmSPI_INPUT_Z, 1);
        put(REGADDR::SX_ALPHA_TEST_CONTROL, 1);
        uint32_t depth = regs[REGADDR::DB_DEPTH_CONTROL] & 0x83;
        append(&depth, 1);
        put(REGADDR::CB_COLOR_CONTROL, 1);
        put(REGADDR::CB_TARGET_MASK, 1);
        put(mmCB_COLOR0_INFO, 8);
    }
    return hash_words4(state.data(), count, hash);
}

// what the GLSL translation reads of texture unit t: the dimension (word0 DIM) and whether the
// format is an integer one (word4 NUM_FORMAT_ALL). Nothing else of the texture or sampler
// registers reaches the GLSL (the depth compare comes from the instructions; the data format and
// address only from Metal's framebuffer-fetch detection).
inline uint32_t texture_unit_state(const uint32_t* regs, bool vertex, uint32_t t) {
    const auto* w = regs + (vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS) + t * 7;
    return (w[0] & 7) | (w[4] & 0x300);
}

// the texture state of the units in mask (bit t: unit t)
uint64_t texture_state_hash(const uint32_t* regs, bool vertex, uint32_t mask) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ mask;
    for (; mask; mask &= mask - 1) {
        uint32_t t = __builtin_ctz(mask);
        h = (h ^ (texture_unit_state(regs, vertex, t) | t << 16)) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    return h;
}

void dump_failure(const char* what, uint64_t key, const std::string& source, const std::string& log) {
    static int dumped = 0;
    LOG("[gl] %s %016llx failed: %s", what, (unsigned long long)key, log.c_str());
    if (dumped++ >= 16) return;
    char path[64];
    snprintf(path, sizeof path, "shaderfail_%016llx.glsl", (unsigned long long)key);
    if (FILE* f = fopen(path, "w")) {
        fprintf(f, "%s\n/*\n%s\n*/\n", source.c_str(), log.c_str());
        fclose(f);
    }
}

// Records reach the file from a background thread, about once a second: on the Switch every write
// and flush to the SD card took ~4 ms, and the render thread paid it for each new translation (round
// 14: 87 translations in one frame took 389 ms instead of ~20). Records keep their order (record 3
// names the GLSL of an earlier record 1); a session that ends within the second loses only those.
std::mutex cacheMutex;
std::condition_variable cacheCv;
std::vector<uint8_t> cachePending;

void cache_writer() {
    host::set_thread_name("shader cache writer");  // (host::start_thread: off the main thread's core)
    int logged = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(cacheMutex);
            cacheCv.wait(lk, [] { return !cachePending.empty(); });
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));  // the rest of a burst goes in the same write
        std::vector<uint8_t> batch;
        {
            std::lock_guard<std::mutex> lk(cacheMutex);
            batch.swap(cachePending);
        }
        const uint64_t start = now_ns();
        fwrite(batch.data(), 1, batch.size(), cacheFile);
        fflush(cacheFile);
        if (logged < 40) {
            logged++;
            LOG("[gl] shader cache: saved %.1f KB in %.1f ms (writer thread)", double(batch.size()) / 1024.0,
                double(now_ns() - start) / 1e6);
        }
    }
}

void cache_write(const void* data, size_t size) {
    if (!cacheFile) return;
    const auto* bytes = static_cast<const uint8_t*>(data);
    bool first;
    {
        std::lock_guard<std::mutex> lk(cacheMutex);
        first = cachePending.empty();
        cachePending.insert(cachePending.end(), bytes, bytes + size);
    }
    if (first) cacheCv.notify_one();
}

void cache_shader(uint64_t hash, bool vertex, const std::string& glsl) {
    if (!cacheFile) return;
    uLongf packed = compressBound(glsl.size());
    std::vector<uint8_t> record(18 + packed);
    if (compress2(record.data() + 18, &packed, (const Bytef*)glsl.data(), glsl.size(), 6) != Z_OK) return;
    uint32_t sizes[2] = {uint32_t(packed), uint32_t(glsl.size())};
    record[0] = 1;
    record[1] = vertex;
    memcpy(record.data() + 2, &hash, 8);
    memcpy(record.data() + 10, sizes, 8);
    cache_write(record.data(), 18 + packed);
}

// Translations (record 3: {3, u32 payload size, payload}): what a draw uses of a translated shader,
// so later sessions skip Cemu's decompiler for keys seen before (each costs ~0.25 ms on the
// Switch; turning the camera in a new place needed 200+ in one frame). The GLSL itself is record 1.
struct ByteWriter {
    std::vector<uint8_t> b;
    template <class T> void put(const T& v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        b.insert(b.end(), p, p + sizeof v);
    }
};
struct ByteReader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (size_t(end - p) < sizeof v) { ok = false; return v; }
        memcpy(&v, p, sizeof v);
        p += sizeof v;
        return v;
    }
};
static_assert(std::is_trivially_copyable_v<LatteDecompilerShaderResourceMapping>);

void cache_translation(const Shader* sh, uint64_t base, uint32_t units) {
    if (!cacheFile || !sh->dec) return;
    const LatteDecompilerShader* d = sh->dec;
    ByteWriter w;
    w.put<uint8_t>(sh->vertex);
    w.put(sh->key);
    w.put(base);
    w.put(sh->glslHash);
    w.put(units);
    w.put<uint32_t>(sh->registerCount);
    w.put<uint32_t>(d->pixelColorOutputMask);
    w.put<uint8_t>(d->textureUnitListCount);
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) w.put<uint8_t>(d->textureUnitList[t]);
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) w.put<uint16_t>(d->textureUnitSamplerAssignment[t]);
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) w.put<uint8_t>(d->textureUsesDepthCompare[t]);
    w.put(sh->mapping);
    w.put<uint32_t>(uint32_t(d->list_remappedUniformEntries.size()));
    w.put<uint32_t>(uint32_t(d->list_remappedUniformEntries_register.size()));
    for (auto& e : d->list_remappedUniformEntries_register) {
        w.put<uint32_t>(e.indexOffset);
        w.put<uint32_t>(e.mappedIndexOffset);
    }
    w.put<uint32_t>(uint32_t(d->list_remappedUniformEntries_bufferGroups.size()));
    for (auto& g : d->list_remappedUniformEntries_bufferGroups) {
        w.put<uint16_t>(g.bufferId);
        w.put<uint16_t>(g.kcacheBankIdOffset);
        w.put<uint32_t>(uint32_t(g.entries.size()));
        for (auto& e : g.entries) {
            w.put<uint16_t>(e.indexOffset);
            w.put<uint16_t>(e.mappedIndexOffset);
        }
    }
    std::vector<uint8_t> record(5);
    record[0] = 3;
    const uint32_t size = uint32_t(w.b.size());
    memcpy(record.data() + 1, &size, 4);
    record.insert(record.end(), w.b.begin(), w.b.end());
    cache_write(record.data(), record.size());
}

// a shader rebuilt from a record 3, or false if the record is unusable (its GLSL is not compiled)
bool load_translation(const uint8_t* payload, uint32_t size) {
    ByteReader r{payload, payload + size};
    const bool vertex = r.get<uint8_t>() != 0;
    const uint64_t key = r.get<uint64_t>(), base = r.get<uint64_t>(), glslHash = r.get<uint64_t>();
    const uint32_t units = r.get<uint32_t>();
    if (!r.ok || !sources.count(glslHash) || shaders.count(key)) return false;
    auto sh = std::make_unique<Shader>();
    sh->key = key;
    sh->vertex = vertex;
    sh->glslHash = glslHash;
    sh->compiled = true;  // (it compiled when it was recorded; a link that cannot compile it fails)
    sh->registerCount = r.get<uint32_t>();
    auto* d = new LatteDecompilerShader(vertex ? LatteConst::ShaderType::Vertex : LatteConst::ShaderType::Pixel);
    d->pixelColorOutputMask = r.get<uint32_t>();
    d->textureUnitListCount = r.get<uint8_t>();
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) d->textureUnitList[t] = r.get<uint8_t>();
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) d->textureUnitSamplerAssignment[t] = r.get<uint16_t>();
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) d->textureUsesDepthCompare[t] = r.get<uint8_t>() != 0;
    sh->mapping = r.get<LatteDecompilerShaderResourceMapping>();
    d->list_remappedUniformEntries.resize(r.get<uint32_t>());
    const uint32_t nReg = r.get<uint32_t>();
    for (uint32_t i = 0; i < nReg && r.ok; i++) {
        LatteFastAccessRemappedUniformEntry_register_t e;
        e.indexOffset = r.get<uint32_t>();
        e.mappedIndexOffset = r.get<uint32_t>();
        d->list_remappedUniformEntries_register.push_back(e);
    }
    const uint32_t nGroups = r.get<uint32_t>();
    for (uint32_t i = 0; i < nGroups && r.ok; i++) {
        const uint16_t bufferId = r.get<uint16_t>(), bank = r.get<uint16_t>();
        d->list_remappedUniformEntries_bufferGroups.emplace_back(bufferId, bank);
        const uint32_t n = r.get<uint32_t>();
        for (uint32_t k = 0; k < n && r.ok; k++) {
            LatteFastAccessRemappedUniformEntry_buffer_t e;
            e.indexOffset = r.get<uint16_t>();
            e.mappedIndexOffset = r.get<uint16_t>();
            d->list_remappedUniformEntries_bufferGroups.back().entries.push_back(e);
        }
    }
    if (!r.ok || r.p != r.end || nReg > 4096 || nGroups > 64) {
        delete d;
        return false;
    }
    sh->dec = d;
    textureUnits.emplace(base, units);
    shaders.emplace(key, std::move(sh));
    return true;
}

void cache_program(uint64_t vs, uint64_t ps) {
    uint8_t record[17] = {2};
    memcpy(record + 1, &vs, 8);
    memcpy(record + 9, &ps, 8);
    cache_write(record, sizeof record);
}

bool uniform_blocks_on_impl() {
    const char* e = getenv("WWHD_GL_UNIFORM_BLOCKS");
    return !(e && *e == '0');
}

// The decompiler's uniform-variable section is "#ifdef VULKAN <ufBlock> #else <loose uniforms>
// #endif", the second such section of the source (after the macros). Its OpenGL branch becomes one
// std140 block, ufBlockVS or ufBlockPS (the names differ: blocks of the same name in two stages are
// one block to the linker); the offsets are read back from the linked program. With draw batching
// the block holds an array of structs, one per draw, and #defines turn each variable into the
// current draw's member; every shader also gets the draw-index plumbing (vertex attribute and flat
// varying), so any vertex shader links with any pixel shader. The shader cache keeps the
// decompiler's text (and its hash), so the cache stays valid however this is set.
namespace {
size_t std140_size(const std::string& type) {
    if (type == "float" || type == "int" || type == "uint" || type == "bool") return 4;
    if (type == "vec2" || type == "ivec2" || type == "uvec2") return 8;
    return 16;  // vec3/vec4 and their integer forms (vec3 occupies 16 in an array or before a vec4)
}
}  // namespace

std::string uniform_vars_to_block(const std::string& glsl, bool vertex) {
    const bool batch = draw_batching_on();
    const char* stage = vertex ? "VS" : "PS";
    std::string out = glsl;
    // the uniform section
    const char* kIf = "#ifdef VULKAN\r\n";
    size_t first = out.find(kIf);
    size_t section = first == std::string::npos ? std::string::npos : out.find(kIf, first + 1);
    size_t els = section == std::string::npos ? std::string::npos : out.find("#else\r\n", section);
    size_t end = section == std::string::npos ? std::string::npos : out.find("#endif\r\n", section);
    if (els != std::string::npos && end != std::string::npos && els < end) {
        const size_t body = els + 7;
        struct Member { std::string type, name, array; };
        std::vector<Member> members;
        bool ok = body < end;
        for (size_t pos = body; ok && pos < end;) {
            size_t nl = out.find("\r\n", pos);
            if (nl == std::string::npos || nl > end || out.compare(pos, 8, "uniform ") != 0 || out[nl - 1] != ';') {
                ok = false;
                break;
            }
            std::string decl = out.substr(pos + 8, nl - 1 - (pos + 8));  // "type name[n]"
            size_t sp = decl.find(' ');
            if (sp == std::string::npos) { ok = false; break; }
            Member m{decl.substr(0, sp), decl.substr(sp + 1), ""};
            if (size_t br = m.name.find('['); br != std::string::npos) {
                m.array = m.name.substr(br);
                m.name = m.name.substr(0, br);
            }
            members.push_back(m);
            pos = nl + 2;
        }
        // the game's own uniform blocks (per-stage limit 14 on nouveau with this one)
        size_t blocks = 0;
        for (size_t at = out.find("UNIFORM_BUFFER_LAYOUT("); at != std::string::npos; at = out.find("UNIFORM_BUFFER_LAYOUT(", at + 1))
            if (at < 8 || out.compare(at - 8, 8, "#define ") != 0) blocks++;
        if (ok && !members.empty() && blocks <= 12) {
            const GLuint binding = vertex ? kUniformVarBindingVS : kUniformVarBindingPS;
            std::string text;
            char line[256];
            if (!batch) {
                snprintf(line, sizeof line, "layout(binding = %u, std140) uniform ufBlock%s\r\n{\r\n", binding, stage);
                text = line;
                for (auto& m : members) text += m.type + " " + m.name + m.array + ";\r\n";
                text += "};\r\n";
            } else {
                // std140 size of the struct, for the array length (a block is at most 64 KB)
                size_t size = 0;
                for (auto& m : members) {
                    size_t one = std140_size(m.type), count = 1, align = one == 16 ? 16 : one;
                    if (!m.array.empty()) {
                        count = std::max<size_t>(1, strtoul(m.array.c_str() + 1, nullptr, 10));
                        one = 16;
                        align = 16;
                    }
                    size = (size + align - 1) / align * align + one * count;
                }
                const size_t stride = std::max<size_t>(16, (size + 15) / 16 * 16);
                const size_t capacity = std::max<size_t>(1, std::min<size_t>(256, 65536 / stride));
                text = std::string("struct WwhdUf") + stage + "\r\n{\r\n";
                for (auto& m : members) text += m.type + " " + m.name + m.array + ";\r\n";
                text += "};\r\n";
                snprintf(line, sizeof line, "layout(binding = %u, std140) uniform ufBlock%s\r\n{\r\nWwhdUf%s wwhd_uf%s[%zu];\r\n};\r\n",
                         binding, stage, stage, stage, capacity);
                text += line;
                for (auto& m : members) text += "#define " + m.name + " wwhd_uf" + stage + "[WWHD_DRAW]." + m.name + "\r\n";
            }
            out = out.substr(0, body) + text + out.substr(end);
        }
    }
    if (!batch) return out;
    // the draw index: declared before main, set at its start
    size_t main = out.find("void main()\r\n{\r\n");
    if (main == std::string::npos) return out;
    char decl[256];
    if (vertex)
        snprintf(decl, sizeof decl,
                 "layout(location = %u) in uint wwhd_drawIndex;\r\nlayout(location = %u) flat out int wwhd_drawPS;\r\n"
                 "#define WWHD_DRAW int(wwhd_drawIndex)\r\n",
                 kDrawIndexAttrib, kDrawIndexVarying);
    else
        snprintf(decl, sizeof decl, "layout(location = %u) flat in int wwhd_drawPS;\r\n#define WWHD_DRAW wwhd_drawPS\r\n",
                 kDrawIndexVarying);
    out.insert(main, decl);
    if (vertex) {
        main = out.find("void main()\r\n{\r\n") + 15;
        out.insert(main, "wwhd_drawPS = WWHD_DRAW;\r\n");
    }
    return out;
}

size_t g_blockShaders = 0;  // compiled with their uniforms in a block (for the log)

// Vertex positions are declared invariant, as the Vulkan (invariant gl_Position) and Metal
// ([[invariant]]) renderers do. The game draws the same mesh with several programs (depth passes,
// then shading with an EQUAL or LEQUAL depth test, decals, outlines); without invariance the GLSL
// compiler may evaluate each program's position differently (fused multiply-adds, reordering), so
// the depths differ in the last bits and the later pass loses the depth test on whole triangles or
// in stripes. WWHD_GL_INVARIANT=0 turns it off (for comparison).
bool g_invariantPosition = [] {
    const char* e = getenv("WWHD_GL_INVARIANT");
    return !(e && strcmp(e, "0") == 0);
}();

// Near-plane clipping by a user clip distance (round 27; off unless WWHD_GL_NEAR_CLIP=1 since round 28: it
// did not fix the searchlights, and the round 27 runs on the Switch stalled at texture uploads, waiting for
// Mesa's GL thread up to 50 times longer than rounds 24-26). Mesa 20.1's nouveau has the hardware clip
// geometry at w = 0 only (VIEW_VOLUME_CLIP_CTRL without frustum clipping) and leaves the near plane to
// a per-pixel depth test, so a triangle reaching behind the camera is drawn from vertices just in front
// of it, at huge screen coordinates. The Forsaken Fortress searchlights are light volumes the camera
// stands inside when a beam comes near Link: on the Switch they came out as long slabs or missing
// pools of light. A clip distance (z + w: the near plane of the GL clip space, and just behind the one
// of the DX clip space) makes the hardware cut those triangles at the plane. draw.cpp enables it unless
// the game turned near clipping off.
uint32_t g_nearClipMissing = 0;  // linked programs whose vertex shader does not write it (logged)
bool near_clip_on_impl() {
    static const bool on = [] {
        const char* e = getenv("WWHD_GL_NEAR_CLIP");
        return e && *e == '1';
    }();
    return on;
}
constexpr const char* kNearClipWrite = "gl_ClipDistance[0] = gl_Position.z + gl_Position.w";
std::string with_near_clip(std::string glsl) {
    // Cemu's vertex shaders redeclare gl_PerVertex and set the position through SET_POSITION
    // (both edits or neither: link() enables the plane only for vertex shaders that write it)
    const size_t block = glsl.find("out gl_PerVertex");
    const size_t pos = block == std::string::npos ? std::string::npos : glsl.find("vec4 gl_Position;", block);
    const size_t define = glsl.find("#define SET_POSITION(_v) ");
    const size_t lineEnd = define == std::string::npos ? std::string::npos : glsl.find_first_of("\r\n", define);
    if (pos == std::string::npos || lineEnd == std::string::npos || glsl.find("gl_ClipDistance") != std::string::npos)
        return glsl;
    // the later edit first, so the earlier offset stays right (Cemu puts the define before the block)
    const std::string decl = "\r\n\tfloat gl_ClipDistance[1];", write = "; " + std::string(kNearClipWrite);
    if (lineEnd > pos) {
        glsl.insert(lineEnd, write);
        glsl.insert(pos + 17, decl);
    } else {
        glsl.insert(pos + 17, decl);
        glsl.insert(lineEnd, write);
    }
    return glsl;
}

std::string with_invariant_position(std::string glsl) {
    const size_t main = glsl.find("void main()");
    if (main != std::string::npos) glsl.insert(main, "invariant gl_Position;\r\n");
    return glsl;
}

GLuint compile_glsl(bool vertex, const std::string& glsl, std::string* error) {
    R.perf.compiled++;
    GLuint obj = glCreateShader(vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
    const char* text = glsl.c_str();
    glShaderSource(obj, 1, &text, nullptr);
    glCompileShader(obj);
    GLint ok = 0;
    glGetShaderiv(obj, GL_COMPILE_STATUS, &ok);
    if (ok) return obj;
    char log[4096] = {};
    glGetShaderInfoLog(obj, sizeof log, nullptr, log);
    if (error) *error = log;
    glDeleteShader(obj);
    return 0;
}

GLuint compile_vertex(const std::string& glsl, std::string* error);
GLuint compile(bool vertex, const std::string& source, std::string* error) {
    std::string glsl = uniform_blocks_on() ? uniform_vars_to_block(source, vertex) : source;
    if (glsl.find(vertex ? "uniform ufBlockVS" : "uniform ufBlockPS") != std::string::npos) g_blockShaders++;
    if (vertex && near_clip_on_impl()) {
        std::string clipped = with_near_clip(glsl);
        if (clipped.size() != glsl.size()) {
            std::string log;
            if (GLuint obj = compile_vertex(clipped, &log)) return obj;
            // a shader the clip distance breaks: compiled as it was (link() then leaves the plane off
            // for its programs), said once
            static bool said = false;
            if (!said) {
                said = true;
                LOG("[gl] a vertex shader does not compile with the near-plane clip distance; compiling it "
                    "without: %s", log.c_str());
            }
        }
    }
    return vertex ? compile_vertex(glsl, error) : compile_glsl(false, glsl, error);
}
GLuint compile_vertex(const std::string& glsl, std::string* error) {
    if (g_invariantPosition) {
        std::string log;
        if (GLuint obj = compile_glsl(true, with_invariant_position(glsl), &log)) return obj;
        // a driver that rejects the declaration: without it from now on (as before), said once
        GLuint obj = compile_glsl(true, glsl, error);
        if (obj) {
            g_invariantPosition = false;
            LOG("[gl] the driver rejects invariant vertex positions; compiling without them: %s", log.c_str());
        }
        return obj;
    }
    return compile_glsl(true, glsl, error);
}

std::vector<uint8_t> pack_glsl(const std::string& glsl) {
    uLongf packed = compressBound(glsl.size());
    std::vector<uint8_t> out(packed);
    if (compress2(out.data(), &packed, (const Bytef*)glsl.data(), glsl.size(), 6) != Z_OK) return {};
    out.resize(packed);
    return out;
}
void keep_source(uint64_t hash, bool vertex, std::vector<uint8_t> packed, uint32_t size) {
    Source& src = sources[hash];
    src.packed = std::move(packed);
    src.size = size;
    src.vertex = vertex;
}
size_t store_bytes() {
    size_t n = 0;
    for (auto& [hash, src] : sources) n += src.packed.size();
    return n;
}
void delete_live(std::unordered_map<uint64_t, LiveObject>::iterator it) {
    glDeleteShader(it->second.obj);
    live.erase(it);
}
// the least recently used objects beyond limit go
void trim_live(size_t limit) {
    while (live.size() > limit) {
        auto oldest = live.begin();
        for (auto it = live.begin(); it != live.end(); ++it)
            if (it->second.used < oldest->second.used) oldest = it;
        delete_live(oldest);
    }
}
void keep_live(uint64_t hash, GLuint obj) {
    live[hash] = {obj, ++liveClock};
    trim_live(liveLimit);
}
// the compiled object of a source: a kept one, or compiled now (0: unknown source or a compile error)
// the stored GLSL of a source
bool source_text(uint64_t hash, std::string& glsl, std::string* error) {
    auto src = sources.find(hash);
    if (src == sources.end()) {
        if (error) *error = "its GLSL is not in the shader store";
        return false;
    }
    glsl.assign(src->second.size, '\0');
    uLongf size = src->second.size;
    if (uncompress((Bytef*)glsl.data(), &size, src->second.packed.data(), src->second.packed.size()) != Z_OK ||
        size != src->second.size) {
        if (error) *error = "its stored GLSL does not decompress";
        return false;
    }
    return true;
}

GLuint object_for(uint64_t hash, std::string* error) {
    if (auto it = live.find(hash); it != live.end()) {
        it->second.used = ++liveClock;
        return it->second.obj;
    }
    auto src = sources.find(hash);
    std::string glsl;
    if (!source_text(hash, glsl, error)) return 0;
    objectCompiles++;
    GLuint obj = compile(src->second.vertex, glsl, error);
    if (obj) {
        // (keep_live may trim: this one is the newest, so it stays)
        live[hash] = {obj, ++liveClock};
        trim_live(std::max<size_t>(liveLimit, 2));
    }
    return obj;
}

// a failed link leaves prog at 0
// stage: what the watchdog reports while the render thread is in here
std::unique_ptr<Program> link(GLuint vsObj, GLuint psObj, uint64_t key, const char* stage = "linking a program") {
    R.perf.linked++;
    auto p = std::make_unique<Program>();
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vsObj);
    glAttachShader(prog, psObj);
    Stage linking(stage);
    // test aid: WWHD_GL_TEST_LINK_STALL=ms holds the first in-game link that long (the watchdog's report)
    if (static int stall = getenv("WWHD_GL_TEST_LINK_STALL") ? atoi(getenv("WWHD_GL_TEST_LINK_STALL")) : 0;
        stall > 0 && strcmp(stage, "linking a program") != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(stall));
        stall = 0;
    }
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        glGetProgramInfoLog(prog, sizeof log, nullptr, log);
        glDeleteProgram(prog);
        prog = 0;
        // inputs of the pixel shader the vertex shader does not write (varyings.h): again with a
        // vertex shader that writes them as zero
        if (strstr(log, "no matching output")) {
            auto source = [](GLuint obj) {
                GLint length = 0;
                glGetShaderiv(obj, GL_SHADER_SOURCE_LENGTH, &length);
                std::string text(size_t(std::max(length, 1)), '\0');
                glGetShaderSource(obj, length, nullptr, text.data());
                text.resize(strlen(text.c_str()));
                return text;
            };
            std::vector<std::string> added;
            const std::string patched = add_missing_outputs(source(vsObj), source(psObj), &added);
            std::string error;
            if (GLuint vs2 = patched.empty() ? 0 : compile_glsl(true, patched, &error)) {
                prog = glCreateProgram();
                glAttachShader(prog, vs2);
                glAttachShader(prog, psObj);
                glLinkProgram(prog);
                glDeleteShader(vs2);  // (kept while the program uses it)
                glGetProgramiv(prog, GL_LINK_STATUS, &ok);
                std::string names;
                for (auto& n : added) names += " " + n;
                if (ok) {
                    LOG("[gl] program %016llx: its pixel shader reads%s, which the vertex shader does not write; "
                        "linked with them set to zero", (unsigned long long)key, names.c_str());
                } else {
                    glGetProgramInfoLog(prog, sizeof log, nullptr, log);
                    glDeleteProgram(prog);
                    prog = 0;
                }
            } else if (!patched.empty())
                snprintf(log, sizeof log, "vertex shader with the missing outputs: %s", error.c_str());
        }
        if (!prog) {
            dump_failure("program", key, "", log);
            return p;
        }
    }
    // a linked program needs its shaders no more: detached, deleting them frees their intermediate code
    {
        GLuint attached[4] = {};
        GLsizei count = 0;
        glGetAttachedShaders(prog, 4, &count, attached);
        for (GLsizei i = 0; i < count; i++) {
            // does the vertex shader write the near-plane clip distance (with_near_clip)? draw.cpp
            // enables the plane only then (an enabled clip distance that is not written is undefined)
            GLint type = 0, length = 0;
            glGetShaderiv(attached[i], GL_SHADER_TYPE, &type);
            if (type == GL_VERTEX_SHADER && near_clip_on_impl()) {
                glGetShaderiv(attached[i], GL_SHADER_SOURCE_LENGTH, &length);
                std::string text(size_t(std::max(length, 1)), '\0');
                glGetShaderSource(attached[i], length, nullptr, text.data());
                p->nearClip = text.find(kNearClipWrite) != std::string::npos;
                if (!p->nearClip && g_nearClipMissing++ == 0)
                    LOG("[gl] program %016llx: its vertex shader has no near-plane clip distance (drawn without "
                        "it; later ones are counted in the shader cache line)", (unsigned long long)key);
            }
            glDetachShader(prog, attached[i]);
        }
    }
    p->prog = prog;
    p->remappedVS = glGetUniformLocation(prog, "uf_remappedVS");
    p->remappedPS = glGetUniformLocation(prog, "uf_remappedPS");
    p->registersVS = glGetUniformLocation(prog, "uf_uniformRegisterVS");
    p->registersPS = glGetUniformLocation(prog, "uf_uniformRegisterPS");
    p->windowToClip = glGetUniformLocation(prog, "uf_windowSpaceToClipSpaceTransform");
    p->alphaRef = glGetUniformLocation(prog, "uf_alphaTestRef");
    p->pointSize = glGetUniformLocation(prog, "uf_pointSize");
    p->fragCoordScale = glGetUniformLocation(prog, "uf_fragCoordScale");
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) {
        char name[32];
        snprintf(name, sizeof name, "uf_tex%dScale", t);
        p->texScale[t] = glGetUniformLocation(prog, name);
    }
    // the stages' uniform-variable blocks (uniform_vars_to_block): size, member offsets and, with
    // batching, the per-draw stride and the array's length
    auto var_block = [&](const char* block, const char* stage, UniformVarBlock& b) {
        GLuint index = glGetUniformBlockIndex(prog, block);
        if (index == GL_INVALID_INDEX) return;
        GLint size = 0;
        glGetActiveUniformBlockiv(prog, index, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
        const std::string element = std::string("wwhd_uf") + stage;  // batching: the per-draw struct array
        auto raw_offset = [&](const std::string& name) -> GLint {
            const std::string indexed = name + "[0]";
            for (const std::string* n : {&name, &indexed}) {
                const GLchar* c = n->c_str();
                GLuint i = GL_INVALID_INDEX;
                glGetUniformIndices(prog, 1, &c, &i);
                if (i == GL_INVALID_INDEX) continue;
                GLint o = -1;
                glGetActiveUniformsiv(prog, 1, &i, GL_UNIFORM_OFFSET, &o);
                return o;
            }
            return -1;
        };
        std::vector<std::string> names = {std::string("uf_remapped") + stage, std::string("uf_uniformRegister") + stage,
                                          "uf_windowSpaceToClipSpaceTransform", "uf_pointSize", "uf_alphaTestRef",
                                          "uf_fragCoordScale"};
        for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) names.push_back("uf_tex" + std::to_string(t) + "Scale");
        bool array = false;  // the batching form: members inside wwhd_ufXS[n]
        for (auto& n : names)
            if (raw_offset(element + "[0]." + n) >= 0) {
                array = true;
                break;
            }
        auto offset = [&](const std::string& name) { return raw_offset(array ? element + "[0]." + name : name); };
        b.size = size;
        b.stride = size;
        b.capacity = 1;
        if (array) {
            // the stride from any member's offsets in elements 0 and 1
            for (auto& n : names) {
                GLint o0 = raw_offset(element + "[0]." + n), o1 = raw_offset(element + "[1]." + n);
                if (o0 >= 0 && o1 > o0) {
                    b.stride = o1 - o0;
                    break;
                }
            }
            b.capacity = std::max<GLint>(1, size / std::max<GLint>(16, b.stride));
            if (b.capacity == 1) b.stride = size;
        }
        b.remapped = offset(names[0]);
        b.registers = offset(names[1]);
        if (stage[0] == 'V') {
            b.windowToClip = offset("uf_windowSpaceToClipSpaceTransform");
            b.pointSize = offset("uf_pointSize");
        } else {
            b.alphaRef = offset("uf_alphaTestRef");
            b.fragCoordScale = offset("uf_fragCoordScale");
        }
        for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) b.texScale[t] = offset("uf_tex" + std::to_string(t) + "Scale");
        // (1, 1) until a draw on a scaled target or texture sets them
        b.data.assign(size_t(b.stride), 0);
        const float one[2] = {1.0f, 1.0f};
        for (size_t i = 5; i < names.size(); i++)
            if (GLint o = offset(names[i]); o >= 0 && o + 8 <= b.stride) memcpy(b.data.data() + o, one, 8);
    };
    var_block("ufBlockVS", "VS", p->blockVS);
    var_block("ufBlockPS", "PS", p->blockPS);
    p->scaleUniforms = p->fragCoordScale >= 0 || p->blockPS.fragCoordScale >= 0;
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++)
        if (p->texScale[t] >= 0 || p->blockVS.texScale[t] >= 0 || p->blockPS.texScale[t] >= 0) p->scaleUniforms = true;
    p->batchable = draw_batching_on() && glGetAttribLocation(prog, "wwhd_drawIndex") == GLint(kDrawIndexAttrib);
    p->batchCapacity = 256;
    for (auto* b : {&p->blockVS, &p->blockPS})
        if (b->size > 0) p->batchCapacity = std::min<uint32_t>(p->batchCapacity, uint32_t(b->capacity));
    // uniform block sizes by binding point: bound ranges must cover the declared arrays
    GLint blocks = 0;
    glGetProgramiv(prog, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
    for (GLint b = 0; b < blocks; b++) {
        GLint binding = 0, size = 0;
        glGetActiveUniformBlockiv(prog, b, GL_UNIFORM_BLOCK_BINDING, &binding);
        glGetActiveUniformBlockiv(prog, b, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
        if (binding >= 0 && binding < (GLint)p->blockSize.size()) p->blockSize[binding] = size;
    }
    // the GLSL sets every binding with layout(binding = n); the program applies uniforms set later
    glUseProgram(prog);
    forget_gl_state();
    if (p->fragCoordScale >= 0) glUniform2f(p->fragCoordScale, 1.0f, 1.0f);
    for (GLint loc : p->texScale)
        if (loc >= 0) glUniform2f(loc, 1.0f, 1.0f);
    return p;
}
}  // namespace

LatteFetchShader* get_fetch_shader(const uint32_t* regs, uint64_t* keyOut, uint64_t frame) {
    if (keyOut) *keyOut = 0;
    uint32_t address = regs[mmSQ_PGM_START_FS] << 8;
    if (!address) return nullptr;
    bool compact = ld32(address) == 0x57574653;
    uint32_t count = compact ? ld32(address + 4) : 0;
    if (compact && count > 64) return nullptr;
    uint32_t size = compact ? 16 + count * 16 : regs[mmSQ_PGM_START_FS + 1] << 3;
    if (!size || size > 0x1000 || uint64_t(address) + size > 0x100000000ull) return nullptr;
    uint64_t key = program_hash(address, size, frame);
    if (keyOut) *keyOut = key;
    if (auto it = fetchShaders.find(key); it != fetchShaders.end()) return it->second;
    auto* fetch = compact ? gx2::build_fetch_shader(address)
                          : LatteShaderRecompiler_createFetchShader(key, const_cast<uint32_t*>(regs),
                                                                    reinterpret_cast<uint32_t*>(ppc_ptr(address)), size);
    fetchShaders.emplace(key, fetch);
    return fetch;
}

bool near_clip_on() { return near_clip_on_impl(); }

bool uniform_blocks_on() {
    static const bool on = uniform_blocks_on_impl();
    return on;
}

bool draw_batching_on() {
    static const bool on = [] {
        const char* e = getenv("WWHD_GL_BATCH");
        return uniform_blocks_on() && !(e && *e == '0');
    }();
    return on;
}

uint64_t shader_state_hash(const uint32_t* regs, bool vertex, uint64_t* core) {
    *core = state_hash(regs, vertex ? 0x1111 : 0x2222, vertex);
    return *core ^ texture_state_hash(regs, vertex, (1u << LATTE_NUM_MAX_TEX_UNITS) - 1) * 0x9E3779B97F4A7C15ull;
}

namespace {
// null, or why the program cannot be translated
const char* decompile(const uint32_t* regs, bool vertex, LatteFetchShader* fetch, uint64_t base, uint32_t address,
                      uint32_t size, LatteDecompilerOutput_t& output) {
    if ((regs[Latte::REGADDR::VGT_GS_MODE] & 3) != 0) return "geometry shaders are not implemented by the OpenGL renderer";
    if (vertex && !fetch) return "vertex shader has no fetch program";
    LatteShader_UpdatePSInputs(const_cast<uint32_t*>(regs));
    LatteDecompilerOptions options;
    if (vertex)
        LatteDecompiler_DecompileVertexShader(base, const_cast<uint32_t*>(regs), ppc_ptr(address), size, fetch, options,
                                              &output);
    else
        LatteDecompiler_DecompilePixelShader(base, const_cast<uint32_t*>(regs), ppc_ptr(address), size, options, &output);
    if (!output.shader || output.shader->hasError || !output.shader->strBuf_shaderSource)
        return "Latte GLSL translation failed";
    return nullptr;
}

// WWHD_GL_KEY_CHECK=1 (testing): each time the full register state (as the Vulkan renderer keys
// shaders: both stages' registers, every texture unit) is new for a program whose shader came from
// the cache, translate again and compare the GLSL. A mismatch means the key misses a register.
const bool g_keyCheck = [] {
    const char* e = getenv("WWHD_GL_KEY_CHECK");
    return e && *e && strcmp(e, "0") != 0;
}();

uint64_t full_state_hash(const uint32_t* regs, bool vertex) {
    std::vector<uint32_t> w(regs + mmSQ_VTX_SEMANTIC_0, regs + mmSQ_VTX_SEMANTIC_0 + 32);
    auto put = [&](uint32_t first, uint32_t length) { w.insert(w.end(), regs + first, regs + first + length); };
    put(mmSPI_VS_OUT_ID_0, 10);
    put(mmSPI_VS_OUT_CONFIG, 1);
    put(mmPA_CL_VS_OUT_CNTL, 1);
    put(mmSPI_PS_IN_CONTROL_0, 2);
    put(mmSPI_PS_INPUT_CNTL_0, 32);
    put(REGADDR::VGT_PRIMITIVE_TYPE, 1);
    put(mmSPI_INTERP_CONTROL_0, 1);
    put(mmVGT_STRMOUT_EN, 1);
    for (uint32_t buffer = 0; buffer < 4; ++buffer) put(mmVGT_STRMOUT_VTX_STRIDE_0 + buffer * 4, 1);
    for (uint32_t r : {uint32_t(REGADDR::VGT_GS_MODE), uint32_t(REGADDR::SQ_CONFIG), uint32_t(mmCB_SHADER_MASK),
                       uint32_t(mmCB_SHADER_CONTROL), uint32_t(mmDB_SHADER_CONTROL), uint32_t(mmSPI_INPUT_Z),
                       uint32_t(REGADDR::SX_ALPHA_TEST_CONTROL), uint32_t(REGADDR::PA_CL_VTE_CNTL),
                       uint32_t(REGADDR::PA_CL_CLIP_CNTL), uint32_t(REGADDR::DB_DEPTH_CONTROL),
                       uint32_t(REGADDR::CB_COLOR_CONTROL), uint32_t(REGADDR::CB_TARGET_MASK)})
        put(r, 1);
    put(mmCB_COLOR0_INFO, 8);
    const uint32_t tex = vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
    put(tex, 7 * LATTE_NUM_MAX_TEX_UNITS);
    put(REGADDR::SQ_TEX_SAMPLER_WORD0_0, 3 * 3 * LATTE_NUM_MAX_TEX_UNITS);
    return hash_bytes(w.data(), w.size() * 4, vertex ? 0x1111 : 0x2222);
}

void check_key(const uint32_t* regs, bool vertex, LatteFetchShader* fetch, uint64_t fsKey, uint64_t base,
               uint32_t address, uint32_t size, decltype(shaders)::iterator found) {
    static std::unordered_map<uint64_t, bool> seen;
    static uint64_t checks = 0, mismatches = 0;
    const uint64_t full = full_state_hash(regs, vertex) ^ base * 31 ^ fsKey * 1009;
    if (!seen.emplace(full, true).second || found == shaders.end() || !found->second->ready()) return;
    LatteDecompilerOutput_t output{};
    if (decompile(regs, vertex, fetch, base, address, size, output)) return;
    const char* text = output.shader->strBuf_shaderSource->c_str();
    uint64_t glslHash = hash_bytes(text, strlen(text), vertex ? 0x1111 : 0x2222);
    delete output.shader->strBuf_shaderSource;
    checks++;
    if (glslHash != found->second->glslHash && mismatches++ < 20)
        LOG("[key check] %s shader %016llx: different GLSL for the same key", vertex ? "vertex" : "pixel",
            (unsigned long long)base);
    if (checks % 1000 == 0)
        LOG("[key check] %llu checks, %llu mismatches", (unsigned long long)checks, (unsigned long long)mismatches);
}
}  // namespace

Shader* translate(const uint32_t* regs, bool vertex, LatteFetchShader* fetch, uint64_t fsKey, uint64_t frame,
                  uint64_t coreHash) {
    uint32_t start = vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
    uint32_t address = regs[start] << 8, size = regs[start + 1] << 3;
    if (!address || !size || size > 0x100000 || uint64_t(address) + size > 0x100000000ull) return nullptr;
    uint64_t base = program_hash(address, size, frame) ^ (vertex ? 0x1111 : 0x2222);
    // the key holds the texture state of only the units the program samples (known after its first
    // translation): the game leaves earlier textures bound in the other units, and keying on those
    // made hundreds of identical translations a second in new areas
    auto keyFor = [&](uint32_t units) {
        return (coreHash ^ (base * 0xFF51AFD7ED558CCDull + 0x2545F4914F6CDD1Dull)) ^ (vertex ? fsKey * 31 : 0) ^
               texture_state_hash(regs, vertex, units) * 0xC2B2AE3D27D4EB4Full;
    };
    auto known = textureUnits.find(base);
    uint64_t key = keyFor(known != textureUnits.end() ? known->second : 0);
    if (auto it = shaders.find(key); it != shaders.end()) {
        if (g_keyCheck) check_key(regs, vertex, fetch, fsKey, base, address, size, it);
        return it->second.get();
    }
    ScopedTime timer{R.perf.shaderNs};
    R.perf.shaders++;
    if (known != textureUnits.end()) R.perf.knownProgramShaders++;
    auto owned = std::make_unique<Shader>();
    Shader* shader = owned.get();
    shader->key = key;
    shader->vertex = vertex;
    shaders.emplace(key, std::move(owned));
    LatteDecompilerOutput_t output{};
    Stage stage(vertex ? "translating a vertex shader" : "translating a pixel shader");
    if (const char* error = decompile(regs, vertex, fetch, base, address, size, output)) {
        shader->error = error;
        return shader;
    }
    // the units this program samples depend only on its code: from now on its key includes those alone
    const uint32_t units = uint32_t(output.textureUnitMask.to_ulong());
    textureUnits[base] = units;
    if (uint64_t unitsKey = keyFor(units); unitsKey != key) {
        auto node = shaders.extract(key);
        node.key() = unitsKey;
        auto inserted = shaders.insert(std::move(node));
        if (!inserted.inserted) return inserted.position->second.get();  // not reached: units were unknown
        key = shader->key = unitsKey;
    }
    shader->dec = FinishDecompiledShader(output);
    shader->mapping = output.resourceMappingGL;
    shader->registerCount = output.uniformOffsetsGL.count_uniformRegister;
    std::string glsl = shader->dec->strBuf_shaderSource->c_str();
    delete shader->dec->strBuf_shaderSource;  // tens of KB per variant, never read again
    shader->dec->strBuf_shaderSource = nullptr;
    shader->glslHash = hash_bytes(glsl.data(), glsl.size(), vertex ? 0x1111 : 0x2222);
    if (sources.count(shader->glslHash)) {
        shader->compiled = true;
        cache_translation(shader, base, units);
        return shader;
    }
    std::string error;
    Stage compiling("compiling a shader");
    GLuint obj = compile(vertex, glsl, &error);
    if (!obj) {
        shader->error = error;
        dump_failure(vertex ? "vertex shader" : "pixel shader", key, glsl, error);
        return shader;
    }
    objectCompiles++;
    keep_source(shader->glslHash, vertex, pack_glsl(glsl), uint32_t(glsl.size()));
    keep_live(shader->glslHash, obj);  // (the link that follows needs it)
    cache_shader(shader->glslHash, vertex, glsl);
    shader->compiled = true;
    cache_translation(shader, base, units);
    return shader;
}

Program* program(Shader* vs, Shader* ps) {
    uint64_t key = pair_key(vs->glslHash, ps->glslHash);
    if (auto it = linked.find(key); it != linked.end()) return it->second->prog ? it->second.get() : nullptr;
    ScopedTime timer{R.perf.shaderNs};
    // The driver compiles the pair's GPU code inside the link (nouveau: nvc0_sp_state_create). The
    // watchdog names the pair if the render thread stays in there (the sources are in
    // shadercache_gl.bin); a failed or slow link is logged.
    static char stage[96];
    snprintf(stage, sizeof stage, "linking vertex shader %016llx with pixel shader %016llx",
             (unsigned long long)vs->glslHash, (unsigned long long)ps->glslHash);
    const uint64_t linkStart = now_ns();
    std::string error;
    const GLuint vsObj = object_for(vs->glslHash, &error), psObj = vsObj ? object_for(ps->glslHash, &error) : 0;
    if (!vsObj || !psObj) {
        LOG("[gl] %s: a shader does not compile (%s)", stage, error.c_str());
        linked.emplace(key, std::make_unique<Program>());
        return nullptr;
    }
    auto owned = link(vsObj, psObj, key, stage);
    if (const double ms = double(now_ns() - linkStart) / 1e6; !owned->prog || ms >= 100)
        LOG("[gl] %s: %s in %.0f ms", stage, owned->prog ? "linked" : "failed", ms);
    Program* p = owned->prog ? owned.get() : nullptr;
    linked.emplace(key, std::move(owned));
    if (p) cache_program(vs->glslHash, ps->glslHash);
    return p;
}

Program* probe_program(Shader* vs, Shader* ps, int mode, const std::string& expr) {
    static std::unordered_map<uint64_t, std::unique_ptr<Program>> probes;
    const uint64_t key = pair_key(vs->glslHash, ps->glslHash) ^ (0x9E3779B97F4A7C15ull * uint64_t(mode + 1));
    if (auto it = probes.find(key); it != probes.end()) return it->second->prog ? it->second.get() : nullptr;
    std::string error, glsl;
    GLuint psObj = 0;
    if (source_text(ps->glslHash, glsl, &error)) {
        const size_t at = glsl.find("passPixelColor0 = ");
        const size_t end = at == std::string::npos ? at : glsl.find(';', at);
        if (end == std::string::npos) {
            error = "no passPixelColor0 output";
        } else {
            const std::string original = glsl.substr(at + 18, end - at - 18);
            std::string e = expr;
            for (size_t d = e.find('$'); d != std::string::npos; d = e.find('$', d + original.size()))
                e.replace(d, 1, "(" + original + ")");
            glsl.replace(at, end - at, "passPixelColor0 = " + e);
            psObj = compile(false, glsl, &error);
        }
    }
    const GLuint vsObj = psObj ? object_for(vs->glslHash, &error) : 0;
    std::unique_ptr<Program> owned = vsObj ? link(vsObj, psObj, key, "linking a probe program") : std::make_unique<Program>();
    if (psObj) glDeleteShader(psObj);  // (link detached it: the program keeps its code)
    LOG("[gl] probe mode %d of pixel shader %016llx: %s%s", mode, (unsigned long long)ps->glslHash,
        owned->prog ? "linked" : "failed: ", owned->prog ? "" : error.c_str());
    Program* p = owned->prog ? owned.get() : nullptr;
    probes.emplace(key, std::move(owned));
    forget_gl_state();
    return p;
}

void load_shader_cache(void (*progress)(size_t done, size_t total)) {
    std::vector<uint8_t> data;
    if (FILE* f = fopen(kCachePath, "rb")) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0) {
            data.resize(size_t(size));
            data.resize(fread(data.data(), 1, data.size(), f));
        }
        fclose(f);
    }
    const bool fresh = data.size() < 4 || memcmp(data.data(), kCacheMagic, 4) != 0;
    std::vector<size_t> records;
    size_t valid = fresh ? 0 : 4;
    while (!fresh && valid < data.size()) {
        size_t length = 0;
        if (data[valid] == 2) length = 17;
        else if (data[valid] == 3 && valid + 5 <= data.size()) {
            uint32_t size;
            memcpy(&size, data.data() + valid + 1, 4);
            length = 5 + size_t(size);
        } else if (data[valid] == 1 && valid + 18 <= data.size()) {
            uint32_t packed;
            memcpy(&packed, data.data() + valid + 10, 4);
            length = 18 + size_t(packed);
        }
        if (!length || valid + length > data.size()) break;
        records.push_back(valid);
        valid += length;
    }
    const uint64_t start = now_ns();
    size_t linkedPrograms = 0, loadedTranslations = 0;
#ifndef __SWITCH__
    const size_t mallocBefore = mallinfo2().uordblks;
#endif
    // first the sources (kept compressed, as stored), the translations and the pairs; then the pairs are
    // linked, each source compiled when a pair first needs it and its object deleted after its last pair
    std::vector<std::pair<uint64_t, uint64_t>> pairs;
    for (size_t i = 0; i < records.size(); i++) {
        const uint8_t* r = data.data() + records[i];
        if (r[0] == 1) {
            uint64_t hash;
            uint32_t sizes[2];
            memcpy(&hash, r + 2, 8);
            memcpy(sizes, r + 10, 8);
            if (!sources.count(hash)) keep_source(hash, r[1] != 0, std::vector<uint8_t>(r + 18, r + 18 + sizes[0]), sizes[1]);
        } else if (r[0] == 3) {
            uint32_t size;
            memcpy(&size, r + 1, 4);
            loadedTranslations += load_translation(r + 5, size);
        } else {
            uint64_t vs, ps;
            memcpy(&vs, r + 1, 8);
            memcpy(&ps, r + 9, 8);
            pairs.push_back({vs, ps});
        }
    }
#ifndef __SWITCH__
    const size_t mallocLoaded = mallinfo2().uordblks;
#endif
    std::unordered_map<uint64_t, size_t> lastUse, uses;
    for (size_t i = 0; i < pairs.size(); i++) {
        lastUse[pairs[i].first] = lastUse[pairs[i].second] = i;
        uses[pairs[i].first]++;
        uses[pairs[i].second]++;
    }
    // the sources in the most cached pairs keep their objects after startup: a new pair in the game most
    // often combines one of them with another shader, and then needs one compile instead of two
    std::unordered_set<uint64_t> keep;
    {
        std::vector<std::pair<size_t, uint64_t>> ranked;
        for (auto& [h, n] : uses) ranked.push_back({n, h});
        std::sort(ranked.begin(), ranked.end(), std::greater<>());
        for (size_t i = 0; i < ranked.size() && i < kLiveObjects; i++) keep.insert(ranked[i].second);
    }
    for (size_t i = 0; i < pairs.size(); i++) {
        const auto [vs, ps] = pairs[i];
        const uint64_t key = pair_key(vs, ps);
        if (!linked.count(key) && sources.count(vs) && sources.count(ps)) {
            const GLuint v = object_for(vs, nullptr), p = v ? object_for(ps, nullptr) : 0;
            if (v && p) {
                linked.emplace(key, link(v, p, key));
                linkedPrograms++;
            }
        }
        for (uint64_t h : {vs, ps})
            if (lastUse[h] == i && !keep.count(h))
                if (auto it = live.find(h); it != live.end()) delete_live(it);
        if (progress) progress(i + 1, pairs.size());
    }
    for (auto it = live.begin(); it != live.end();) {
        auto next = std::next(it);
        if (!keep.count(it->first)) delete_live(it);
        it = next;
    }
    liveLimit = kLiveObjects;
    trim_live(liveLimit);
    if (!records.empty())
        LOG("[gl] shader cache: %zu sources kept (%zu KB compressed), %zu programs linked in %.1f s (%zu shader "
            "compiles); %zu shaders with uniform blocks (WWHD_GL_UNIFORM_BLOCKS); %zu translations loaded%s", sources.size(),
            store_bytes() >> 10, linkedPrograms, double(now_ns() - start) / 1e9, objectCompiles, g_blockShaders,
            loadedTranslations,
            near_clip_on_impl() ? ("; " + std::to_string(g_nearClipMissing) + " programs without the near-plane clip "
                                   "distance").c_str() : "");
#ifdef __SWITCH__
    log_heap("after the shader cache");
#else
    LOG("[gl] shader cache memory: sources and translations %zu MiB, programs %zu MiB (malloc)",
        (mallocLoaded - mallocBefore) >> 20, (size_t(mallinfo2().uordblks) - mallocLoaded) >> 20);
#endif
    LOG("[gl] vertex positions invariant: %s (WWHD_GL_INVARIANT); ambient occlusion mode %d (WWHD_AO_MODE); "
        "near-plane clip distance: %s (WWHD_GL_NEAR_CLIP)", g_invariantPosition ? "on" : "off", ao_mode(),
        near_clip_on() ? "on" : "off");
    // a new file, or one whose last record was cut short, is rewritten before appending
    if (fresh || valid < data.size()) {
        if (FILE* f = fopen(kCachePath, "wb")) {
            if (fresh) fwrite(kCacheMagic, 1, 4, f);
            else fwrite(data.data(), 1, valid, f);
            fclose(f);
        }
    }
    cacheFile = fopen(kCachePath, "ab");
    if (!cacheFile) LOG("[gl] shader cache: cannot write %s", kCachePath);
    else host::start_thread(cache_writer, 128 << 10);
}

void shader_memory(size_t& sourceCount, size_t& storeBytes, size_t& objects, size_t& programs) {
    sourceCount = sources.size();
    storeBytes = store_bytes();
    objects = live.size();
    programs = linked.size();
}

// a program's entry in programHashes (draw.cpp's combinations keep it: map nodes do not move, and
// reset_shader_memoization, which clears the map, also advances R.shaderEpoch, which drops them)
void* program_hash_ref(uint32_t address, uint32_t size) {
    return &programHashes[(uint64_t(address) << 32) | size];
}
uint64_t program_hash_of(void* ref, uint32_t address, uint32_t size, uint64_t frame) {
    return program_hash_at(*static_cast<ProgramHash*>(ref), address, size, frame);
}

void reset_shader_memoization() {
    programHashes.clear();
    R.shaderEpoch++;
}

}  // namespace gfxgl
