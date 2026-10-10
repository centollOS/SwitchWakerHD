// The shader manifest (shader_manifest.h).
//
// shader_manifest.bin: "WSM1", u32 version, then records {u8 kind (1), u32 packed size, u32 size, zlib data}; the
// data of a variant record:
//   u8 vertex, u64 program hash, u32 program size, u64 hash of its first 64 bytes (or all, when smaller: what a
//   search through the dump's shader files looks for first),
//   u8 fetch compact, u32 fetch size, fetch bytes (vertex shaders; size 0 otherwise),
//   u32 register count, then {u16 index, u32 value} for each non-zero context register outside the uniform
//   constants (0xC000-0xCFFF, which the decompiler does not read),
//   u64 the runtime's variant key (translate's): a variant recorded in an earlier session is not built again
//   (readers that stop after the registers ignore it).
// A record whose data is already in the file (same hash) is not written again.
//
// The render thread only builds a new variant's data and queues it; a writer thread compresses and appends the
// queue every 2 s, opening and closing the file each time (a file the game holds open cannot be fetched on the
// console, and nothing touches the SD card from the render thread).
#include "shader_manifest.h"

#include <zlib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <thread>
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

std::mutex g_mutex;                          // the members below
std::unordered_set<uint64_t> g_keys;         // variant keys recorded (file and session)
std::vector<std::vector<uint8_t>> g_queue;   // records' data for the writer
bool g_started = false;

uint64_t hash_data(const std::vector<uint8_t>& d) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint8_t b : d) h = (h ^ b) * 0x100000001B3ull;
    return h;
}

// FNV-1a over the bytes (tools/switch/shader_manifest.py computes the same over the dump's files)
uint64_t prefix_hash(const uint8_t* p, uint32_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

template <class T> void put(std::vector<uint8_t>& b, const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof v);
}

// the writer thread: reads the file once (its records' hashes and keys), then appends the queue every 2 s
void writer() {
    std::unordered_set<uint64_t> seen;  // hashes of the records' data in the file
    std::vector<uint8_t> data;
    if (FILE* f = fopen(kPath, "rb")) {
        fseek(f, 0, SEEK_END);
        data.resize(size_t(ftell(f)));
        fseek(f, 0, SEEK_SET);
        if (!data.empty() && fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        fclose(f);
    }
    size_t valid = 0;
    std::vector<uint64_t> keys;
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
                seen.insert(hash_data(out));
                // the key after the registers, when the record has one
                if (size >= 30) {
                    uint32_t fs, count;
                    memcpy(&fs, out.data() + 22, 4);
                    if (26 + size_t(fs) + 4 <= size) {
                        memcpy(&count, out.data() + 26 + fs, 4);
                        const size_t keyAt = 26 + size_t(fs) + 4 + size_t(count) * 6;
                        if (keyAt + 8 <= size) {
                            uint64_t key;
                            memcpy(&key, out.data() + keyAt, 8);
                            keys.push_back(key);
                        }
                    }
                }
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
    data.clear();
    data.shrink_to_fit();
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_keys.insert(keys.begin(), keys.end());
    }
    LOG("[dk] shader manifest %s: %zu variants recorded before (for building shadercache_dksh.bin on a computer: "
        "make_sd.py --shaders)", kPath, seen.size());
    uint64_t written = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::vector<std::vector<uint8_t>> batch;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            batch.swap(g_queue);
        }
        if (batch.empty()) continue;
        std::vector<uint8_t> out;
        for (auto& d : batch) {
            if (!seen.insert(hash_data(d)).second) continue;
            uLongf packed = compressBound(d.size());
            const size_t at = out.size();
            out.resize(at + 9 + packed);
            if (compress2(out.data() + at + 9, &packed, d.data(), d.size(), 6) != Z_OK) {
                out.resize(at);
                continue;
            }
            out.resize(at + 9 + packed);
            out[at] = 1;
            const uint32_t packed32 = uint32_t(packed), size = uint32_t(d.size());
            memcpy(out.data() + at + 1, &packed32, 4);
            memcpy(out.data() + at + 5, &size, 4);
            written++;
        }
        if (out.empty()) continue;
        if (FILE* f = fopen(kPath, "ab")) {
            fwrite(out.data(), 1, out.size(), f);
            fclose(f);
        }
        if (written / 500 != (written - batch.size()) / 500)
            LOG("[dk] shader manifest: %llu variants recorded this session", (unsigned long long)written);
    }
}

}  // namespace

bool enabled() {
    static const bool on = [] {
        const char* e = getenv("WWHD_SHADER_MANIFEST");
        return !(e && *e == '0');
    }();
    return on;
}

void record(uint64_t key, bool vertex, const uint32_t* regs, uint64_t programHash, uint32_t programAddress,
            uint32_t programSize, uint32_t fetchAddress, uint32_t fetchSize, bool fetchCompact) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_started) {
            g_started = true;
            std::thread(writer).detach();
        }
        if (!g_keys.insert(key).second) return;
    }
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
    put(d, key);

    // diagnostics for the plan's step 3 (WWHD_SHADER_MANIFEST_DUMP=1): each program's bytes in shader_programs/, to
    // compare on the computer with the dump's files. Game code: for the owner's own analysis, never shared.
    static const bool dump = [] {
        const char* e = getenv("WWHD_SHADER_MANIFEST_DUMP");
        if (!(e && *e == '1')) return false;
        mkdir("shader_programs", 0777);
        return true;
    }();
    if (dump) {
        char name[64];
        snprintf(name, sizeof name, "shader_programs/%016llx.bin", (unsigned long long)programHash);
        if (FILE* f = fopen(name, "wb")) {
            fwrite(ppc_ptr(programAddress), 1, programSize, f);
            fclose(f);
        }
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    g_queue.push_back(std::move(d));
}

}  // namespace gfxdk::shader_manifest
