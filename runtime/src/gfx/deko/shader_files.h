#pragma once
// The deko3d renderer's shader cache files, readable and writable on the host (tools/switch/dksh_cache, the
// offline cache) and on the Switch:
//
// shadercache_gl.bin (WGS1, written by gfx/gl/shaders.cpp; the deko3d backend keeps its keys and hashes):
//     magic 'WGS1', then records {u8 1, u8 vertex, u64 hash, u32 packed, u32 size, zlib GLSL},
//     {u8 2, u64 vertex hash, u64 pixel hash} and {u8 3, u32 size, translation}
//
// shadercache_dksh.bin (WDK1): Cemu's GLSL of a WGS1 source compiled for deko3d.
//     header  {char magic[4] = 'WDK1', u32 version = kWdk1Version, u64 uamId}
//     records {u8 stage (uam::Stage: 0 vertex, 4 fragment), u64 glslHash (the WGS1 hash of the source),
//              i8 ubo[64], i8 sampler[64] (ConvertedBindings: Vulkan binding -> deko3d slot, -1 unused),
//              u8 uboCount, u8 samplerCount, i8 ufBlockSlot, i8 ufBlockVkBinding,
//              u32 size, size bytes of DKSH}
//     All little-endian, packed (a record header is 145 bytes). size 0 = the conversion or uam failed
//     for this source with this uamId (do not try again). Records can be appended to the file; a record
//     cut short ends it. uamId (dksh_uam_id()) changes with uam, its patches or glsl_to_deko's output: a file
//     with another uamId is stale as a whole.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "glsl_convert.h"

namespace gfxdk {

struct Wgs1Source {
    uint64_t hash = 0;
    bool vertex = false;
    std::string glsl;
};
// every source of a WGS1 file, first occurrence of each hash, in file order; false (why in *error) when
// data is not a WGS1 file or a source does not inflate. pairs (optional): the {2} records' (vertex, pixel)
// hashes, every one.
bool read_wgs1(const std::vector<uint8_t>& data, std::vector<Wgs1Source>* out, std::string* error,
               std::vector<std::pair<uint64_t, uint64_t>>* pairs = nullptr);

constexpr uint32_t kWdk1Version = 1;
constexpr size_t kWdk1HeaderSize = 16;
constexpr size_t kWdk1RecordHeaderSize = 1 + 8 + 2 * kMaxVkBinding + 4 + 4;
// the compiler's identity: uam's version, our patch set (runtime/third_party/uam/PATCHES.md) and
// glsl_to_deko's revision (kConvertRevision); bump the strings when any of them changes the DKSH bytes
constexpr char kDkshCompilerName[] = "uam 1.1.0 (devkitPro/uam 5a5afc2) + wwhd patches 1-8";
uint64_t dksh_uam_id();

struct DkshRecord {
    uint8_t stage = 0;
    uint64_t glslHash = 0;
    int8_t ubo[kMaxVkBinding];
    int8_t sampler[kMaxVkBinding];
    uint8_t uboCount = 0, samplerCount = 0;
    int8_t ufBlockSlot = -1, ufBlockVkBinding = -1;
    std::vector<uint8_t> dksh;  // empty: failed on the machine that wrote the record
    DkshRecord();
    void set_bindings(const ConvertedBindings& b);
};

std::vector<uint8_t> wdk1_header(uint64_t uamId);
void append_wdk1_record(std::vector<uint8_t>* file, const DkshRecord& r);
// the records of a WDK1 file in file order (a later record of a hash replaces nothing: the caller decides);
// false (why in *error) when it is not a WDK1 file. *uamId gets the file's id, records are read whatever it is.
bool read_wdk1(const std::vector<uint8_t>& data, std::vector<DkshRecord>* out, uint64_t* uamId, std::string* error);

}  // namespace gfxdk
