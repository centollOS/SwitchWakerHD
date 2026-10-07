// The deko3d renderer's draw path (dk_draw.h): GX2 register state -> deko3d commands. A structural port of
// gfx/gl/draw.cpp (shader memo and combinations, index conversion, vertex trimming, target and texture caches,
// GamePad skip, trace) with GL's state calls replaced by deko3d's state structs, and gfx/vulkan/draw.cpp's
// viewport, scissor and front-face math adapted to deko3d's clip space (y up; see set_viewport).
//
// Every draw: shaders (dk_shaders.h; a draw whose shaders are not ready is skipped quietly), indices,
// render targets, textures and uniform blocks (dk_surfaces.h; resolved before anything is bound), then the
// bindings and state that differ from what the command buffer already has (StateCache, reset by
// forget_state), the vertex streams and the draw itself.
#include "dk_draw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "dk_capture.h"
#include "dk_shaders.h"
#include "dk_surfaces.h"
#include "runtime.h"

using namespace Latte;
extern "C" uint64_t g_shader_state_gen;  // gx2_core.cpp: bumped by shader-relevant register changes
extern "C" uint64_t g_shader_regs_gen;   // the same, except for changes of shader program registers

namespace gfxdk {

bool g_traceFrame = false, g_captureDraws = false;

namespace {
float f32(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}
// a switch read from WWHD_DK_<name>, else WWHD_GL_<name> (the OpenGL renderer's name for the same thing)
bool env_switch(const char* name, bool fallback) {
    std::string dk = std::string("WWHD_DK_") + name, gl = std::string("WWHD_GL_") + name;
    const char* e = getenv(dk.c_str());
    if (!e) e = getenv(gl.c_str());
    if (!e || !*e) return fallback;
    return *e != '0';
}
void log_once(uint64_t key, const char* fmt, const std::string& what) {
    static std::unordered_set<uint64_t> seen;
    if (seen.insert(key).second) LOG(fmt, what.c_str());
}

// ---- Latte -> deko3d enums
DkBlendFactor blend_factor(uint32_t v) {
    static const DkBlendFactor t[] = {
        DkBlendFactor_Zero,        DkBlendFactor_One,           DkBlendFactor_SrcColor,      DkBlendFactor_InvSrcColor,
        DkBlendFactor_SrcAlpha,    DkBlendFactor_InvSrcAlpha,   DkBlendFactor_DstAlpha,      DkBlendFactor_InvDstAlpha,
        DkBlendFactor_DstColor,    DkBlendFactor_InvDstColor,   DkBlendFactor_SrcAlphaSaturate, DkBlendFactor_SrcAlpha,
        DkBlendFactor_InvSrcAlpha, DkBlendFactor_ConstColor,    DkBlendFactor_InvConstColor, DkBlendFactor_Src1Color,
        DkBlendFactor_InvSrc1Color, DkBlendFactor_Src1Alpha,    DkBlendFactor_InvSrc1Alpha,  DkBlendFactor_ConstAlpha,
        DkBlendFactor_InvConstAlpha};
    return v < std::size(t) ? t[v] : DkBlendFactor_One;
}
bool constant_factor(DkBlendFactor f) {
    return f == DkBlendFactor_ConstColor || f == DkBlendFactor_InvConstColor || f == DkBlendFactor_ConstAlpha ||
           f == DkBlendFactor_InvConstAlpha;
}
DkBlendOp blend_op(uint32_t v) {
    static const DkBlendOp t[] = {DkBlendOp_Add, DkBlendOp_Sub, DkBlendOp_Min, DkBlendOp_Max, DkBlendOp_RevSub};
    return v < 5 ? t[v] : DkBlendOp_Add;
}
// Latte's compare functions and stencil operations are in GL's order; deko3d's start at 1
DkCompareOp compare_op(uint32_t v) { return DkCompareOp((v & 7) + 1); }
DkStencilOp stencil_op(uint32_t v) { return DkStencilOp((v & 7) + 1); }
DkLogicOp logic_op(uint32_t rop) {
    switch (rop) {
    case 0x00: return DkLogicOp_Clear;
    case 0x88: return DkLogicOp_And;
    case 0x44: return DkLogicOp_AndReverse;
    case 0x22: return DkLogicOp_AndInverted;
    case 0xAA: return DkLogicOp_NoOp;
    case 0x66: return DkLogicOp_Xor;
    case 0xEE: return DkLogicOp_Or;
    case 0x11: return DkLogicOp_Nor;
    case 0x99: return DkLogicOp_Equivalent;
    case 0x55: return DkLogicOp_Invert;
    case 0xDD: return DkLogicOp_OrReverse;
    case 0x33: return DkLogicOp_CopyInverted;
    case 0xBB: return DkLogicOp_OrInverted;
    case 0x77: return DkLogicOp_Nand;
    case 0xFF: return DkLogicOp_Set;
    default: return DkLogicOp_Copy;
    }
}
// raw unsigned fetch of each Latte vertex format (as gfx/gl): the GLSL decodes endianness and type
bool vertex_format(E_HWFMT f, DkVtxAttribSize& size, uint32_t& bytes) {
    switch (f) {
    case E_HWFMT::HWFMT_32_32_32_32_FLOAT: case E_HWFMT::HWFMT_32_32_32_32: size = DkVtxAttribSize_4x32; bytes = 16; break;
    case E_HWFMT::HWFMT_32_32_32_FLOAT: case E_HWFMT::HWFMT_32_32_32: size = DkVtxAttribSize_3x32; bytes = 12; break;
    case E_HWFMT::HWFMT_32_32_FLOAT: case E_HWFMT::HWFMT_32_32: size = DkVtxAttribSize_2x32; bytes = 8; break;
    case E_HWFMT::HWFMT_32_FLOAT: case E_HWFMT::HWFMT_32: case E_HWFMT::HWFMT_2_10_10_10:
        size = DkVtxAttribSize_1x32; bytes = 4; break;
    case E_HWFMT::HWFMT_8_8_8_8: size = DkVtxAttribSize_4x8; bytes = 4; break;
    case E_HWFMT::HWFMT_8_8_8: size = DkVtxAttribSize_3x8; bytes = 3; break;
    case E_HWFMT::HWFMT_8_8: size = DkVtxAttribSize_2x8; bytes = 2; break;
    case E_HWFMT::HWFMT_8: size = DkVtxAttribSize_1x8; bytes = 1; break;
    case E_HWFMT::HWFMT_16_16_16_16: case E_HWFMT::HWFMT_16_16_16_16_FLOAT: size = DkVtxAttribSize_4x16; bytes = 8; break;
    case E_HWFMT::HWFMT_16_16_16: case E_HWFMT::HWFMT_16_16_16_FLOAT: size = DkVtxAttribSize_3x16; bytes = 6; break;
    case E_HWFMT::HWFMT_16_16: case E_HWFMT::HWFMT_16_16_FLOAT: size = DkVtxAttribSize_2x16; bytes = 4; break;
    case E_HWFMT::HWFMT_16: case E_HWFMT::HWFMT_16_FLOAT: size = DkVtxAttribSize_1x16; bytes = 2; break;
    default: return false;
    }
    return true;
}
template <class T> T zeroed() {
    T v;
    memset(&v, 0, sizeof v);  // deko3d's state structs are bit fields: their unused bits compare equal too
    return v;
}
template <class T> bool same_bytes(const T& a, const T& b) { return !memcmp(&a, &b, sizeof(T)); }

// ---- what the command buffer has bound (draws only issue what changed). Code outside draw() that binds
// anything calls forget_state() (dk_draw.h): every entry becomes unknown. WWHD_DK_NO_STATE_CACHE=1 binds
// everything for every draw.
constexpr int kMaxColor = 8, kVtxBuffers = 16, kVtxAttribs = 32;
struct StateCache {
    uint64_t epoch = 0;
    // render targets (also re-bound when a surface got a new image: R.textureEpoch)
    bool targetsKnown = false;
    uint64_t targetsTextureEpoch = 0;
    std::array<Surface*, kMaxColor> colors{};
    uint32_t slices[kMaxColor]{};
    Surface* depth = nullptr;
    uint32_t depthSlice = 0;
    const DkShader *vs = nullptr, *ps = nullptr;
    bool rasterKnown = false, depthStencilKnown = false, colorKnown = false, colorWriteKnown = false;
    DkRasterizerState raster;
    DkDepthStencilState depthStencil;
    DkColorState color;
    DkColorWriteState colorWrite;
    uint8_t blendKnown = 0;
    DkBlendState blend[kMaxColor];
    bool blendConstKnown = false;
    uint32_t blendConst[4]{};
    bool stencilKnown = false;
    uint32_t stencil[2]{};  // DB_STENCILREFMASK, ..._BF as bound
    bool depthBiasKnown = false;
    float depthBias[3]{};
    bool viewportKnown = false;
    DkViewport viewport;
    int swizzle = -1;  // 0: identity, 1: y negated
    bool scissorKnown = false;
    DkScissor scissor;
    float pointSize = -1;
    int restart = -1;  // 0: off, else on with restartIndex
    uint32_t restartIndex = 0;
    DkGpuAddr indexAddr = 0;
    int indexFormat = -1;
    uint32_t attribCount = ~0u, bufferStateCount = ~0u;
    DkVtxAttribState attribs[kVtxAttribs];
    DkVtxBufferState bufferStates[kVtxBuffers];
    DkBufExtents vtx[kVtxBuffers];
    uint32_t vtxKnown = 0;  // bit per vertex buffer
    DkBufExtents ubo[2][kMaxUniformBuffers];
    uint32_t uboKnown[2]{};
    DkResHandle tex[2][kMaxSamplers];
    uint32_t texKnown[2]{};
} gs;

void sync_cache() {
    static const bool off = getenv("WWHD_DK_NO_STATE_CACHE") != nullptr;
    if (gs.epoch == R.stateEpoch && !off) return;
    gs = StateCache{};
    gs.epoch = R.stateEpoch;
}

// ---- trace switches (as gfx/gl): WWHD_DK_TRACE_DRAWS (or WWHD_GL_TRACE_DRAWS) logs every draw of a traced frame
const bool g_traceDraws = env_switch("TRACE_DRAWS", false);
std::string g_traceTextures;  // the textures of the draw being prepared
uint32_t g_frameDraws = 0;    // draws executed in the frame being recorded: the trace's and captures' #n

// the draw being prepared: renders the GamePad picture; its targets' internal resolution
bool g_gamepadDrawing = false;
float g_drawScale = 1.0f;
bool g_drawSamplesRendered = false;  // it samples a texture the GPU rendered
// each stage's texture units: texture pixels per guest pixel (programs with uf_texNScale; gfx/gl g_unitScale)
float g_unitScale[2][LATTE_NUM_MAX_TEX_UNITS][2];
const bool g_unitScaleInit = [] {
    for (auto& stage : g_unitScale)
        for (auto& unit : stage) unit[0] = unit[1] = 1.0f;
    return true;
}();
uint64_t g_zcullSeen = 0;            // R.zcullEpoch when zcull data was last dropped

// ---- per stage: the textures and uniform blocks of a draw, in deko3d slots (resolved before any binding)
constexpr int kVertexStage = 0, kPixelStage = 1;
struct StageBindings {
    DkResHandle tex[kMaxSamplers];
    uint32_t texMask = 0;
    DkBufExtents ubo[kMaxUniformBuffers];
    uint32_t uboMask = 0;
};

struct TextureCacheEntry {
    uint64_t epoch = 0;  // R.surfaceEpoch when filled; 0: not reusable
    uint64_t textureEpoch = 0;
    uint32_t words[7], sampler[3];
    bool compare = false;
    Surface* s = nullptr;
    uint32_t view = 0, smp = 0;
};
TextureCacheEntry textureCache[2][LATTE_NUM_MAX_TEX_UNITS];  // vertex, pixel
const bool textureCacheOn = !env_switch("NO_TEXTURE_CACHE", false);
// P4 resources lane: behind the per-unit entry (the last lookup of the unit), a table of recent lookups by their
// words (WWHD_DK_TEX_TABLE, on unless 0): draws that alternate materials on a unit find them there instead of
// redoing the lookup (find_or_create_surface's multimap, the view and sampler maps)
const bool textureTableOn = env_switch("TEX_TABLE", true);
constexpr uint32_t kTextureTableSize = 1024;
TextureCacheEntry textureTable[kTextureTableSize];
inline TextureCacheEntry& texture_table_entry(const uint32_t* words, const uint32_t* sampler, bool compare) {
    uint64_t h = compare ? 0x9E3779B97F4A7C15ull : 0xCBF29CE484222325ull;
    for (int i = 0; i < 7; i++) h = (h ^ words[i]) * 0x100000001B3ull;
    for (int i = 0; i < 3; i++) h = (h ^ sampler[i]) * 0x100000001B3ull;
    return textureTable[(h ^ (h >> 29) ^ (h >> 47)) & (kTextureTableSize - 1)];
}
inline bool texture_entry_hit(const TextureCacheEntry& e, const uint32_t* words, const uint32_t* sampler, bool compare) {
    return e.epoch == R.surfaceEpoch && e.textureEpoch == R.textureEpoch && e.compare == compare &&
           !memcmp(e.words, words, sizeof e.words) && !memcmp(e.sampler, sampler, sizeof e.sampler);
}

// The ufBlock (P4 resources lane, WWHD_DK_UF_CACHE): 0 = pack_uniforms into a new stream slice every draw (P2's
// path); 1 = a copy per shader, a new slice only when a value changed; 2 (default) = the copy per shader in one
// slice per frame, changed pieces pushed into it (dkCmdBufPushConstants), the address unchanged (no rebind)
const int g_ufMode = [] {
    const char* e = getenv("WWHD_DK_UF_CACHE");
    if (!e || !*e) return 2;
    const int v = atoi(e);
    return v < 0 || v > 2 ? 2 : v;
}();

// The resources stage's finer figures (P4 resources lane), per 5 s report (log_resource_stats): times of timed
// draws (x kDrawTimeSample, as R.perf) and counts of every draw
struct ResourcePerf {
    uint64_t targetNs = 0, uboNs = 0, textureNs = 0, textureMissNs = 0, uniformNs = 0, descriptorNs = 0, bindNs = 0;
    uint64_t textureMisses = 0, tableHits = 0, tableLookups = 0;
    uint64_t uboBlocks = 0, uboReused = 0;  // guest uniform blocks of draws; ... found already in this frame's stream
    uint64_t uboMemoHits = 0;               // ... by the stage's memo (WWHD_DK_UBO_MEMO)
    uint64_t texBindCalls = 0, texHandles = 0, texWanted = 0;  // dkCmdBufBindTextures calls, handles; slots draws use
    uint64_t uboBindCalls = 0, uboBuffers = 0, uboWanted = 0;  // dkCmdBufBindUniformBuffers calls, buffers; slots
} g_res;

// the draw's stage timer: lap() adds the time since the last mark to a stage total; sub() to one part of the
// resources stage (and to the stage's total, R.perf.resourceNs)
struct Lap {
    bool on;
    uint64_t at;
    void operator()(uint64_t& total) {
        if (!on) return;
        const uint64_t now = now_ns();
        total += (now - at) * kDrawTimeSample;
        at = now;
    }
    void sub(uint64_t& part) {
        if (!on) return;
        const uint64_t now = now_ns(), d = (now - at) * kDrawTimeSample;
        part += d;
        R.perf.resourceNs += d;
        at = now;
    }
};

// a zero-filled uniform block for a GX2 block with no address, once per frame
StreamSlice zero_block() {
    static StreamSlice slice;
    static uint64_t frame = ~0ull;
    if (frame != R.frame || !slice) {
        static const std::vector<uint8_t> zeros(0x10000, 0);
        slice = stream_upload(zeros.data(), uint32_t(zeros.size()), DK_UNIFORM_BUF_ALIGNMENT);
        frame = R.frame;
    }
    return slice;
}

// Ambient-occlusion quirks, as gfx/gl draw.cpp (and the Metal and Vulkan renderers): WWHD_AO_MODE=0..2
// chooses one; WWHD_NO_AO_QUIRK=1 means 0. The game downsamples the scene depth to 640x360 and computes ambient
// occlusion from it at 960x540 (vertex shader 44BDF900, pixel shader 44BDFD00), then blurs it into the shadow mask:
//   0 = as the hardware renders it: the occlusion pass point-samples its centre depth, and every third row and
//       column lands half a texel off (screen-fixed lines, and with the noise below grainy bands, on sloped
//       ground in shadow)
//   1 = that one fetch is bilinear, like the pass's neighbour fetches: the lines go
//   2 = (default) 1, and the 4x4 noise texture is tiled per 960x540 pixel instead of per 640x360 pixel, so the
//       game's blur averages it out (pack_uniforms aoNoise)
constexpr uint32_t kOcclusionVS = 0x44BDF900, kOcclusionPS = 0x44BDFD00;
const int g_aoMode = [] {
    if (const char* e = getenv("WWHD_AO_MODE")) return ((atoi(e) % 3) + 3) % 3;
    return getenv("WWHD_NO_AO_QUIRK") ? 0 : 2;
}();
// The occlusion pass's program in this stage: at its address and with its contents (size and hash of the guest
// program; gfx/gl's values). Another area could load another program at that address; it is left as it is.
constexpr uint32_t kOcclusionSize[2] = {1584, 384};  // pixel, vertex (Outset, US v0)
constexpr uint64_t kOcclusionHash[2] = {0x26870ca3f2e34dfaull, 0x36e37317f62658e9ull};
bool is_occlusion(const uint32_t* r, bool vertex) {
    const uint32_t reg = vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
    const uint32_t addr = r[reg] << 8, size = r[reg + 1] << 3;
    if (addr != (vertex ? kOcclusionVS : kOcclusionPS)) return false;
    const uint64_t frame = R.frame + 1;  // the frame being recorded
    static uint64_t checked[2] = {~0ull, ~0ull};
    static uint32_t sizeSeen[2] = {};
    static bool result[2] = {};
    if (checked[vertex] == frame && sizeSeen[vertex] == size) return result[vertex];
    const uint64_t h = program_hash_of(program_hash_ref(addr, size), addr, size, frame);
    const bool match = size == kOcclusionSize[vertex] && h == kOcclusionHash[vertex];
    if (match != result[vertex] || checked[vertex] == ~0ull)
        LOG("[dk] %s program at %08X (size %u, hash %016llx): %s", vertex ? "vertex" : "pixel", addr, size,
            (unsigned long long)h, match ? "the occlusion pass, AO fix applied" : "not the occlusion pass, AO fix not applied");
    checked[vertex] = frame;
    sizeSeen[vertex] = size;
    result[vertex] = match;
    return match;
}

// Each stage's guest uniform blocks as the last draw found them in the stream (WWHD_DK_UBO_MEMO, on unless 0):
// a block at the same address and size is the same slice while the frame and R.streamGen are (stream_guest's
// own rule), without stream_guest's table probe (P4 resources lane)
struct UboMemo {
    uint32_t addr = 0, copy = 0, bound = 0;
    uint64_t frame = ~0ull, gen = 0;
    StreamSlice slice;
};
UboMemo uboMemo[2][LATTE_NUM_MAX_UNIFORM_BUFFERS];
const bool uboMemoOn = env_switch("UBO_MEMO", true);

// false: the stream slice is full (the draw is skipped)
bool prepare_stage(const uint32_t* r, Shader* sh, const std::array<Surface*, 8>& colors, Surface* depth,
                   StageBindings& out, Lap& lap) {
    const int stage = sh->vertex ? kVertexStage : kPixelStage;
    out.texMask = out.uboMask = 0;
    // ---- uniform blocks: the guest's bytes, then zeros up to a 256-byte multiple (as gfx/gl: shaders must
    // read zeros past the guest's data, and the hardware rounds a constant buffer's bound size up to 256
    // bytes, so unpadded it would read the next upload's data). Reads past the bound range return zero.
    const uint32_t block = sh->vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
    for (int i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++) {
        const int slot = sh->uboSlot[i];
        if (slot < 0 || slot >= kMaxUniformBuffers) continue;
        const uint32_t addr = r[block + i * 7], size = std::min<uint32_t>(r[block + i * 7 + 1] + 1, 0x10000);
        const uint32_t want = sh->uboBytes[i] ? std::min<uint32_t>(sh->uboBytes[i], size) : size;
        const uint32_t bound = std::min<uint32_t>((want + 255) & ~255u, 0x10000);
        StreamSlice slice;
        g_res.uboBlocks++;
        if (!addr) slice = zero_block();
        else {
            const uint32_t copy = std::min(size, bound);
            UboMemo& m = uboMemo[stage][i];
            const uint64_t frame = R.frame + 1;
            if (uboMemoOn && m.addr == addr && m.copy == copy && m.bound == bound && m.frame == frame &&
                m.gen == R.streamGen && m.slice) {
                slice = m.slice;
                R.perf.reusedBytes += copy;
                g_res.uboReused++;
                g_res.uboMemoHits++;
            } else {
                const uint64_t reused = R.perf.reusedBytes;
                slice = stream_guest(addr, copy, DK_UNIFORM_BUF_ALIGNMENT, bound - copy);
                g_res.uboReused += R.perf.reusedBytes != reused;
                m = {addr, copy, bound, frame, R.streamGen, slice};
            }
        }
        if (!slice) return false;
        R.perf.uboBytes += bound;
        out.ubo[slot] = {slice.gpu, bound};
        out.uboMask |= 1u << slot;
    }
    lap.sub(g_res.uboNs);
    // ---- textures
    const uint32_t texbase = sh->vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
    for (int i = 0; i < sh->dec->textureUnitListCount; i++) {
        const uint32_t unit = sh->dec->textureUnitList[i];
        if (unit >= LATTE_NUM_MAX_TEX_UNITS) continue;
        const int slot = sh->textureSlot[unit];
        const uint32_t samplerIndex = sh->dec->textureUnitSamplerAssignment[unit];
        if (slot < 0 || slot >= kMaxSamplers || samplerIndex >= 18) continue;
        const uint32_t* words = r + texbase + unit * 7;
        const uint32_t* samplerWords = r + REGADDR::SQ_TEX_SAMPLER_WORD0_0 + ((sh->vertex ? 18 : 0) + samplerIndex) * 3;
        uint32_t aoSampler[3];
        if (g_aoMode >= 1 && unit == 0 && !sh->vertex && is_occlusion(r, false)) {
            // AO quirk 1: the occlusion pass's centre depth fetch, bilinear (XY mag/min filter)
            memcpy(aoSampler, samplerWords, sizeof aoSampler);
            aoSampler[0] = (aoSampler[0] & ~0x7E00u) | (1u << 9) | (1u << 12);
            samplerWords = aoSampler;
        }
        const bool compare = sh->dec->textureUsesDepthCompare[unit];
        // the last lookup for this unit, reused while its words and the surface set are unchanged
        TextureCacheEntry& cached = textureCache[stage][unit];
        R.perf.textureLookups++;
        Surface* s;
        uint32_t view, smp;
        TextureCacheEntry* found = nullptr;
        if (textureCacheOn && texture_entry_hit(cached, words, samplerWords, compare)) found = &cached;
        else if (textureCacheOn && textureTableOn) {
            g_res.tableLookups++;
            TextureCacheEntry& t = texture_table_entry(words, samplerWords, compare);
            if (texture_entry_hit(t, words, samplerWords, compare)) {
                g_res.tableHits++;
                cached = t;  // the unit's entry for the next draw
                found = &t;
            }
        }
        if (found) {
            R.perf.textureCacheHits++;
            s = found->s;
            upload_surface(s);  // once per frame: CPU changes to the texture
            view = found->view;
            smp = found->smp;
            sampler_used(smp);  // (the sampler cache must not rewrite it while this frame may use it)
        } else {
            g_res.textureMisses++;
            const uint64_t missStart = lap.on ? now_ns() : 0;
            bool unique = false;
            s = sampled_texture(words, compare, &unique);
            if (!s) {  // nothing there: transparent black
                out.tex[slot] = dkMakeTextureHandle(null_image_id(), sampler_id(samplerWords, compare, false));
                out.texMask |= 1u << slot;
                cached.epoch = 0;
                if (lap.on) g_res.textureMissNs += (now_ns() - missStart) * kDrawTimeSample;
                continue;
            }
            view = sampled_view_id(s, words);
            smp = sampler_id(samplerWords, compare, s->fmt.kind != FormatInfo::FLOAT);
            cached.epoch = unique ? R.surfaceEpoch : 0;  // several surfaces at one address: chosen by recency
            cached.textureEpoch = R.textureEpoch;
            memcpy(cached.words, words, sizeof cached.words);
            memcpy(cached.sampler, samplerWords, sizeof cached.sampler);
            cached.compare = compare;
            cached.s = s;
            cached.view = view;
            cached.smp = smp;
            if (textureTableOn && unique) texture_table_entry(words, samplerWords, compare) = cached;
            if (lap.on) g_res.textureMissNs += (now_ns() - missStart) * kDrawTimeSample;
        }
        if (g_gamepadDrawing) {  // a texture of the GamePad picture
            if (!s->gamepadSource) s->gamepadSourceSince = R.frame;
            s->gamepadSource = true;
        } else if (s->drcScanFrame != ~0ull && gamepad_only(s)) {
            // the GamePad picture itself read by another draw (gfx/gl draw.cpp): it counts as a read once
            // something else reads what this draw renders
            for (auto* c : colors)
                if (c && c != s) c->derivedFrom = s;
        } else
            note_read(s);
        if (g_traceFrame) {
            trace_draw(s);
            if (g_traceDraws || g_captureDraws) {
                char t[160];
                snprintf(t, sizeof t, " %s%u=%s(smp %08X %08X %08X%s)", sh->vertex ? "vt" : "t", unit,
                         trace_name(s).c_str(), samplerWords[0], samplerWords[1], samplerWords[2], compare ? " cmp" : "");
                g_traceTextures += t;
            }
        }
        bool aliases = depth && s == depth;
        for (auto* c : colors)
            if (c && c == s) aliases = true;
        if (g_capture) capture_note_texture(s, sh->vertex, unit, words, samplerWords, compare, aliases);
        if (aliases) {  // sampling a bound render target is undefined: a snapshot
            s = feedback_copy(s);
            view = sampled_view_id(s, words);
            smp = sampler_id(samplerWords, compare, s->fmt.kind != FormatInfo::FLOAT);
        }
        if (s->gpuWritten) g_drawSamplesRendered = true;
        if (sh->scaleUniforms) {
            g_unitScale[stage][unit][0] = float(s->img.pw) / float(s->width);
            g_unitScale[stage][unit][1] = float(s->img.ph) / float(s->height);
        }
        out.tex[slot] = dkMakeTextureHandle(view, smp);
        out.texMask |= 1u << slot;
    }
    lap.sub(g_res.textureNs);
    return true;
}

// binds the slots of mask whose value differs from the cache, in runs of consecutive slots
template <class T, class Bind>
void bind_runs(const T* want, uint32_t mask, T* cached, uint32_t& known, Bind&& bind) {
    uint32_t dirty = 0;
    for (uint32_t m = mask; m; m &= m - 1) {
        const uint32_t i = __builtin_ctz(m);
        if ((known & (1u << i)) && same_bytes(cached[i], want[i])) continue;
        cached[i] = want[i];
        dirty |= 1u << i;
    }
    known |= dirty;
    while (dirty) {
        const uint32_t first = __builtin_ctz(dirty);
        uint32_t n = 0;
        while (first + n < 32 && (dirty & (1u << (first + n)))) n++;
        bind(first, &want[first], n);
        dirty &= n >= 32 ? 0 : ~(((1u << n) - 1) << first);
    }
}

void bind_stage(int stage, const StageBindings& b) {
    const DkStage dkStage = stage == kVertexStage ? DkStage_Vertex : DkStage_Fragment;
    g_res.uboWanted += __builtin_popcount(b.uboMask);
    g_res.texWanted += __builtin_popcount(b.texMask);
    bind_runs(b.ubo, b.uboMask, gs.ubo[stage], gs.uboKnown[stage], [&](uint32_t first, const DkBufExtents* e, uint32_t n) {
        dkCmdBufBindUniformBuffers(R.cmd, dkStage, first, e, n);
        g_res.uboBindCalls++;
        g_res.uboBuffers += n;
    });
    bind_runs(b.tex, b.texMask, gs.tex[stage], gs.texKnown[stage], [&](uint32_t first, const DkResHandle* h, uint32_t n) {
        dkCmdBufBindTextures(R.cmd, dkStage, first, h, n);
        g_res.texBindCalls++;
        g_res.texHandles += n;
    });
}

// ---- indices: guest index formats and the primitives converted on the CPU (fans, quads, quad strips,
// line loops, as gfx/gl; P4 may use deko3d's native ones) become host-order index lists, 16-bit when the
// values fit. Converted lists are reused within a frame until guest buffers may have changed (R.streamGen).
template <int Type> inline uint32_t read_index(const uint8_t* p, uint32_t i) {
    if constexpr (Type == 0) { uint16_t v; memcpy(&v, p + i * 2, 2); return v; }
    else if constexpr (Type == 1) { uint32_t v; memcpy(&v, p + i * 4, 4); return v; }
    else if constexpr (Type == 4) { uint16_t v; memcpy(&v, p + i * 2, 2); return __builtin_bswap16(v); }
    else if constexpr (Type == 9) { uint32_t v; memcpy(&v, p + i * 4, 4); return __builtin_bswap32(v); }
    else return i;  // not indexed
}

struct IndexList {
    StreamSlice slice;
    uint32_t count = 0;
    int format = -1;  // DkIdxFormat; -1: no index list (dkCmdBufDraw)
    uint32_t minIndex = 0, maxIndex = 0;  // the vertices the list reads (restart index excluded)
};

template <int Type, class Out>
void build_indices(const uint8_t* src, uint32_t count, uint32_t prim, bool restart, uint32_t restartIndex,
                   std::vector<Out>& out, uint32_t& minIndex, uint32_t& maxIndex) {
    constexpr Out kRestart = Out(~Out(0));
    uint32_t m = 0, lo = ~0u;
    auto get = [&](uint32_t i) -> Out {
        uint32_t v = read_index<Type>(src, i);
        if (restart && v == restartIndex) return kRestart;
        m = std::max(m, v);
        lo = std::min(lo, v);
        return Out(v);
    };
    switch (prim) {
    case 5:  // triangle fan
        out.resize(count > 2 ? size_t(count - 2) * 3 : 0);
        for (uint32_t i = 1, o = 0; i + 1 < count; i++, o += 3) {
            out[o] = get(0);
            out[o + 1] = get(i);
            out[o + 2] = get(i + 1);
        }
        break;
    case 0x13:  // quad list
        out.resize(size_t(count / 4) * 6);
        for (uint32_t i = 0, o = 0; i + 3 < count; i += 4, o += 6) {
            Out a = get(i), b = get(i + 1), c = get(i + 2), d = get(i + 3);
            out[o] = a; out[o + 1] = b; out[o + 2] = c; out[o + 3] = a; out[o + 4] = c; out[o + 5] = d;
        }
        break;
    case 0x14:  // quad strip
        out.resize(count >= 4 ? size_t((count - 2) / 2) * 6 : 0);
        for (uint32_t i = 0, o = 0; i + 3 < count; i += 2, o += 6) {
            Out a = get(i), b = get(i + 1), c = get(i + 2), d = get(i + 3);
            out[o] = a; out[o + 1] = b; out[o + 2] = c; out[o + 3] = b; out[o + 4] = d; out[o + 5] = c;
        }
        break;
    case 0x12:  // line loop
        out.resize(size_t(count) + 1);
        for (uint32_t i = 0; i < count; i++) out[i] = get(i);
        out[count] = get(0);
        break;
    default:
        out.resize(count);
        if (!restart) {
            for (uint32_t i = 0; i < count; i++) {
                uint32_t v = read_index<Type>(src, i);
                m = std::max(m, v);
                lo = std::min(lo, v);
                out[i] = Out(v);
            }
        } else
            for (uint32_t i = 0; i < count; i++) out[i] = get(i);
        break;
    }
    maxIndex = m;
    minIndex = lo == ~0u ? 0 : std::min(lo, m);
}

template <int Type> IndexList convert_typed(const uint8_t* src, uint32_t count, uint32_t prim, bool restart, uint32_t restartIndex) {
    IndexList list;
    // 16-bit output keeps the guest's 16-bit lists (and short generated lists) at half the bytes; primitive
    // restart then uses 0xFFFF, so a different restart index needs 32-bit output
    constexpr bool source16 = Type == 0 || Type == 4;
    static const bool wide = env_switch("WIDE_INDICES", false);
    const bool narrow = !wide && (Type < 0 ? count < 0xFFFF : source16 && (!restart || restartIndex == 0xFFFF));
    if (narrow) {
        static std::vector<uint16_t> out;
        build_indices<Type>(src, count, prim, restart, restartIndex, out, list.minIndex, list.maxIndex);
        list.count = uint32_t(out.size());
        list.format = DkIdxFormat_Uint16;
        if (out.empty()) return list;  // nothing to draw (no upload)
        list.slice = stream_upload(out.data(), uint32_t(out.size() * 2), 4);
        R.perf.indexBytes += out.size() * 2;
    } else {
        static std::vector<uint32_t> out;
        build_indices<Type>(src, count, prim, restart, restartIndex, out, list.minIndex, list.maxIndex);
        list.count = uint32_t(out.size());
        list.format = DkIdxFormat_Uint32;
        if (out.empty()) return list;
        list.slice = stream_upload(out.data(), uint32_t(out.size() * 4), 4);
        R.perf.indexBytes += out.size() * 4;
    }
    return list;
}

IndexList index_list(uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, bool restart, uint32_t restartIndex) {
    struct Entry {
        uint64_t key = 0, stamp = ~0ull, gen = 0;
        uint32_t addr = 0, count = 0, type = 0, prim = 0, restartIndex = 0;
        bool restart = false;
        IndexList list;
    };
    static FrameTable<Entry> cache;
    const uint64_t stamp = R.frame + 1;  // the frame being recorded (its stream slice)
    const uint64_t key = (uint64_t(indexAddr) << 32 | count) ^ (uint64_t(prim) << 56 | uint64_t(indexType) << 48) ^
                         (restart ? 0x5bd1e995ull * (restartIndex + 1) : 0);
    static const bool noCache = env_switch("NO_INDEX_CACHE", false);
    if (const Entry* e = cache.find(key, stamp); !noCache && e->stamp == stamp) {
        if (e->gen == R.streamGen && e->addr == indexAddr && e->count == count && e->type == indexType && e->prim == prim &&
            e->restart == restart && e->restartIndex == restartIndex)
            return e->list;
    }
    const uint8_t* src = indexAddr ? mem::ptr(indexAddr) : nullptr;
    IndexList list;
    switch (indexAddr ? indexType : ~0u) {
    case 0: list = convert_typed<0>(src, count, prim, restart, restartIndex); break;
    case 1: list = convert_typed<1>(src, count, prim, restart, restartIndex); break;
    case 4: list = convert_typed<4>(src, count, prim, restart, restartIndex); break;
    case 9: list = convert_typed<9>(src, count, prim, restart, restartIndex); break;
    default: list = convert_typed<-1>(src, count, prim, false, 0); break;
    }
    if (list.slice) cache.put({key, stamp, R.streamGen, indexAddr, count, indexType, prim, restartIndex, restart, list});
    return list;
}

// The vertex flat (non-interpolated) varyings take: gfx/gl's convention, the last vertex of each primitive.
// WWHD_DK_PROVOKING_VERTEX=latte follows Latte's PA_SU_SC_MODE_CNTL.PROVOKING_VTX_LAST (bit 19; first when
// clear, as gfx/vulkan) and =first always takes the first (test aids).
enum ProvokingMode { kProvokingLast, kProvokingFirst, kProvokingLatte };
ProvokingMode provoking_mode() {
    static const ProvokingMode mode = [] {
        const char* e = getenv("WWHD_DK_PROVOKING_VERTEX");
        return !e || !*e ? kProvokingLast : !strcmp(e, "latte") ? kProvokingLatte : !strcmp(e, "first") ? kProvokingFirst
                                                                                                     : kProvokingLast;
    }();
    return mode;
}
DkProvokingVertex provoking_vertex(const uint32_t* r) {
    switch (provoking_mode()) {
    case kProvokingFirst: return DkProvokingVertex_First;
    case kProvokingLatte:
        return (r[REGADDR::PA_SU_SC_MODE_CNTL] >> 19) & 1 ? DkProvokingVertex_Last : DkProvokingVertex_First;
    default: return DkProvokingVertex_Last;
    }
}

// the previous draw's shader lookup, reused while no shader-relevant register changed
struct ShaderMemo {
    uint64_t gen = 0, frame = ~0ull, epoch = 0;
    uint32_t prim = ~0u;
    LatteFetchShader* fs = nullptr;
    Shader *vs = nullptr, *ps = nullptr;
} memo;

}  // namespace
DrawSkips g_drawSkips;
namespace {
// a skipped draw's reason, counted for the stats (R.skippedDraws counts them all)
void skip(uint64_t& reason) {
    reason++;
    R.skippedDraws++;
}
}  // namespace

int ao_mode() { return g_aoMode; }

DrawSkips draw_skips_take() {
    const DrawSkips s = g_drawSkips;
    g_drawSkips = {};
    return s;
}

// ---- the GamePad picture (WWHD_DK_SKIP_GAMEPAD, or WWHD_GL_SKIP_GAMEPAD; on unless 0): as gfx/gl draw.cpp.
// The Switch shows only the TV picture; the GamePad's own buffers are not drawn (their depth clears are kept).
bool skip_gamepad() {
    static const bool on = env_switch("SKIP_GAMEPAD", true);
    return on;
}
bool gamepad_only(const Surface* s) {
    // a read by another draw or a copy keeps a buffer drawn, for 300 frames (gfx/gl)
    constexpr uint64_t kReadWindow = 300;
    const bool unread = s->readFrame == ~0ull || R.frame - s->readFrame > kReadWindow;
    if (s->tvScanFrame != ~0ull) return false;
    if (s->drcScanFrame != ~0ull && R.frame - s->drcScanFrame <= 60) return unread;
    return s->gamepadSource && !s->tvShared && (unread || s->readFrame < s->gamepadSourceSince);
}

void draw_frame_start() {
    // a new command list: whatever earlier frames bound is not relied upon
    forget_state();
    g_frameDraws = 0;
    static bool logged = false;
    if (!logged) {
        logged = true;
        const char* submit = getenv("WWHD_DK_SUBMIT_DRAWS");
        LOG("[dk] draw path: state cache %s, memo %s, texture cache %s, index cache %s, vertex trim %s, barrier per "
            "pass %s, GamePad skip %s, front face %s, submit every %s draws; trace draws %s",
            getenv("WWHD_DK_NO_STATE_CACHE") ? "off" : "on", env_switch("NO_MEMO", false) ? "off" : "on",
            textureCacheOn ? "on" : "off", env_switch("NO_INDEX_CACHE", false) ? "off" : "on",
            env_switch("VERTEX_TRIM", true) ? "on" : "off", env_switch("PASS_BARRIER", true) ? "on" : "off",
            skip_gamepad() ? "on" : "off", env_switch("FLIP_FRONT", false) ? "FLIPPED (test)" : "as Latte",
            submit && *submit ? submit : "256", g_traceDraws ? "on" : "off");
        static const char* const kAo[] = {"0, off: as the hardware renders it",
                                          "1, the occlusion pass's centre depth fetch bilinear",
                                          "2, the centre fetch bilinear and the noise tiled per 960x540 pixel"};
        LOG("[dk] AO quirk fix: mode %s (WWHD_AO_MODE=0..2, WWHD_NO_AO_QUIRK=1 for 0)", kAo[g_aoMode]);
        static const char* const kProvoking[] = {"the last vertex, as gfx/gl", "the FIRST vertex (test)",
                                                 "Latte's PROVOKING_VTX_LAST bit (test)"};
        LOG("[dk] flat varyings: %s (WWHD_DK_PROVOKING_VERTEX=last|first|latte)", kProvoking[provoking_mode()]);
        LOG("[dk] zcull: on (queue), dropped at every depth-target bind and after copies, uploads or new images of "
            "a depth buffer; depth clears reset it");
        static const char* const kUf[] = {"0, packed into a new stream slice every draw (P2)",
                                          "1, a copy per shader, a new slice only when a value changed",
                                          "2, a copy per shader in one slice per frame, changed pieces pushed "
                                          "(dkCmdBufPushConstants), no rebind"};
        LOG("[dk] resources (P4): texture lookup table %s (WWHD_DK_TEX_TABLE=0 off), ufBlock mode %s "
            "(WWHD_DK_UF_CACHE=0|1|2), guest uniform block memo %s (WWHD_DK_UBO_MEMO=0 off); textures and uniform "
            "blocks bound only for slots that changed", textureCacheOn && textureTableOn ? "on" : "off", kUf[g_ufMode],
            uboMemoOn ? "on" : "off");
    }
}

void log_resource_stats(uint64_t executed, uint64_t frames) {
    const ResourcePerf p = g_res;
    const UniformPackStats u = uniform_pack_stats_take();
    g_res = {};
    auto us = [&](uint64_t ns) { return executed ? double(ns) / 1e3 / double(executed) : 0.0; };
    auto perDraw = [&](uint64_t n) { return executed ? double(n) / double(executed) : 0.0; };
    auto perFrame = [&](uint64_t n) { return frames ? double(n) / double(frames) : 0.0; };
    auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
    LOG("[dk] resources us per draw: targets %.2f + uniform blocks %.2f + textures %.2f (misses %.2f: %.1f/frame, "
        "%.2f us each; table %.0f%% of %.0f/frame) + ufBlock %.2f + descriptors %.2f; binds (in state) %.2f us: "
        "textures %.2f calls %.2f handles of %.2f slots, uniform blocks %.2f calls %.2f buffers of %.2f slots; guest "
        "uniform blocks %.2f/draw (%.0f%% already in the stream, %.0f%% by the memo); ufBlock mode %d: %.0f/frame, %.0f%% unchanged, %.0f "
        "slices (%.0f KiB) + %.0f pushes (%.1f KiB) per frame",
        us(p.targetNs), us(p.uboNs), us(p.textureNs), us(p.textureMissNs), perFrame(p.textureMisses),
        p.textureMisses ? double(p.textureMissNs) / 1e3 / double(p.textureMisses) : 0.0,
        pct(p.tableHits, p.tableLookups), perFrame(p.tableLookups), us(p.uniformNs), us(p.descriptorNs), us(p.bindNs),
        perDraw(p.texBindCalls), perDraw(p.texHandles), perDraw(p.texWanted), perDraw(p.uboBindCalls),
        perDraw(p.uboBuffers), perDraw(p.uboWanted), perDraw(p.uboBlocks), pct(p.uboReused, p.uboBlocks), pct(p.uboMemoHits, p.uboBlocks), g_ufMode,
        perFrame(u.blocks), pct(u.unchanged, u.blocks), perFrame(u.slices), perFrame(u.sliceBytes) / 1024.0,
        perFrame(u.pushes), perFrame(u.pushBytes) / 1024.0);
}

namespace {
// The viewport. Latte maps clip-space y to window rows (row 0 at the top) as
//     row = yc * ys + yo                      (ys = PA_CL_VPORT_YSCALE, yo = PA_CL_VPORT_YOFFSET)
// which is what gfx/vulkan/draw.cpp's VkViewport {y = yo - ys, height = 2 ys} gives (Vulkan's clip y points
// down; a negative height flips). deko3d 0.5.0 with DkDeviceFlags_OriginUpperLeft computes
//     row = -(h / 2) * yd + (y + h / 2)       (dkCmdBufSetViewports: scaleY = -height / 2)
// for the clip-space y it receives, yd, and its height must stay positive (the viewport's pixel rectangle is
// computed in uint32_t). The game's vertex shaders negate y (SET_POSITION, dk.h), so yd = -yc and
//     ys > 0:  h = 2 ys,   y = yo - ys     -> row = yc * ys + yo   (Vulkan's numbers, as they are)
//     ys < 0:  a viewport swizzle negates y back (yd = yc), h = -2 ys, y = yo + ys
//                                          -> row = -(-ys) * yc + yo + ys - ys = yc * ys + yo
// Facing: deko3d's rasterizer decides it in framebuffer coordinates (row 0 at the top, deko3d's
// windingFlip() is off with OriginUpperLeft), as Latte and Vulkan do; window positions are Latte's in both
// cases, so Latte's front face is used unchanged (WWHD_DK_FLIP_FRONT=1 inverts it, a test aid).
// Depth: the shaders' SET_POSITION gives z in 0..1 (Vulkan branch), so near/far are Vulkan's.
void set_viewport(const uint32_t* r, float scale) {
    const float xs = f32(r[REGADDR::PA_CL_VPORT_XSCALE]), xo = f32(r[REGADDR::PA_CL_VPORT_XOFFSET]);
    const float ys = f32(r[REGADDR::PA_CL_VPORT_YSCALE]), yo = f32(r[REGADDR::PA_CL_VPORT_YOFFSET]);
    const float zs = f32(r[REGADDR::PA_CL_VPORT_ZSCALE]), zo = f32(r[REGADDR::PA_CL_VPORT_ZOFFSET]);
    LATTE_PA_CL_CLIP_CNTL clip;
    memcpy((void*)&clip, r + REGADDR::PA_CL_CLIP_CNTL, 4);
    const bool flip = ys < 0;
    DkViewport v;
    v.x = (xo - std::fabs(xs)) * scale;
    v.width = 2 * std::fabs(xs) * scale;
    v.y = (flip ? yo + ys : yo - ys) * scale;
    v.height = 2 * std::fabs(ys) * scale;
    v.near = clip.get_DX_CLIP_SPACE_DEF() ? zo : zo - zs;
    v.far = zo + zs;
    if (xs < 0) log_once(0x0E6A0000u, "[dk] negative viewport x scale (%s): drawn unmirrored", std::to_string(xs));
    if (!gs.viewportKnown || !same_bytes(gs.viewport, v)) {
        dkCmdBufSetViewports(R.cmd, 0, &v, 1);
        gs.viewport = v;
        gs.viewportKnown = true;
    }
    if (gs.swizzle != int(flip)) {
        const DkViewportSwizzle sw = {DkSwizzle_PositiveX, flip ? DkSwizzle_NegativeY : DkSwizzle_PositiveY,
                                      DkSwizzle_PositiveZ, DkSwizzle_PositiveW};
        dkCmdBufSetViewportSwizzles(R.cmd, 0, &sw, 1);
        gs.swizzle = int(flip);
    }
}

void draw_impl(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
               uint32_t baseVertex, uint32_t instances);
}  // namespace

void draw(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, uint32_t baseVertex,
          uint32_t instances) {
    R.counts.draws++;
    draw_impl(r, prim, count, indexType, indexAddr, baseVertex, instances);
    // the GPU starts on the frame before it is complete: every kSubmitDraws executed draws go to it
    static const uint32_t kSubmitDraws = [] {
        const char* e = getenv("WWHD_DK_SUBMIT_DRAWS");
        return e && *e ? uint32_t(strtoul(e, nullptr, 10)) : 256u;
    }();
    static uint64_t lastSubmit = 0;
    if (kSubmitDraws && R.drawCount - lastSubmit >= kSubmitDraws) {
        lastSubmit = R.drawCount;
        submit_commands("draws");
    }
}

namespace {
void draw_impl(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
               uint32_t baseVertex, uint32_t instances) {
    static uint32_t drawSample = 0;
    const bool timed = R.timedDraw = (++drawSample % kDrawTimeSample) == 0;
    SampledTime timer{R.perf.drawNs, timed};
    if (!count || !instances || ((prim == 0x13 || prim == 0x14) && count < 4)) return;
    if (r[REGADDR::PA_CL_CLIP_CNTL] & (1 << 22)) return;  // rasterization disabled
    Lap lap{timed, timed ? now_ns() : 0};
    const uint64_t drawStart = lap.at;
    ((uint32_t*)r)[REGADDR::VGT_PRIMITIVE_TYPE] = prim;

    // ---- shaders: the last draw's, a recent combination of programs and register state, or translate
    Shader *vs, *ps;
    LatteFetchShader* fs;
    static const bool noMemo = env_switch("NO_MEMO", false);
    const uint64_t frame = R.frame + 1;  // the frame being recorded
    if (!noMemo && memo.gen == g_shader_state_gen && memo.frame == frame && memo.epoch == R.shaderEpoch && memo.prim == prim) {
        fs = memo.fs;
        vs = memo.vs;
        ps = memo.ps;
        R.perf.memoHits++;
    } else {
        // the register part of the shader keys, kept while only programs change
        static struct { uint64_t gen = 0; uint32_t prim = ~0u; uint64_t vs = 0, ps = 0, vsCore = 0, psCore = 0; } stateHash;
        if (noMemo || stateHash.gen != g_shader_regs_gen || stateHash.prim != prim) {
            stateHash.vs = shader_state_hash(r, true, &stateHash.vsCore);
            stateHash.ps = shader_state_hash(r, false, &stateHash.psCore);
            stateHash.gen = g_shader_regs_gen;
            stateHash.prim = prim;
        }
        // recent (programs, register state) combinations (gfx/gl): one made in an earlier frame is used again
        // only if its programs still have the hashes they had (checked once per frame) and the fetch shader
        // is the same. A combination whose shaders are not ready is not remembered.
        struct Combo {
            uint64_t frame = ~0ull, epoch = 0, vsState = 0, psState = 0;
            uint32_t programs[6] = {};
            LatteFetchShader* fs = nullptr;
            Shader *vs = nullptr, *ps = nullptr;
            void *vsRef = nullptr, *psRef = nullptr;  // program_hash_ref
            uint64_t vsHash = 0, psHash = 0;
        };
        static const bool comboAcrossFrames = env_switch("COMBO_FRAMES", true);
        auto stillValid = [&](Combo& c) {
            if (c.frame == frame) return true;
            if (!comboAcrossFrames || !c.vsRef || !c.psRef) return false;
            uint64_t fsKey = 0;
            if (get_fetch_shader(r, &fsKey, frame) != c.fs ||
                program_hash_of(c.vsRef, c.programs[2] << 8, c.programs[3] << 3, frame) != c.vsHash ||
                program_hash_of(c.psRef, c.programs[4] << 8, c.programs[5] << 3, frame) != c.psHash)
                return false;
            c.frame = frame;
            return true;
        };
        static Combo combos[4096];
        const uint32_t programs[6] = {r[mmSQ_PGM_START_FS], r[mmSQ_PGM_START_FS + 1], r[mmSQ_PGM_START_VS],
                                      r[mmSQ_PGM_START_VS + 1], r[mmSQ_PGM_START_PS], r[mmSQ_PGM_START_PS + 1]};
        uint64_t h = stateHash.vs * 31 + stateHash.ps;
        for (uint32_t v : programs) h = (h ^ v) * 0x100000001B3ull;
        Combo& c = combos[(h ^ (h >> 29)) & 4095];
        if (!noMemo && c.epoch == R.shaderEpoch && c.vsState == stateHash.vs && c.psState == stateHash.ps &&
            !memcmp(c.programs, programs, sizeof programs) && stillValid(c)) {
            R.perf.comboHits++;
            fs = c.fs;
            vs = c.vs;
            ps = c.ps;
        } else {
            uint64_t fsKey = 0;
            fs = get_fetch_shader(r, &fsKey, frame);
            vs = fs ? translate(r, true, fs, fsKey, frame, stateHash.vsCore) : nullptr;
            ps = fs ? translate(r, false, fs, fsKey, frame, stateHash.psCore) : nullptr;
            c.frame = frame;
            c.epoch = R.shaderEpoch;
            c.vsState = stateHash.vs;
            c.psState = stateHash.ps;
            memcpy(c.programs, programs, sizeof programs);
            c.fs = fs;
            c.vs = vs;
            c.ps = ps;
            const uint32_t vsAddr = programs[2] << 8, vsSize = programs[3] << 3, psAddr = programs[4] << 8,
                           psSize = programs[5] << 3;
            const bool hashable = vsAddr && vsSize && psAddr && psSize;
            c.vsRef = hashable ? program_hash_ref(vsAddr, vsSize) : nullptr;
            c.psRef = hashable ? program_hash_ref(psAddr, psSize) : nullptr;
            c.vsHash = hashable ? program_hash_of(c.vsRef, vsAddr, vsSize, frame) : 0;
            c.psHash = hashable ? program_hash_of(c.psRef, psAddr, psSize, frame) : 0;
            if (!(vs && ps && vs->ready() && ps->ready())) {  // looked up again next time
                c.frame = ~0ull;
                c.vsRef = c.psRef = nullptr;
            }
        }
        memo = {g_shader_state_gen, frame, R.shaderEpoch, prim, fs, vs, ps};
    }
    if (!fs) {
        skip(g_drawSkips.noFetchShader);
        return;
    }
    if (!vs || !ps || !vs->ready() || !ps->ready()) {
        // quietly: a pending shader asks the worker to hurry; a failed one was logged by the shader lane
        bool failed = false;
        for (Shader* sh : {vs, ps}) {
            if (!sh || sh->ready()) continue;
            if (sh->pending()) shader_wanted(sh);
            else {
                failed = true;
                char what[64];
                snprintf(what, sizeof what, "%s shader %016llx", sh->vertex ? "vertex" : "pixel",
                         (unsigned long long)sh->glslHash);
                log_once(sh->key ^ 0xFA11ED00ull, "[dk] draws with the failed %s skipped", what);
            }
        }
        skip(failed || !vs || !ps ? g_drawSkips.shaderFailed : g_drawSkips.shaderPending);
        return;
    }
    lap(R.perf.lookupNs);

    // ---- indices
    const bool stripRestart = indexAddr && (prim == 3 || prim == 6) && (r[REGADDR::VGT_MULTI_PRIM_IB_RESET_EN] & 1);
    const uint32_t restartIndex = r[REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX];
    DkPrimitive mode;
    switch (prim) {
    case 1: mode = DkPrimitive_Points; break;
    case 2: mode = DkPrimitive_Lines; break;
    case 3: case 0x12: mode = DkPrimitive_LineStrip; break;
    case 4: case 5: case 0x13: case 0x14: mode = DkPrimitive_Triangles; break;
    case 6: mode = DkPrimitive_TriangleStrip; break;
    default:
        log_once(0xD0000000u | prim, "[dk] draws with the unsupported primitive type %s skipped", std::to_string(prim));
        skip(g_drawSkips.unsupported);
        return;
    }
    const bool generated = prim == 5 || prim == 0x12 || prim == 0x13 || prim == 0x14;
    IndexList indices;
    if (indexAddr || generated) {
        indices = index_list(prim, count, indexType, indexAddr, stripRestart, restartIndex);
        if (!indices.count) return;  // (a fan or quad list too short for one primitive)
        if (!indices.slice) {
            R.perf.streamFullSkips++;
            skip(g_drawSkips.streamFull);
            return;
        }
    }
    const bool indexed = indices.format >= 0;
    const uint64_t maxVertex = indexed ? uint64_t(indices.maxIndex) + baseVertex : uint64_t(baseVertex) + count - 1;
    const uint64_t firstVertex = indexed ? uint64_t(indices.minIndex) + baseVertex : uint64_t(baseVertex);
    lap(R.perf.indexNs);

    // ---- render targets
    const auto& lcr = *reinterpret_cast<const LatteContextRegister*>(r);
    std::array<Surface*, 8> colors{};
    uint32_t slices[8]{}, depthSlice = 0;
    const auto mask = LatteMRT::GetActiveColorBufferMask(ps->dec, lcr);
    for (int i = 0; i < 8; i++)
        if (mask & (1 << i)) colors[i] = color_target(r, i, &slices[i]);
    Surface* depth = LatteMRT::GetActiveDepthBufferMask(lcr) ? depth_target(r, &depthSlice) : nullptr;
    // each render target at the internal resolution it should have now (rescale_surface keeps contents); when
    // some could take it but others wait for an allocation budget, all of them take it now
    if (depth) {
        before_write(depth);
        fit_scale(depth, true);
    }
    float shared = depth ? depth->scale : 0.0f;
    bool mixed = false;
    for (auto* c : colors)
        if (c) {
            before_write(c);
            if (c->hudFull && depth) c->hudFull = false;
            fit_scale(c, true);
            if (shared == 0.0f) shared = c->scale;
            mixed |= c->scale != shared;
        }
    if (mixed) {  // some took the new scale, some wait for an allocation: all of them now
        if (depth) fit_scale(depth, true, true);
        for (auto* c : colors)
            if (c) fit_scale(c, true, true);
    }
    Surface* target = depth;
    for (auto* c : colors)
        if (c) {
            target = c;
            break;
        }
    if (!target) {
        skip(g_drawSkips.noTarget);
        return;
    }
    g_drawScale = target->scale;
    if (depth && depth->scale != g_drawScale)
        log_once(0x5CA1E000u ^ depth->addr, "[dk] draw with targets at different internal resolutions: %s",
                 trace_name(depth) + " at " + std::to_string(depth->scale) + ", color at " + std::to_string(g_drawScale));
    bool gamepadDraw = target != depth;  // (a draw without a color target is not classified)
    for (auto* c : colors)
        if (c && !gamepad_only(c)) gamepadDraw = false;
    if (gamepadDraw) {
        R.perf.gamepadDraws++;
        if (skip_gamepad()) {
            R.perf.gamepadSkipped++;
            for (auto* c : colors)
                if (c && !c->skipLogged) {
                    c->skipLogged = true;
                    LOG("[dk] draws into %s skipped from frame %llu: GamePad picture only (WWHD_DK_SKIP_GAMEPAD)",
                        trace_name(c).c_str(), (unsigned long long)frame);
                }
            return;
        }
    }
    for (auto* c : colors)
        if (c) upload_surface(c);
    if (depth) upload_surface(depth);
    lap.sub(g_res.targetNs);

    // ---- the scissor (before anything is uploaded for a draw that would be discarded)
    uint32_t sx, sy, sex, sey;
    {
        const uint32_t width = target->width, height = target->height;
        const uint32_t tl = r[REGADDR::PA_SC_GENERIC_SCISSOR_TL], br = r[REGADDR::PA_SC_GENERIC_SCISSOR_BR];
        sx = std::min(tl & 0x7fff, width);
        sy = std::min((tl >> 16) & 0x7fff, height);
        sex = std::min(br & 0x7fff, width);
        sey = std::min((br >> 16) & 0x7fff, height);
        if (sex <= sx || sey <= sy) {
            skip(g_drawSkips.scissorEmpty);
            return;
        }
        if (g_drawScale != 1.0f) {  // image pixels: outward
            sx = uint32_t(float(sx) * g_drawScale);
            sy = uint32_t(float(sy) * g_drawScale);
            sex = std::min(scaled_size(sex, g_drawScale), target->img.pw);
            sey = std::min(scaled_size(sey, g_drawScale), target->img.ph);
        }
    }

    // ---- textures and uniform blocks (may upload and copy; nothing of the draw is bound yet)
    static StageBindings stages[2];
    g_gamepadDrawing = gamepadDraw;
    g_drawSamplesRendered = false;
    g_traceTextures.clear();
    if (g_capture) capture_draw_begin();
    lap.sub(g_res.targetNs);  // (the scissor)
    if (!prepare_stage(r, vs, colors, depth, stages[kVertexStage], lap) ||
        !prepare_stage(r, ps, colors, depth, stages[kPixelStage], lap)) {
        R.perf.streamFullSkips++;
        skip(g_drawSkips.streamFull);
        return;
    }
    // The HUD at full resolution (gfx/gl draw.cpp): the game draws it into the same buffer as the scene, last,
    // after the post-processing that reads the scene. At a lower internal resolution, the first draw into the TV
    // picture's buffer that comes after a read of it this frame, has no depth buffer and samples only the game's
    // own textures (nothing rendered) switches that buffer to its full-size image, the scene scaled up into it;
    // the next frame's clear (or 3D drawing) switches back
    if (target->scale < 1.0f && target == S.tvSource && !depth && !g_drawSamplesRendered && colors[0] == target &&
        target->readFrame == R.frame) {
        bool single = true;
        for (int i = 1; i < 8; i++)
            if (colors[i]) single = false;
        if (single) {
            target->hudFull = true;
            fit_scale(target, true, true);
            g_drawScale = target->scale;
            R.perf.hudSwitches++;
        }
    }
    // the loose uniforms (ufBlock) of each stage, packed at the targets' scale
    for (Shader* sh : {vs, ps}) {
        const int slot = sh->bindings.ufBlockSlot;
        if (slot < 0 || slot >= kMaxUniformBuffers) continue;
        const bool aoNoise = g_aoMode == 2 && sh->vertex && is_occlusion(r, true);
        const float(*texScale)[2] = g_unitScale[sh->vertex ? kVertexStage : kPixelStage];
        const StreamSlice u = g_ufMode ? pack_uniforms_cached(g_ufMode, sh->vertex, *sh, r, g_drawScale, g_drawScale, texScale, aoNoise)
                                       : pack_uniforms(sh->vertex, *sh, r, g_drawScale, g_drawScale, texScale, aoNoise);
        if (!u) {
            R.perf.streamFullSkips++;
            skip(g_drawSkips.streamFull);
            return;
        }
        StageBindings& b = stages[sh->vertex ? kVertexStage : kPixelStage];
        b.ubo[slot] = {u.gpu, u.size};
        b.uboMask |= 1u << slot;
    }
    lap.sub(g_res.uniformNs);
    commit_descriptors();  // the descriptors this draw's textures were given
    lap.sub(g_res.descriptorNs);

    // ---- render targets: bound when they change (deko3d binds color targets 0..n-1 without gaps: a gap gets
    // the first color target's view with its writes masked off below)
    sync_cache();
    {
        if (gs.targetsTextureEpoch != R.textureEpoch) gs.targetsKnown = false;
        bool same = gs.targetsKnown && gs.depth == depth && gs.depthSlice == depthSlice;
        for (int i = 0; i < 8 && same; i++) same = gs.colors[i] == colors[i] && gs.slices[i] == slices[i];
        if (!same) {
            static const bool passBarrier = env_switch("PASS_BARRIER", true);
            // what earlier passes rendered becomes visible to this pass's texture reads (plan section 6.3:
            // conservative, one barrier per change of targets), and the depth buffer's zcull data is
            // dropped whenever a depth buffer is bound (conservative too: deko3d drops it itself only when the
            // depth target's address changes; copies, uploads and reused heap memory keep the address)
            if (passBarrier)
                dkCmdBufBarrier(R.cmd, DkBarrier_Fragments,
                                DkInvalidateFlags_Image | (depth ? DkInvalidateFlags_Zcull : 0u));
            else if (depth)
                dkCmdBufBarrier(R.cmd, DkBarrier_None, DkInvalidateFlags_Zcull);
            if (depth) g_zcullSeen = R.zcullEpoch;
            int n = 0, first = -1;
            for (int i = 0; i < 8; i++)
                if (colors[i]) {
                    n = i + 1;
                    if (first < 0) first = i;
                }
            DkImageView views[8], depthView;
            const DkImageView* viewPtrs[8];
            for (int i = 0; i < n; i++) {
                const int c = colors[i] ? i : first;
                target_view(colors[c], 0, slices[c], &views[i]);
                viewPtrs[i] = &views[i];
            }
            if (depth) target_view(depth, 0, depthSlice, &depthView);
            dkCmdBufBindRenderTargets(R.cmd, viewPtrs, uint32_t(n), depth ? &depthView : nullptr);
            gs.colors = colors;
            memcpy(gs.slices, slices, sizeof slices);
            gs.depth = depth;
            gs.depthSlice = depthSlice;
            gs.targetsKnown = true;
            gs.targetsTextureEpoch = R.textureEpoch;
            gs.colorWriteKnown = false;  // the gap targets' masks
            if (g_traceFrame) trace_pass(colors, depth);
            gpu_pass_mark("draw", first >= 0 ? colors[first] : nullptr, depth);
        }
    }

    // the bound depth buffer's contents changed outside the 3D engine since its zcull data was gathered (a copy
    // or upload into it, R.zcullEpoch): dropped, as at a change of targets
    if (depth && g_zcullSeen != R.zcullEpoch) {
        dkCmdBufBarrier(R.cmd, DkBarrier_None, DkInvalidateFlags_Zcull);
        g_zcullSeen = R.zcullEpoch;
    }

    // ---- shaders, textures, uniform blocks
    if (gs.vs != &vs->dk || gs.ps != &ps->dk) {
        const DkShader* shaders[] = {&vs->dk, &ps->dk};
        dkCmdBufBindShaders(R.cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
        gs.vs = &vs->dk;
        gs.ps = &ps->dk;
    }
    {
        const uint64_t bindStart = timed ? now_ns() : 0;
        bind_stage(kVertexStage, stages[kVertexStage]);
        bind_stage(kPixelStage, stages[kPixelStage]);
        if (timed) g_res.bindNs += (now_ns() - bindStart) * kDrawTimeSample;
    }

    // ---- viewport, scissor
    set_viewport(r, g_drawScale);
    {
        const DkScissor sc = {sx, sy, sex - sx, sey - sy};
        if (!gs.scissorKnown || !same_bytes(gs.scissor, sc)) {
            dkCmdBufSetScissors(R.cmd, 0, &sc, 1);
            gs.scissor = sc;
            gs.scissorKnown = true;
        }
    }

    // ---- rasterizer
    LATTE_PA_SU_SC_MODE_CNTL pm;
    memcpy((void*)&pm, r + REGADDR::PA_SU_SC_MODE_CNTL, 4);
    LATTE_PA_CL_CLIP_CNTL clip;
    memcpy((void*)&clip, r + REGADDR::PA_CL_CLIP_CNTL, 4);
    {
        static const bool flipFront = env_switch("FLIP_FRONT", false);
        DkRasterizerState rs = zeroed<DkRasterizerState>();
        dkRasterizerStateDefaults(&rs);
        // depth clamp (no far clipping) when the game turns far-plane clipping off; deko3d's depth clamp also
        // drops near clipping, as GL's did before gfx/gl added its clip distance (P3)
        rs.depthClampEnable = clip.get_ZCLIP_FAR_DISABLE();
        const bool cullFront = pm.get_CULL_FRONT(), cullBack = pm.get_CULL_BACK();
        rs.cullMode = cullFront && cullBack ? DkFace_FrontAndBack : cullFront ? DkFace_Front : cullBack ? DkFace_Back : DkFace_None;
        const bool ccw = pm.get_FRONT_FACE() == LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW;
        rs.frontFace = ccw != flipFront ? DkFrontFace_CCW : DkFrontFace_CW;  // (set_viewport: Latte's facing)
        // flat varyings' vertex: the last, as gfx/gl (it never calls glProvokingVertex: GL's default
        // GL_LAST_VERTEX_CONVENTION, and the triangle lists both make of fans and quads have the same order)
        rs.provokingVertex = provoking_vertex(r);
        const bool offset = pm.get_OFFSET_FRONT_ENABLED();
        rs.depthBiasEnableMask = offset ? DkPolygonFlag_All : 0;
        if (!gs.rasterKnown || !same_bytes(gs.raster, rs)) {
            dkCmdBufBindRasterizerState(R.cmd, &rs);
            gs.raster = rs;
            gs.rasterKnown = true;
        }
        if (offset) {
            const float bias[3] = {f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET]), f32(r[REGADDR::PA_SU_POLY_OFFSET_CLAMP]),
                                   f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE]) / 16};
            if (!gs.depthBiasKnown || memcmp(gs.depthBias, bias, sizeof bias)) {
                dkCmdBufSetDepthBias(R.cmd, bias[0], bias[1], bias[2]);
                memcpy(gs.depthBias, bias, sizeof bias);
                gs.depthBiasKnown = true;
            }
        }
    }
    if (prim == 1) {  // points: the fixed size (a shader that writes gl_PointSize overrides it)
        float point = float(r[REGADDR::PA_SU_POINT_SIZE] & 0xFFFF) / 8.0f;
        point = (point == 0 ? 0.125f : point) * g_drawScale;
        if (gs.pointSize != point) {
            dkCmdBufSetPointSize(R.cmd, point);
            gs.pointSize = point;
        }
    }

    // ---- depth / stencil
    {
        LATTE_DB_DEPTH_CONTROL dc;
        memcpy((void*)&dc, r + REGADDR::DB_DEPTH_CONTROL, 4);
        DkDepthStencilState ds = zeroed<DkDepthStencilState>();
        dkDepthStencilStateDefaults(&ds);
        ds.depthTestEnable = depth && dc.get_Z_ENABLE();
        ds.depthWriteEnable = ds.depthTestEnable && dc.get_Z_WRITE_ENABLE();
        ds.depthCompareOp = compare_op(uint32_t(dc.get_Z_FUNC()));
        const bool stencil = depth && depth->fmt.stencil && dc.get_STENCIL_ENABLE();
        ds.stencilTestEnable = stencil;
        if (stencil) {
            const bool separate = dc.get_BACK_STENCIL_ENABLE();
            ds.stencilFrontFailOp = stencil_op(uint32_t(dc.get_STENCIL_FAIL_F()));
            ds.stencilFrontDepthFailOp = stencil_op(uint32_t(dc.get_STENCIL_ZFAIL_F()));
            ds.stencilFrontPassOp = stencil_op(uint32_t(dc.get_STENCIL_ZPASS_F()));
            ds.stencilFrontCompareOp = compare_op(uint32_t(dc.get_STENCIL_FUNC_F()));
            ds.stencilBackFailOp = stencil_op(uint32_t(separate ? dc.get_STENCIL_FAIL_B() : dc.get_STENCIL_FAIL_F()));
            ds.stencilBackDepthFailOp = stencil_op(uint32_t(separate ? dc.get_STENCIL_ZFAIL_B() : dc.get_STENCIL_ZFAIL_F()));
            ds.stencilBackPassOp = stencil_op(uint32_t(separate ? dc.get_STENCIL_ZPASS_B() : dc.get_STENCIL_ZPASS_F()));
            ds.stencilBackCompareOp = compare_op(uint32_t(separate ? dc.get_STENCIL_FUNC_B() : dc.get_STENCIL_FUNC_F()));
            // DB_STENCILREFMASK: ref (bits 0-7), compare mask (8-15), write mask (16-23)
            const uint32_t f = r[REGADDR::DB_STENCILREFMASK], b = separate ? r[REGADDR::DB_STENCILREFMASK_BF] : f;
            if (!gs.stencilKnown || gs.stencil[0] != f || gs.stencil[1] != b) {
                dkCmdBufSetStencil(R.cmd, DkFace_Front, uint8_t(f >> 16), uint8_t(f), uint8_t(f >> 8));
                dkCmdBufSetStencil(R.cmd, DkFace_Back, uint8_t(b >> 16), uint8_t(b), uint8_t(b >> 8));
                gs.stencil[0] = f;
                gs.stencil[1] = b;
                gs.stencilKnown = true;
            }
        }
        if (!gs.depthStencilKnown || !same_bytes(gs.depthStencil, ds)) {
            dkCmdBufBindDepthStencilState(R.cmd, &ds);
            gs.depthStencil = ds;
            gs.depthStencilKnown = true;
        }
    }

    // ---- color output: blending (float targets only), write masks, logic op, blend constant
    {
        DkColorState cs = zeroed<DkColorState>();
        dkColorStateDefaults(&cs);
        DkColorWriteState cw = zeroed<DkColorWriteState>();
        cw.masks = 0;
        bool constantColor = false;
        for (uint32_t i = 0; i < 8; i++) {
            if (!colors[i]) continue;  // (a gap: mask 0)
            dkColorWriteStateSetMask(&cw, i, (r[REGADDR::CB_TARGET_MASK] >> (4 * i)) & 15);
            const bool on = colors[i]->fmt.kind == FormatInfo::FLOAT && ((r[REGADDR::CB_COLOR_CONTROL] >> (8 + i)) & 1);
            if (!on) continue;
            dkColorStateSetBlendEnable(&cs, i, true);
            LATTE_CB_BLENDN_CONTROL b;
            memcpy((void*)&b, r + REGADDR::CB_BLEND0_CONTROL + i, 4);
            DkBlendState bs = zeroed<DkBlendState>();
            dkBlendStateDefaults(&bs);
            bs.srcColorBlendFactor = blend_factor(uint32_t(b.get_COLOR_SRCBLEND()));
            bs.dstColorBlendFactor = blend_factor(uint32_t(b.get_COLOR_DSTBLEND()));
            bs.colorBlendOp = blend_op(uint32_t(b.get_COLOR_COMB_FCN()));
            const bool separate = b.get_SEPARATE_ALPHA_BLEND();
            bs.srcAlphaBlendFactor = separate ? blend_factor(uint32_t(b.get_ALPHA_SRCBLEND())) : bs.srcColorBlendFactor;
            bs.dstAlphaBlendFactor = separate ? blend_factor(uint32_t(b.get_ALPHA_DSTBLEND())) : bs.dstColorBlendFactor;
            bs.alphaBlendOp = separate ? blend_op(uint32_t(b.get_ALPHA_COMB_FCN())) : bs.colorBlendOp;
            for (DkBlendFactor f : {DkBlendFactor(bs.srcColorBlendFactor), DkBlendFactor(bs.dstColorBlendFactor),
                                     DkBlendFactor(bs.srcAlphaBlendFactor), DkBlendFactor(bs.dstAlphaBlendFactor)})
                constantColor |= constant_factor(f);
            if (!(gs.blendKnown & (1u << i)) || !same_bytes(gs.blend[i], bs)) {
                dkCmdBufBindBlendStates(R.cmd, i, &bs, 1);
                gs.blend[i] = bs;
                gs.blendKnown |= uint8_t(1u << i);
            }
        }
        const uint32_t rop = (r[REGADDR::CB_COLOR_CONTROL] >> 16) & 255;
        if (rop != 0xCC) cs.logicOp = logic_op(rop);
        if (!gs.colorKnown || !same_bytes(gs.color, cs)) {
            dkCmdBufBindColorState(R.cmd, &cs);
            gs.color = cs;
            gs.colorKnown = true;
        }
        if (!gs.colorWriteKnown || gs.colorWrite.masks != cw.masks) {
            dkCmdBufBindColorWriteState(R.cmd, &cw);
            gs.colorWrite = cw;
            gs.colorWriteKnown = true;
        }
        if (constantColor) {
            const uint32_t* constant = r + REGADDR::CB_BLEND_RED;
            if (!gs.blendConstKnown || memcmp(gs.blendConst, constant, sizeof gs.blendConst)) {
                dkCmdBufSetBlendConst(R.cmd, f32(constant[0]), f32(constant[1]), f32(constant[2]), f32(constant[3]));
                memcpy(gs.blendConst, constant, sizeof gs.blendConst);
                gs.blendConstKnown = true;
            }
        }
    }

    // ---- vertex streams (guest bytes as stored; the GLSL decodes them). Vertex trimming (WWHD_DK_VERTEX_TRIM,
    // on unless 0): only the vertices from the lowest one the draw reads are copied, and the draw's base vertex
    // moves back by as many (a model's parts share one vertex buffer and each draws its own range)
    static const bool trimOn = env_switch("VERTEX_TRIM", true);
    struct Group {
        uint32_t index, addr, size, stride;
        bool instance;
        uint64_t copied;
    };
    Group groups[kVtxBuffers];
    int groupCount = 0;
    DkVtxAttribState attribs[kVtxAttribs];
    uint32_t attribCount = 0;
    bool trimmable = trimOn && firstVertex > 0;
    for (auto& g : fs->bufferGroups) {
        const uint32_t base = mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7;
        const uint32_t addr = r[base], size = r[base + 1] + 1, stride = (r[base + 2] >> 11) & 0xFFFF;
        if (!addr) continue;
        if (g.attributeBufferIndex >= kVtxBuffers) {
            log_once(0xB0F00000u | g.attributeBufferIndex, "[dk] vertex buffer %s is past deko3d's 16: not fetched",
                     std::to_string(g.attributeBufferIndex));
            continue;
        }
        bool instance = false;
        uint64_t attributeEnd = 0;
        for (int j = 0; j < g.attribCount; j++) {
            auto& a = g.attrib[j];
            const int loc = vs->mapping.attributeMapping[a.semanticId];
            if (loc < 0 || loc >= kVtxAttribs) continue;
            DkVtxAttribSize asize;
            uint32_t bytes;
            if (!vertex_format(a.format, asize, bytes)) {
                log_once(0xF0F00000u | uint32_t(a.format), "[dk] vertex format %s not fetched",
                         std::to_string(uint32_t(a.format)));
                continue;
            }
            if (a.offset > 0x3FFF) {
                log_once(0x0FF50000u ^ a.offset, "[dk] vertex attribute offset %s past deko3d's 14 bits: not fetched",
                         std::to_string(a.offset));
                continue;
            }
            DkVtxAttribState v = zeroed<DkVtxAttribState>();
            v.bufferId = g.attributeBufferIndex;
            v.isFixed = 0;
            v.offset = a.offset;
            v.size = asize;
            v.type = DkVtxAttribType_Uint;
            while (attribCount <= uint32_t(loc)) {  // locations the shader does not read: a fixed zero
                DkVtxAttribState fixed = zeroed<DkVtxAttribState>();
                fixed.isFixed = 1;
                fixed.size = DkVtxAttribSize_1x32;
                fixed.type = DkVtxAttribType_Uint;
                attribs[attribCount++] = fixed;
            }
            attribs[loc] = v;
            attributeEnd = std::max<uint64_t>(attributeEnd, uint64_t(a.offset) + bytes);
            if (a.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA) instance = true;
        }
        const uint64_t last = instance ? instances - 1 : maxVertex;
        const uint64_t copied = std::min<uint64_t>(size, last * stride + std::max<uint64_t>(attributeEnd, stride));
        if (!instance && stride && firstVertex * stride >= copied) trimmable = false;
        groups[groupCount++] = {g.attributeBufferIndex, addr, size, stride, instance, copied};
    }
    const uint32_t trim = trimmable ? uint32_t(firstVertex) : 0;  // vertices left out of every per-vertex stream
    DkVtxBufferState bufferStates[kVtxBuffers];
    DkBufExtents vtx[kVtxBuffers];
    uint32_t bufferCount = 0, vtxMask = 0;
    for (int i = 0; i < groupCount; i++) {
        const Group& g = groups[i];
        const uint64_t skipBytes = g.instance ? 0 : uint64_t(trim) * g.stride;
        const StreamSlice slice =
            stream_guest(g.addr + uint32_t(skipBytes), uint32_t(std::max<uint64_t>(g.copied - skipBytes, 4)), 16);
        if (!slice) {
            R.perf.streamFullSkips++;
            skip(g_drawSkips.streamFull);
            return;
        }
        R.perf.vertexBytes += g.copied - skipBytes;
        while (bufferCount <= g.index) bufferStates[bufferCount++] = {0, 0};
        bufferStates[g.index] = {g.stride, g.instance ? 1u : 0u};
        vtx[g.index] = {slice.gpu, slice.size};
        vtxMask |= 1u << g.index;
    }
    if (gs.attribCount != attribCount || memcmp(gs.attribs, attribs, attribCount * sizeof(DkVtxAttribState))) {
        dkCmdBufBindVtxAttribState(R.cmd, attribs, attribCount);
        memcpy(gs.attribs, attribs, attribCount * sizeof(DkVtxAttribState));
        gs.attribCount = attribCount;
    }
    if (gs.bufferStateCount != bufferCount || memcmp(gs.bufferStates, bufferStates, bufferCount * sizeof(DkVtxBufferState))) {
        dkCmdBufBindVtxBufferState(R.cmd, bufferStates, bufferCount);
        memcpy(gs.bufferStates, bufferStates, bufferCount * sizeof(DkVtxBufferState));
        gs.bufferStateCount = bufferCount;
    }
    bind_runs(vtx, vtxMask, gs.vtx, gs.vtxKnown, [&](uint32_t first, const DkBufExtents* e, uint32_t n) {
        dkCmdBufBindVtxBuffers(R.cmd, first, e, n);
    });
    lap(R.perf.stateNs);

    // ---- draw
    if (indexed) {
        const int restart = stripRestart ? 1 : 0;
        const uint32_t hostRestart = indices.format == DkIdxFormat_Uint16 ? 0xFFFFu : 0xFFFFFFFFu;
        if (gs.restart != restart || (restart && gs.restartIndex != hostRestart)) {
            dkCmdBufSetPrimitiveRestart(R.cmd, restart != 0, hostRestart);
            gs.restart = restart;
            gs.restartIndex = hostRestart;
        }
        if (gs.indexAddr != indices.slice.gpu || gs.indexFormat != indices.format) {
            dkCmdBufBindIdxBuffer(R.cmd, DkIdxFormat(indices.format), indices.slice.gpu);
            gs.indexAddr = indices.slice.gpu;
            gs.indexFormat = indices.format;
        }
        dkCmdBufDrawIndexed(R.cmd, mode, indices.count, instances, 0, int32_t(baseVertex) - int32_t(trim), 0);
    } else
        dkCmdBufDraw(R.cmd, mode, count, instances, baseVertex - trim, 0);
    lap(R.perf.submitNs);

    auto tally = [&](Surface* s) {
        if (s->drawFrame != R.frame) {
            s->drawFrame = R.frame;
            s->frameDraws = 0;
        }
        s->frameDraws++;
    };
    for (auto* c : colors)
        if (c) {
            mark_gpu_written(c);
            tally(c);
        }
    if (depth) {
        mark_gpu_written(depth);
        tally(depth);
    }
    const uint32_t drawIndex = g_frameDraws++;
    if (g_capture) capture_note_draw(drawIndex, colors, slices, depth, depthSlice);
    if (gamepadDraw && timed) R.perf.gamepadDrawNs += (now_ns() - drawStart) * kDrawTimeSample;
    if (g_traceFrame) {
        trace_draw(nullptr);
        if (g_traceDraws || g_captureDraws) {
            LATTE_DB_DEPTH_CONTROL tdc;
            memcpy((void*)&tdc, r + REGADDR::DB_DEPTH_CONTROL, 4);
            LOG("[trace]   draw #%u raster: cull %s%s, front %s, viewport y %s, z %g..%g, clip %s, z clip near %s far %s",
                drawIndex, pm.get_CULL_FRONT() ? "F" : "", pm.get_CULL_BACK() ? "B" : (pm.get_CULL_FRONT() ? "" : "none"),
                pm.get_FRONT_FACE() == LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW ? "ccw" : "cw",
                f32(r[REGADDR::PA_CL_VPORT_YSCALE]) < 0 ? "down (swizzled)" : "up",
                f32(r[REGADDR::PA_CL_VPORT_ZOFFSET]) - (clip.get_DX_CLIP_SPACE_DEF() ? 0.0f : f32(r[REGADDR::PA_CL_VPORT_ZSCALE])),
                f32(r[REGADDR::PA_CL_VPORT_ZOFFSET]) + f32(r[REGADDR::PA_CL_VPORT_ZSCALE]),
                clip.get_DX_CLIP_SPACE_DEF() ? "0..1" : "-1..1", clip.get_ZCLIP_NEAR_DISABLE() ? "off" : "on",
                clip.get_ZCLIP_FAR_DISABLE() ? "off" : "on");
            LOG("[trace]   draw #%u c0=%s d=%s vs %08X/%016llX ps %08X/%016llX prim %u count %u inst %u%s; depth %s func %u "
                "write %u, stencil %u, poly offset %u (%g, %g), blend %08X, mask %08X, trim %u;%s",
                drawIndex, trace_name(colors[0]).c_str(), trace_name(depth).c_str(), r[mmSQ_PGM_START_VS] << 8,
                (unsigned long long)vs->glslHash, r[mmSQ_PGM_START_PS] << 8, (unsigned long long)ps->glslHash, prim, count,
                instances, indexed ? (indices.format == DkIdxFormat_Uint16 ? " idx16" : " idx32") : "",
                depth && tdc.get_Z_ENABLE() ? "on" : "off", uint32_t(tdc.get_Z_FUNC()), uint32_t(tdc.get_Z_WRITE_ENABLE()),
                uint32_t(tdc.get_STENCIL_ENABLE()), (r[REGADDR::PA_SU_SC_MODE_CNTL] >> 11) & 1,
                f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE]) / 16, f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET]),
                r[REGADDR::CB_BLEND0_CONTROL], r[REGADDR::CB_TARGET_MASK], trim, g_traceTextures.c_str());
        }
    }
    g_traceTextures.clear();
    R.drawCount++;
}
}  // namespace

// ---- the trace of a frame's passes (WWHD_DK_TRACE_FRAMES / capture, backend.cpp), as gfx/gl's
namespace {
struct PassTrace {
    std::string pass, last;  // the open pass's targets, the last pass's
    uint64_t draws = 0;
    std::vector<const Surface*> sampled;
    void close() {
        if (pass.empty()) return;
        std::string s;
        for (const Surface* t : sampled) s += " " + trace_name(t);
        LOG("[trace] frame %llu pass %s: %llu draws, samples%s", (unsigned long long)(R.frame + 1), pass.c_str(),
            (unsigned long long)draws, s.empty() ? " nothing" : s.c_str());
        pass.clear();
        draws = 0;
        sampled.clear();
    }
} passTrace;
}  // namespace

std::string trace_name(const Surface* s) {
    if (!s) return "-";
    char b[96];
    snprintf(b, sizeof b, "%08X:%ux%u/f%X%s%s", s->addr, s->width, s->height, s->format, s->isDepth ? "/depth" : "",
             s->gpuWritten ? "" : "/cpu");
    std::string n = b;
    if (s->drcScanFrame != ~0ull) n += "/drc-" + std::to_string(R.frame - s->drcScanFrame);
    if (s->drcScanFrame != ~0ull || s->gamepadSource)
        n += s->readFrame == ~0ull ? "/unread" : "/read-" + std::to_string(R.frame - s->readFrame);
    if (s->gamepadSource) n += "/gpsrc";
    return n;
}
void trace_pass(const std::array<Surface*, 8>& colors, const Surface* depth) {
    passTrace.close();
    std::string p;
    for (int i = 0; i < 8; i++)
        if (colors[i]) p += " c" + std::to_string(i) + "=" + trace_name(colors[i]);
    p += " d=" + trace_name(depth);
    passTrace.pass = passTrace.last = p;
}
// a draw's textures are resolved before its targets are bound (and a new pass traced): its samples wait in
// pending until the draw is counted, in the pass it belongs to
void trace_draw(const Surface* sampled) {
    static std::vector<const Surface*> pending;
    if (sampled) {
        if (std::find(pending.begin(), pending.end(), sampled) == pending.end()) pending.push_back(sampled);
        return;
    }
    if (passTrace.pass.empty()) passTrace.pass = passTrace.last + " (continued)";
    passTrace.draws++;
    for (const Surface* t : pending)
        if (std::find(passTrace.sampled.begin(), passTrace.sampled.end(), t) == passTrace.sampled.end())
            passTrace.sampled.push_back(t);
    pending.clear();
}
void trace_event(const char* fmt, ...) {
    passTrace.close();
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    LOG("[trace] frame %llu %s", (unsigned long long)(R.frame + 1), b);
}

}  // namespace gfxdk
