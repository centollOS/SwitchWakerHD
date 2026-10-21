// The extended item icon archive (runtime/src/mods/item_icons.cpp) for tools/story_mods/test_lyt.py, which
// compares its output byte for byte with tools/story_mods/lyt/item_icons.py:
//   item_icons_test PACK_IN PACK_OUT ICON.rgba...   extend_pack (each icon: 128 x 128 RGBA bytes)
// Without arguments: the builder's own checks (refusals, the Yaz0 reader).
#include "mods/item_icons.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

void log_msg(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}

static item_icons::Bytes read(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

template <class F>
static bool refused(F f) {
    try {
        f();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

int main(int argc, char** argv) {
    using item_icons::Bytes;
    if (argc >= 3) {
        std::vector<Bytes> icons;
        for (int i = 3; i < argc; i++) icons.push_back(read(argv[i]));
        try {
            const Bytes out = item_icons::extend_pack(read(argv[1]), icons);
            std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
        } catch (const std::exception& e) {
            fprintf(stderr, "item_icons_test: %s\n", e.what());
            return 1;
        }
        return 0;
    }
    // Yaz0: a literal run, then a back reference (3 bytes from 3 back) and a long one (0x12 + 2)
    const Bytes yaz{'Y', 'a', 'z', '0', 0, 0, 0, 26, 0, 0, 0, 0, 0, 0, 0, 0, 0xE0, 'a', 'b', 'c', 0x10, 0x02, 0x00, 0x02, 0x02};
    const Bytes plain = item_icons::yaz0_decode(yaz);
    assert(std::string(plain.begin(), plain.end()) == "abcabcabcabcabcabcabcabcab");
    assert(refused([] { item_icons::yaz0_decode(Bytes{'Y', 'a', 'z', '0', 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x10}); }));
    assert(refused([] { item_icons::read_sarc(Bytes(64, 0)); }));
    assert(refused([] { item_icons::glow(Bytes(16, 0)); }));
    // a transparent icon glows nowhere; an opaque one everywhere but at the softened edge
    const Bytes none = item_icons::glow(Bytes(128 * 128 * 4, 0));
    for (size_t i = 0; i < none.size(); i += 4) assert(none[i] == 255 && none[i + 3] == 0);
    const Bytes full = item_icons::glow(Bytes(128 * 128 * 4, 255));
    assert(full[(64 * 128 + 64) * 4 + 3] == 255 && full[3] < 255);
    puts("item_icons_test: ok");
    return 0;
}
