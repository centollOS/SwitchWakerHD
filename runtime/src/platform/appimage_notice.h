#pragma once
// v0.2.13's AppImage had its own portable folder "<image>.data" (issue #55). Later releases use the
// AppImage runtime's standard "<image>.home" instead and never read ".data" again; when one is still
// there, the game (main.cpp) and the setup window (tools/installer/gui/setup_gui.cpp) say so once per
// start. AppRun (tools/release/appimage.py) and setup.py print the same text.
// No paths come from ".data": this only builds the hint.
#include <string>
#include <sys/stat.h>

namespace appimage {
// `image` is $APPIMAGE (set by the AppImage runtime); "" when not an AppImage or no ".data" folder.
inline std::string legacy_data_notice(const char* image) {
#ifdef _WIN32
    (void)image;
    return std::string();  // AppImages are Linux only
#else
    if (!image || !*image) return std::string();
    const std::string data = std::string(image) + ".data";
    struct stat st;
    if (stat(data.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return std::string();
    std::string name = image;
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    return "Found " + name + ".data: this folder is no longer used. Move its contents to " + name +
           ".home (see the changelog) to keep your saves and settings.";
#endif
}
}  // namespace appimage
