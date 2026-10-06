// ZArchive (.wua) reader: see zarchive.h. zstd (BSD) decompresses the blocks.
#include "zarchive.h"

#include "crypto.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;

namespace zarchive {

namespace {

constexpr uint32_t MAGIC = 0x169f52d6, VERSION1 = 0x61bf3a01;
constexpr uint64_t FOOTER = 144, RECORD = 8 + 2 * 16, ENTRY = 16;

uint64_t be(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}

[[noreturn]] void bad(const std::string& msg) { throw Error{false, msg}; }
[[noreturn]] void damaged(const std::string& msg) { throw Error{true, msg}; }

// names are Windows-1252 by the format's definition (UTF-8 works in practice): keep valid UTF-8,
// read anything else as Latin-1
std::string to_utf8(const uint8_t* p, size_t n) {
    bool utf8 = true;
    for (size_t i = 0; i < n && utf8;) {
        uint8_t c = p[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n) utf8 = false;
        for (size_t k = 1; utf8 && k < len; k++) utf8 = (p[i + k] & 0xC0) == 0x80;
        i += len ? len : 1;
    }
    if (utf8) return std::string((const char*)p, n);
    std::string s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < 0x80) s += (char)p[i];
        else s += (char)(0xC0 | p[i] >> 6), s += (char)(0x80 | (p[i] & 0x3F));
    }
    return s;
}

}  // namespace

bool Reader::detect(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    auto size = (uint64_t)f.tellg();
    if (size <= FOOTER) return false;
    uint8_t t[8];
    f.seekg((std::streamoff)(size - 8));
    f.read((char*)t, 8);
    return f.gcount() == 8 && be(t, 4) == VERSION1 && be(t + 4, 4) == MAGIC;
}

void Reader::raw_read(uint64_t off, uint8_t* dst, uint64_t len) {
    if (off > size_ || len > size_ - off) bad("read beyond the end of the archive");
    f_.clear();
    f_.seekg((std::streamoff)off);
    f_.read((char*)dst, (std::streamsize)len);
    if ((uint64_t)f_.gcount() != len) bad("the archive is truncated or unreadable");
}

Reader::Reader(const fs::path& p) : f_(p, std::ios::binary) {
    if (!f_) bad("cannot open the archive");
    f_.seekg(0, std::ios::end);
    size_ = (uint64_t)f_.tellg();
    if (size_ <= FOOTER) bad("not a Cemu Wii U archive (.wua): too small");
    uint8_t ft[FOOTER];
    raw_read(size_ - FOOTER, ft, FOOTER);
    if (be(ft + 140, 4) != MAGIC || be(ft + 136, 4) != VERSION1) bad("not a Cemu Wii U archive (.wua)");
    if (be(ft + 128, 8) != size_)
        bad("the archive is truncated (its size does not match the size recorded in it; incomplete download or copy?)");
    memcpy(hash_, ft + 96, 32);
    struct Section {
        uint64_t off, size;
    } sec[6];
    for (int i = 0; i < 6; i++) {
        sec[i] = {be(ft + 16 * i, 8), be(ft + 16 * i + 8, 8)};
        if (sec[i].off > size_ || sec[i].size > size_ - sec[i].off) bad("corrupt archive (section out of range)");
    }
    const Section &data = sec[0], &records = sec[1], &names = sec[2], &tree = sec[3];
    data_off_ = data.off, data_size_ = data.size;
    if (records.size % RECORD || names.size > 0x7FFFFFFF || tree.size % ENTRY || tree.size == 0 ||
        tree.size / ENTRY > 0xFFFFFFFF)
        bad("corrupt archive (section sizes)");

    // offset records -> offset and length of every block
    std::vector<uint8_t> rec(records.size);
    raw_read(records.off, rec.data(), rec.size());
    for (size_t r = 0; r < rec.size() / RECORD; r++) {
        const uint8_t* q = &rec[r * RECORD];
        uint64_t off = be(q, 8);
        for (int k = 0; k < 16; k++) {
            uint32_t len = (uint32_t)be(q + 8 + 2 * k, 2) + 1;
            block_off_.push_back(off);
            block_len_.push_back(len);
            off += len;
        }
    }

    std::vector<uint8_t> nt(names.size);
    raw_read(names.off, nt.data(), nt.size());
    auto name_at = [&](uint32_t o, std::string& out) {
        if (o >= nt.size()) return false;
        uint32_t len = nt[o] & 0x7F;
        if (nt[o] & 0x80) {
            if (o + 1 >= nt.size()) return false;
            len |= (uint32_t)nt[o + 1] << 7;
            o += 2;
        } else {
            o += 1;
        }
        if (len > nt.size() - std::min<size_t>(o, nt.size())) return false;
        out = to_utf8(&nt[o], len);
        return true;
    };

    std::vector<uint8_t> t(tree.size);
    raw_read(tree.off, t.data(), t.size());
    uint64_t n = tree.size / ENTRY, data_end = (uint64_t)block_off_.size() * BLOCK;
    nodes_.resize(n);
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t* e = &t[i * ENTRY];
        uint32_t w0 = (uint32_t)be(e, 4), w1 = (uint32_t)be(e + 4, 4), w2 = (uint32_t)be(e + 8, 4),
                 w3 = (uint32_t)be(e + 12, 4);
        Node& nd = nodes_[i];
        nd.is_file = w0 >> 31;
        uint32_t name_off = w0 & 0x7FFFFFFF;
        if (i == 0) {
            if (nd.is_file) bad("corrupt archive (the root is not a folder)");
        } else if (!name_at(name_off, nd.name) || nd.name.empty()) {
            bad("corrupt archive (name table)");
        }
        if (nd.is_file) {
            nd.offset = w1 | (uint64_t)(w3 & 0xFFFF) << 32;
            nd.size = w2 | (uint64_t)(w3 & 0xFFFF0000) << 16;
            nd.first = nd.count = 0;
            if (nd.offset > data_end || nd.size > data_end - nd.offset) bad("corrupt archive (file data out of range)");
        } else {
            nd.offset = nd.size = 0;
            nd.first = w1, nd.count = w2;
            // children always come after their folder (the writer numbers the tree breadth-first):
            // this also rules out loops in a damaged tree
            if (nd.count && (nd.first <= i || nd.first > n || nd.count > n - nd.first))
                bad("corrupt archive (folder entries out of range)");
        }
    }
    cache_.resize(BLOCK);
}

std::vector<uint32_t> Reader::children(uint32_t dir) const {
    const Node& d = nodes_.at(dir);
    std::vector<uint32_t> v;
    if (d.is_file) return v;
    for (uint32_t i = 0; i < d.count; i++) v.push_back(d.first + i);
    return v;
}

const uint8_t* Reader::block(uint64_t index) {
    if (index == cached_) return cache_.data();
    if (index >= block_off_.size()) bad("corrupt archive (block index)");
    uint64_t off = block_off_[index], len = block_len_[index];
    if (off > data_size_ || len > data_size_ - off) bad("corrupt archive (block out of range)");
    cached_ = ~0ull;
    if (len == BLOCK) {  // stored uncompressed
        raw_read(data_off_ + off, cache_.data(), BLOCK);
    } else {
        packed_.resize(len);
        raw_read(data_off_ + off, packed_.data(), len);
        size_t r = ZSTD_decompress(cache_.data(), BLOCK, packed_.data(), len);
        if (ZSTD_isError(r) || r != BLOCK)
            damaged("block " + std::to_string(index) + " cannot be decompressed: the archive is damaged");
    }
    cached_ = index;
    return cache_.data();
}

void Reader::read_file(uint32_t file, const std::function<void(const uint8_t*, uint64_t)>& out) {
    const Node& nd = nodes_.at(file);
    uint64_t pos = nd.offset, remaining = nd.size;
    while (remaining > 0) {
        uint64_t within = pos % BLOCK, n = std::min(remaining, BLOCK - within);
        const uint8_t* b = block(pos / BLOCK);
        out(b + within, n);
        pos += n, remaining -= n;
    }
}

bool Reader::verify(const std::function<void(uint64_t, uint64_t)>& progress) {
    wudcrypto::Sha256 sha;
    std::vector<uint8_t> buf(4 << 20);
    uint64_t pos = 0, hash_at = size_ - FOOTER + 96;
    progress(0, size_);
    while (pos < size_) {
        uint64_t n = std::min<uint64_t>(buf.size(), size_ - pos);
        raw_read(pos, buf.data(), n);
        // the hash field itself counts as zeros
        for (uint64_t i = std::max(pos, hash_at); i < std::min(pos + n, hash_at + 32); i++) buf[i - pos] = 0;
        sha.update(buf.data(), n);
        pos += n;
        progress(pos, size_);
    }
    uint8_t d[32];
    sha.final(d);
    return memcmp(d, hash_, 32) == 0;
}

}  // namespace zarchive
