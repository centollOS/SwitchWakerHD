// Latte microcode -> Cemu GLSL (OpenGL flavour) -> GL shader objects and linked programs.
// Shader variants are keyed exactly as in gfx/vulkan/shaders.cpp.
#include "shaders.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "gx2/gx2.h"
#include "ppc.h"
#include "runtime.h"
#include "util/helpers/StringBuf.h"

LatteDecompilerShader* FinishDecompiledShader(LatteDecompilerOutput_t& output);
LatteFetchShader* LatteShaderRecompiler_createFetchShader(LatteFetchShader::CacheHash hash, uint32* regs, uint32* code,
                                                          uint32 size);

namespace gfxgl {
using Latte::REGADDR;
namespace {
std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders;
std::unordered_map<uint64_t, LatteFetchShader*> fetchShaders;
std::unordered_map<uint64_t, GLuint> objects;                   // GLSL hash -> compiled shader object
std::unordered_map<uint64_t, std::unique_ptr<Program>> linked;  // vertex and pixel GLSL hashes -> program
struct ProgramHash { uint64_t hash = 0, frame = ~uint64_t{0}; };
std::unordered_map<uint64_t, ProgramHash> programHashes;

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

uint64_t program_hash(uint32_t address, uint32_t size, uint64_t frame) {
    auto& entry = programHashes[(uint64_t(address) << 32) | size];
    if (entry.frame != frame) {
        entry.hash = hash_bytes(ppc_ptr(address), size);
        entry.frame = frame;
    }
    return entry.hash;
}

// every register the GLSL analyzer reads (gfx/vulkan/shaders.cpp state_hash)
uint64_t state_hash(const uint32_t* regs, uint64_t hash, bool vertex) {
    std::array<uint32_t, 105 + 5 * LATTE_NUM_MAX_TEX_UNITS> state;
    size_t count = 0;
    auto append = [&](const uint32_t* words, size_t length) {
        memcpy(state.data() + count, words, length * sizeof(uint32_t));
        count += length;
    };
    auto put = [&](uint32_t first, uint32_t length) { append(regs + first, length); };
    put(mmSQ_VTX_SEMANTIC_0, 32);
    put(mmSPI_VS_OUT_ID_0, 10);
    put(mmSPI_VS_OUT_CONFIG, 1);
    put(mmPA_CL_VS_OUT_CNTL, 1);
    put(mmSPI_PS_IN_CONTROL_0, 2);
    put(mmSPI_PS_INPUT_CNTL_0, 32);
    uint32_t primitiveState[] = {regs[REGADDR::VGT_PRIMITIVE_TYPE] & 0x3F, regs[mmSPI_INTERP_CONTROL_0] & (1u << 1),
                                 regs[mmVGT_STRMOUT_EN]};
    append(primitiveState, 3);
    if (regs[mmVGT_STRMOUT_EN])
        for (uint32_t buffer = 0; buffer < 4; ++buffer) put(mmVGT_STRMOUT_VTX_STRIDE_0 + buffer * 4, 1);
    put(REGADDR::VGT_GS_MODE, 1);
    put(REGADDR::SQ_CONFIG, 1);
    put(mmCB_SHADER_MASK, 1);
    put(mmCB_SHADER_CONTROL, 1);
    put(mmDB_SHADER_CONTROL, 1);
    put(mmSPI_INPUT_Z, 1);
    put(REGADDR::SX_ALPHA_TEST_CONTROL, 1);
    uint32_t transformState[] = {regs[REGADDR::PA_CL_VTE_CNTL] & 0x3F, regs[REGADDR::PA_CL_CLIP_CNTL] & (1u << 19),
                                 regs[REGADDR::DB_DEPTH_CONTROL] & 0x83};
    append(transformState, 3);
    put(REGADDR::CB_COLOR_CONTROL, 1);
    put(REGADDR::CB_TARGET_MASK, 1);
    put(mmCB_COLOR0_INFO, 8);
    uint32_t base = vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
    for (uint32_t t = 0; t < LATTE_NUM_MAX_TEX_UNITS; ++t) {
        const auto* w = regs + base + t * 7;
        uint32_t relevant[] = {(w[0] & 7) | (w[4] & 0x300), w[1] & 0x3F00000};
        append(relevant, 2);
    }
    for (uint32_t t = 0; t < LATTE_NUM_MAX_TEX_UNITS * 3; ++t) {
        uint32_t compare = regs[REGADDR::SQ_TEX_SAMPLER_WORD0_0 + t * 3] & 0xF8000000;
        append(&compare, 1);
    }
    return hash_bytes(state.data(), count * sizeof(uint32_t), hash);
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

void cache_write(const void* data, size_t size) {
    if (!cacheFile) return;
    fwrite(data, 1, size, cacheFile);
    fflush(cacheFile);
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

void cache_program(uint64_t vs, uint64_t ps) {
    uint8_t record[17] = {2};
    memcpy(record + 1, &vs, 8);
    memcpy(record + 9, &ps, 8);
    cache_write(record, sizeof record);
}

GLuint compile(bool vertex, const std::string& glsl, std::string* error) {
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

// a failed link leaves prog at 0
std::unique_ptr<Program> link(GLuint vsObj, GLuint psObj, uint64_t key) {
    R.perf.linked++;
    auto p = std::make_unique<Program>();
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vsObj);
    glAttachShader(prog, psObj);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        glGetProgramInfoLog(prog, sizeof log, nullptr, log);
        dump_failure("program", key, "", log);
        glDeleteProgram(prog);
        return p;
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

Shader* translate(const uint32_t* regs, bool vertex, LatteFetchShader* fetch, uint64_t fsKey, uint64_t frame) {
    uint32_t start = vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
    uint32_t address = regs[start] << 8, size = regs[start + 1] << 3;
    if (!address || !size || size > 0x100000 || uint64_t(address) + size > 0x100000000ull) return nullptr;
    uint64_t base = program_hash(address, size, frame) ^ (vertex ? 0x1111 : 0x2222);
    uint64_t key = state_hash(regs, base, vertex) ^ (vertex ? fsKey * 31 : 0);
    if (auto it = shaders.find(key); it != shaders.end()) return it->second.get();
    ScopedTime timer{R.perf.shaderNs};
    R.perf.shaders++;
    auto owned = std::make_unique<Shader>();
    Shader* shader = owned.get();
    shader->key = key;
    shader->vertex = vertex;
    shaders.emplace(key, std::move(owned));
    if ((regs[Latte::REGADDR::VGT_GS_MODE] & 3) != 0) {
        shader->error = "geometry shaders are not implemented by the OpenGL renderer";
        return shader;
    }
    if (vertex && !fetch) {
        shader->error = "vertex shader has no fetch program";
        return shader;
    }
    LatteShader_UpdatePSInputs(const_cast<uint32_t*>(regs));
    LatteDecompilerOptions options;
    LatteDecompilerOutput_t output{};
    if (vertex)
        LatteDecompiler_DecompileVertexShader(base, const_cast<uint32_t*>(regs), ppc_ptr(address), size, fetch, options,
                                              &output);
    else
        LatteDecompiler_DecompilePixelShader(base, const_cast<uint32_t*>(regs), ppc_ptr(address), size, options, &output);
    if (!output.shader || output.shader->hasError || !output.shader->strBuf_shaderSource) {
        shader->error = "Latte GLSL translation failed";
        return shader;
    }
    shader->dec = FinishDecompiledShader(output);
    shader->mapping = output.resourceMappingGL;
    shader->registerCount = output.uniformOffsetsGL.count_uniformRegister;
    std::string glsl = shader->dec->strBuf_shaderSource->c_str();
    delete shader->dec->strBuf_shaderSource;  // tens of KB per variant, never read again
    shader->dec->strBuf_shaderSource = nullptr;
    shader->glslHash = hash_bytes(glsl.data(), glsl.size(), vertex ? 0x1111 : 0x2222);
    if (auto it = objects.find(shader->glslHash); it != objects.end()) {
        shader->obj = it->second;
        return shader;
    }
    std::string error;
    GLuint obj = compile(vertex, glsl, &error);
    if (!obj) {
        shader->error = error;
        dump_failure(vertex ? "vertex shader" : "pixel shader", key, glsl, error);
        return shader;
    }
    objects.emplace(shader->glslHash, obj);
    cache_shader(shader->glslHash, vertex, glsl);
    shader->obj = obj;
    return shader;
}

Program* program(Shader* vs, Shader* ps) {
    uint64_t key = pair_key(vs->glslHash, ps->glslHash);
    if (auto it = linked.find(key); it != linked.end()) return it->second->prog ? it->second.get() : nullptr;
    ScopedTime timer{R.perf.shaderNs};
    auto owned = link(vs->obj, ps->obj, key);
    Program* p = owned->prog ? owned.get() : nullptr;
    linked.emplace(key, std::move(owned));
    if (p) cache_program(vs->glslHash, ps->glslHash);
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
        else if (data[valid] == 1 && valid + 18 <= data.size()) {
            uint32_t packed;
            memcpy(&packed, data.data() + valid + 10, 4);
            length = 18 + size_t(packed);
        }
        if (!length || valid + length > data.size()) break;
        records.push_back(valid);
        valid += length;
    }
    const uint64_t start = now_ns();
    size_t compiledShaders = 0, linkedPrograms = 0;
    for (size_t i = 0; i < records.size(); i++) {
        const uint8_t* r = data.data() + records[i];
        if (r[0] == 1) {
            uint64_t hash;
            uint32_t sizes[2];
            memcpy(&hash, r + 2, 8);
            memcpy(sizes, r + 10, 8);
            if (!objects.count(hash)) {
                std::string glsl(sizes[1], '\0');
                uLongf size = sizes[1];
                if (uncompress((Bytef*)glsl.data(), &size, r + 18, sizes[0]) == Z_OK && size == sizes[1])
                    if (GLuint obj = compile(r[1] != 0, glsl, nullptr)) {
                        objects.emplace(hash, obj);
                        compiledShaders++;
                    }
            }
        } else {
            uint64_t vs, ps;
            memcpy(&vs, r + 1, 8);
            memcpy(&ps, r + 9, 8);
            uint64_t key = pair_key(vs, ps);
            auto v = objects.find(vs), p = objects.find(ps);
            if (!linked.count(key) && v != objects.end() && p != objects.end()) {
                linked.emplace(key, link(v->second, p->second, key));
                linkedPrograms++;
            }
        }
        if (progress) progress(i + 1, records.size());
    }
    if (!records.empty())
        LOG("[gl] shader cache: %zu shaders and %zu programs compiled in %.1f s", compiledShaders, linkedPrograms,
            double(now_ns() - start) / 1e9);
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
}

void reset_shader_memoization() {
    programHashes.clear();
    R.shaderEpoch++;
}

}  // namespace gfxgl
