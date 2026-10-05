// AES-128 (CBC decryption) and SHA-1 for the disc extractor: small, dependency-free
// implementations (FIPS-197, FIPS-180-4). Only what reading a Wii U disc image needs.
#pragma once
#include <cstddef>
#include <cstdint>

namespace wudcrypto {

struct Aes128Dec {
    uint32_t rk[44];  // decryption round keys (equivalent inverse cipher)
    explicit Aes128Dec(const uint8_t key[16]);
    void decrypt_block(const uint8_t in[16], uint8_t out[16]) const;
};

struct Aes128Enc {  // used by the tests to build synthetic images
    uint32_t rk[44];
    explicit Aes128Enc(const uint8_t key[16]);
    void encrypt_block(const uint8_t in[16], uint8_t out[16]) const;
};

// CBC decryption in place; len must be a multiple of 16; iv is updated to the last ciphertext block
void aes128_cbc_decrypt(const Aes128Dec& k, uint8_t iv[16], uint8_t* data, size_t len);
void aes128_cbc_encrypt(const Aes128Enc& k, uint8_t iv[16], uint8_t* data, size_t len);

void sha1(const uint8_t* data, size_t len, uint8_t out[20]);

}  // namespace wudcrypto
