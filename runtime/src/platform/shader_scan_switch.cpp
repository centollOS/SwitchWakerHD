// The game's shader programs from the SD card (shader_scan_switch.h): tools/shaderprep.py's yaz0 / sarc / walk /
// sharcfb_shaders / game_shaders, in C++.
#include "shader_scan_switch.h"

#include <dirent.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace shader_scan {
namespace {

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | p[2] << 16 | p[1] << 8 | p[0]; }

// tools/yaz0.c
bool yaz0(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    if (in.size() < 16 || memcmp(in.data(), "Yaz0", 4)) return false;
    const size_t n = be32(&in[4]);
    out.resize(n);
    const uint8_t* src = in.data();
    const size_t srclen = in.size();
    uint8_t* dst = out.data();
    size_t s = 16, d = 0;
    while (d < n && s < srclen) {
        uint8_t code = src[s++];
        for (int bit = 0; bit < 8 && d < n; bit++, code <<= 1) {
            if (code & 0x80) {
                if (s >= srclen) return false;
                dst[d++] = src[s++];
            } else {
                if (s + 1 >= srclen) return false;
                const uint32_t b1 = src[s], b2 = src[s + 1];
                s += 2;
                const size_t dist = ((b1 & 0x0F) << 8 | b2) + 1;
                size_t len = b1 >> 4;
                if (len == 0) {
                    if (s >= srclen) return false;
                    len = src[s++] + 0x12;
                } else {
                    len += 2;
                }
                if (dist > d) return false;
                for (size_t i = 0; i < len && d < n; i++, d++) dst[d] = dst[d - dist];
            }
        }
    }
    out.resize(d);
    return true;
}

struct Scanner {
    Stats& st;
    std::vector<Program> out;
    std::unordered_set<std::string> seen;  // (type, microcode)

    // SHARCFB ("BAHS"): binary section entries {entry size, type, ?, data size, GX2 structure}
    void sharcfb(const uint8_t* b, size_t n) {
        if (n < 0x20 || memcmp(b, "BAHS", 4)) return;
        st.archives++;
        size_t o = 0x18 + le32(b + 0x14);
        if (o + 8 > n) return;
        const uint32_t count = le32(b + o + 4);
        size_t p = o + 8;
        for (uint32_t k = 0; k < count && p + 16 <= n; k++) {
            const uint32_t size = le32(b + p), typ = le32(b + p + 4), dsz = le32(b + p + 12);
            if (size == 0 || p + 16 + dsz > n) break;
            const uint8_t* data = b + p + 16;
            p += size;
            size_t nregs, szOff, ptrOff;
            if (typ == 0 && dsz >= 0xDC) nregs = 52, szOff = 0xD0, ptrOff = 0xD4;
            else if (typ == 1 && dsz >= 0xAC) nregs = 41, szOff = 0xA4, ptrOff = 0xA8;
            else continue;
            const uint32_t codeSize = le32(data + szOff), ptr = le32(data + ptrOff);
            if (uint64_t(ptr) + codeSize > dsz) continue;
            st.shaders++;
            std::string key(1, char(typ));
            key.append(reinterpret_cast<const char*>(data + ptr), codeSize);
            if (!seen.insert(std::move(key)).second) continue;
            Program pr;
            pr.vertex = typ == 0;
            pr.words.resize(nregs);
            for (size_t i = 0; i < nregs; i++) pr.words[i] = le32(data + 4 * i);
            pr.code.assign(data + ptr, data + ptr + codeSize);
            out.push_back(std::move(pr));
        }
    }

    // Yaz0 / SARC nesting, as shaderprep.walk
    void walk(const uint8_t* b, size_t n, int depth) {
        if (depth > 6 || n < 8) return;
        if (!memcmp(b, "Yaz0", 4)) {
            std::vector<uint8_t> in(b, b + n), inflated;
            const double t0 = now();
            const bool ok = yaz0(in, inflated);
            st.inflateSeconds += now() - t0;
            st.bytesInflated += inflated.size();
            if (ok) walk(inflated.data(), inflated.size(), depth + 1);
            return;
        }
        if (!memcmp(b, "SARC", 4)) {
            const bool big = b[6] == 0xFE && b[7] == 0xFF;
            auto u16 = [&](size_t o) { return big ? uint16_t(b[o] << 8 | b[o + 1]) : uint16_t(b[o + 1] << 8 | b[o]); };
            auto u32 = [&](size_t o) { return big ? be32(b + o) : le32(b + o); };
            const size_t hl = u16(4), doff = u32(0xC);
            if (hl + 12 > n) return;
            const size_t count = u16(hl + 6);
            for (size_t i = 0; i < count; i++) {
                const size_t e = hl + 12 + 16 * i;
                if (e + 16 > n) break;
                const size_t s = u32(e + 8), t = u32(e + 12);
                if (doff + t > n || s > t) continue;
                walk(b + doff + s, t - s, depth + 1);
            }
            return;
        }
        sharcfb(b, n);
    }

    void file(const std::string& path) {
        const double t0 = now();
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return;
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> data(size > 0 ? size_t(size) : 0);
        const bool ok = !data.empty() && fread(data.data(), 1, data.size(), f) == data.size();
        fclose(f);
        st.readSeconds += now() - t0;
        if (!ok) return;
        st.files++;
        st.bytesRead += data.size();
        walk(data.data(), data.size(), 0);
    }

    void dir(const std::string& d) {
        DIR* h = opendir(d.c_str());
        if (!h) return;
        std::vector<std::string> subdirs, files;
        while (dirent* e = readdir(h)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            const std::string p = d + "/" + name;
            struct stat s;
            if (stat(p.c_str(), &s) != 0) continue;
            if (S_ISDIR(s.st_mode)) subdirs.push_back(p);
            else {
                const size_t dot = name.rfind('.');
                const std::string ext = dot == std::string::npos ? "" : name.substr(dot);
                if (ext == ".szs" || ext == ".pack" || ext == ".sarc" || ext == ".sharcfb") files.push_back(p);
            }
        }
        closedir(h);
        for (const std::string& f : files) file(f);
        for (const std::string& s : subdirs) dir(s);
    }
};

}  // namespace

std::vector<Program> scan(const std::string& game_dir, Stats& stats) {
    stats = {};
    const double t0 = now();
    Scanner s{stats, {}, {}};
    s.dir(game_dir + "/content");
    stats.distinct = s.out.size();
    stats.totalSeconds = now() - t0;
    return std::move(s.out);
}

}  // namespace shader_scan
