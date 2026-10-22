#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Whole-surface change hash, only ever compared with the same surface's earlier hash. On AArch64 it
// runs 8 independent crc32c lanes over 8-byte words (crc32cx: 3-cycle latency, 1/cycle throughput on the
// A57, so 8 lanes keep the unit busy); elsewhere 4 multiply lanes (one multiply chain is latency-bound).
// The tail (< one block) is folded in word by word, then byte by byte, so unaligned sizes are covered.
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#include <arm_acle.h>
__attribute__((target("+crc")))
inline uint64_t surface_hash(const uint8_t* p, size_t n) {
    constexpr int L = 8;
    uint32_t h[L];
    for (int l = 0; l < L; l++) h[l] = 0x9E3779B9u * uint32_t(l + 1);
    size_t i = 0;
    for (; n - i >= 8 * L; i += 8 * L)
        for (int l = 0; l < L; l++) {
            uint64_t word;
            memcpy(&word, p + i + l * 8, sizeof(word));
            h[l] = __crc32cd(h[l], word);
        }
    uint64_t r = uint64_t(n);
    for (int l = 0; l < L; l++) {
        r = (r ^ h[l]) * 0x9E3779B97F4A7C15ull;
        r ^= r >> 29;
    }
    for (; n - i >= 8; i += 8) {
        uint64_t word;
        memcpy(&word, p + i, sizeof(word));
        r = (r ^ word) * 0x100000001B3ull;
        r ^= r >> 32;
    }
    for (; i < n; ++i) r = (r ^ p[i]) * 0x100000001B3ull;
    return r ^ (r >> 29);
}
#else
inline uint64_t surface_hash(const uint8_t* p, size_t n) {
    uint64_t h[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
    size_t i = 0;
    for (; n - i >= 32; i += 32)
        for (int l = 0; l < 4; l++) {
            uint64_t word;
            memcpy(&word, p + i + l * 8, sizeof(word));
            h[l] = (h[l] ^ word) * 0xFF51AFD7ED558CCDull;
            h[l] ^= h[l] >> 32;
        }
    uint64_t r = h[0] ^ (h[1] * 31) ^ (h[2] * 1009) ^ (h[3] * 65537);
    for (; i < n; ++i) r = (r ^ p[i]) * 0x100000001B3ull;
    return r ^ (r >> 29) ^ uint64_t(n);
}
#endif
