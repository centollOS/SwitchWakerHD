// The shader manifest (docs/shader-cache-from-dump-plan.md, step 1): what the decompiler was given for each shader
// variant, as identifiers only, so that a computer can translate the same variants again from the player's own dump
// (tools/switch/dksh_cache) and build shadercache_dksh.bin there. No game code: the program is named by its hash
// and size (its bytes are in the dump), and the record holds the GPU's context registers (minus the uniform
// constants) and the fetch shader the GX2 library made from the vertex attributes.
//
// shader_manifest.bin next to the other caches (sdmc:/switch/wwhd), recorded unless WWHD_SHADER_MANIFEST=0: every
// variant the first time it is met (also those rebuilt from translation records), so a player's own play builds it.
// The player copies it to the computer for make_sd.py --shaders (it is never shipped: docs/shader-cache-from-dump-plan.md).
#pragma once
#include <cstdint>

namespace gfxdk::shader_manifest {
bool enabled();
// translate(), for a variant new in this session, before the decompiler runs on these registers (it changes the
// pixel-shader inputs in them). key: the variant's key (a variant recorded before is skipped at once); programHash:
// hash_bytes of the program's bytes (at programAddress); fetchAddress/fetchSize/fetchCompact: fetch_shader_range's.
void record(uint64_t key, bool vertex, const uint32_t* regs, uint64_t programHash, uint32_t programAddress,
            uint32_t programSize, uint32_t fetchAddress, uint32_t fetchSize, bool fetchCompact);
}  // namespace gfxdk::shader_manifest
