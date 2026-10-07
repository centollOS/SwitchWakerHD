// The deko3d renderer's game shaders (P2, shader lane; docs/deko3d-plan.md section 2 and "P2 lanes"):
// GX2 registers -> Cemu's GLSL (the decompiler's OpenGL mode: the same keys and hashes as gfx/gl and
// shadercache_gl.bin) -> glsl_to_deko -> uam (one worker thread) -> DKSH -> code memory. A structural port of
// gfx/gl/shaders.cpp (keys, translation memo, WGS1 cache, budget) without links: a draw needs a ready vertex
// shader and a ready pixel shader, nothing else. The uniform block layout (ufBlock) is the decompiler's Vulkan
// one, packed as gfx/vulkan/shaders.cpp pack_uniforms_into does.
//
// File: shaders_dk.cpp. Threads:
// - render thread: everything declared here except where noted; it alone touches deko3d (code_load,
//   dkShaderInitialize), guest memory, the registers and the Shader objects.
// - the uam worker (one thread, 8 MB stack, priority 0x3C below the game's threads, cores 0 and 2, uam_api.h):
//   takes Cemu's GLSL (a copy, or zlib'd from shadercache_gl.bin), runs glsl_to_deko and uam and gives back
//   the DKSH bytes and the bindings, or an error. It never touches deko3d objects, R, Shader objects or guest
//   memory.
// - a writer thread appends new records to shadercache_gl.bin and shadercache_dksh_local.bin about once a
//   second (as gfx/gl's cache_writer: SD card writes stay off the render thread).
#pragma once
#include <deko3d.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "dk.h"
#include "glsl_convert.h"

struct LatteFetchShader;
namespace gfxdk {
struct ShaderCode;  // shaders_dk.cpp: one GLSL source's DKSH, shared by every Shader with that glslHash

enum class ShaderStatus : uint8_t {
    Pending,  // translated; its DKSH is not in memory yet (queued for or compiling on the worker, or done and
              // waiting for a frame's load budget)
    Ready,    // `dk` holds the loaded shader
    Failed,   // translation, conversion or uam failed (error says why; logged once): its draws are skipped
};

struct Shader {
    uint64_t key = 0;
    bool vertex = false;
    LatteDecompilerShader* dec = nullptr;
    uint64_t glslHash = 0;  // the WGS1 hash of its GLSL: the key of the DKSH caches (shader_files.h)
    ShaderStatus status = ShaderStatus::Pending;
    std::string error;
    DkShader dk{};          // valid when status == Ready
    // resources: the decompiler's Vulkan mapping (resourceMappingVK) and where glsl_to_deko put each binding
    LatteDecompilerShaderResourceMapping mapping;  // resourceMappingVK
    ConvertedBindings bindings;                    // Vulkan binding -> deko3d slot (glsl_convert.h)
    // the same resolved per GX2 resource for the draw path, -1 = the shader does not use it:
    std::array<int8_t, LATTE_NUM_MAX_UNIFORM_BUFFERS> uboSlot;  // GX2 uniform block i -> deko3d UBO slot
    std::array<int8_t, LATTE_NUM_MAX_TEX_UNITS> textureSlot;    // texture unit -> deko3d texture slot
    // bytes the shader reads from GX2 uniform block i (its declared array); 0 = unknown: the draw binds
    // min(guest size, 64 KiB) rounded up to 256 with zeros past the guest's data (as gfx/gl)
    std::array<uint32_t, LATTE_NUM_MAX_UNIFORM_BUFFERS> uboBytes{};
    // the loose uniforms block (ufBlock): uniformOffsetsVK layout, in UBO slot bindings.ufBlockSlot
    LatteDecompilerOutputUniformOffsets uniforms;  // uniformOffsetsVK
    bool scaleUniforms = false;  // reads uf_fragCoordScale / uf_texNScale (internal resolution, P3)
    // draws skipped since it became pending: the worker's queue serves the most wanted first
    uint32_t wanted = 0;
    ShaderCode* code = nullptr;  // its GLSL's DKSH (shaders_dk.cpp); null when translation failed
    Shader() {
        uboSlot.fill(-1);
        textureSlot.fill(-1);
    }
    bool ready() const { return status == ShaderStatus::Ready; }
    bool pending() const { return status == ShaderStatus::Pending; }
};

// ---- lookup (as gfx/gl/shaders.h)
// frame: program bytes are rehashed once per frame
LatteFetchShader* get_fetch_shader(const uint32_t* regs, uint64_t* keyOut, uint64_t frame);
// the hash of the program at address (checked once per frame), through a reference to its entry
void* program_hash_ref(uint32_t address, uint32_t size);
uint64_t program_hash_of(void* ref, uint32_t address, uint32_t size, uint64_t frame);
// hash of every register (other than the program's) that translation reads; core: the same without the
// texture units (translate adds those of the units the program samples)
uint64_t shader_state_hash(const uint32_t* regs, bool vertex, uint64_t* core);
// the shader for these registers: memoized; a new GLSL source is looked up by its hash among the DKSH in
// code memory (the cache files' and this session's) and otherwise queued for the worker. Never blocks on uam
// (unless WWHD_DK_SHADER_BUDGET=0 asks for that). null only when the registers describe no program (no
// address or size).
Shader* translate(const uint32_t* regs, bool vertex, LatteFetchShader* fetch, uint64_t fsKey, uint64_t frame,
                  uint64_t coreHash);
// a draw was skipped because sh is pending: raises its priority in the worker's queue
void shader_wanted(Shader* sh);
// save state loaded / guest programs replaced: forget the memos (R.shaderEpoch++); compiled shaders stay
void reset_shader_memoization();

// ---- the worker and the caches
// main thread, from backend init after memory_init: the caches and the uam worker thread (one, 8 MB stack,
// below the game's priority). Every DKSH of sdmc:/switch/wwhd/shadercache_dksh.bin (offline, built on a computer
// from a harvest, shipped with the NRO) and of shadercache_dksh_local.bin (compiled on this console) goes into
// code memory now (memory.cpp code_load, a 32 MB bump allocator that never frees) and is Ready by glslHash; a
// file with another uamId is ignored (logged; the local one is started again). shadercache_gl.bin's sources
// without DKSH are queued for the worker in the background. progress (may be null): a start-up bar, called
// with done of total.
void shaders_init(void (*progress)(size_t done, size_t total) = nullptr);
// render thread, from begin_commands once per frame (and by translate when it has budget): the worker's
// finished DKSH -> code_load + dkShaderInitialize -> Ready (or Failed, logged). At most `budget` loads.
void shaders_frame_start();
// main thread at shutdown (render::shutdown): the worker takes no more jobs; the records the writer thread
// has not written yet (DKSH to shadercache_dksh_local.bin, sources and translations to shadercache_gl.bin as
// gfx/gl writes them) are written now
void save_shader_cache();

// ---- uniforms
// the loose uniforms of one stage (ufBlock, uniformOffsetsVK layout) packed from the registers as
// gfx/vulkan pack_uniforms_into does, into this frame's stream slice (DK_UNIFORM_BUF_ALIGNMENT). scale: the
// render targets' internal resolution (uf_fragCoordScale, point size); texScale (null: all 1): per texture
// unit, texture pixels per guest pixel (uf_texNScale, as gfx/gl's g_unitScale); noiseW: the first remapped
// constant's .w is multiplied by it (draw.cpp's AO quirk 2, gfx/gl ao_noise_constant). Empty (gpu 0) when the
// shader has no ufBlock or the stream slice is full.
StreamSlice pack_uniforms(bool vertex, const Shader& sh, const uint32_t* regs, float scaleX = 1.0f,
                          float scaleY = 1.0f, const float (*texScale)[2] = nullptr, float noiseW = 1.0f);

// ---- statistics (backend.cpp's 5 s line and the hitch log)
struct ShaderStats {
    uint64_t translations = 0;  // new (registers, program) translations on the render thread
    uint64_t memoryHits = 0;    // GLSL whose DKSH was already in RAM
    uint64_t cacheHits = 0;     // ... loaded from a DKSH cache file at start-up and used
    uint64_t offlineHits = 0, localHits = 0;  // cacheHits by file (shadercache_dksh.bin, ..._local.bin)
    uint64_t queued = 0, compiled = 0, failed = 0;  // worker jobs
    uint64_t compileNs = 0;     // worker time in uam (its own thread)
    uint64_t loadNs = 0;        // render thread: code_load + dkShaderInitialize
    uint64_t pendingNow = 0;    // shaders waiting for the worker right now
    uint64_t skippedDraws = 0;  // draws skipped for a pending shader
    uint64_t codeBytes = 0;     // code memory used by game shaders
};
ShaderStats shader_stats_take();  // counts since the last call; pendingNow and codeBytes as they are

}  // namespace gfxdk
