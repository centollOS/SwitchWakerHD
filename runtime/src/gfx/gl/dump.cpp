// Debugging pictures of the OpenGL renderer: surfaces and framebuffers as PNG files.
#include <algorithm>
#include <cmath>
#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gl.h"
#include "runtime.h"

namespace gfxgl {
namespace {
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; i--) v.push_back(uint8_t(x >> (i * 8)));
}
void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    put32(out, (uint32_t)data.size());
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uLong crc = crc32(0, out.data() + 4, (uInt)(out.size() - 4));
    put32(out, (uint32_t)crc);
    fwrite(out.data(), 1, out.size(), f);
}
// rgba: rows top to bottom (flip: bottom to top)
void write_png(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba, bool flip) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (w * 4 + 1));
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        const uint8_t* row = rgba.data() + size_t(flip ? h - 1 - y : y) * w * 4;
        raw.insert(raw.end(), row, row + w * 4);
    }
    uLongf size = compressBound(raw.size());
    std::vector<uint8_t> z(size);
    compress2(z.data(), &size, raw.data(), raw.size(), 6);
    z.resize(size);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        LOG("[gl] cannot write %s", path.c_str());
        return;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, w);
    put32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    fclose(f);
    LOG("[gl] wrote %s (%dx%d)", path.c_str(), w, h);
}
bool read_surface(Surface* s, std::vector<uint8_t>& rgba) {
    flush_draws();
    if (!s || !s->tex || s->fmt.depth || s->fmt.compressed || s->fmt.kind != FormatInfo::FLOAT) return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, R.readFbo);
    attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s, 0, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    rgba.assign(size_t(s->width) * s->height * 4, 0);
    glReadPixels(0, 0, s->width, s->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, nullptr, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, R.drawFbo);
    return true;
}
}  // namespace

void dump_surface(Surface* s, const std::string& path, bool encodeSrgb) {
    std::vector<uint8_t> rgba;
    if (!read_surface(s, rgba)) return;
    if (encodeSrgb) {  // as the display shows it (see Renderer::tvSrgb)
        static uint8_t table[256];
        static bool built = false;
        if (!built) {
            for (int i = 0; i < 256; i++) {
                double c = i / 255.0;
                c = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
                table[i] = uint8_t(std::lround(std::clamp(c, 0.0, 1.0) * 255.0));
            }
            built = true;
        }
        for (size_t i = 0; i < rgba.size(); i++)
            if ((i & 3) != 3) rgba[i] = table[rgba[i]];
    }
    write_png(path, s->width, s->height, rgba, false);
}

void dump_framebuffer(GLuint fbo, int width, int height, const std::string& path) {
    flush_draws();
    std::vector<uint8_t> rgba(size_t(width) * height * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadBuffer(fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, R.drawFbo);
    write_png(path, width, height, rgba, true);
}

void framebuffer_mean(GLuint fbo, int width, int height, float out[4]) {
    flush_draws();
    std::vector<uint8_t> rgba(size_t(width) * height * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadBuffer(fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, R.drawFbo);
    uint64_t sum[4] = {};
    for (size_t i = 0; i < rgba.size(); i++) sum[i & 3] += rgba[i];
    for (int c = 0; c < 4; c++) out[c] = float(sum[c]) / (255.0f * float(rgba.size() / 4));
}

void surface_mean(Surface* s, float out[4]) {
    std::vector<uint8_t> rgba;
    if (!read_surface(s, rgba) || rgba.empty()) return;
    uint64_t sum[4] = {};
    for (size_t i = 0; i < rgba.size(); i++) sum[i & 3] += rgba[i];
    for (int c = 0; c < 4; c++) out[c] = float(sum[c]) / (255.0f * float(rgba.size() / 4));
}
}  // namespace gfxgl
