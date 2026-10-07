#include "shader_files.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace gfxdk {
namespace {
template <class T> T get(const uint8_t* p) {
    T v;
    memcpy(&v, p, sizeof v);
    return v;
}
template <class T> void put(std::vector<uint8_t>* out, const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out->insert(out->end(), p, p + sizeof v);
}
// FNV-1a 64
uint64_t fnv(const char* s, uint64_t h = 0xCBF29CE484222325ull) {
    for (; *s; ++s) h = (h ^ uint8_t(*s)) * 0x100000001B3ull;
    return h;
}
}  // namespace

bool read_wgs1(const std::vector<uint8_t>& data, std::vector<Wgs1Source>* out, std::string* error,
               std::vector<std::pair<uint64_t, uint64_t>>* pairs) {
    if (data.size() < 4 || memcmp(data.data(), "WGS1", 4) != 0) {
        *error = "not a shadercache_gl.bin (no WGS1 magic)";
        return false;
    }
    std::unordered_set<uint64_t> seen;
    size_t i = 4;
    const uint8_t* d = data.data();
    while (i < data.size()) {
        const uint8_t kind = d[i];
        if (kind == 1 && i + 18 <= data.size()) {
            const uint32_t packed = get<uint32_t>(d + i + 10), size = get<uint32_t>(d + i + 14);
            if (i + 18 + packed > data.size()) break;
            Wgs1Source s;
            s.vertex = d[i + 1] != 0;
            s.hash = get<uint64_t>(d + i + 2);
            if (seen.insert(s.hash).second) {
                s.glsl.resize(size);
                uLongf len = size;
                if (uncompress(reinterpret_cast<Bytef*>(&s.glsl[0]), &len, d + i + 18, packed) != Z_OK || len != size) {
                    char b[96];
                    snprintf(b, sizeof b, "source %016llx does not inflate", (unsigned long long)s.hash);
                    *error = b;
                    return false;
                }
                out->push_back(std::move(s));
            }
            i += 18 + packed;
        } else if (kind == 2 && i + 17 <= data.size()) {
            if (pairs) pairs->emplace_back(get<uint64_t>(d + i + 1), get<uint64_t>(d + i + 9));
            i += 17;
        } else if (kind == 3 && i + 5 <= data.size()) {
            i += 5 + get<uint32_t>(d + i + 1);
        } else {
            break;  // a record cut short ends the file
        }
    }
    return true;
}

uint64_t dksh_uam_id() {
    char conv[48];
    snprintf(conv, sizeof conv, "|glsl_to_deko %d|WDK%u", kConvertRevision, kWdk1Version);
    return fnv(conv, fnv(kDkshCompilerName));
}

DkshRecord::DkshRecord() {
    memset(ubo, -1, sizeof ubo);
    memset(sampler, -1, sizeof sampler);
}

void DkshRecord::set_bindings(const ConvertedBindings& b) {
    memcpy(ubo, b.ubo, sizeof ubo);
    memcpy(sampler, b.sampler, sizeof sampler);
    uboCount = uint8_t(b.uboCount);
    samplerCount = uint8_t(b.samplerCount);
    ufBlockSlot = int8_t(b.ufBlockSlot);
    ufBlockVkBinding = int8_t(b.ufBlockVkBinding);
}

std::vector<uint8_t> wdk1_header(uint64_t uamId) {
    std::vector<uint8_t> h(kWdk1HeaderSize);
    memcpy(h.data(), "WDK1", 4);
    memcpy(h.data() + 4, &kWdk1Version, 4);
    memcpy(h.data() + 8, &uamId, 8);
    return h;
}

void append_wdk1_record(std::vector<uint8_t>* file, const DkshRecord& r) {
    put(file, r.stage);
    put(file, r.glslHash);
    file->insert(file->end(), reinterpret_cast<const uint8_t*>(r.ubo), reinterpret_cast<const uint8_t*>(r.ubo) + kMaxVkBinding);
    file->insert(file->end(), reinterpret_cast<const uint8_t*>(r.sampler),
                 reinterpret_cast<const uint8_t*>(r.sampler) + kMaxVkBinding);
    put(file, r.uboCount);
    put(file, r.samplerCount);
    put(file, r.ufBlockSlot);
    put(file, r.ufBlockVkBinding);
    put(file, uint32_t(r.dksh.size()));
    file->insert(file->end(), r.dksh.begin(), r.dksh.end());
}

bool read_wdk1(const std::vector<uint8_t>& data, std::vector<DkshRecord>* out, uint64_t* uamId, std::string* error) {
    if (data.size() < kWdk1HeaderSize || memcmp(data.data(), "WDK1", 4) != 0) {
        *error = "not a shadercache_dksh.bin (no WDK1 magic)";
        return false;
    }
    const uint8_t* d = data.data();
    if (get<uint32_t>(d + 4) != kWdk1Version) {
        *error = "WDK1 version " + std::to_string(get<uint32_t>(d + 4)) + ", this build reads " +
                 std::to_string(kWdk1Version);
        return false;
    }
    *uamId = get<uint64_t>(d + 8);
    size_t i = kWdk1HeaderSize;
    while (i + kWdk1RecordHeaderSize <= data.size()) {
        const uint8_t* p = d + i;
        const uint32_t size = get<uint32_t>(p + kWdk1RecordHeaderSize - 4);
        if (i + kWdk1RecordHeaderSize + size > data.size()) break;  // cut short
        DkshRecord r;
        r.stage = p[0];
        r.glslHash = get<uint64_t>(p + 1);
        memcpy(r.ubo, p + 9, kMaxVkBinding);
        memcpy(r.sampler, p + 9 + kMaxVkBinding, kMaxVkBinding);
        p += 9 + 2 * kMaxVkBinding;
        r.uboCount = p[0];
        r.samplerCount = p[1];
        r.ufBlockSlot = int8_t(p[2]);
        r.ufBlockVkBinding = int8_t(p[3]);
        r.dksh.assign(d + i + kWdk1RecordHeaderSize, d + i + kWdk1RecordHeaderSize + size);
        out->push_back(std::move(r));
        i += kWdk1RecordHeaderSize + size;
    }
    return true;
}

}  // namespace gfxdk
