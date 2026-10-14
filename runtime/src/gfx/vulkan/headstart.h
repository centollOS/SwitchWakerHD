#pragma once
#include <cstdint>
#include <array>
#include <memory>
namespace gfxvk {
struct PipelineRecipe;
int warm_headstart_shaders();
void record_headstart_shader(const uint32_t* regs, bool vertex, bool linkedVertex = false);
void record_headstart_pipeline(const std::array<uint8_t, 48>& fingerprint,
    const std::shared_ptr<PipelineRecipe>& recipe);
void save_headstart_recording();
}
