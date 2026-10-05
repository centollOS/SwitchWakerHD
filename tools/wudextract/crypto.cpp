// AES-128 and SHA-1, written from FIPS-197 and FIPS-180-4. The S-box and the round tables are
// computed at startup (no hand-typed tables); tools/wudextract/crypto_test.cpp checks them
// against the standards' test vectors.
#include "crypto.h"

#include <cstring>

namespace wudcrypto {
namespace {

uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

uint32_t rotr8(uint32_t x) { return (x >> 8) | (x << 24); }

struct Tables {
    uint8_t sbox[256], inv_sbox[256];
    uint32_t te[4][256], td[4][256];
    Tables() {
        // S-box: multiplicative inverse in GF(2^8) followed by the affine transform
        for (int i = 0; i < 256; i++) {
            uint8_t inv = 0;
            if (i)
                for (int j = 1; j < 256; j++)
                    if (gmul((uint8_t)i, (uint8_t)j) == 1) { inv = (uint8_t)j; break; }
            uint8_t s = inv;
            uint8_t x = inv;
            for (int r = 0; r < 4; r++) {
                x = (uint8_t)((x << 1) | (x >> 7));
                s ^= x;
            }
            s ^= 0x63;
            sbox[i] = s;
            inv_sbox[s] = (uint8_t)i;
        }
        for (int i = 0; i < 256; i++) {
            uint8_t s = sbox[i];
            // column (2s, s, s, 3s) as a big-endian word: byte 0 is the most significant
            uint32_t e = ((uint32_t)gmul(s, 2) << 24) | ((uint32_t)s << 16) | ((uint32_t)s << 8) | gmul(s, 3);
            uint8_t v = inv_sbox[i];
            uint32_t d = ((uint32_t)gmul(v, 14) << 24) | ((uint32_t)gmul(v, 9) << 16) | ((uint32_t)gmul(v, 13) << 8) |
                         gmul(v, 11);
            for (int t = 0; t < 4; t++) {
                te[t][i] = e;
                td[t][i] = d;
                e = rotr8(e);
                d = rotr8(d);
            }
        }
    }
};

const Tables& T() {
    static const Tables t;
    return t;
}

uint32_t load_be(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void store_be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

uint32_t sub_word(uint32_t w) {
    const uint8_t* s = T().sbox;
    return ((uint32_t)s[w >> 24] << 24) | ((uint32_t)s[(w >> 16) & 0xFF] << 16) | ((uint32_t)s[(w >> 8) & 0xFF] << 8) |
           s[w & 0xFF];
}

void expand_key(const uint8_t key[16], uint32_t w[44]) {
    for (int i = 0; i < 4; i++) w[i] = load_be(key + 4 * i);
    uint8_t rcon = 1;
    for (int i = 4; i < 44; i++) {
        uint32_t t = w[i - 1];
        if (i % 4 == 0) {
            t = sub_word((t << 8) | (t >> 24)) ^ ((uint32_t)rcon << 24);
            rcon = xtime(rcon);
        }
        w[i] = w[i - 4] ^ t;
    }
}

// InvMixColumns of one word: td applied to sbox(x) undoes the inverse S-box inside td
uint32_t inv_mix(uint32_t x) {
    const Tables& t = T();
    return t.td[0][t.sbox[x >> 24]] ^ t.td[1][t.sbox[(x >> 16) & 0xFF]] ^ t.td[2][t.sbox[(x >> 8) & 0xFF]] ^
           t.td[3][t.sbox[x & 0xFF]];
}

}  // namespace

Aes128Enc::Aes128Enc(const uint8_t key[16]) { expand_key(key, rk); }

void Aes128Enc::encrypt_block(const uint8_t in[16], uint8_t out[16]) const {
    const Tables& t = T();
    uint32_t s0 = load_be(in) ^ rk[0], s1 = load_be(in + 4) ^ rk[1], s2 = load_be(in + 8) ^ rk[2],
             s3 = load_be(in + 12) ^ rk[3];
    for (int r = 1; r < 10; r++) {
        const uint32_t* k = rk + 4 * r;
        uint32_t t0 = t.te[0][s0 >> 24] ^ t.te[1][(s1 >> 16) & 0xFF] ^ t.te[2][(s2 >> 8) & 0xFF] ^ t.te[3][s3 & 0xFF] ^ k[0];
        uint32_t t1 = t.te[0][s1 >> 24] ^ t.te[1][(s2 >> 16) & 0xFF] ^ t.te[2][(s3 >> 8) & 0xFF] ^ t.te[3][s0 & 0xFF] ^ k[1];
        uint32_t t2 = t.te[0][s2 >> 24] ^ t.te[1][(s3 >> 16) & 0xFF] ^ t.te[2][(s0 >> 8) & 0xFF] ^ t.te[3][s1 & 0xFF] ^ k[2];
        uint32_t t3 = t.te[0][s3 >> 24] ^ t.te[1][(s0 >> 16) & 0xFF] ^ t.te[2][(s1 >> 8) & 0xFF] ^ t.te[3][s2 & 0xFF] ^ k[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    const uint8_t* s = t.sbox;
    const uint32_t* k = rk + 40;
    auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        return ((uint32_t)s[a >> 24] << 24) | ((uint32_t)s[(b >> 16) & 0xFF] << 16) | ((uint32_t)s[(c >> 8) & 0xFF] << 8) |
               s[d & 0xFF];
    };
    store_be(out, last(s0, s1, s2, s3) ^ k[0]);
    store_be(out + 4, last(s1, s2, s3, s0) ^ k[1]);
    store_be(out + 8, last(s2, s3, s0, s1) ^ k[2]);
    store_be(out + 12, last(s3, s0, s1, s2) ^ k[3]);
}

Aes128Dec::Aes128Dec(const uint8_t key[16]) {
    uint32_t w[44];
    expand_key(key, w);
    // equivalent inverse cipher: round keys in reverse order, InvMixColumns on the middle ones
    for (int r = 0; r <= 10; r++)
        for (int i = 0; i < 4; i++) {
            uint32_t k = w[4 * (10 - r) + i];
            rk[4 * r + i] = (r == 0 || r == 10) ? k : inv_mix(k);
        }
}

void Aes128Dec::decrypt_block(const uint8_t in[16], uint8_t out[16]) const {
    const Tables& t = T();
    uint32_t s0 = load_be(in) ^ rk[0], s1 = load_be(in + 4) ^ rk[1], s2 = load_be(in + 8) ^ rk[2],
             s3 = load_be(in + 12) ^ rk[3];
    for (int r = 1; r < 10; r++) {
        const uint32_t* k = rk + 4 * r;
        uint32_t t0 = t.td[0][s0 >> 24] ^ t.td[1][(s3 >> 16) & 0xFF] ^ t.td[2][(s2 >> 8) & 0xFF] ^ t.td[3][s1 & 0xFF] ^ k[0];
        uint32_t t1 = t.td[0][s1 >> 24] ^ t.td[1][(s0 >> 16) & 0xFF] ^ t.td[2][(s3 >> 8) & 0xFF] ^ t.td[3][s2 & 0xFF] ^ k[1];
        uint32_t t2 = t.td[0][s2 >> 24] ^ t.td[1][(s1 >> 16) & 0xFF] ^ t.td[2][(s0 >> 8) & 0xFF] ^ t.td[3][s3 & 0xFF] ^ k[2];
        uint32_t t3 = t.td[0][s3 >> 24] ^ t.td[1][(s2 >> 16) & 0xFF] ^ t.td[2][(s1 >> 8) & 0xFF] ^ t.td[3][s0 & 0xFF] ^ k[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    const uint8_t* s = t.inv_sbox;
    const uint32_t* k = rk + 40;
    auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        return ((uint32_t)s[a >> 24] << 24) | ((uint32_t)s[(b >> 16) & 0xFF] << 16) | ((uint32_t)s[(c >> 8) & 0xFF] << 8) |
               s[d & 0xFF];
    };
    store_be(out, last(s0, s3, s2, s1) ^ k[0]);
    store_be(out + 4, last(s1, s0, s3, s2) ^ k[1]);
    store_be(out + 8, last(s2, s1, s0, s3) ^ k[2]);
    store_be(out + 12, last(s3, s2, s1, s0) ^ k[3]);
}

void aes128_cbc_decrypt(const Aes128Dec& k, uint8_t iv[16], uint8_t* data, size_t len) {
    uint8_t prev[16], cur[16];
    memcpy(prev, iv, 16);
    for (size_t off = 0; off + 16 <= len; off += 16) {
        memcpy(cur, data + off, 16);
        k.decrypt_block(cur, data + off);
        for (int i = 0; i < 16; i++) data[off + i] ^= prev[i];
        memcpy(prev, cur, 16);
    }
    memcpy(iv, prev, 16);
}

void aes128_cbc_encrypt(const Aes128Enc& k, uint8_t iv[16], uint8_t* data, size_t len) {
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for (size_t off = 0; off + 16 <= len; off += 16) {
        for (int i = 0; i < 16; i++) data[off + i] ^= prev[i];
        k.encrypt_block(data + off, data + off);
        memcpy(prev, data + off, 16);
    }
    memcpy(iv, prev, 16);
}

void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = load_be(p + 4 * i);
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    };
    size_t full = len / 64 * 64;
    for (size_t off = 0; off < full; off += 64) block(data + off);
    uint8_t tail[128] = {};
    size_t rem = len - full;
    memcpy(tail, data + full, rem);
    tail[rem] = 0x80;
    size_t tl = rem + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    block(tail);
    if (tl == 128) block(tail + 64);
    for (int i = 0; i < 5; i++) store_be(out + 4 * i, h[i]);
}

}  // namespace wudcrypto
