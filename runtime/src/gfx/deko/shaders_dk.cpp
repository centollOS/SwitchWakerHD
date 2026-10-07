// The deko3d renderer's game shaders (dk_shaders.h). P2 stub, replaced by the shader lane: no translation
// (draws are counted, not executed, as in P1), no worker, no caches.
#include "dk_shaders.h"

#include "runtime.h"

namespace gfxdk {

LatteFetchShader* get_fetch_shader(const uint32_t*, uint64_t* keyOut, uint64_t) {
    if (keyOut) *keyOut = 0;
    return nullptr;
}
void* program_hash_ref(uint32_t, uint32_t) { return nullptr; }
uint64_t program_hash_of(void*, uint32_t, uint32_t, uint64_t) { return 0; }
uint64_t shader_state_hash(const uint32_t*, bool, uint64_t* core) {
    if (core) *core = 0;
    return 0;
}
Shader* translate(const uint32_t*, bool, LatteFetchShader*, uint64_t, uint64_t, uint64_t) { return nullptr; }
void shader_wanted(Shader*) {}
void reset_shader_memoization() { R.shaderEpoch++; }

void shaders_init() {}
void shaders_frame_start() {}
void save_shader_cache() {}

StreamSlice pack_uniforms(bool, const Shader&, const uint32_t*, float, float) { return {}; }

ShaderStats shader_stats_take() { return {}; }

}  // namespace gfxdk
