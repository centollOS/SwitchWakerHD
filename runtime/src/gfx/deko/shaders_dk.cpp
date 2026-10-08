// The deko3d renderer's game shaders (dk_shaders.h): GX2 registers -> Cemu's GLSL (the decompiler's OpenGL
// mode, keyed exactly as gfx/gl/shaders.cpp, so the GLSL hashes are those of shadercache_gl.bin) -> DKSH by
// hash from code memory, or from the uam worker. No links: a shader is ready when its DKSH is loaded.
//
// Where a DKSH comes from (ShaderCode::origin):
// - shadercache_dksh.bin (WDK1, shader_files.h): built offline from a harvest of shadercache_gl.bin files
//   (tools/switch/dksh_cache), copied next to the NRO; all of it goes into code memory at start-up.
// - shadercache_dksh_local.bin (WDK1): what this console compiled itself, appended as it goes.
// - the worker: one thread (uam is not reentrant) that converts (glsl_to_deko) and compiles; the render
//   thread loads its results at the next frame start. Until then the draws that need the shader are skipped
//   (the draw lane asks translate(), sees !ready() and calls shader_wanted(): the queue serves the shader
//   whose draws were skipped most in the last frame first, and the other stage of the same draw right
//   after it).
// shadercache_gl.bin (WGS1) keeps getting the sources and translations (records 1 and 3) exactly as gfx/gl
// writes them, so the OpenGL build reads what this one saw; its sources without DKSH are compiled in the
// background (lowest priority) from start-up.
#include "dk_shaders.h"

#include <switch.h>
#include <sys/stat.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "gx2/gx2.h"
#include "platform/host.h"
#include "ppc.h"
#include "runtime.h"
#include "shader_files.h"
#include "uam_api.h"
#include "util/helpers/StringBuf.h"

LatteDecompilerShader* FinishDecompiledShader(LatteDecompilerOutput_t& output);
LatteFetchShader* LatteShaderRecompiler_createFetchShader(LatteFetchShader::CacheHash hash, uint32* regs, uint32* code,
                                                          uint32 size);

namespace gfxdk {
using Latte::REGADDR;

// One GLSL source's DKSH (by its WGS1 hash), shared by every Shader translated to that source
struct ShaderCode {
    enum State : uint8_t { Queued, Ready, Failed };  // Queued: for the worker, or its result for a frame start
    enum Origin : uint8_t { Offline, Local, Session };
    uint64_t hash = 0;  // its GLSL's
    State state = Queued;
    Origin origin = Session;
    bool vertex = false;
    DkShader dk{};
    ConvertedBindings bindings;
    std::string error;
    uint32_t wantedFrame = 0;  // draws skipped for it in this frame (shader_wanted)
    uint64_t partner = 0;      // the other stage of a draw that waited for both: compiled right after it
    uint64_t queuedAt = 0;     // now_ns() when queued, for the log of slow ones
};

namespace {
constexpr char kGlCachePath[] = "shadercache_gl.bin";             // sdmc:/switch/wwhd (the working directory)
// (round 45) every translation's result for the draw path, so later sessions skip the decompiler (translate)
constexpr char kTransPath[] = "shadercache_dk_translations.bin";
constexpr char kTransMagic[4] = {'W', 'D', 'T', '1'};
// bump when the shader keys (gather_state, texture_state_hash, translate's keyFor), the decompiler's output or the
// record layout change: a file of another version is started again
constexpr uint32_t kTransVersion = 1;
constexpr char kOfflinePath[] = "shadercache_dksh.bin";
constexpr char kLocalPath[] = "shadercache_dksh_local.bin";
constexpr uint8_t kGlCacheMagic[4] = {'W', 'G', 'S', '1'};
constexpr size_t kWorkerStack = 8u << 20;  // Mesa's GLSL parser and nv50_ir recurse deeply (uam_api.h)
// 0x3B (59), the game's own threads' priority: an application's NPDM allows 0x1C-0x3B, and 0x3C (one below
// them) failed with 0xE001 (invalid priority) on the console, ending both P2 builds at start
constexpr int kWorkerPriority = 0x3B;
constexpr int kLoggedFailures = 32;

std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders;  // translation key -> shader
std::unordered_map<uint64_t, ShaderCode> codes;                 // GLSL hash -> DKSH (nodes never move)
std::unordered_map<uint64_t, LatteFetchShader*> fetchShaders;
struct ProgramHash { uint64_t hash = 0, frame = ~uint64_t{0}, sample = 0; };
std::unordered_map<uint64_t, ProgramHash> programHashes;
std::unordered_map<uint64_t, uint32_t> textureUnits;  // program hash -> texture units it samples (bit t: unit t)
// shadercache_gl.bin: the sources (record 1) and translations (record 3) it already has
std::unordered_set<uint64_t> glSources, glTranslations;
std::vector<ShaderCode*> wantedCodes;  // codes with wantedFrame > 0
Shader* g_lastVs = nullptr;            // the last translated vertex / pixel shader (the current draw's pair)
Shader* g_lastPs = nullptr;
ShaderStats g_total;                   // since start-up (shader_stats_take and the log take differences)
uint64_t g_codeBytes = 0;              // code memory used by game shaders
uint64_t g_failuresLogged = 0;
uint64_t g_strictMulLogged = 0;  // vertex shaders translated with strict multiplication (translate)
// WWHD_DK_SHADER_BUDGET: DKSH loads per frame (default 64; they are cheap: a copy into code memory); 0 = no
// budget: a draw waits for the worker to compile what it needs (no skipped draws, the frame stalls; for
// comparisons).
int g_loadBudget = 64;
bool g_waitForWorker = false;

// ---- hashes (as gfx/gl/shaders.cpp: the same keys and the same GLSL hashes)
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

// four independent multiply chains (one chain is latency-bound); for in-memory keys only
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

// the registers the GLSL translation of one stage reads, other than the texture units' (gfx/gl/shaders.cpp
// state_hash says which reader needs which), appended to out from count on: the part both stages read
// (shared) and this stage's own part (own). primitive: the primitive-type word as hashed.
size_t gather_state(const uint32_t* regs, bool vertex, uint32_t primitive, bool shared, bool own, uint32_t* out,
                    size_t count) {
    auto append = [&](const uint32_t* words, size_t length) {
        memcpy(out + count, words, length * sizeof(uint32_t));
        count += length;
    };
    auto put = [&](uint32_t first, uint32_t length) { append(regs + first, length); };
    if (shared) {
        put(mmSPI_PS_IN_CONTROL_0, 2);
        put(mmSPI_PS_INPUT_CNTL_0, std::min<uint32_t>(regs[mmSPI_PS_IN_CONTROL_0] & 0x3F, 32));
        uint32_t primitiveState[] = {primitive, regs[mmSPI_INTERP_CONTROL_0] & (1u << 1), regs[mmVGT_STRMOUT_EN]};
        append(primitiveState, 3);
        if (regs[mmVGT_STRMOUT_EN])
            for (uint32_t buffer = 0; buffer < 4; ++buffer) put(mmVGT_STRMOUT_VTX_STRIDE_0 + buffer * 4, 1);
        put(REGADDR::VGT_GS_MODE, 1);
        put(REGADDR::SQ_CONFIG, 1);
        uint32_t transformState[] = {regs[REGADDR::PA_CL_VTE_CNTL] & 0x3F, regs[REGADDR::PA_CL_CLIP_CNTL] & (1u << 19)};
        append(transformState, 2);
    }
    if (!own) return count;
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
    return count;
}
constexpr size_t kStateWords = 112;  // one stage: at most 2 + 32 + 3 + 4 + 2 + 2 + 44 (vertex)

uint64_t state_hash(const uint32_t* regs, uint64_t hash, bool vertex) {
    std::array<uint32_t, kStateWords> state;
    const size_t count = gather_state(regs, vertex, regs[REGADDR::VGT_PRIMITIVE_TYPE] & 0x3F, true, true, state.data(), 0);
    return hash_words4(state.data(), count, hash);
}

// what the GLSL translation reads of texture unit t: the dimension and whether the format is an integer one
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

const char* stage_name(bool vertex) { return vertex ? "vertex shader" : "pixel shader"; }

// ---- the writer thread: shadercache_gl.bin and shadercache_dksh_local.bin get their new records from it,
// about once a second (gfx/gl cache_writer: each SD card write and flush took ~4 ms on the render thread)
struct CacheFile {
    const char* path = nullptr;
    FILE* f = nullptr;
    std::vector<uint8_t> pending;  // under writerMutex
    uint64_t written = 0;
};
CacheFile glFile{kGlCachePath}, localFile{kLocalPath}, transFile{kTransPath};
std::mutex writerMutex;
std::condition_variable writerCv;
bool writerStarted = false;

// writes what is pending (with writerMutex held by the caller's lock lk, released while writing)
void write_pending(CacheFile& file, std::unique_lock<std::mutex>& lk) {
    if (file.pending.empty() || !file.f) return;
    std::vector<uint8_t> batch;
    batch.swap(file.pending);
    lk.unlock();
    const size_t n = fwrite(batch.data(), 1, batch.size(), file.f);
    fflush(file.f);
    lk.lock();
    file.written += n;
    if (n != batch.size()) LOG("[dk] shader cache: writing %s failed (%zu of %zu bytes)", file.path, n, batch.size());
}

void cache_writer() {
    host::set_thread_name("dk shader cache writer");
    std::unique_lock<std::mutex> lk(writerMutex);
    for (;;) {
        writerCv.wait(lk, [] { return !glFile.pending.empty() || !localFile.pending.empty() || !transFile.pending.empty(); });
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::seconds(1));  // the rest of a burst goes in the same write
        lk.lock();
        write_pending(glFile, lk);
        write_pending(localFile, lk);
        write_pending(transFile, lk);
    }
}

void cache_write(CacheFile& file, const void* data, size_t size) {
    if (!file.f) return;
    const auto* bytes = static_cast<const uint8_t*>(data);
    bool first;
    {
        std::lock_guard<std::mutex> lk(writerMutex);
        first = glFile.pending.empty() && localFile.pending.empty() && transFile.pending.empty();
        file.pending.insert(file.pending.end(), bytes, bytes + size);
    }
    if (first) writerCv.notify_one();
}

// record 1 of shadercache_gl.bin (gfx/gl cache_shader)
void cache_gl_source(uint64_t hash, bool vertex, const std::string& glsl) {
    if (!glFile.f || !glSources.insert(hash).second) return;
    uLongf packed = compressBound(glsl.size());
    std::vector<uint8_t> record(18 + packed);
    if (compress2(record.data() + 18, &packed, (const Bytef*)glsl.data(), glsl.size(), 6) != Z_OK) return;
    uint32_t sizes[2] = {uint32_t(packed), uint32_t(glsl.size())};
    record[0] = 1;
    record[1] = vertex;
    memcpy(record.data() + 2, &hash, 8);
    memcpy(record.data() + 10, sizes, 8);
    cache_write(glFile, record.data(), 18 + packed);
}

// record 3 of shadercache_gl.bin, byte for byte as gfx/gl cache_translation writes it (with the OpenGL
// mapping and register count, which only the OpenGL build reads): {3, u32 size, payload}
struct ByteWriter {
    std::vector<uint8_t> b;
    template <class T> void put(const T& v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        b.insert(b.end(), p, p + sizeof v);
    }
};
static_assert(std::is_trivially_copyable_v<LatteDecompilerShaderResourceMapping>);
void cache_gl_translation(const Shader* sh, uint64_t base, uint32_t units, const LatteDecompilerShaderResourceMapping& glMapping,
                          uint32_t registerCount) {
    if (!glFile.f || !sh->dec || !glTranslations.insert(sh->key).second) return;
    const LatteDecompilerShader* d = sh->dec;
    ByteWriter w;
    w.put<uint8_t>(sh->vertex);
    w.put(sh->key);
    w.put(base);
    w.put(sh->glslHash);
    w.put(units);
    w.put<uint32_t>(registerCount);
    w.put<uint32_t>(d->pixelColorOutputMask);
    w.put<uint8_t>(d->textureUnitListCount);
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) w.put<uint8_t>(d->textureUnitList[t]);
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) w.put<uint16_t>(d->textureUnitSamplerAssignment[t]);
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) w.put<uint8_t>(d->textureUsesDepthCompare[t]);
    w.put(glMapping);
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
    cache_write(glFile, record.data(), record.size());
}

// ---- translation records (round 45, WWHD_DK_TRANSLATION_CACHE=0 turns them off): a new shader variant costs
// ~0.3 ms on the render thread for the decompiler, and new effects or places bring 30-150 of them in a frame or
// two (the stutters of explosions and arrivals). A variant translated in an earlier session is rebuilt from its
// record instead (its DKSH is already in code memory by its GLSL hash): what the draw path reads of the decompiler's
// output, the Vulkan resource mapping and ufBlock layout, the uniform block sizes and the pixel-shader flags.
const bool g_transCache = [] {
    const char* e = getenv("WWHD_DK_TRANSLATION_CACHE");
    return !(e && *e == '0');
}();
std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> storedTrans;  // key -> payload (offset, size) in storedBlob
std::vector<uint8_t> storedBlob;
std::unordered_set<uint64_t> transWritten;  // keys written this session (or stored)
uint64_t g_storedHits = 0, g_storedMisses = 0;
static_assert(std::is_trivially_copyable_v<LatteDecompilerOutputUniformOffsets>);

void cache_dk_translation(const Shader* sh, uint64_t base, uint32_t units) {
    if (!g_transCache || !transFile.f || !sh->dec || !transWritten.insert(sh->key).second) return;
    const LatteDecompilerShader* d = sh->dec;
    ByteWriter w;
    w.put<uint8_t>(sh->vertex);
    w.put(sh->key);
    w.put(base);
    w.put(units);
    w.put(sh->glslHash);
    w.put<uint8_t>(sh->fragmentWhy);
    w.put<uint32_t>(d->pixelColorOutputMask);
    w.put<uint8_t>(d->textureUnitListCount);
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) w.put<uint8_t>(d->textureUnitList[t]);
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) w.put<uint16_t>(d->textureUnitSamplerAssignment[t]);
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) w.put<uint8_t>(d->textureUsesDepthCompare[t]);
    w.put(sh->mapping);
    w.put(sh->uniforms);
    for (uint32_t b : sh->uboBytes) w.put(b);
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
    const uint32_t size = uint32_t(w.b.size());
    std::vector<uint8_t> record(4);
    memcpy(record.data(), &size, 4);
    record.insert(record.end(), w.b.begin(), w.b.end());
    cache_write(transFile, record.data(), record.size());
}

struct ByteReader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (size_t(end - p) < sizeof v) {
            ok = false;
            return v;
        }
        memcpy(&v, p, sizeof v);
        p += sizeof v;
        return v;
    }
};

// the hot copies of the decompiler's texture lists (dk_shaders.h)
void fill_texture_lists(Shader* shader) {
    shader->texCount = 0;
    for (int i = 0; i < shader->dec->textureUnitListCount && i < LATTE_NUM_MAX_TEX_UNITS; i++) {
        const uint32_t unit = shader->dec->textureUnitList[i];
        if (unit >= LATTE_NUM_MAX_TEX_UNITS) continue;
        shader->texUnit[shader->texCount++] = uint8_t(unit);
        const uint32_t smp = shader->dec->textureUnitSamplerAssignment[unit];
        shader->texSampler[unit] = smp < 18 ? uint8_t(smp) : 0xFF;
        if (shader->dec->textureUsesDepthCompare[unit]) shader->texCompare |= 1u << unit;
    }
}

// a DKSH record of this session for shadercache_dksh_local.bin (dksh empty: it failed here)
void cache_dksh(uint64_t hash, bool vertex, const ConvertedBindings& b, const std::vector<uint8_t>& dksh) {
    DkshRecord r;
    r.stage = uint8_t(vertex ? uam::Stage::Vertex : uam::Stage::Fragment);
    r.glslHash = hash;
    r.set_bindings(b);
    r.dksh = dksh;
    std::vector<uint8_t> bytes;
    append_wdk1_record(&bytes, r);
    cache_write(localFile, bytes.data(), bytes.size());
}

std::vector<uint8_t> read_file(const char* path, void (*progress)(size_t, size_t), size_t progressBase,
                               size_t progressTotal) {
    std::vector<uint8_t> data;
    FILE* f = fopen(path, "rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) {
        data.resize(size_t(size));
        size_t got = 0;
        while (got < data.size()) {
            const size_t n = fread(data.data() + got, 1, std::min<size_t>(data.size() - got, 1u << 20), f);
            if (!n) break;
            got += n;
            if (progress) progress(progressBase + got, progressTotal);
        }
        data.resize(got);
    }
    fclose(f);
    return data;
}

// ---- the uam worker
struct Job {
    uint64_t hash = 0;
    bool vertex = false;
    bool background = false;      // a shadercache_gl.bin source no draw has asked for yet
    std::string glsl;             // Cemu's GLSL, or empty and ...
    std::vector<uint8_t> packed;  // ... zlib'd as in shadercache_gl.bin (background jobs)
    uint32_t size = 0;            // its inflated size
    uint32_t priority = 0;        // draws skipped for it in the last frame that skipped any
    uint64_t seq = 0;
    uint64_t partner = 0;
};
struct JobResult {
    uint64_t hash = 0;
    bool vertex = false, background = false;
    std::vector<uint8_t> dksh;  // empty: failed (error)
    ConvertedBindings bindings;
    std::string error;
    uint64_t convertNs = 0, compileNs = 0;
};
std::mutex workerMutex;
std::condition_variable workerCv, doneCv;
std::vector<Job> jobs;           // under workerMutex
std::vector<JobResult> results;  // under workerMutex
uint64_t jobSeq = 0;
uint64_t workerCurrent = 0;      // the hash being compiled (0: none)
uint64_t workerLastPartner = 0;  // compiled next if it is queued
bool workerStop = false;
bool workerReady = false;        // uam::init done and its self-test compiled
size_t foregroundQueued = 0;     // jobs a draw asked for (not background), under workerMutex
Thread workerThread;

// the next job: the partner of the last one, else the most wanted (draw-requested before background, then
// the oldest)
size_t pick_job() {
    size_t best = 0;
    for (size_t i = 0; i < jobs.size(); i++) {
        const Job& j = jobs[i];
        if (workerLastPartner && j.hash == workerLastPartner) return i;
        const Job& b = jobs[best];
        if (j.priority != b.priority ? j.priority > b.priority
                                     : j.background != b.background ? !j.background : j.seq < b.seq)
            best = i;
    }
    return best;
}

// failed conversions and compiles: the converted GLSL and uam's log to shaderfail_dk_<hash>.glsl (16 at most)
void dump_failure(uint64_t hash, const std::string& glsl, const std::string& log) {
    static int dumped = 0;
    if (dumped++ >= 16) return;
    char path[64];
    snprintf(path, sizeof path, "shaderfail_dk_%016llx.glsl", (unsigned long long)hash);
    if (FILE* f = fopen(path, "w")) {
        fprintf(f, "%s\n/*\n%s\n*/\n", glsl.c_str(), log.c_str());
        fclose(f);
    }
}

void compile_job(Job& job, JobResult& r) {
    r.hash = job.hash;
    r.vertex = job.vertex;
    r.background = job.background;
    const uint64_t t0 = now_ns();
    if (job.glsl.empty() && !job.packed.empty()) {
        job.glsl.assign(job.size, '\0');
        uLongf size = job.size;
        if (uncompress((Bytef*)job.glsl.data(), &size, job.packed.data(), job.packed.size()) != Z_OK || size != job.size) {
            r.error = "its source in shadercache_gl.bin does not inflate";
            return;
        }
    }
    const std::string glsl = glsl_to_deko(job.glsl, job.vertex, &r.bindings);
    const uint64_t t1 = now_ns();
    r.convertNs = t1 - t0;
    if (glsl.empty()) {
        r.error = "glsl_to_deko: " + r.bindings.error;
        dump_failure(job.hash, job.glsl, r.error);
        return;
    }
    uam::Result res = uam::compile(job.vertex ? uam::Stage::Vertex : uam::Stage::Fragment, glsl.c_str());
    r.compileNs = now_ns() - t1;
    if (!res.ok || res.dksh.empty()) {
        r.error = "uam: " + (res.log.empty() ? std::string("failed without a message") : res.log);
        dump_failure(job.hash, glsl, res.log);
        return;
    }
    r.dksh = std::move(res.dksh);
}

void worker_main(void*) {
    // cores 0 and 2: core 1 runs the main thread and the guest threads placed there (WWHD_CORE_LAYOUT)
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 0x5);
    host::set_thread_name("dk shader compiler");
    const uint64_t t0 = now_ns();
    uam::init(true);
    // self-test: a small vertex shader through glsl_to_deko's output form and uam (also builds the resident
    // frontend's tables now rather than at the game's first new shader)
    static const char kTest[] =
        "#version 460\nlayout(location = 0) in vec4 pos;\nlayout(binding = 0, std140) uniform ufBlock\n{\nvec4 "
        "uf_scale;\n};\nout gl_PerVertex\n{\nvec4 gl_Position;\n};\ninvariant gl_Position;\nvoid main()\n{\n"
        "gl_Position = pos * uf_scale;\ngl_Position.y = -gl_Position.y;\n}\n";
    const uint64_t t1 = now_ns();
    uam::Result test = uam::compile(uam::Stage::Vertex, kTest);
    const uint64_t t2 = now_ns();
    if (test.ok && !test.dksh.empty())
        LOG("[dk] shader worker: uam ready (init %.0f ms; self-test vertex shader %zu bytes of DKSH in %.0f ms); "
            "priority 0x%X, cores 0 and 2, %zu MiB stack",
            double(t1 - t0) / 1e6, test.dksh.size(), double(t2 - t1) / 1e6, kWorkerPriority, kWorkerStack >> 20);
    else
        LOG("[dk] shader worker: uam's SELF-TEST FAILED (%.0f ms): %s -- new shaders will likely fail too",
            double(t2 - t1) / 1e6, test.log.c_str());
    std::unique_lock<std::mutex> lk(workerMutex);
    workerReady = true;
    for (;;) {
        workerCv.wait(lk, [] { return workerStop || !jobs.empty(); });
        if (workerStop) break;
        const size_t i = pick_job();
        Job job = std::move(jobs[i]);
        jobs[i] = std::move(jobs.back());
        jobs.pop_back();
        if (!job.background) foregroundQueued--;
        workerCurrent = job.hash;
        workerLastPartner = 0;
        lk.unlock();
        JobResult r;
        compile_job(job, r);
        lk.lock();
        workerCurrent = 0;
        workerLastPartner = job.partner;
        results.push_back(std::move(r));
        doneCv.notify_all();
    }
}

void start_worker() {
    // the stack comes from the heap; core 2 to begin with (worker_main widens it to 0 and 2)
    const Result rc = threadCreate(&workerThread, worker_main, nullptr, nullptr, kWorkerStack, kWorkerPriority, 2);
    if (R_FAILED(rc) || R_FAILED(threadStart(&workerThread)))
        fatal("[dk] cannot start the shader compiler thread (%zu MiB stack, priority 0x%X): result 0x%X", kWorkerStack >> 20,
              kWorkerPriority, unsigned(rc));
}

// queues code's job (render thread, or main thread at start-up)
void queue_job(Job&& job) {
    {
        std::lock_guard<std::mutex> lk(workerMutex);
        job.seq = jobSeq++;
        if (!job.background) foregroundQueued++;
        jobs.push_back(std::move(job));
    }
    workerCv.notify_one();
}

// ---- results -> code memory (render thread)
void set_bindings(Shader* sh, const ShaderCode& code) {
    sh->dk = code.dk;
    sh->bindings = code.bindings;
    sh->uboSlot.fill(-1);
    sh->textureSlot.fill(-1);
    const auto& b = code.bindings;
    for (int i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++) {
        const int vk = sh->mapping.uniformBuffersBindingPoint[i];
        if (vk >= 0 && vk < kMaxVkBinding) sh->uboSlot[i] = b.ubo[vk];
    }
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) {
        const int vk = sh->mapping.textureUnitToBindingPoint[t];
        if (vk >= 0 && vk < kMaxVkBinding) sh->textureSlot[t] = b.sampler[vk];
    }
    sh->ufBlockSlot = int8_t(b.ufBlockSlot >= 0 && b.ufBlockSlot < kMaxUniformBuffers ? b.ufBlockSlot : -1);
    sh->uboCount = 0;
    for (int i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
        if (sh->uboSlot[i] >= 0 && sh->uboSlot[i] < kMaxUniformBuffers) sh->uboList[sh->uboCount++] = uint8_t(i);
    // the decompiler's Vulkan mapping and the bindings uam saw must agree (a mismatch would bind the wrong data)
    std::string why;
    if (b.ufBlockVkBinding != sh->mapping.uniformVarsBufferBindingPoint)
        why = "ufBlock binding " + std::to_string(b.ufBlockVkBinding) + " in the GLSL, " +
              std::to_string(sh->mapping.uniformVarsBufferBindingPoint) + " in the mapping";
    for (int i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS && why.empty(); i++)
        if (sh->mapping.uniformBuffersBindingPoint[i] >= 0 && sh->uboSlot[i] < 0)
            why = "uniform block " + std::to_string(i) + " is mapped but not in the GLSL";
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS && why.empty(); t++)
        if (sh->mapping.textureUnitToBindingPoint[t] >= 0 && sh->textureSlot[t] < 0)
            why = "texture unit " + std::to_string(t) + " is mapped but not in the GLSL";
    if (!why.empty()) {
        sh->status = ShaderStatus::Failed;
        sh->error = "resource mapping mismatch: " + why;
        if (g_failuresLogged++ < kLoggedFailures)
            LOG("[dk] %s %016llx (GLSL %016llx): %s; its draws are skipped", stage_name(sh->vertex),
                (unsigned long long)sh->key, (unsigned long long)sh->glslHash, sh->error.c_str());
        return;
    }
    sh->status = ShaderStatus::Ready;
}

// the shader's state from its code's (a code that became ready or failed since)
void refresh(Shader* sh) {
    const ShaderCode* code = sh->code;
    if (!code || !sh->pending()) return;
    if (code->state == ShaderCode::Ready) {
        set_bindings(sh, *code);
    } else if (code->state == ShaderCode::Failed) {
        sh->status = ShaderStatus::Failed;
        sh->error = code->error;
    }
}

// the worker's finished jobs into code memory, at most budget of them (render thread)
size_t load_results(size_t budget) {
    std::vector<JobResult> done;
    {
        std::lock_guard<std::mutex> lk(workerMutex);
        const size_t n = std::min(budget, results.size());
        done.assign(std::make_move_iterator(results.begin()), std::make_move_iterator(results.begin() + n));
        results.erase(results.begin(), results.begin() + n);
    }
    for (JobResult& r : done) {
        g_total.compileNs += r.compileNs + r.convertNs;
        auto it = codes.find(r.hash);
        if (it == codes.end()) continue;  // (not reached: every job has its code)
        ShaderCode& code = it->second;
        code.bindings = r.bindings;
        const double waitedMs = code.queuedAt ? double(now_ns() - code.queuedAt) / 1e6 : 0;
        if (r.dksh.empty()) {
            code.state = ShaderCode::Failed;
            code.error = r.error;
            g_total.failed++;
            if (g_failuresLogged++ < kLoggedFailures)
                LOG("[dk] %s GLSL %016llx failed%s: %s", stage_name(r.vertex), (unsigned long long)r.hash,
                    r.background ? " (background, from shadercache_gl.bin)" : "", r.error.c_str());
        } else {
            const uint64_t t0 = now_ns();
            char name[64];
            snprintf(name, sizeof name, "%s %016llx", stage_name(r.vertex), (unsigned long long)r.hash);
            if (code_load(code.dk, r.dksh.data(), uint32_t(r.dksh.size()), name)) {
                code.state = ShaderCode::Ready;
                g_total.compiled++;
                g_codeBytes += (r.dksh.size() + DK_SHADER_CODE_ALIGNMENT - 1) & ~size_t(DK_SHADER_CODE_ALIGNMENT - 1);
                R.perf.dkshLoads++;
            } else {
                code.state = ShaderCode::Failed;
                code.error = "its DKSH does not load (code memory full or not valid: see above)";
                g_total.failed++;
            }
            g_total.loadNs += now_ns() - t0;
            if (!r.background && waitedMs >= 1000)
                LOG("[dk] %s GLSL %016llx: ready %.1f s after it was queued (uam %.0f ms)", stage_name(r.vertex),
                    (unsigned long long)r.hash, waitedMs / 1e3, double(r.compileNs) / 1e6);
        }
        // recorded either way: a failure is not tried again with the same compiler (uamId)
        cache_dksh(r.hash, r.vertex, r.bindings, r.dksh);
    }
    return done.size();
}

// the priorities of the queued jobs from the draws skipped in the last frame (render thread)
void update_priorities() {
    if (wantedCodes.empty()) return;
    std::unordered_map<uint64_t, std::pair<uint32_t, uint64_t>> wanted;  // hash -> (skipped draws, partner)
    for (ShaderCode* c : wantedCodes) {
        wanted[c->hash] = {c->wantedFrame, c->partner};
        c->wantedFrame = 0;
    }
    wantedCodes.clear();
    std::lock_guard<std::mutex> lk(workerMutex);
    for (Job& j : jobs) {
        auto it = wanted.find(j.hash);
        j.priority = it == wanted.end() ? 0 : it->second.first;
        if (it != wanted.end() && it->second.second) j.partner = it->second.second;
    }
}

// ---- translation
// null, or why the program cannot be translated
const char* decompile(const uint32_t* regs, bool vertex, LatteFetchShader* fetch, uint64_t base, uint32_t address,
                      uint32_t size, LatteDecompilerOutput_t& output) {
    if ((regs[Latte::REGADDR::VGT_GS_MODE] & 3) != 0) return "geometry shaders are not implemented by the deko3d renderer";
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

// the bytes of GX2 uniform block i the GLSL declares ("vec4 uf_blockVS<i>[n];"): 0 when not found
uint32_t declared_block_bytes(const std::string& glsl, bool vertex, int i) {
    char name[32];
    snprintf(name, sizeof name, "uf_block%s%d[", vertex ? "VS" : "PS", i);
    const size_t at = glsl.find(name);
    if (at == std::string::npos) return 0;
    const unsigned long n = strtoul(glsl.c_str() + at + strlen(name), nullptr, 10);
    return uint32_t(std::min<unsigned long>(n, DK_UNIFORM_BUF_MAX_SIZE / 16) * 16);
}

// the shader's code: the one of its GLSL hash, or a new one queued for the worker
ShaderCode* code_for(Shader* sh, std::string&& glsl) {
    auto [it, inserted] = codes.try_emplace(sh->glslHash);
    ShaderCode& code = it->second;
    if (!inserted) {
        if (code.state == ShaderCode::Ready) {
            g_total.memoryHits += code.origin == ShaderCode::Session;
            g_total.cacheHits += code.origin != ShaderCode::Session;
            g_total.offlineHits += code.origin == ShaderCode::Offline;
            g_total.localHits += code.origin == ShaderCode::Local;
        }
        return &code;
    }
    code.hash = sh->glslHash;
    code.vertex = sh->vertex;
    code.state = ShaderCode::Queued;
    code.origin = ShaderCode::Session;
    code.queuedAt = now_ns();
    g_total.queued++;
    Job job;
    job.hash = sh->glslHash;
    job.vertex = sh->vertex;
    job.glsl = std::move(glsl);
    job.priority = g_waitForWorker ? ~0u : 1;  // (a draw asked for it now)
    queue_job(std::move(job));
    return &code;
}

// wait mode (WWHD_DK_SHADER_BUDGET=0): the render thread waits for the worker to finish code
void wait_for(ShaderCode* code, uint64_t hash) {
    Stage stage("deko3d: waiting for the shader compiler (WWHD_DK_SHADER_BUDGET=0)");
    {
        std::lock_guard<std::mutex> lk(workerMutex);
        for (Job& j : jobs)
            if (j.hash == hash) j.priority = ~0u;  // (a background job from shadercache_gl.bin too)
    }
    while (code->state == ShaderCode::Queued) {
        {
            std::unique_lock<std::mutex> lk(workerMutex);
            doneCv.wait(lk, [&] {
                for (const JobResult& r : results)
                    if (r.hash == hash) return true;
                return false;
            });
        }
        load_results(~size_t{0});
    }
}

// the counts since b (which becomes now); pendingNow and codeBytes as they are
ShaderStats stats_since(ShaderStats& b) {
    ShaderStats d;
    const ShaderStats a = g_total;
    d.translations = a.translations - b.translations;
    d.memoryHits = a.memoryHits - b.memoryHits;
    d.cacheHits = a.cacheHits - b.cacheHits;
    d.offlineHits = a.offlineHits - b.offlineHits;
    d.localHits = a.localHits - b.localHits;
    d.queued = a.queued - b.queued;
    d.compiled = a.compiled - b.compiled;
    d.failed = a.failed - b.failed;
    d.compileNs = a.compileNs - b.compileNs;
    d.loadNs = a.loadNs - b.loadNs;
    d.skippedDraws = a.skippedDraws - b.skippedDraws;
    {
        std::lock_guard<std::mutex> lk(workerMutex);
        d.pendingNow = jobs.size() + results.size() + (workerCurrent ? 1 : 0);
    }
    d.codeBytes = g_codeBytes;
    b = a;
    return d;
}

// the 5 s line of the worker and the caches (logged when anything happened)
void log_total() {
    static auto lastAt = std::chrono::steady_clock::now();
    static ShaderStats last;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastAt < std::chrono::seconds(5)) return;
    const double secs = std::chrono::duration<double>(now - lastAt).count();
    lastAt = now;
    size_t queued, foreground, ready;
    {
        std::lock_guard<std::mutex> lk(workerMutex);
        queued = jobs.size() + (workerCurrent ? 1 : 0);
        foreground = foregroundQueued;
        ready = results.size();
    }
    const ShaderStats d = stats_since(last);
    static uint64_t lastStoredHits = 0, lastStoredMisses = 0;
    if (!d.translations && !d.compiled && !d.failed && !d.skippedDraws && !queued && !ready &&
        g_storedHits == lastStoredHits)
        return;
    LOG("[dk] shaders: queue %zu (%zu asked by draws, %zu done waiting for a frame), %.1f compiles/s, %.0f ms per compile, "
        "%llu failures (%llu since start-up); %llu translations: cache hits %llu offline / %llu local / %llu this "
        "session; %llu draws skipped for a pending shader; shader code %llu KiB; %llu variants from translation "
        "records (%llu records without their DKSH)",
        queued, foreground, ready, double(d.compiled) / secs,
        d.compiled + d.failed ? double(d.compileNs) / 1e6 / double(d.compiled + d.failed) : 0.0,
        (unsigned long long)d.failed, (unsigned long long)g_total.failed, (unsigned long long)d.translations,
        (unsigned long long)d.offlineHits, (unsigned long long)d.localHits, (unsigned long long)d.memoryHits,
        (unsigned long long)d.skippedDraws, (unsigned long long)(g_codeBytes >> 10),
        (unsigned long long)(g_storedHits - lastStoredHits), (unsigned long long)(g_storedMisses - lastStoredMisses));
    lastStoredHits = g_storedHits;
    lastStoredMisses = g_storedMisses;
}
}  // namespace

// ---- lookup
bool fetch_shader_range(const uint32_t* regs, uint32_t& address, uint32_t& size, bool* compactOut) {
    address = regs[mmSQ_PGM_START_FS] << 8;
    if (!address) return false;
    const bool compact = ld32(address) == 0x57574653;
    const uint32_t count = compact ? ld32(address + 4) : 0;
    if (compact && count > 64) return false;
    size = compact ? 16 + count * 16 : regs[mmSQ_PGM_START_FS + 1] << 3;
    if (!size || size > 0x1000 || uint64_t(address) + size > 0x100000000ull) return false;
    if (compactOut) *compactOut = compact;
    return true;
}

LatteFetchShader* get_fetch_shader(const uint32_t* regs, uint64_t* keyOut, uint64_t frame) {
    if (keyOut) *keyOut = 0;
    uint32_t address, size;
    bool compact = false;
    if (!fetch_shader_range(regs, address, size, &compact)) return nullptr;
    uint64_t key = program_hash(address, size, frame);
    if (keyOut) *keyOut = key;
    if (auto it = fetchShaders.find(key); it != fetchShaders.end()) return it->second;
    auto* fetch = compact ? gx2::build_fetch_shader(address)
                          : LatteShaderRecompiler_createFetchShader(key, const_cast<uint32_t*>(regs),
                                                                    reinterpret_cast<uint32_t*>(ppc_ptr(address)), size);
    fetchShaders.emplace(key, fetch);
    return fetch;
}

// a program's entry in programHashes (map nodes do not move; reset_shader_memoization, which clears the map,
// also advances R.shaderEpoch, which drops the references)
void* program_hash_ref(uint32_t address, uint32_t size) { return &programHashes[(uint64_t(address) << 32) | size]; }
uint64_t program_hash_of(void* ref, uint32_t address, uint32_t size, uint64_t frame) {
    return program_hash_at(*static_cast<ProgramHash*>(ref), address, size, frame);
}

uint64_t shader_state_hash(const uint32_t* regs, bool vertex, uint64_t* core) {
    *core = state_hash(regs, vertex ? 0x1111 : 0x2222, vertex);
    return *core ^ texture_state_hash(regs, vertex, (1u << LATTE_NUM_MAX_TEX_UNITS) - 1) * 0x9E3779B97F4A7C15ull;
}

uint64_t shader_combo_state_hash(const uint32_t* regs, uint32_t primitive) {
    // the shared words once, each stage's own words, then both stages' texture unit states two to a word
    std::array<uint32_t, 2 * kStateWords + LATTE_NUM_MAX_TEX_UNITS> state;
    size_t count = gather_state(regs, true, primitive & 0x3F, true, true, state.data(), 0);
    count = gather_state(regs, false, 0, false, true, state.data(), count);
    for (uint32_t t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++)
        state[count++] = texture_unit_state(regs, true, t) << 16 | texture_unit_state(regs, false, t);
    return hash_words4(state.data(), count, 0x3333);
}

namespace {
// a shader rebuilt from its translation record (round 45): null when the record does not fit or its DKSH is not
// in code memory (then the decompiler runs as before)
Shader* shader_from_record(uint64_t key, bool vertex, std::pair<uint32_t, uint32_t> at) {
    ByteReader r{storedBlob.data() + at.first, storedBlob.data() + at.first + at.second};
    const bool recVertex = r.get<uint8_t>() != 0;
    const uint64_t recKey = r.get<uint64_t>();
    r.get<uint64_t>();  // base
    r.get<uint32_t>();  // units
    const uint64_t glslHash = r.get<uint64_t>();
    if (!r.ok || recVertex != vertex || recKey != key) return nullptr;
    const auto cit = codes.find(glslHash);
    if (cit == codes.end() || cit->second.state != ShaderCode::Ready) {
        g_storedMisses++;
        return nullptr;
    }
    auto owned = std::make_unique<Shader>();
    Shader* shader = owned.get();
    shader->fragmentWhy = r.get<uint8_t>();
    auto dec = std::make_unique<LatteDecompilerShader>(vertex ? LatteConst::ShaderType::Vertex : LatteConst::ShaderType::Pixel);
    dec->pixelColorOutputMask = r.get<uint32_t>();
    dec->textureUnitListCount = r.get<uint8_t>();
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) dec->textureUnitList[t] = r.get<uint8_t>();
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) dec->textureUnitSamplerAssignment[t] = r.get<uint16_t>();
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) dec->textureUsesDepthCompare[t] = r.get<uint8_t>() != 0;
    shader->mapping = r.get<LatteDecompilerShaderResourceMapping>();
    shader->uniforms = r.get<LatteDecompilerOutputUniformOffsets>();
    for (uint32_t& b : shader->uboBytes) b = r.get<uint32_t>();
    const uint32_t nRegs = r.get<uint32_t>();
    for (uint32_t i = 0; i < nRegs && r.ok && i < 4096; i++) {
        LatteFastAccessRemappedUniformEntry_register_t e;
        e.indexOffset = r.get<uint32_t>();
        e.mappedIndexOffset = r.get<uint32_t>();
        dec->list_remappedUniformEntries_register.push_back(e);
    }
    const uint32_t nGroups = r.get<uint32_t>();
    for (uint32_t g = 0; g < nGroups && r.ok && g < 64; g++) {
        const uint16_t bufferId = r.get<uint16_t>(), bank = r.get<uint16_t>();
        dec->list_remappedUniformEntries_bufferGroups.emplace_back(bufferId, bank);
        const uint32_t n = r.get<uint32_t>();
        for (uint32_t i = 0; i < n && r.ok && i < 4096; i++) {
            LatteFastAccessRemappedUniformEntry_buffer_t e;
            e.indexOffset = r.get<uint16_t>();
            e.mappedIndexOffset = r.get<uint16_t>();
            dec->list_remappedUniformEntries_bufferGroups.back().entries.push_back(e);
        }
    }
    if (!r.ok || r.p != r.end) return nullptr;  // (a record of another layout: the decompiler runs)
    shader->key = key;
    shader->vertex = vertex;
    shader->glslHash = glslHash;
    shader->dec = dec.release();
    fill_texture_lists(shader);
    const auto& u = shader->uniforms;
    shader->scaleUniforms = u.offset_fragCoordScale >= 0;
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) shader->scaleUniforms |= u.offset_texScale[t] >= 0;
    shader->fragmentEffects = vertex || shader->fragmentWhy != 0;
    shader->code = code_for(shader, std::string());  // (the Ready code: counted as a cache hit)
    refresh(shader);
    shaders.emplace(key, std::move(owned));
    (vertex ? g_lastVs : g_lastPs) = shader;
    g_storedHits++;
    return shader;
}
}  // namespace

Shader* translate(const uint32_t* regs, bool vertex, LatteFetchShader* fetch, uint64_t fsKey, uint64_t frame,
                  uint64_t coreHash) {
    uint32_t start = vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
    uint32_t address = regs[start] << 8, size = regs[start + 1] << 3;
    if (!address || !size || size > 0x100000 || uint64_t(address) + size > 0x100000000ull) return nullptr;
    uint64_t base = program_hash(address, size, frame) ^ (vertex ? 0x1111 : 0x2222);
    // the key holds the texture state of only the units the program samples (known after its first
    // translation), as gfx/gl. Declared inputs no fetch attribute fills turn the decompiler's strict multiplication
    // on: they are in the key (the keys and translation records of the other shaders stay as they were).
    const uint32_t unfetched = vertex ? LatteDecompiler_UnfetchedVertexInputs(regs, fetch) : 0;
    auto keyFor = [&](uint32_t units) {
        return (coreHash ^ (base * 0xFF51AFD7ED558CCDull + 0x2545F4914F6CDD1Dull)) ^ (vertex ? fsKey * 31 : 0) ^
               texture_state_hash(regs, vertex, units) * 0xC2B2AE3D27D4EB4Full ^ uint64_t(unfetched) * 0x9E3779B97F4A7C15ull;
    };
    auto known = textureUnits.find(base);
    uint64_t key = keyFor(known != textureUnits.end() ? known->second : 0);
    if (auto it = shaders.find(key); it != shaders.end()) {
        Shader* sh = it->second.get();
        if (sh->pending()) {
            refresh(sh);
            if (g_waitForWorker && sh->pending() && sh->code) {
                wait_for(sh->code, sh->glslHash);
                refresh(sh);
            }
        }
        (vertex ? g_lastVs : g_lastPs) = sh;
        return sh;
    }
    ScopedTime timer{R.perf.shaderNs};
    if (g_transCache)  // (round 45) a variant of an earlier session: rebuilt from its record, no decompiler
        if (auto it = storedTrans.find(key); it != storedTrans.end())
            if (Shader* sh = shader_from_record(key, vertex, it->second)) return sh;
    R.perf.shaders++;
    g_total.translations++;
    auto owned = std::make_unique<Shader>();
    Shader* shader = owned.get();
    shader->key = key;
    shader->vertex = vertex;
    shaders.emplace(key, std::move(owned));
    (vertex ? g_lastVs : g_lastPs) = shader;
    LatteDecompilerOutput_t output{};
    Stage stage(vertex ? "translating a vertex shader" : "translating a pixel shader");
    if (const char* error = decompile(regs, vertex, fetch, base, address, size, output)) {
        shader->status = ShaderStatus::Failed;
        shader->error = error;
        if (g_failuresLogged++ < kLoggedFailures)
            LOG("[dk] %s at %08X (%u bytes, program %016llx): %s; its draws are skipped", stage_name(vertex), address,
                size, (unsigned long long)base, error);
        return shader;
    }
    if (output.strictMulInputs && g_strictMulLogged++ < kLoggedFailures)
        LOG("[dk] vertex shader at %08X (program %016llx) uses input slots %X that its fetch shader does not fill: "
            "translated with the GPU's multiplication (0 * x = 0), as the Rito post office's letters need",
            address, (unsigned long long)base, output.strictMulInputs);
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
    fill_texture_lists(shader);  // (the hot copies, dk_shaders.h)
    shader->mapping = output.resourceMappingVK;
    shader->uniforms = output.uniformOffsetsVK;
    std::string glsl = shader->dec->strBuf_shaderSource->c_str();
    delete shader->dec->strBuf_shaderSource;  // tens of KB per variant, never read again
    shader->dec->strBuf_shaderSource = nullptr;
    shader->glslHash = hash_bytes(glsl.data(), glsl.size(), vertex ? 0x1111 : 0x2222);
    for (int i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
        if (shader->mapping.uniformBuffersBindingPoint[i] >= 0) shader->uboBytes[i] = declared_block_bytes(glsl, vertex, i);
    const auto& u = shader->uniforms;
    shader->scaleUniforms = u.offset_fragCoordScale >= 0;
    if (!vertex) {
        auto count = [&](const char* w) {
            size_t n = 0;
            for (size_t at = glsl.find(w); at != std::string::npos; at = glsl.find(w, at + 1)) n++;
            return n;
        };
        // the decompiler's alpha test: "if( ((...).a > uf_alphaTestRef) == false) discard;" (or a bare discard
        // for NEVER); KILL instructions give other discards
        const size_t discards = count("discard"), alphaTests = count("uf_alphaTestRef) == false) discard");
        const bool depthOrMemory = count("gl_FragDepth") || count("imageStore") || count("imageAtomic") || count("atomic");
        shader->fragmentWhy = uint8_t((alphaTests ? 1 : 0) | (discards > alphaTests ? 2 : 0) | (depthOrMemory ? 4 : 0));
        shader->fragmentEffects = shader->fragmentWhy != 0;
    }
    for (int t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++) shader->scaleUniforms |= u.offset_texScale[t] >= 0;
    // shadercache_gl.bin as gfx/gl writes it
    cache_gl_source(shader->glslHash, vertex, glsl);
    cache_gl_translation(shader, base, units, output.resourceMappingGL, uint32_t(output.uniformOffsetsGL.count_uniformRegister));
    cache_dk_translation(shader, base, units);
    shader->code = code_for(shader, std::move(glsl));
    refresh(shader);
    if (g_waitForWorker && shader->pending()) {
        wait_for(shader->code, shader->glslHash);
        refresh(shader);
    }
    return shader;
}

void shader_wanted(Shader* sh) {
    if (!sh) return;
    sh->wanted++;
    g_total.skippedDraws++;
    ShaderCode* code = sh->code;
    if (!code || !sh->pending()) return;
    if (!code->wantedFrame++) wantedCodes.push_back(code);
    // the other stage of this draw (translated just before) waits too: compiled right after this one
    Shader* other = sh->vertex ? g_lastPs : g_lastVs;
    if (other && other != sh && other->pending() && other->code) {
        code->partner = other->glslHash;
        if (!other->code->wantedFrame++) wantedCodes.push_back(other->code);
        other->code->partner = sh->glslHash;
    }
}

void reset_shader_memoization() {
    programHashes.clear();
    g_lastVs = g_lastPs = nullptr;
    R.shaderEpoch++;
}

// ---- start-up, frames, shutdown
void shaders_init(void (*progress)(size_t done, size_t total)) {
    const uint64_t t0 = now_ns();
    const uint64_t uamId = dksh_uam_id();
    {
        const char* e = getenv("WWHD_DK_SHADER_BUDGET");
        if (e && *e) {
            g_loadBudget = atoi(e);
            g_waitForWorker = g_loadBudget <= 0;
            if (g_loadBudget <= 0) g_loadBudget = 1 << 30;
        }
        if (g_waitForWorker)
            LOG("[dk] shader budget off (WWHD_DK_SHADER_BUDGET=0): a draw whose shader is new waits for the compiler");
        else
            LOG("[dk] shader budget: draws of shaders the compiler has not finished are skipped; up to %d DKSH loads "
                "a frame (WWHD_DK_SHADER_BUDGET)", g_loadBudget);
    }
    // the two DKSH files, whole, into code memory: progress counts their bytes read, then loaded
    struct stat st;
    size_t total = 0;
    for (const char* p : {kOfflinePath, kLocalPath})
        if (stat(p, &st) == 0) total += size_t(st.st_size) * 2;
    total = std::max<size_t>(total, 1);
    size_t done = 0;
    if (progress) progress(0, total);
    size_t loadedFiles[2] = {}, failedRecords[2] = {}, badLoads = 0;
    uint64_t readNs = 0;
    bool localValid = false;
    std::vector<uint8_t> localKeep;  // the local file's valid part, when it must be rewritten
    for (int which = 0; which < 2; which++) {
        const char* path = which ? kLocalPath : kOfflinePath;
        const uint64_t r0 = now_ns();
        std::vector<uint8_t> data = read_file(path, progress, done, total);
        readNs += now_ns() - r0;
        done += data.size();
        if (data.empty()) {
            LOG("[dk] shader cache %s: none%s", path,
                which ? "" : " (copy build/shader-harvest/shadercache_dksh.bin next to the NRO: without it every "
                             "shader is compiled on the console, ~100 ms each, and its draws wait)");
            continue;
        }
        std::vector<DkshRecord> records;
        uint64_t fileId = 0;
        std::string error;
        if (!read_wdk1(data, &records, &fileId, &error)) {
            LOG("[dk] shader cache %s: not used: %s", path, error.c_str());
            continue;
        }
        if (fileId != uamId) {
            LOG("[dk] shader cache %s: not used: compiled for uamId %016llx, this build is %016llx (%s, glsl_to_deko "
                "revision %d)%s", path, (unsigned long long)fileId, (unsigned long long)uamId, kDkshCompilerName,
                kConvertRevision, which ? ": started again" : ": rebuild it with tools/switch/dksh_cache");
            continue;
        }
        if (which) localValid = true;
        size_t step = 0, recordBytes = data.size() / std::max<size_t>(records.size(), 1);
        for (DkshRecord& r : records) {
            done += recordBytes;
            if (progress && (++step & 63) == 0) progress(done, total);
            auto [it, inserted] = codes.try_emplace(r.glslHash);
            if (!inserted) continue;  // (the offline file's comes first; a hash twice in a file: the first)
            ShaderCode& code = it->second;
            code.hash = r.glslHash;
            code.vertex = r.stage == uint8_t(uam::Stage::Vertex);
            code.origin = which ? ShaderCode::Local : ShaderCode::Offline;
            code.bindings.clear();
            memcpy(code.bindings.ubo, r.ubo, sizeof r.ubo);
            memcpy(code.bindings.sampler, r.sampler, sizeof r.sampler);
            code.bindings.uboCount = r.uboCount;
            code.bindings.samplerCount = r.samplerCount;
            code.bindings.ufBlockSlot = r.ufBlockSlot;
            code.bindings.ufBlockVkBinding = r.ufBlockVkBinding;
            if (r.dksh.empty()) {
                code.state = ShaderCode::Failed;
                code.error = std::string("failed with this compiler when ") + (which ? "this console" : "the offline cache") +
                             " compiled it (" + path + ")";
                failedRecords[which]++;
                continue;
            }
            char name[64];
            snprintf(name, sizeof name, "%s %016llx (%s)", stage_name(code.vertex), (unsigned long long)r.glslHash, path);
            if (code_load(code.dk, r.dksh.data(), uint32_t(r.dksh.size()), name)) {
                code.state = ShaderCode::Ready;
                loadedFiles[which]++;
                g_codeBytes += (r.dksh.size() + DK_SHADER_CODE_ALIGNMENT - 1) & ~size_t(DK_SHADER_CODE_ALIGNMENT - 1);
            } else {
                // logged by code_load; compiled again by the worker if a draw needs it
                codes.erase(it);
                badLoads++;
            }
        }
        if (which && localValid && data.size() > kWdk1HeaderSize) {
            // a record cut short (a session that ended mid-write) ends the file: keep the whole records only
            std::vector<uint8_t> rewritten = wdk1_header(uamId);
            for (const DkshRecord& r : records) append_wdk1_record(&rewritten, r);
            if (rewritten.size() != data.size()) localKeep = std::move(rewritten);
        }
    }
    if (progress) progress(total, total);
    // the local file: started again unless valid; then appended
    if (!localValid || !localKeep.empty()) {
        if (FILE* f = fopen(kLocalPath, "wb")) {
            const std::vector<uint8_t> bytes = localValid ? localKeep : wdk1_header(uamId);
            fwrite(bytes.data(), 1, bytes.size(), f);
            fclose(f);
        }
    }
    localFile.f = fopen(kLocalPath, "ab");
    if (!localFile.f) LOG("[dk] shader cache: cannot write %s: what this console compiles is not kept", kLocalPath);
    const uint64_t t1 = now_ns();
    LOG("[dk] shader caches (uamId %016llx): %zu DKSH from %s, %zu from %s (%zu and %zu recorded as failed); %zu did "
        "not load; %.0f ms (reading %.0f ms), game shader code %llu KiB of %u MiB",
        (unsigned long long)uamId, loadedFiles[0], kOfflinePath, loadedFiles[1], kLocalPath, failedRecords[0],
        failedRecords[1], badLoads, double(t1 - t0) / 1e6, double(readNs) / 1e6, (unsigned long long)(g_codeBytes >> 10),
        kCodeSize >> 20);
    if (badLoads) LOG("[dk] shader caches: %zu DKSH DID NOT LOAD (see the lines above)", badLoads);

    // translation records (round 45): a variant of an earlier session skips the decompiler (shader_from_record);
    // each record also gives its program's texture units, which the keys are made with
    if (g_transCache) {
        std::vector<uint8_t> t = read_file(kTransPath, nullptr, 0, 0);
        uint32_t version = 0;
        if (t.size() >= 8) memcpy(&version, t.data() + 4, 4);
        const bool good = t.size() >= 8 && !memcmp(t.data(), kTransMagic, 4) && version == kTransVersion;
        size_t at = good ? 8 : 0;
        while (good && at + 4 <= t.size()) {
            uint32_t size;
            memcpy(&size, t.data() + at, 4);
            if (size < 1 + 8 + 8 + 4 + 8 || at + 4 + size > t.size()) break;
            const uint8_t* payload = t.data() + at + 4;
            uint64_t key, base;
            uint32_t units;
            memcpy(&key, payload + 1, 8);
            memcpy(&base, payload + 9, 8);
            memcpy(&units, payload + 17, 4);
            storedTrans[key] = {uint32_t(at + 4), size};
            transWritten.insert(key);
            textureUnits[base] = units;
            at += 4 + size;
        }
        if (!good || at < t.size()) {  // another version, a new file or a cut record: written again (valid part kept)
            if (FILE* f = fopen(kTransPath, "wb")) {
                if (good) fwrite(t.data(), 1, at, f);
                else {
                    fwrite(kTransMagic, 1, 4, f);
                    fwrite(&kTransVersion, 1, 4, f);
                }
                fclose(f);
            }
            if (!good) {
                storedTrans.clear();
                transWritten.clear();
            }
        }
        storedBlob = std::move(t);
        transFile.f = fopen(kTransPath, "ab");
        LOG("[dk] %s: %zu translation records (version %u%s); variants seen before skip the decompiler "
            "(WWHD_DK_TRANSLATION_CACHE=0 off)", kTransPath, storedTrans.size(), kTransVersion,
            good ? "" : ", started again");
    } else
        LOG("[dk] translation records: off (WWHD_DK_TRANSLATION_CACHE=0)");

    // shadercache_gl.bin (gfx/gl load_shader_cache's scan): its sources and translations are not written again;
    // its sources without DKSH are compiled in the background
    std::vector<uint8_t> data = read_file(kGlCachePath, nullptr, 0, 0);
    const bool fresh = data.size() < 4 || memcmp(data.data(), kGlCacheMagic, 4) != 0;
    size_t valid = fresh ? 0 : 4, background = 0;
    while (!fresh && valid < data.size()) {
        const uint8_t* r = data.data() + valid;
        size_t length = 0;
        if (r[0] == 2) length = 17;
        else if (r[0] == 3 && valid + 5 <= data.size()) {
            uint32_t size;
            memcpy(&size, r + 1, 4);
            length = 5 + size_t(size);
        } else if (r[0] == 1 && valid + 18 <= data.size()) {
            uint32_t packed;
            memcpy(&packed, r + 10, 4);
            length = 18 + size_t(packed);
        }
        if (!length || valid + length > data.size()) break;
        if (r[0] == 1) {
            uint64_t hash;
            uint32_t sizes[2];
            memcpy(&hash, r + 2, 8);
            memcpy(sizes, r + 10, 8);
            if (glSources.insert(hash).second && !codes.count(hash)) {
                ShaderCode& code = codes[hash];
                code.hash = hash;
                code.vertex = r[1] != 0;
                code.state = ShaderCode::Queued;
                code.origin = ShaderCode::Session;
                Job job;
                job.hash = hash;
                job.vertex = code.vertex;
                job.background = true;
                job.packed.assign(r + 18, r + 18 + sizes[0]);
                job.size = sizes[1];
                queue_job(std::move(job));
                background++;
            }
        } else if (r[0] == 3 && length >= 5 + 9) {
            uint64_t key;
            memcpy(&key, r + 6, 8);  // payload: u8 vertex, u64 key, ...
            glTranslations.insert(key);
        }
        valid += length;
    }
    // a new file, or one whose last record was cut short, is rewritten before appending (as gfx/gl)
    if (fresh || valid < data.size()) {
        if (FILE* f = fopen(kGlCachePath, "wb")) {
            if (fresh) fwrite(kGlCacheMagic, 1, 4, f);
            else fwrite(data.data(), 1, valid, f);
            fclose(f);
        }
    }
    glFile.f = fopen(kGlCachePath, "ab");
    if (!glFile.f) LOG("[dk] shader cache: cannot write %s", kGlCachePath);
    LOG("[dk] %s: %zu sources, %zu translations; %zu sources without DKSH queued for the compiler (background)",
        kGlCachePath, glSources.size(), glTranslations.size(), background);
    std::vector<uint8_t>().swap(data);
    if (!writerStarted && (glFile.f || localFile.f || transFile.f)) {
        writerStarted = true;
        host::start_thread(cache_writer, 128 << 10);
    }
    start_worker();
    log_heap("after the deko3d shader caches");
}

void shaders_frame_start() {
    update_priorities();
    load_results(size_t(g_loadBudget));
    log_total();
}

void save_shader_cache() {
    {
        std::lock_guard<std::mutex> lk(workerMutex);
        workerStop = true;  // (a compile in progress finishes; its result is not written)
    }
    workerCv.notify_all();
    std::unique_lock<std::mutex> lk(writerMutex);
    write_pending(glFile, lk);
    write_pending(localFile, lk);
    write_pending(transFile, lk);
    LOG("[dk] shader caches saved: %llu bytes to %s, %llu to %s this session", (unsigned long long)glFile.written,
        kGlCachePath, (unsigned long long)localFile.written, kLocalPath);
}

// ---- uniforms (gfx/vulkan/shaders.cpp pack_uniforms_into, with the per-unit texture scale of gfx/gl)
StreamSlice pack_uniforms(bool vertex, const Shader& sh, const uint32_t* regs, float scaleX, float scaleY,
                          const float (*texScale)[2], bool aoNoise) {
    if (sh.bindings.ufBlockSlot < 0 || !sh.dec) return {};
    ScopedTime timer{R.perf.uniformPackNs};
    const auto& offsets = sh.uniforms;
    static std::vector<uint8_t> data;  // (render thread)
    data.assign(size_t(std::max(offsets.offset_endOfBlock, 16) + 15) & ~size_t(15), 0);
    auto copy = [&](int offset, const void* src, size_t size) {
        if (offset >= 0 && size_t(offset) + size <= data.size()) memcpy(data.data() + offset, src, size);
    };
    auto put = [&](int offset, float value) { copy(offset, &value, sizeof value); };
    auto bitsf = [](uint32_t value) {
        float result;
        memcpy(&result, &value, 4);
        return result;
    };
    const uint32_t aluBase = mmSQ_ALU_CONSTANT0_0 + (vertex ? 0x400 : 0);
    const uint32_t blockBase = vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
    if (offsets.offset_remapped >= 0) {
        for (const auto& entry : sh.dec->list_remappedUniformEntries_register)
            copy(offsets.offset_remapped + entry.mappedIndexOffset, regs + aluBase + entry.indexOffset / 4, 16);
        for (const auto& group : sh.dec->list_remappedUniformEntries_bufferGroups) {
            const uint32_t address = regs[blockBase + group.kcacheBankIdOffset / 4];
            if (!address) continue;
            for (const auto& entry : group.entries)
                copy(offsets.offset_remapped + entry.mappedIndexOffset, ppc_ptr(address + entry.indexOffset), 16);
        }
        // AO mode 2 (the occlusion vertex shader): the constant at remapped offset 0, .w x1.5 (as gfx/gl, whose
        // patch also leaves a constant that is not there at zero)
        if (aoNoise && size_t(offsets.offset_remapped) + 16 <= data.size()) {
            float w;
            memcpy(&w, data.data() + offsets.offset_remapped + 12, 4);
            w *= 1.5f;
            memcpy(data.data() + offsets.offset_remapped + 12, &w, 4);
        }
    }
    if (offsets.offset_uniformRegister >= 0)
        copy(offsets.offset_uniformRegister, regs + aluBase, size_t(offsets.count_uniformRegister) * 16);
    put(offsets.offset_alphaTestRef, bitsf(regs[Latte::REGADDR::SX_ALPHA_REF]));
    const float point = float(regs[Latte::REGADDR::PA_SU_POINT_SIZE] & 0xFFFF) / 8.0f;
    put(offsets.offset_pointSize, (point == 0 ? 0.125f : point) * scaleX);
    if (offsets.offset_windowSpaceToClipSpaceTransform >= 0) {
        const float width = 2.0f * bitsf(regs[Latte::REGADDR::PA_CL_VPORT_XSCALE]);
        const float height = -2.0f * bitsf(regs[Latte::REGADDR::PA_CL_VPORT_YSCALE]);
        put(offsets.offset_windowSpaceToClipSpaceTransform, width != 0 ? 2.0f / width : 0);
        put(offsets.offset_windowSpaceToClipSpaceTransform + 4, height != 0 ? 2.0f / height : 0);
    }
    if (offsets.offset_fragCoordScale >= 0) {
        const float scale[] = {scaleX != 0 ? 1.0f / scaleX : 1.0f, scaleY != 0 ? 1.0f / scaleY : 1.0f, 0, 0};
        copy(offsets.offset_fragCoordScale, scale, sizeof scale);
    }
    for (int unit = 0; unit < LATTE_NUM_MAX_TEX_UNITS; ++unit) {
        if (offsets.offset_texScale[unit] < 0) continue;
        const float one[2] = {1.0f, 1.0f};
        copy(offsets.offset_texScale[unit], texScale ? texScale[unit] : one, sizeof one);
    }
    return stream_upload(data.data(), uint32_t(data.size()), DK_UNIFORM_BUF_ALIGNMENT);
}

// ---- the ufBlock with a copy kept per shader (dk_shaders.h pack_uniforms_cached; P4 resources lane). The values
// and their order are pack_uniforms': remapped register constants, remapped constants of guest uniform blocks
// (zeros when the block has no address), the uniform registers, then the loose values.
struct UniformCache {
    struct Op {
        uint32_t dst;    // offset in the block (dst + 16 <= size)
        uint32_t src;    // register index (block == ~0u), else offset in the guest block
        uint32_t block;  // register holding the guest block's address, ~0u for a register constant
    };
    std::vector<Op> ops;
    std::vector<uint8_t> data;  // the values packed last; in mode 2 also what the GPU's copy holds
    uint32_t size = 0, bound = 0;  // bytes (pack_uniforms' size), bound bytes (a multiple of 256)
    int32_t aoDst = -1;            // offset_remapped: the AO fix's constant (aoNoise)
    int32_t regs = -1;             // offset_uniformRegister, when the whole register range fits
    uint32_t regBase = 0, regBytes = 0;
    int32_t alphaRef = -1, pointSize = -1, windowToClip = -1, fragCoordScale = -1;
    std::vector<std::pair<uint32_t, uint32_t>> texScales;  // offset, unit
    uint64_t frame = ~0ull;  // the frame its stream slice belongs to
    DkGpuAddr gpu = 0;
};

namespace {
UniformPackStats g_ufStats;

UniformCache* uniform_cache(bool vertex, Shader& sh) {
    if (sh.uf) return sh.uf;
    auto c = std::make_shared<UniformCache>();
    const auto& offsets = sh.uniforms;
    c->size = uint32_t(std::max(offsets.offset_endOfBlock, 16) + 15) & ~15u;
    c->bound = (c->size + 255) & ~255u;
    c->data.assign(c->size, 0);
    auto fits = [&](int offset, uint32_t bytes) { return offset >= 0 && uint32_t(offset) + bytes <= c->size; };
    const uint32_t aluBase = mmSQ_ALU_CONSTANT0_0 + (vertex ? 0x400 : 0);
    const uint32_t blockBase = vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
    if (offsets.offset_remapped >= 0) {
        c->aoDst = offsets.offset_remapped;
        for (const auto& entry : sh.dec->list_remappedUniformEntries_register) {
            const int dst = offsets.offset_remapped + int(entry.mappedIndexOffset);
            if (fits(dst, 16)) c->ops.push_back({uint32_t(dst), aluBase + entry.indexOffset / 4, ~0u});
        }
        for (const auto& group : sh.dec->list_remappedUniformEntries_bufferGroups)
            for (const auto& entry : group.entries) {
                const int dst = offsets.offset_remapped + int(entry.mappedIndexOffset);
                if (fits(dst, 16))
                    c->ops.push_back({uint32_t(dst), uint32_t(entry.indexOffset), blockBase + group.kcacheBankIdOffset / 4u});
            }
    }
    if (offsets.offset_uniformRegister >= 0 && fits(offsets.offset_uniformRegister, uint32_t(offsets.count_uniformRegister) * 16)) {
        c->regs = offsets.offset_uniformRegister;
        c->regBase = aluBase;
        c->regBytes = uint32_t(offsets.count_uniformRegister) * 16;
    }
    if (fits(offsets.offset_alphaTestRef, 4)) c->alphaRef = offsets.offset_alphaTestRef;
    if (fits(offsets.offset_pointSize, 4)) c->pointSize = offsets.offset_pointSize;
    if (fits(offsets.offset_windowSpaceToClipSpaceTransform, 8)) c->windowToClip = offsets.offset_windowSpaceToClipSpaceTransform;
    if (fits(offsets.offset_fragCoordScale, 16)) c->fragCoordScale = offsets.offset_fragCoordScale;
    for (int unit = 0; unit < LATTE_NUM_MAX_TEX_UNITS; ++unit)
        if (fits(offsets.offset_texScale[unit], 8)) c->texScales.push_back({uint32_t(offsets.offset_texScale[unit]), uint32_t(unit)});
    sh.ufCache = c;
    sh.uf = c.get();
    return c.get();
}
}  // namespace

StreamSlice pack_uniforms_cached(int mode, bool vertex, Shader& sh, const uint32_t* regs, float scaleX, float scaleY,
                                 const float (*texScale)[2], bool aoNoise) {
    if (sh.bindings.ufBlockSlot < 0 || !sh.dec) return {};
    UniformCache& c = *uniform_cache(vertex, sh);
    g_ufStats.blocks++;
    uint8_t* data = c.data.data();
    // changed 16-byte granules (the pieces mode 2 pushes)
    static uint64_t dirty[(0x10000 / 16) / 64];
    uint32_t lo = ~0u, hi = 0;
    auto mark = [&](uint32_t offset, uint32_t bytes) {
        const uint32_t first = offset >> 4, last = (offset + bytes - 1) >> 4;
        for (uint32_t g = first; g <= last; g++) dirty[g >> 6] |= 1ull << (g & 63);
        lo = std::min(lo, first);
        hi = std::max(hi, last + 1);
    };
    auto put = [&](uint32_t offset, const void* src, uint32_t bytes) {
        if (!memcmp(data + offset, src, bytes)) return;
        memcpy(data + offset, src, bytes);
        mark(offset, bytes);
    };
    auto put16 = [&](uint32_t offset, const void* src) {  // (the common case, compared as two words)
        uint64_t v[2], old[2];
        memcpy(v, src, 16);
        memcpy(old, data + offset, 16);
        if (v[0] == old[0] && v[1] == old[1]) return;
        memcpy(data + offset, v, 16);
        mark(offset, 16);
    };
    static const uint8_t zeros[16] = {};
    for (const UniformCache::Op& op : c.ops) {
        const uint8_t* src;
        if (op.block == ~0u) src = reinterpret_cast<const uint8_t*>(regs + op.src);
        else {
            const uint32_t address = regs[op.block];
            src = address ? static_cast<const uint8_t*>(ppc_ptr(address + op.src)) : zeros;
        }
        if (aoNoise && int32_t(op.dst) == c.aoDst) {  // AO mode 2: .w x1.5 (pack_uniforms)
            uint8_t v[16];
            memcpy(v, src, 16);
            float w;
            memcpy(&w, v + 12, 4);
            w *= 1.5f;
            memcpy(v + 12, &w, 4);
            put16(op.dst, v);
        } else
            put16(op.dst, src);
    }
    if (c.regs >= 0) {
        const auto* src = reinterpret_cast<const uint8_t*>(regs + c.regBase);
        for (uint32_t o = 0; o < c.regBytes; o += 16) put16(uint32_t(c.regs) + o, src + o);
    }
    auto bitsf = [](uint32_t value) {
        float result;
        memcpy(&result, &value, 4);
        return result;
    };
    if (c.alphaRef >= 0) {
        const float ref = bitsf(regs[Latte::REGADDR::SX_ALPHA_REF]);
        put(uint32_t(c.alphaRef), &ref, 4);
    }
    if (c.pointSize >= 0) {
        const float point = float(regs[Latte::REGADDR::PA_SU_POINT_SIZE] & 0xFFFF) / 8.0f;
        const float v = (point == 0 ? 0.125f : point) * scaleX;
        put(uint32_t(c.pointSize), &v, 4);
    }
    if (c.windowToClip >= 0) {
        const float width = 2.0f * bitsf(regs[Latte::REGADDR::PA_CL_VPORT_XSCALE]);
        const float height = -2.0f * bitsf(regs[Latte::REGADDR::PA_CL_VPORT_YSCALE]);
        const float v[2] = {width != 0 ? 2.0f / width : 0, height != 0 ? 2.0f / height : 0};
        put(uint32_t(c.windowToClip), v, 8);
    }
    if (c.fragCoordScale >= 0) {
        const float v[4] = {scaleX != 0 ? 1.0f / scaleX : 1.0f, scaleY != 0 ? 1.0f / scaleY : 1.0f, 0, 0};
        put(uint32_t(c.fragCoordScale), v, 16);
    }
    for (auto [offset, unit] : c.texScales) {
        static const float one[2] = {1.0f, 1.0f};
        put(offset, texScale ? texScale[unit] : one, 8);
    }
    const bool changed = lo < hi;  // (the granule bits are cleared at the end whatever the mode)
    auto clear_dirty = [&] {
        for (uint32_t w = lo >> 6; w < ((hi + 63) >> 6); w++) dirty[w] = 0;
    };
    const uint64_t frame = R.frame + 1;  // the frame being recorded: its stream slice
    StreamSlice out;
    if (mode == 1) {
        if (changed || c.frame != frame || !c.gpu) {
            const StreamSlice slice = stream_upload(data, c.size, DK_UNIFORM_BUF_ALIGNMENT);
            c.gpu = slice.gpu;
            c.frame = slice ? frame : ~0ull;
            if (slice) {
                g_ufStats.slices++;
                g_ufStats.sliceBytes += c.size;
            }
        } else
            g_ufStats.unchanged++;
        out = {c.gpu, c.size};
    } else {
        if (c.frame != frame || !c.gpu) {
            // this frame's copy: the whole block by the CPU (the slice is the frame's own memory; nothing reads
            // it before the commands recorded from now on)
            const StreamAlloc a = stream_alloc(c.bound, DK_UNIFORM_BUF_ALIGNMENT);
            c.gpu = a.gpu;
            c.frame = a ? frame : ~0ull;
            if (a) {
                memcpy(a.cpu, data, c.size);
                R.perf.streamBytes += c.size;
                g_ufStats.slices++;
                g_ufStats.sliceBytes += c.size;
            }
        } else if (changed) {
            // the changed granules, in runs; runs closer than 3 granules (a push's 7 command words) merge
            uint32_t g = lo;
            while (g < hi) {
                if (!(dirty[g >> 6] & (1ull << (g & 63)))) {
                    g++;
                    continue;
                }
                uint32_t end = g + 1, gap = 0;
                for (uint32_t k = g + 1; k < hi && gap < 3; k++) {
                    if (dirty[k >> 6] & (1ull << (k & 63))) {
                        end = k + 1;
                        gap = 0;
                    } else
                        gap++;
                }
                const uint32_t offset = g * 16, bytes = std::min(end * 16, c.size) - offset;
                dkCmdBufPushConstants(R.cmd, c.gpu, c.bound, offset, bytes, data + offset);
                g_ufStats.pushes++;
                g_ufStats.pushBytes += bytes;
                g = end;
            }
        } else
            g_ufStats.unchanged++;
        out = {c.gpu, c.bound};
    }
    if (changed) clear_dirty();
    return c.gpu ? out : StreamSlice{};
}

void prefetch_shader(const Shader* sh) {
    __builtin_prefetch(sh);
    __builtin_prefetch(reinterpret_cast<const char*>(sh) + 64);
}
void prefetch_shader_caches(const Shader* sh) {
    if (const UniformCache* c = sh->uf) {
        __builtin_prefetch(c->ops.data());
        __builtin_prefetch(c->data.data());
        if (c->data.size() > 64) __builtin_prefetch(c->data.data() + 64);
    }
    if (sh->vtx) {
        __builtin_prefetch(sh->vtx);
        __builtin_prefetch(reinterpret_cast<const char*>(sh->vtx) + 64);
    }
}

UniformPackStats uniform_pack_stats_take() {
    const UniformPackStats s = g_ufStats;
    g_ufStats = {};
    return s;
}

ShaderStats shader_stats_take() {
    static ShaderStats taken;
    return stats_since(taken);
}

}  // namespace gfxdk
