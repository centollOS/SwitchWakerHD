// surface_hash (gfx/deko/surface_hash.h): deterministic, alignment-independent, sensitive to every bit.
// `surface_hash_test bench` prints MB/s against the old one-multiply-per-byte FNV.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gfx/deko/surface_hash.h"

static uint64_t fnv_bytes(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

template <class F> static double mbps(F f, const uint8_t* p, size_t n) {
    static volatile uint64_t sink;
    uint64_t s = 0;
    auto t = std::chrono::steady_clock::now();
    int it = 200;
    for (int k = 0; k < it; k++) s += f(p, n - (k & 7));
    sink = s;
    return double(n) * it / std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count() / 1048576.0;
}

int main(int argc, char** argv) {
    int fail = 0;
    // sizes around the block boundaries, starts at every alignment
    for (size_t n : {0, 1, 7, 8, 9, 31, 32, 63, 64, 65, 127, 128, 129, 1000, 4099}) {
        std::vector<uint8_t> buf(n + 16);
        for (size_t i = 0; i < buf.size(); i++) buf[i] = uint8_t(i * 2654435761u >> 13);
        for (size_t off = 0; off < 8; off++) {
            std::vector<uint8_t> a(n + 16), copy(n + 16);
            memcpy(a.data() + off, buf.data(), n);
            memcpy(copy.data() + 8 - off, buf.data(), n);  // same content, another alignment
            uint64_t h0 = surface_hash(a.data() + off, n);
            if (surface_hash(copy.data() + 8 - off, n) != h0 || surface_hash(a.data() + off, n) != h0) {
                printf("FAIL: n=%zu off=%zu not deterministic\n", n, off);
                fail++;
            }
            for (size_t bit = 0; bit < n * 8; bit++) {
                a[off + bit / 8] ^= uint8_t(1u << (bit % 8));
                if (surface_hash(a.data() + off, n) == h0) {
                    printf("FAIL: n=%zu off=%zu bit %zu not seen\n", n, off, bit);
                    fail++;
                }
                a[off + bit / 8] ^= uint8_t(1u << (bit % 8));
            }
        }
    }
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        std::vector<uint8_t> v((64 << 10) + 16);
        for (size_t i = 0; i < v.size(); i++) v[i] = uint8_t(i * 2654435761u >> 13);
        printf("fnv per byte %.0f MB/s\nsurface_hash %.0f MB/s\n", mbps(fnv_bytes, v.data() + 1, 64 << 10),
               mbps(surface_hash, v.data() + 1, 64 << 10));
    }
    printf(fail ? "surface_hash_test: %d failures\n" : "surface_hash_test: ok\n", fail);
    return fail != 0;
}
