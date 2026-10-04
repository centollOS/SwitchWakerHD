#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "gl.h"

struct LatteFetchShader;
namespace gfxgl {
struct Shader {
    uint64_t key = 0;
    bool vertex = false;
    LatteDecompilerShader* dec = nullptr;
    LatteDecompilerShaderResourceMapping mapping;  // OpenGL binding points
    uint32_t registerCount = 0;                    // uf_uniformRegister* entries (full register file mode)
    GLuint obj = 0;
    uint64_t glslHash = 0;  // equal sources share one shader object and the programs linked from it
    std::string error;
    bool ready() const { return dec && obj; }
};
// WWHD_GL_UNIFORM_BLOCKS (on unless =0): each stage's loose uniforms (uf_remapped*, the register
// file, the window/alpha/point/scale values) are compiled into one std140 uniform block, filled from
// the stream buffer. Mesa's nouveau driver copies the whole default uniform block into the GPU
// command stream whenever one loose uniform changes (nearly every draw); a uniform buffer is bound
// with a few command words and the GPU reads it from memory.
bool uniform_blocks_on();
constexpr GLuint kUniformVarBindingVS = 30, kUniformVarBindingPS = 62;  // free: game blocks use 0-15, 32-47
// WWHD_GL_BATCH (on unless =0; needs the uniform blocks): consecutive draws that differ only in
// their shader constants and index/vertex ranges go to the driver as one multi-draw. Each stage's
// block then holds an array of per-draw structs; a draw finds its own through a per-draw index,
// read in the vertex shader from attribute kDrawIndexAttrib (baseInstance + a buffer of 0, 1,
// 2...) and passed to the pixel shader in varying kDrawIndexVarying. Mesa then validates the GL
// state once per run of draws instead of once per draw.
bool draw_batching_on();
constexpr GLuint kDrawIndexAttrib = 15, kDrawIndexBinding = 15, kDrawIndexVarying = 14;  // unused by the game
struct UniformVarBlock {
    GLint size = 0;  // 0: the stage has no such block (loose uniforms, or none)
    GLint stride = 0;    // batching: bytes per draw (the struct array's stride); else the block size
    GLint capacity = 1;  // batching: draws the declared array holds
    GLint remapped = -1, registers = -1, windowToClip = -1, alphaRef = -1, pointSize = -1;
    std::vector<uint8_t> data;  // the block as last uploaded
    StreamSlice slice;          // where that copy is
    uint64_t gen = ~0ull;       // R.streamGen of the copy (stream memory is reused after a wrap or a frame)
};
// a linked vertex + pixel shader pair and the locations of Cemu's loose uniforms
struct Program {
    GLuint prog = 0;
    GLint remappedVS = -1, remappedPS = -1, registersVS = -1, registersPS = -1;
    GLint windowToClip = -1, alphaRef = -1, pointSize = -1, fragCoordScale = -1;
    std::array<GLint, LATTE_NUM_MAX_TEX_UNITS> texScale{};
    std::array<GLint, 64> blockSize{};  // uniform block data size by binding point (VS 0-15, PS 32-47)
    // the values last given to the program's loose uniforms (draw.cpp skips unchanged uploads)
    std::vector<uint8_t> shadowVS, shadowPS, registerShadowVS, registerShadowPS;
    float lastPointSize = NAN, lastAlphaRef = NAN, lastWindowToClip[2] = {NAN, NAN};
    UniformVarBlock blockVS, blockPS;
    bool batchable = false;     // the vertex shader reads the draw index (draw_batching_on)
    uint32_t batchCapacity = 1;  // draws one multi-draw may hold (the blocks' array sizes)
};
// frame: program bytes are rehashed once per frame
LatteFetchShader* get_fetch_shader(const uint32_t* regs, uint64_t* keyOut, uint64_t frame);
// hash of every register (other than the program's) that shader translation reads; core: the same
// without the texture units (translate adds those of the units the program samples)
uint64_t shader_state_hash(const uint32_t* regs, bool vertex, uint64_t* core);
Shader* translate(const uint32_t* regs, bool vertex, LatteFetchShader* fetch, uint64_t fsKey, uint64_t frame,
                  uint64_t coreHash);
Program* program(Shader* vs, Shader* ps);  // null if linking failed
// sdmc:/switch/wwhd/shadercache_gl.bin: every shader source and linked pair seen so far, compiled at
// startup (progress: done of total) and extended while the game runs
void load_shader_cache(void (*progress)(size_t done, size_t total));
}  // namespace gfxgl
