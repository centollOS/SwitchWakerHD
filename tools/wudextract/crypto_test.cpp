// Known-answer tests for tools/wudextract/crypto.cpp (FIPS-197 appendix C.1, NIST SP 800-38A
// F.2.1/F.2.2 CBC-AES128, FIPS-180 SHA-1 and SHA-256 examples). Run by ctest (extract_crypto).
#include "crypto.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace wudcrypto;

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    for (; s[0] && s[1]; s += 2) v.push_back((uint8_t)std::stoi(std::string(s, 2), nullptr, 16));
    return v;
}

static int failures = 0;
static void check(const char* what, const uint8_t* got, const std::vector<uint8_t>& want) {
    if (memcmp(got, want.data(), want.size())) {
        printf("FAIL %s\n", what);
        failures++;
    } else {
        printf("ok   %s\n", what);
    }
}

int main() {
    {  // FIPS-197 C.1
        auto key = hex("0001020304050607" "08090a0b0c0d0e0f");
        auto pt = hex("0011223344556677" "8899aabbccddeeff");
        auto ct = hex("69c4e0d86a7b0430" "d8cdb78070b4c55a");
        uint8_t out[16];
        Aes128Enc(key.data()).encrypt_block(pt.data(), out);
        check("AES-128 encrypt (FIPS-197 C.1)", out, ct);
        Aes128Dec(key.data()).decrypt_block(ct.data(), out);
        check("AES-128 decrypt (FIPS-197 C.1)", out, pt);
    }
    {  // SP 800-38A F.2.2 CBC-AES128.Decrypt
        auto key = hex("2b7e151628aed2a6" "abf7158809cf4f3c");
        auto iv = hex("0001020304050607" "08090a0b0c0d0e0f");
        auto ct = hex("7649abac8119b246" "cee98e9b12e9197d" "5086cb9b507219ee" "95db113a917678b2"
                      "73bed6b8e3c1743b" "7116e69e22229516" "3ff1caa1681fac09" "120eca307586e1a7");
        auto pt = hex("6bc1bee22e409f96" "e93d7e117393172a" "ae2d8a571e03ac9c" "9eb76fac45af8e51"
                      "30c81c46a35ce411" "e5fbc1191a0a52ef" "f69f2445df4f9b17" "ad2b417be66c3710");
        std::vector<uint8_t> buf = ct;
        uint8_t ivb[16];
        memcpy(ivb, iv.data(), 16);
        aes128_cbc_decrypt(Aes128Dec(key.data()), ivb, buf.data(), buf.size());
        check("AES-128-CBC decrypt (SP 800-38A F.2.2)", buf.data(), pt);
        check("AES-128-CBC chained IV", ivb, std::vector<uint8_t>(ct.end() - 16, ct.end()));
        // split decryption (two calls) must equal one call
        buf = ct;
        memcpy(ivb, iv.data(), 16);
        aes128_cbc_decrypt(Aes128Dec(key.data()), ivb, buf.data(), 32);
        aes128_cbc_decrypt(Aes128Dec(key.data()), ivb, buf.data() + 32, 32);
        check("AES-128-CBC decrypt in two parts", buf.data(), pt);
        buf = pt;
        memcpy(ivb, iv.data(), 16);
        aes128_cbc_encrypt(Aes128Enc(key.data()), ivb, buf.data(), buf.size());
        check("AES-128-CBC encrypt (SP 800-38A F.2.1)", buf.data(), ct);
    }
    {
        uint8_t d[20];
        sha1((const uint8_t*)"abc", 3, d);
        check("SHA-1 abc", d, hex("a9993e364706816a" "ba3e25717850c26c" "9cd0d89d"));
        const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        sha1((const uint8_t*)m, strlen(m), d);
        check("SHA-1 448-bit message", d, hex("84983e441c3bd26e" "baae4aa1f95129e5" "e54670f1"));
        sha1((const uint8_t*)"", 0, d);
        check("SHA-1 empty", d, hex("da39a3ee5e6b4b0d" "3255bfef95601890" "afd80709"));
        std::vector<uint8_t> a(1000000, 'a');
        sha1(a.data(), a.size(), d);
        check("SHA-1 million a", d, hex("34aa973cd4c4daa4" "f61eeb2bdbad2731" "6534016f"));
    }
    {
        auto sha256 = [](const uint8_t* p, size_t n, uint8_t out[32]) {
            Sha256 s;
            s.update(p, n);
            s.final(out);
        };
        uint8_t d[32];
        sha256((const uint8_t*)"abc", 3, d);
        check("SHA-256 abc", d, hex("ba7816bf8f01cfea" "414140de5dae2223" "b00361a396177a9c" "b410ff61f20015ad"));
        const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        sha256((const uint8_t*)m, strlen(m), d);
        check("SHA-256 448-bit message", d, hex("248d6a61d20638b8" "e5c026930c3e6039" "a33ce45964ff2167" "f6ecedd419db06c1"));
        sha256((const uint8_t*)"", 0, d);
        check("SHA-256 empty", d, hex("e3b0c44298fc1c14" "9afbf4c8996fb924" "27ae41e4649b934c" "a495991b7852b855"));
        // a million 'a' in uneven pieces (streaming across block boundaries)
        Sha256 s;
        std::vector<uint8_t> a(1000000, 'a');
        size_t off = 0, piece = 1;
        while (off < a.size()) {
            size_t n = std::min(piece, a.size() - off);
            s.update(a.data() + off, n);
            off += n, piece = piece * 3 % 997 + 1;
        }
        s.final(d);
        check("SHA-256 million a (streamed)", d, hex("cdc76e5c9914fb92" "81a1c7e284d73e67" "f1809a48a497200e" "046d39ccc7112cd0"));
    }
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
