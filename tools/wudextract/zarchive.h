// Reader for ZArchive files (Cemu's Wii U archives, .wua), written from the format of the reference
// implementation by Exzap (https://github.com/Exzap/ZArchive, MIT No Attribution):
//
//   [compressed data: 64 KiB blocks, each zstd-compressed, or stored as is when that is not smaller]
//   [offset records: per 16 blocks a 64-bit base offset + 16 x (compressed size - 1), 16-bit]
//   [name table: per name a 1-byte length (or 2 bytes when the MSB is set) + the name]
//   [file tree: 16-byte entries; entry 0 is the root directory; a directory lists its children as
//    a range of entries, a file has a 48-bit offset into the uncompressed data and a 48-bit size]
//   [meta data: empty in version 1]
//   [footer, 144 bytes: offset/size of each section, SHA-256 of the archive, total size, version, magic]
//
// Everything is big-endian. The SHA-256 covers the whole file with the footer's hash field zeroed.
// A .wua holds one folder per Wii U title, named <16 hex digit title id>_v<version>.
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace zarchive {

struct Error {
    bool damaged;  // false: not a ZArchive / unreadable structure; true: data does not match its hash or zstd frame
    std::string msg;
};

class Reader {
public:
    static constexpr uint64_t BLOCK = 64 * 1024;

    // true if the file ends with a ZArchive footer (magic and version)
    static bool detect(const std::filesystem::path& p);

    explicit Reader(const std::filesystem::path& p);  // throws Error

    struct Node {
        std::string name;  // UTF-8
        bool is_file;
        uint64_t offset, size;  // file: offset and size in the uncompressed data
        uint32_t first, count;  // directory: its children are entries [first, first + count)
    };
    const Node& node(uint32_t i) const { return nodes_.at(i); }
    uint32_t root() const { return 0; }
    std::vector<uint32_t> children(uint32_t dir) const;

    // calls out(data, n) with the file's contents in order; throws Error
    void read_file(uint32_t file, const std::function<void(const uint8_t*, uint64_t)>& out);

    uint64_t archive_size() const { return size_; }
    // SHA-256 of the whole archive against the footer's hash; progress(done, total) along the way.
    // Returns false on a mismatch.
    bool verify(const std::function<void(uint64_t, uint64_t)>& progress);

private:
    std::ifstream f_;
    uint64_t size_ = 0, data_off_ = 0, data_size_ = 0;
    std::vector<uint64_t> block_off_;  // compressed offset of every block (relative to data_off_)
    std::vector<uint32_t> block_len_;  // compressed length of every block
    std::vector<Node> nodes_;
    uint8_t hash_[32] = {};
    uint64_t cached_ = ~0ull;
    std::vector<uint8_t> cache_, packed_;

    void raw_read(uint64_t off, uint8_t* dst, uint64_t len);
    const uint8_t* block(uint64_t index);
};

}  // namespace zarchive
