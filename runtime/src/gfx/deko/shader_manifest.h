// The shader manifest (docs/shader-cache-from-dump-plan.md, step 1): what the decompiler was given for each shader
// variant, as identifiers only, so that a computer can translate the same variants again from the player's own dump
// (tools/switch/dksh_cache) and build shadercache_dksh.bin there. No game code: the program is named by its hash
// and size (its bytes are in the dump), and the record holds the GPU's context registers (minus the uniform
// constants) and the fetch shader the GX2 library made from the vertex attributes.
//
// shader_manifest.bin next to the other caches (sdmc:/switch/wwhd), off unless WWHD_SHADER_MANIFEST=1. Only variants
// that go through the decompiler are recorded: for a full harvest, WWHD_DK_TRANSLATION_CACHE=0 too.
#pragma once
#include <cstdint>

namespace gfxdk::shader_manifest {
bool enabled();
// translate(), before the decompiler runs on these registers (it changes the pixel-shader inputs in them).
// programHash: hash_bytes of the program's bytes; fetchAddress/fetchSize/fetchCompact: fetch_shader_range's.
void record(bool vertex, const uint32_t* regs, uint64_t programHash, uint32_t programSize, uint32_t fetchAddress,
            uint32_t fetchSize, bool fetchCompact);
}  // namespace gfxdk::shader_manifest
