// SwitchWakerHD (this fork, issue #121): see game_layouts_fork.h.
#include "game_layouts_fork.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <stdexcept>

namespace game_layouts::fork {
namespace {
uint32_t be16(const Bytes& d, size_t o) { return o + 2 <= d.size() ? uint32_t(d[o]) << 8 | d[o + 1] : 0; }
uint32_t be32(const Bytes& d, size_t o) {
    return o + 4 <= d.size() ? uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3] : 0;
}
std::string cstr(const Bytes& d, size_t o) {
    std::string s;
    for (size_t i = o; i < d.size() && d[i]; i++) s += char(d[i]);
    return s;
}

struct PackEntry { uint64_t offset = 0; uint32_t size = 0; };
std::string g_index_path;                 // the pack g_index belongs to
std::map<std::string, PackEntry> g_index;
struct Picture { std::string spec; Image image; };
std::list<Picture> g_pictures;            // most recently used first
size_t g_picture_bytes = 0;
constexpr size_t kMaxPictureBytes = 4u << 20;
}  // namespace

Image upscale(const Image& in, uint32_t factor) {
    if (factor < 2 || factor > 4 || !in.width || !in.height) return in;
    const uint32_t w = in.width * factor, h = in.height * factor;
    if (w > 2048 || h > 2048) return in;
    auto lanczos = [](double x) {
        x = std::fabs(x);
        if (x < 1e-9) return 1.0;
        if (x >= 3) return 0.0;
        const double px = 3.14159265358979323846 * x;
        return 3 * std::sin(px) * std::sin(px / 3) / (px * px);
    };
    // the six Lanczos-3 taps depend only on an output texel's phase (i % factor): computed once per phase and
    // normalised, instead of two sin() per tap and texel
    struct Taps { int first; float wgt[6]; };
    Taps taps[4];
    for (uint32_t ph = 0; ph < factor; ph++) {
        const double off = (ph + 0.5) / factor - 0.5;  // the centre's offset from the source texel i / factor
        const int base = int(std::floor(off)) - 2;
        double wgt[6], sum = 0;
        for (int j = 0; j < 6; j++) sum += wgt[j] = lanczos(off - (base + j));
        taps[ph].first = base;
        for (int j = 0; j < 6; j++) taps[ph].wgt[j] = float(wgt[j] / sum);
    }
    // premultiplied, linear floats; edges clamped
    std::vector<float> src(size_t(in.width) * in.height * 4);
    for (size_t i = 0; i < src.size(); i += 4) {
        const float a = in.rgba[i + 3] / 255.0f;
        for (int c = 0; c < 3; c++) src[i + c] = in.rgba[i + c] / 255.0f * a;
        src[i + 3] = a;
    }
    auto pass = [&](const std::vector<float>& from, uint32_t fw, uint32_t fh, bool horizontal) {
        const uint32_t tw = horizontal ? fw * factor : fw, th = horizontal ? fh : fh * factor;
        const int n = int(horizontal ? fw : fh);
        std::vector<float> to(size_t(tw) * th * 4, 0.0f);
        const uint32_t outer = horizontal ? th : tw, inner = horizontal ? tw : th;
        for (uint32_t i = 0; i < inner; i++) {
            const Taps& t = taps[i % factor];
            const int first = int(i / factor) + t.first;
            size_t at[6];
            for (int j = 0; j < 6; j++) {
                const size_t kk = size_t(std::clamp(first + j, 0, n - 1));
                at[j] = horizontal ? kk * 4 : kk * fw * 4;
            }
            for (uint32_t o = 0; o < outer; o++) {
                const size_t line = horizontal ? size_t(o) * fw * 4 : size_t(o) * 4;
                float acc[4] = {0, 0, 0, 0};
                for (int j = 0; j < 6; j++)
                    for (int c = 0; c < 4; c++) acc[c] += t.wgt[j] * from[line + at[j] + c];
                const size_t out = horizontal ? (size_t(o) * tw + i) * 4 : (size_t(i) * tw + o) * 4;
                for (int c = 0; c < 4; c++) to[out + c] = acc[c];
            }
        }
        return to;
    };
    const std::vector<float> wide = pass(src, in.width, in.height, true);
    const std::vector<float> full = pass(wide, w, in.height, false);
    // straight colour again, then sharpened as an enlargement needs: an unsharp mask on the colour (blur:
    // two box passes of the factor's radius, amount 1.2) and a steeper alpha ramp (x1.8 about one half) for
    // crisp outlines, as the soft source edges would otherwise stay blurred at the larger size
    std::vector<float> rgb(size_t(w) * h * 3), alpha(size_t(w) * h);
    for (size_t p = 0; p < alpha.size(); p++) {
        const float a = std::clamp(full[p * 4 + 3], 0.0f, 1.0f);
        alpha[p] = a;
        for (int c = 0; c < 3; c++) rgb[p * 3 + c] = a > 1e-4f ? std::clamp(full[p * 4 + c] / a, 0.0f, 1.0f) : 0.0f;
    }
    // the blur is alpha-weighted (premultiplied colour and alpha blurred, then divided), so invisible
    // texels never pull the colour at an outline
    std::vector<float> blur(size_t(w) * h * 4), tmp(blur.size());
    for (size_t p = 0; p < alpha.size(); p++) {
        for (int c = 0; c < 3; c++) blur[p * 4 + c] = rgb[p * 3 + c] * alpha[p];
        blur[p * 4 + 3] = alpha[p];
    }
    const int r = int(factor);
    const float inv = 1.0f / float(2 * r + 1);
    // a box of 2r+1 texels with clamped edges, as a running sum along one line (stride: texels between neighbours)
    auto box = [&](const float* from, float* to, int count, size_t stride) {
        for (int c = 0; c < 4; c++) {
            float sum = 0;
            for (int k = -r; k <= r; k++) sum += from[size_t(std::clamp(k, 0, count - 1)) * stride * 4 + c];
            for (int x = 0; x < count; x++) {
                to[size_t(x) * stride * 4 + c] = sum * inv;
                sum += from[size_t(std::min(x + r + 1, count - 1)) * stride * 4 + c] -
                       from[size_t(std::max(x - r, 0)) * stride * 4 + c];
            }
        }
    };
    for (int round = 0; round < 2; round++) {
        for (uint32_t y = 0; y < h; y++) box(&blur[size_t(y) * w * 4], &tmp[size_t(y) * w * 4], int(w), 1);
        for (uint32_t x = 0; x < w; x++) box(&tmp[size_t(x) * 4], &blur[size_t(x) * 4], int(h), w);
    }
    Image out;
    out.width = w, out.height = h, out.srgb = in.srgb;
    out.rgba.resize(size_t(w) * h * 4);
    for (size_t p = 0; p < alpha.size(); p++) {
        for (int c = 0; c < 3; c++) {
            const float b = blur[p * 4 + 3] > 1e-4f ? blur[p * 4 + c] / blur[p * 4 + 3] : rgb[p * 3 + c];
            const float v = rgb[p * 3 + c] + 1.2f * (rgb[p * 3 + c] - b);
            out.rgba[p * 4 + c] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        }
        out.rgba[p * 4 + 3] = uint8_t(std::lround(std::clamp((alpha[p] - 0.5f) * 1.8f + 0.5f, 0.0f, 1.0f) * 255.0f));
    }
    return out;
}

bool pack_member(const std::string& pack_path, const std::string& name, Bytes& out, std::string& error) {
    std::ifstream f(pack_path, std::ios::binary);
    if (!f) {
        error = "can't read the game's 2D pack";
        return false;
    }
    auto read_at = [&f](uint64_t offset, size_t size, Bytes& data) {
        data.assign(size, 0);
        f.seekg(std::streamoff(offset));
        return !size || bool(f.read(reinterpret_cast<char*>(data.data()), std::streamsize(size)));
    };
    if (g_index_path != pack_path) {
        // SARC (big-endian): header, SFAT (16 bytes an entry), SFNT names; the members' data starts at the header's
        // data offset, so everything before it is the index
        Bytes head, d;
        if (!read_at(0, 0x14, head) || std::memcmp(head.data(), "SARC", 4)) {
            error = "the game's 2D pack is not a SARC";
            return false;
        }
        const uint32_t start = be32(head, 12);
        if (start < 0x20 || start > (16u << 20) || !read_at(0, start, d)) {
            error = "bad SARC header";
            return false;
        }
        const uint32_t hs = be16(d, 4), n = be16(d, hs + 6);
        const size_t names = hs + 12 + 16 * size_t(n) + 8;
        if (size_t(hs) + 12 > d.size() || names > d.size()) {
            error = "bad SARC table";
            return false;
        }
        std::map<std::string, PackEntry> index;
        for (uint32_t i = 0; i < n; i++) {
            const size_t e = hs + 12 + 16 * size_t(i);
            const uint32_t attr = be32(d, e + 4), b = be32(d, e + 8), en = be32(d, e + 12);
            const size_t name_at = names + size_t(attr & 0xFFFF) * 4;
            if (en < b || name_at >= d.size()) {
                error = "bad SARC entry";
                return false;
            }
            index[cstr(d, name_at)] = {uint64_t(start) + b, en - b};
        }
        g_index = std::move(index);
        g_index_path = pack_path;
    }
    const auto it = g_index.find(name);
    if (it == g_index.end()) {
        error = "no layout " + name.substr(0, name.size() > 4 ? name.size() - 4 : name.size());
        return false;
    }
    if (!read_at(it->second.offset, it->second.size, out)) {
        error = "can't read the game's 2D pack";
        return false;
    }
    return true;
}

bool cached(const std::string& spec, Image& out) {
    for (auto p = g_pictures.begin(); p != g_pictures.end(); ++p)
        if (p->spec == spec) {
            g_pictures.splice(g_pictures.begin(), g_pictures, p);
            out = p->image;
            return true;
        }
    return false;
}

void remember(const std::string& spec, const Image& image) {
    if (image.rgba.size() > kMaxPictureBytes / 4) return;
    g_pictures.push_front({spec, image});
    g_picture_bytes += image.rgba.size();
    while (g_picture_bytes > kMaxPictureBytes) {
        g_picture_bytes -= g_pictures.back().image.rgba.size();
        g_pictures.pop_back();
    }
}

void clear() {
    g_index.clear(), g_index_path.clear();
    g_pictures.clear(), g_picture_bytes = 0;
}

}  // namespace game_layouts::fork
