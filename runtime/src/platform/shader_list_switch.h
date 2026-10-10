// The console's own shader list (docs/background-shaders-plan.md): tools/switch/shader_manifest.py speculate on the
// console. Every game program (shader_scan) the manifest has not seen gets up to `limit` register states recorded for
// programs of its family (the same register block in its own GX2 structure: upstream shaderprep's own_block, with
// this runtime's SQ_VTX_SEMANTIC_CLEAR), so places not visited yet get their shaders too.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "shader_scan_switch.h"

namespace shader_list {

using Regs = std::vector<std::pair<uint16_t, uint32_t>>;  // non-zero context registers, by index

// a record of shader_manifest.bin (gfx/deko/shader_manifest.cpp)
struct Recorded {
    bool vertex = false;
    uint64_t programHash = 0;
    uint32_t programSize = 0;
    bool fetchCompact = false;
    std::vector<uint8_t> fetch;
    Regs regs;
};
std::vector<Recorded> read_manifest(const std::string& path);

// a variant to translate: the program (an index into the scan's programs), its registers and fetch shader
struct Variant {
    size_t program;
    Regs regs;
    std::vector<uint8_t> fetch;
    bool fetchCompact;
};
std::vector<Variant> speculate(const std::vector<shader_scan::Program>& programs, const std::vector<Recorded>& recorded,
                               size_t limit = 2);

// gfx/deko shaders_dk.cpp hash_bytes (the manifest names programs with it)
uint64_t hash_bytes(const void* bytes, size_t size, uint64_t hash = 0x9E3779B97F4A7C15ull);

}  // namespace shader_list
