#pragma once
#include <cstdint>
#include <string>

// Cemu's GLSL (the OpenGL-mode decompiler output that shadercache_gl.bin stores) -> GLSL that uam, the
// deko3d shader compiler, accepts. Pure C++: builds on the host (offline DKSH cache, tests) and on the Switch.
namespace gfxdk {
// deko3d per-stage limits
constexpr int kMaxUniformBuffers = 16;
constexpr int kMaxSamplers = 32;
// Cemu's Vulkan binding numbers (one set per stage, UBOs and textures share the numbering) stay below this
constexpr int kMaxVkBinding = 64;
// bump when glsl_to_deko's output changes: it is part of the DKSH caches' uamId (shader_files.h)
constexpr int kConvertRevision = 1;

// Where each of the shader's Vulkan bindings (LatteDecompilerShaderResourceMapping of resourceMappingVK)
// landed: UBOs and combined image+samplers are renumbered separately from 0, in order of appearance.
struct ConvertedBindings {
    int8_t ubo[kMaxVkBinding];      // Vulkan binding -> deko3d UBO slot, -1 = not used by the shader
    int8_t sampler[kMaxVkBinding];  // Vulkan binding -> deko3d texture slot, -1 = not used
    int uboCount = 0, samplerCount = 0;
    int ufBlockSlot = -1;       // the UBO holding the loose uniforms (uniformOffsetsVK layout), -1 = none
    int ufBlockVkBinding = -1;  // its Vulkan binding (resourceMappingVK.uniformVarsBufferBindingPoint)
    std::string error;          // why the conversion failed (glsl_to_deko returned "")
    ConvertedBindings() { clear(); }
    void clear();
};

// The Vulkan branch of every `#ifdef VULKAN` block (its SET_POSITION remaps z to 0..1, its ufBlock holds the
// loose uniforms) minus the gl_VertexID/gl_InstanceID renames to Vulkan built-ins, which uam's GL frontend
// does not know; layout macros expanded without `set =`; bindings renumbered (out); `#version 460`; vertex
// shaders get `invariant gl_Position;` as in gfx/gl. Line ends are kept (Cemu writes \r\n). Returns "" with
// out->error set when the source has an unexpected shape or needs more slots than deko3d has.
std::string glsl_to_deko(const std::string& cemuGlsl, bool vertex, ConvertedBindings* out);
}  // namespace gfxdk
