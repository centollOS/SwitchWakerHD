// Lenzo's Picto Box test in converted GameCube saves (runtime/src/hle/picto_quest.h, #306)
#include "hle/picto_quest.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace picto_quest;

#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);      \
            std::abort();                                                     \
        }                                                                     \
    } while (0)

static void seal(uint8_t* b) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < kSaveSummed; i++) sum += b[i];
    store_be32(b + kSaveSummed, sum);
    store_be32(b + kSaveSummed + 4, kSaveSummed * 0xFFFFFFFFu - sum);
}

// a used Quest Log: Lenzo's register `reg` (other bits of the byte set), `camera` in slot 8
static std::vector<uint8_t> block(uint8_t reg, uint8_t camera, uint8_t got) {
    std::vector<uint8_t> b(kSaveBlock);
    for (uint32_t i = 0; i < 0x768; i++) b[i] = uint8_t(i * 7 + 3);  // unrelated data
    b[kLenzoReg] = uint8_t(0xA8 | reg);
    b[kCameraItem] = camera;
    b[kCameraGetItem] = got;
    seal(b.data());
    return b;
}

static bool sealed(const uint8_t* b) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < kSaveSummed; i++) sum += b[i];
    return load_be32(b + kSaveSummed) == sum && load_be32(b + kSaveSummed + 4) == kSaveSummed * 0xFFFFFFFFu - sum;
}

int main() {
    // every stage of the test; only "passed, black-and-white Picto Box" changes
    struct Case { uint8_t reg, camera, got; bool repaired; };
    const Case cases[] = {
        {0, 0xFF, 0x00, false},              // no Picto Box
        {0, kPictoBox, 0x01, false},         // Picto Box, test not started
        {1, kPictoBox, 0x01, false}, {2, kPictoBox, 0x01, false}, {3, kPictoBox, 0x01, false},
        {4, kPictoBox, 0x01, false}, {5, kPictoBox, 0x01, false},  // HD gives Deluxe at the 3rd
        {6, kPictoBox, 0x01, true},          // GameCube: waiting for the Forest Firefly
        {7, kPictoBox, 0x01, true},          // out of range, >= 6 in both games
        {6, kDeluxePictoBox, 0x03, false},   // finished (both games), a normal HD save
        {6, kPictoBox, 0x03, false},         // Deluxe owned (e.g. a cheat put the old box back)
        {6, 0xFF, 0x00, false},              // no Picto Box at all: not this quest's state
    };
    for (const Case& c : cases) {
        std::vector<uint8_t> b = block(c.reg, c.camera, c.got), before = b;
        CHECK(stuck(b.data()) == c.repaired);
        CHECK(repair(b.data()) == c.repaired);
        CHECK(sealed(b.data()));
        if (!c.repaired) {
            CHECK(b == before);
            continue;
        }
        for (uint32_t i = 0; i < kSaveSummed; i++)
            if (i != kCameraItem && i != kCameraGetItem) CHECK(b[i] == before[i]);
        CHECK(b[kCameraItem] == kDeluxePictoBox);
        CHECK(b[kCameraGetItem] == (c.got | kDeluxeGetBit));
        CHECK(b[kLenzoReg] == before[kLenzoReg]);  // the test stays passed
        // once: a second read finds nothing to do
        std::vector<uint8_t> again = b;
        CHECK(!repair(again.data()) && again == b);
    }
    // a damaged Quest Log is left alone
    std::vector<uint8_t> bad = block(6, kPictoBox, 0x01);
    bad[0x100] ^= 1;
    std::vector<uint8_t> copy = bad;
    CHECK(!repair(bad.data()) && bad == copy);
    // an unused Quest Log (all zero, checksum 0 / complement) stays
    std::vector<uint8_t> unused(kSaveBlock);
    seal(unused.data());
    copy = unused;
    CHECK(!repair(unused.data()) && unused == copy);
    std::puts("picto_quest_test: ok");
    return 0;
}
