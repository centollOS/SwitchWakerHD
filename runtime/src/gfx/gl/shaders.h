#pragma once
#include <array>
#include <cstdint>
#include <string>

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
// a linked vertex + pixel shader pair and the locations of Cemu's loose uniforms
struct Program {
    GLuint prog = 0;
    GLint remappedVS = -1, remappedPS = -1, registersVS = -1, registersPS = -1;
    GLint windowToClip = -1, alphaRef = -1, pointSize = -1, fragCoordScale = -1;
    std::array<GLint, LATTE_NUM_MAX_TEX_UNITS> texScale{};
    std::array<GLint, 64> blockSize{};  // uniform block data size by binding point (VS 0-15, PS 32-47)
};
// frame: program bytes are rehashed once per frame
LatteFetchShader* get_fetch_shader(const uint32_t* regs, uint64_t* keyOut, uint64_t frame);
Shader* translate(const uint32_t* regs, bool vertex, LatteFetchShader* fetch, uint64_t fsKey, uint64_t frame);
Program* program(Shader* vs, Shader* ps);  // null if linking failed
// sdmc:/switch/wwhd/shadercache_gl.bin: every shader source and linked pair seen so far, compiled at
// startup (progress: done of total) and extended while the game runs
void load_shader_cache(void (*progress)(size_t done, size_t total));
}  // namespace gfxgl
