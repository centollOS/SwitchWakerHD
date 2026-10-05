// Known-answer tests for tools/wudextract/crypto.cpp (FIPS-197 appendix C.1, NIST SP 800-38A
// F.2.1/F.2.2 CBC-AES128, FIPS-180 SHA-1 examples). Run by ctest (extract_crypto).
#include "crypto.h"

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
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
