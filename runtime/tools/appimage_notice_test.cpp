// platform/appimage_notice.h: the v0.2.13 "<AppImage>.data" hint (issue #55), shown by the game and the
// setup window. Without such a folder (or outside an AppImage) there is nothing to say.
#include "platform/appimage_notice.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

int main() {
#ifndef _WIN32
    const fs::path dir = fs::temp_directory_path() / ("wwhd-appimage-notice-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string image = (dir / "Wind Waker [HD].AppImage").string();
    assert(appimage::legacy_data_notice(nullptr).empty());
    assert(appimage::legacy_data_notice("").empty());
    assert(appimage::legacy_data_notice(image.c_str()).empty());   // no .data folder
    { FILE* f = fopen((image + ".data").c_str(), "w"); assert(f); fclose(f); }
    assert(appimage::legacy_data_notice(image.c_str()).empty());   // a file of that name is not the folder
    fs::remove(image + ".data");
    fs::create_directories(image + ".data");
    const std::string want = "Found Wind Waker [HD].AppImage.data: this folder is no longer used. Move its contents "
                             "to Wind Waker [HD].AppImage.home (see the changelog) to keep your saves and settings.";
    assert(appimage::legacy_data_notice(image.c_str()) == want);
    fs::remove_all(dir);
#endif
    std::puts("appimage notice: ok");
    return 0;
}
