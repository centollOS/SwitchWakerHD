// Synthetic controls/settings-style I/O. No game files, window or GPU required.
#include "platform/host.h"
#include <cassert>
#include <clocale>
#include <fstream>
#include <chrono>
int main(int argc, char** argv) {
    const auto codePage = GetACP();
    if (codePage != CP_UTF8) std::fprintf(stderr, "Expected manifest ANSI code page 65001, got %lu\n", static_cast<unsigned long>(codePage));
    assert(codePage == CP_UTF8); // Embedded activeCodePage manifest is effective.
    assert(std::string(std::localeconv()->decimal_point) == ".");
    if (argc == 2 && std::string(argv[1]) == "--print-root") {
        std::printf("%s/shader-prebuild\n", host::config_dir().c_str());
        return 0;
    }
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / fs::u8path("wwhd-J\xc3\xbcrgen-\xe9\x9b\xaa-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path);
    _wputenv_s(L"WWHD_USER_DIR", path.c_str());
    const auto config = host::config_dir();
    assert(fs::u8path(config) == path);
    const auto controls = config + "/controls.json";
    { std::ofstream out(controls); out << "synthetic controls"; assert(out.good()); }
    { std::ifstream in(controls); std::string text; std::getline(in, text); assert(text == "synthetic controls"); }
    const auto settings = config + "/settings.json";
    FILE* out = std::fopen(settings.c_str(), "wb"); assert(out);
    assert(std::fputs("synthetic settings", out) >= 0); assert(std::fclose(out) == 0);
    { std::ifstream in{fs::path(settings)}; std::string text; std::getline(in, text); assert(text == "synthetic settings"); }
    assert(fs::is_regular_file(path / "controls.json"));
    assert(fs::is_regular_file(path / "settings.json"));
    std::printf("[log] UTF-8 paths: J\xc3\xbcrgen \xe9\x9b\xaa\n");
    fs::remove_all(path);
}
