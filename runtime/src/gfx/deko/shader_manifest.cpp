// The shader manifest (shader_manifest.h).
//
// shader_manifest.bin: "WSM1", u32 version, then records {u8 kind (1), u32 packed size, u32 size, zlib data}; the
// data of a variant record:
//   u8 vertex, u64 program hash, u32 program size, u64 hash of its first 64 bytes (or all, when smaller: what a
//   search through the dump's shader files looks for first),
//   u8 fetch compact, u32 fetch size, fetch bytes (vertex shaders; size 0 otherwise),
//   u32 register count, then {u16 index, u32 value} for each non-zero context register outside the uniform
//   constants (0xC000-0xCFFF, which the decompiler does not read).
// A record whose data is already in the file (same hash) is not written again.
#include "shader_manifest.h"

#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <unordered_set>
#include <vector>

#include "../../runtime.h"

namespace gfxdk::shader_manifest {
namespace {

constexpr char kPath[] = "shader_manifest.bin";
constexpr uint8_t kMagic[4] = {'W', 'S', 'M', '1'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kNumRegs = 0x10000;
constexpr uint32_t kUniformsFirst = 0xC000, kUniformsEnd = 0xD000;

std::mutex g_mutex;
bool g_writable = false;
std::unordered_set<uint64_t> g_seen;  // hashes of the records' data in the file
uint64_t g_written = 0;

uint64_t hash_data(const std::vector<uint8_t>& d) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint8_t b : d) h = (h ^ b) * 0x100000001B3ull;
    return h;
}

// FNV-1a over the bytes (tools/switch/dksh_cache computes the same over the dump's files)
uint64_t prefix_hash(const uint8_t* p, uint32_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

template <class T> void put(std::vector<uint8_t>& b, const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof v);
}

// the file's records are read once: their hashes, so a variant of an earlier session is not written twice
bool open_file() {
    std::vector<uint8_t> data;
    if (FILE* f = fopen(kPath, "rb")) {
        fseek(f, 0, SEEK_END);
        data.resize(size_t(ftell(f)));
        fseek(f, 0, SEEK_SET);
        if (!data.empty() && fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        fclose(f);
    }
    size_t valid = 0;
    if (data.size() >= 8 && !memcmp(data.data(), kMagic, 4)) {
        uint32_t version;
        memcpy(&version, data.data() + 4, 4);
        if (version == kVersion) {
            valid = 8;
            while (valid + 9 <= data.size() && data[valid] == 1) {
                uint32_t packed, size;
                memcpy(&packed, data.data() + valid + 1, 4);
                memcpy(&size, data.data() + valid + 5, 4);
                if (valid + 9 + packed > data.size()) break;
                std::vector<uint8_t> out(size);
                uLongf outSize = size;
                if (uncompress(out.data(), &outSize, data.data() + valid + 9, packed) != Z_OK || outSize != size) break;
                g_seen.insert(hash_data(out));
                valid += 9 + packed;
            }
        }
    }
    if (valid < data.size() || valid == 0) {  // a new file, another version or a cut record: written again
        if (FILE* f = fopen(kPath, "wb")) {
            if (valid) fwrite(data.data(), 1, valid, f);
            else {
                fwrite(kMagic, 1, 4, f);
                fwrite(&kVersion, 1, 4, f);
            }
            fclose(f);
        }
    }
    // opened for each record and closed again: a file the game holds open cannot be read on the console (the debug
    // server's get), and the harvest wants to fetch it while playing
    if (FILE* f = fopen(kPath, "ab")) {
        g_writable = true;
        fclose(f);
    }
    LOG("[dk] shader manifest %s: %zu variants recorded before%s", kPath, g_seen.size(),
        g_writable ? "" : "; cannot write it");
    return g_writable;
}

}  // namespace

bool enabled() {
    static const bool on = [] {
        const char* e = getenv("WWHD_SHADER_MANIFEST");
        return e && *e == '1';
    }();
    return on;
}

void record(bool vertex, const uint32_t* regs, uint64_t programHash, uint32_t programAddress, uint32_t programSize,
            uint32_t fetchAddress, uint32_t fetchSize, bool fetchCompact) {
    std::vector<uint8_t> d;
    d.reserve(8192);
    put<uint8_t>(d, vertex);
    put(d, programHash);
    put(d, programSize);
    put(d, prefix_hash(ppc_ptr(programAddress), programSize < 64 ? programSize : 64));
    put<uint8_t>(d, vertex && fetchCompact);
    const uint32_t fs = vertex && fetchAddress ? fetchSize : 0;
    put(d, fs);
    if (fs) {
        const uint8_t* p = ppc_ptr(fetchAddress);
        d.insert(d.end(), p, p + fs);
    }
    const size_t countAt = d.size();
    put<uint32_t>(d, 0);
    uint32_t count = 0;
    for (uint32_t i = 0; i < kNumRegs; i++) {
        if (!regs[i] || (i >= kUniformsFirst && i < kUniformsEnd)) continue;
        put<uint16_t>(d, uint16_t(i));
        put(d, regs[i]);
        count++;
    }
    memcpy(d.data() + countAt, &count, 4);

    std::lock_guard<std::mutex> lk(g_mutex);
    // diagnostics for the plan's step 3 (WWHD_SHADER_MANIFEST_DUMP=1): each program's bytes in shader_programs/, to
    // compare on the computer with the dump's files. Game code: for the owner's own analysis, never shared.
    static const bool dump = [] {
        const char* e = getenv("WWHD_SHADER_MANIFEST_DUMP");
        if (!(e && *e == '1')) return false;
        mkdir("shader_programs", 0777);
        return true;
    }();
    static std::unordered_set<uint64_t> dumped;
    if (dump && dumped.insert(programHash).second) {
        char name[64];
        snprintf(name, sizeof name, "shader_programs/%016llx.bin", (unsigned long long)programHash);
        if (FILE* f = fopen(name, "wb")) {
            fwrite(ppc_ptr(programAddress), 1, programSize, f);
            fclose(f);
        }
    }
    static const bool opened = open_file();
    if (!opened || !g_seen.insert(hash_data(d)).second) return;
    uLongf packed = compressBound(d.size());
    std::vector<uint8_t> rec(9 + packed);
    if (compress2(rec.data() + 9, &packed, d.data(), d.size(), 6) != Z_OK) return;
    rec[0] = 1;
    const uint32_t packed32 = uint32_t(packed), size = uint32_t(d.size());
    memcpy(rec.data() + 1, &packed32, 4);
    memcpy(rec.data() + 5, &size, 4);
    FILE* f = fopen(kPath, "ab");
    if (!f) return;
    fwrite(rec.data(), 1, 9 + packed, f);
    fclose(f);
    if (++g_written % 500 == 0) LOG("[dk] shader manifest: %llu variants recorded this session", (unsigned long long)g_written);
}

}  // namespace gfxdk::shader_manifest
