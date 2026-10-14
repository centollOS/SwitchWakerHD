// The story block: what story mods record for each Quest Log (letters, the notebook, quests and their
// flags, the tracked quest), kept in cking.sav after the game's own bytes (docs/mod-sdk-v2.md, "Where
// story progress is saved"). Pure code, unit-tested in runtime/tools/story_notebook_test.cpp.
//
//   cking.sav = the game's bytes | payload | footer
//   footer (16 bytes, big-endian): "WSTB", payload length u32, format version u16, 0 u16, CRC-32 of the
//   payload u32.
//   payload, version 1: three areas of kAreaSize bytes, Quest Logs 1-3, each a story_mail::Book with
//   kAreaRecords records (an all-zero area is an empty one).
//
// The port finds the block from the end of the file, so the game's part is the file size minus the
// block, whatever the game wrote. A footer whose length doesn't fit the file is no footer: the whole
// file is the game's. A footer that fits but whose checksum or version is wrong marks a damaged block:
// the game's part still ends before it, its content is not used, and it is replaced at the next save.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "story_mail_book.h"

namespace story_block {

inline constexpr unsigned kQuestLogs = 3;
inline constexpr size_t kAreaSize = 0x400;
inline constexpr unsigned kAreaRecords = (kAreaSize - 4) / 4;  // 255
inline constexpr size_t kPayloadSize = kQuestLogs * kAreaSize;
inline constexpr size_t kFooterSize = 16;
inline constexpr uint8_t kFooterMagic[4] = {'W', 'S', 'T', 'B'};
inline constexpr uint16_t kFormat = 1;

inline uint32_t crc32(const uint8_t* p, size_t n) {
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

enum class Found { None, Valid, Damaged };
struct Split {
    size_t game_size = 0;        // the game's part of the file
    Found found = Found::None;
    const uint8_t* payload = nullptr;  // Valid: kPayloadSize bytes
};

// Where the game's part ends, and the block if there is one.
inline Split split(const uint8_t* file, size_t size) {
    Split s;
    s.game_size = size;
    if (size < kFooterSize) return s;
    const uint8_t* f = file + size - kFooterSize;
    if (memcmp(f, kFooterMagic, 4) != 0) return s;
    const uint32_t length = be32(f + 4);
    if (length > size - kFooterSize) return s;  // doesn't fit: not a footer
    s.game_size = size - kFooterSize - length;
    const uint8_t* payload = file + s.game_size;
    const uint16_t format = uint16_t(f[8] << 8 | f[9]);
    s.found = format == kFormat && length == kPayloadSize && !f[10] && !f[11] && crc32(payload, length) == be32(f + 12)
                  ? Found::Valid
                  : Found::Damaged;
    if (s.found == Found::Valid) s.payload = payload;
    return s;
}

// payload + footer for the areas (kPayloadSize bytes)
inline std::vector<uint8_t> build(const uint8_t* areas) {
    std::vector<uint8_t> out(kPayloadSize + kFooterSize, 0);
    memcpy(out.data(), areas, kPayloadSize);
    uint8_t* f = out.data() + kPayloadSize;
    memcpy(f, kFooterMagic, 4);
    put32(f + 4, uint32_t(kPayloadSize));
    f[8] = uint8_t(kFormat >> 8), f[9] = uint8_t(kFormat);
    put32(f + 12, crc32(areas, kPayloadSize));
    return out;
}

inline bool all_zero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i]) return false;
    return true;
}

// One-time move of a Quest Log's record from the game's reserved save bytes (the format the port used
// before the block, story_mail_book.h) into its area: every record of ours is copied. False when the
// old bytes hold nothing of ours (then they are left alone).
inline bool migrate(uint8_t* old_bytes, uint8_t* area) {
    story_mail::Book old(old_bytes);
    if (old.empty() || !old.usable()) return false;
    story_mail::Book to(area, kAreaRecords);
    for (unsigned r = 0; r < story_mail::kRecords; r++)
        if (old.record_kind(r) != story_mail::kRecordFree && !to.has(old.record_kind(r), old.record_key(r)))
            to.set(old.record_kind(r), old.record_key(r), old.record_value(r));
    memset(old_bytes, 0, story_mail::kBookSize);  // the game's bytes are the game's again
    return true;
}

}  // namespace story_block
