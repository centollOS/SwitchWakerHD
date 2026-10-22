// The port's layout writer (runtime/src/mods/lyt_build.cpp) for tools/story_mods/test_lyt.py, which
// compares its files byte for byte with the reference generator's:
//   lyt_build_test DESCRIPTION.json OUT_DIR      writes OUT_DIR/<file name with / as __> and archive.szs
// Without arguments: the writer's own checks (refusals).
#include "mods/lyt_build.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
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

static bool refused(const std::string& json) {
    try {
        lyt_build::build_files(json, ".");
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    if (argc == 3) {
        std::ifstream f(argv[1], std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(f), {}};
        const fs::path art = fs::path(argv[1]).parent_path();
        try {
            for (const auto& [name, bytes] : lyt_build::build_files(text, art)) {
                std::string flat = name;
                for (char& c : flat)
                    if (c == '/') c = '_';
                std::ofstream o(fs::path(argv[2]) / flat, std::ios::binary);
                o.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            }
            const auto szs = lyt_build::build(text, art);
            std::ofstream o(fs::path(argv[2]) / "archive.szs", std::ios::binary);
            o.write(reinterpret_cast<const char*>(szs.data()), std::streamsize(szs.size()));
        } catch (const std::exception& e) {
            fprintf(stderr, "refused: %s\n", e.what());
            return 3;
        }
        return 0;
    }
    // refusals: formats the port does not write (bc4/bc5), resizing, unknown pane types, a track outside the animation's groups
    const std::string base = R"({"name": "T", "textures": {"A": {"fill": {"color": [1, 2, 3]}, "width": 64, "height": 4%s}}})";
    char buf[512];
    snprintf(buf, sizeof buf, base.c_str(), "");
    assert(!refused(buf));
    snprintf(buf, sizeof buf, base.c_str(), R"(, "format": "bc5la")");
    assert(refused(buf));
    assert(refused(R"({"name": "T", "panes": [{"type": "sprite", "name": "A", "size": [1, 1]}]})"));
    assert(refused(R"({"name": "T", "panes": [{"name": "A", "size": [1, 1]}, {"name": "B", "size": [1, 1]}], "groups": {"G": ["A"]},
                     "animations": {"X": {"frames": 2, "groups": ["G"], "tracks": [{"pane": "B", "target": "alpha", "keys": [[0, 0]]}]}}})"));
    assert(refused(R"({"name": "T", "panes": [{"name": "A", "size": [1, 1]}], "groups": {"G": ["Missing"]}})"));
    assert(lyt_build::layout_name(R"({"name": "StoryLyt_00"})") == "StoryLyt_00");
    // an archive above the safe size is refused: 4 RGBA8 fill textures of 1024 x 1024 (16 MB)
    std::string big = R"({"name": "T", "textures": {)";
    for (int i = 0; i < 4; i++)
        big += std::string(i ? "," : "") + "\"F" + std::to_string(i) + R"(": {"fill": {"color": [1, 2, 3]}, "width": 1024, "height": 1024})";
    big += "}}";
    bool too_big = false;
    try {
        lyt_build::build(big, ".");
    } catch (const std::runtime_error&) {
        too_big = true;
    }
    assert(too_big);
    printf("lyt_build_test: ok\n");
    return 0;
}
