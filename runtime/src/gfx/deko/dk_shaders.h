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
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "dk.h"
#include "glsl_convert.h"

struct LatteFetchShader;
namespace gfxdk {
struct ShaderCode;  // shaders_dk.cpp: one GLSL source's DKSH, shared by every Shader with that glslHash
struct UniformCache;  // shaders_dk.cpp: the ufBlock's copy kept per shader (pack_uniforms_cached)
struct VertexLayout;  // draw.cpp: a vertex shader's attribute layout for its fetch shader (WWHD_DK_VTX_LAYOUT_CACHE)

enum class ShaderStatus : uint8_t {
    Pending,  // translated; its DKSH is not in memory yet (queued for or compiling on the worker, or done and
              // waiting for a frame's load budget)
    Ready,    // `dk` holds the loaded shader
    Failed,   // translation, conversion or uam failed (error says why; logged once): its draws are skipped
};

// alignas(64) and the fields every draw reads first (round 42): the draw path reads a shader's slots, texture units
// and its per-shader caches at each draw, ~1,500 shader changes a frame; spread over the object (and the decompiler's)
// they were several cache misses per stage. They now fill its first two cache lines, which draw.cpp prefetches as soon
// as it knows the shaders (WWHD_DK_PREFETCH).
struct alignas(64) Shader {
    // ---- hot: what the draw path reads (copies of the decompiler's lists and of the bindings, set once)
    LatteDecompilerShader* dec = nullptr;
    UniformCache* uf = nullptr;    // ufCache.get() (pack_uniforms_cached)
    VertexLayout* vtx = nullptr;   // vtxLayout.get() (draw.cpp, vertex shaders)
    ShaderStatus status = ShaderStatus::Pending;
    bool vertex = false;
    bool scaleUniforms = false;  // reads uf_fragCoordScale / uf_texNScale (internal resolution, P3)
    // a pixel shader whose GLSL can change depth or stencil results or memory: it discards (kills, the alpha
    // test), writes gl_FragDepth, or stores to images. Without it, a draw with no color target needs no pixel
    // shader at all (draw.cpp, WWHD_DK_DEPTH_ONLY). Vertex shaders: unused.
    bool fragmentEffects = true;
    uint8_t fragmentWhy = 0;  // (the log) 1: the alpha test, 2: another discard, 4: gl_FragDepth or memory writes
    int8_t ufBlockSlot = -1;  // bindings.ufBlockSlot: the UBO slot of the loose uniforms, -1 = none
    uint8_t uboCount = 0;     // GX2 uniform blocks it reads (uboList)
    uint8_t texCount = 0;     // texture units it samples (texUnit)
    // GX2 uniform block i -> deko3d UBO slot, texture unit -> deko3d texture slot; -1 = the shader does not use it
    std::array<int8_t, LATTE_NUM_MAX_UNIFORM_BUFFERS> uboSlot;
    std::array<int8_t, LATTE_NUM_MAX_TEX_UNITS> textureSlot;
    uint8_t uboList[LATTE_NUM_MAX_UNIFORM_BUFFERS];  // the blocks with a slot
    uint8_t texUnit[LATTE_NUM_MAX_TEX_UNITS];        // dec->textureUnitList
    uint8_t texSampler[LATTE_NUM_MAX_TEX_UNITS];     // by unit: dec->textureUnitSamplerAssignment, 0xFF = none
    uint32_t texCompare = 0;                         // bit per unit: dec->textureUsesDepthCompare
    // ---- the rest
    uint64_t key = 0;
    uint64_t glslHash = 0;  // the WGS1 hash of its GLSL: the key of the DKSH caches (shader_files.h)
    std::string error;
    DkShader dk{};          // valid when status == Ready
    // resources: the decompiler's Vulkan mapping (resourceMappingVK) and where glsl_to_deko put each binding
    LatteDecompilerShaderResourceMapping mapping;  // resourceMappingVK
    ConvertedBindings bindings;                    // Vulkan binding -> deko3d slot (glsl_convert.h)
    // bytes the shader reads from GX2 uniform block i (its declared array); 0 = unknown: the draw binds
    // min(guest size, 64 KiB) rounded up to 256 with zeros past the guest's data (as gfx/gl)
    std::array<uint32_t, LATTE_NUM_MAX_UNIFORM_BUFFERS> uboBytes{};
    // the loose uniforms block (ufBlock): uniformOffsetsVK layout, in UBO slot bindings.ufBlockSlot
    LatteDecompilerOutputUniformOffsets uniforms;  // uniformOffsetsVK
    // draws skipped since it became pending: the worker's queue serves the most wanted first
    uint32_t wanted = 0;
    ShaderCode* code = nullptr;  // its GLSL's DKSH (shaders_dk.cpp); null when translation failed
    std::shared_ptr<UniformCache> ufCache;    // pack_uniforms_cached's state, made at the first use (uf)
    std::shared_ptr<VertexLayout> vtxLayout;  // (vertex shaders) the last attribute layout built, draw.cpp (vtx)
    Shader() {
        uboSlot.fill(-1);
        textureSlot.fill(-1);
        memset(texSampler, 0xFF, sizeof texSampler);
    }
    bool ready() const { return status == ShaderStatus::Ready; }
    bool pending() const { return status == ShaderStatus::Pending; }
};
// the hot fields' cache lines and the per-shader caches they point to, toward the cache (draw.cpp, WWHD_DK_PREFETCH)
void prefetch_shader(const Shader* sh);
void prefetch_shader_caches(const Shader* sh);

// ---- lookup (as gfx/gl/shaders.h)
// frame: program bytes are rehashed once per frame
LatteFetchShader* get_fetch_shader(const uint32_t* regs, uint64_t* keyOut, uint64_t frame);
// the hash of the program at address (checked once per frame), through a reference to its entry
void* program_hash_ref(uint32_t address, uint32_t size);
uint64_t program_hash_of(void* ref, uint32_t address, uint32_t size, uint64_t frame);
// hash of every register (other than the program's) that translation reads; core: the same without the
// texture units (translate adds those of the units the program samples)
uint64_t shader_state_hash(const uint32_t* regs, bool vertex, uint64_t* core);
// P4 (draw.cpp's combinations): one hash of everything shader_state_hash reads for both
// stages (each shared word once, the texture units' state two to a word), with the primitive-type word given
// (the draw path passes its primitive class). Equal keys mean equal shader_state_hash values of both stages.
uint64_t shader_combo_state_hash(const uint32_t* regs, uint32_t primitive);
// where get_fetch_shader reads the fetch shader (false: there is none); program_hash_ref(address, size) is
// the entry whose hash is get_fetch_shader's key
bool fetch_shader_range(const uint32_t* regs, uint32_t& address, uint32_t& size, bool* compact = nullptr);
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
// each draw (draw_impl): ends the start-up shader wait at the first big frame (WWHD_DK_SHADER_BOOT_WAIT)
void shaders_draw_seen();
// main thread at shutdown (render::shutdown): the worker takes no more jobs; the records the writer thread
// has not written yet (DKSH to shadercache_dksh_local.bin, sources and translations to shadercache_gl.bin as
// gfx/gl writes them) are written now
void save_shader_cache();

// ---- uniforms
// the loose uniforms of one stage (ufBlock, uniformOffsetsVK layout) packed from the registers as
// gfx/vulkan pack_uniforms_into does, into this frame's stream slice (DK_UNIFORM_BUF_ALIGNMENT). scale: the
// render targets' internal resolution (uf_fragCoordScale, point size); texScale (null: all 1): per texture
// unit, texture pixels per guest pixel (uf_texNScale, as gfx/gl's g_unitScale). aoNoise: the AO quirk fix's
// mode 2 (gfx/gl draw.cpp ao_noise_constant): the first remapped constant's .w (the occlusion pass's noise
// tiling) x1.5. Empty (gpu 0) when the shader has no ufBlock or the stream slice is full.
StreamSlice pack_uniforms(bool vertex, const Shader& sh, const uint32_t* regs, float scaleX = 1.0f,
                          float scaleY = 1.0f, const float (*texScale)[2] = nullptr, bool aoNoise = false);

// The same block with a copy kept per shader (P4 resources lane; WWHD_DK_UF_CACHE, draw.cpp chooses the mode).
// The values come from a list of operations made once per shader (no walk of the decompiler's lists per draw)
// and are compared with the copy. mode 1: a new stream slice only when a value changed or the frame is new.
// mode 2: one stream slice per shader and frame; changed 16-byte pieces are pushed into it in the command stream
// (dkCmdBufPushConstants: every deko3d uniform buffer has push-constant semantics, the draws recorded before
// keep the values they were recorded with), so its address stays and the draw path skips the rebind. The
// result's size is the size to bind (mode 2: a multiple of 256, as dkCmdBufPushConstants requires). Empty when
// the stream slice is full.
StreamSlice pack_uniforms_cached(int mode, bool vertex, Shader& sh, const uint32_t* regs, float scaleX, float scaleY,
                                 const float (*texScale)[2], bool aoNoise);
struct UniformPackStats {
    uint64_t blocks = 0;      // pack_uniforms_cached calls
    uint64_t unchanged = 0;   // ... whose values were all the same as the copy's (same frame)
    uint64_t slices = 0, sliceBytes = 0;  // new stream slices and their bytes
    uint64_t pushes = 0, pushBytes = 0;   // dkCmdBufPushConstants calls and their data bytes (mode 2)
};
UniformPackStats uniform_pack_stats_take();

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
uint64_t shaders_pending();        // shaders queued for or held by the worker right now (any thread)

}  // namespace gfxdk
