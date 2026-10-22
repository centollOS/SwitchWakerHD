#pragma once
#include <cstdlib>
#include <string_view>

namespace gaming_mode {
enum class Platform { Linux, Windows, Other };
struct Environment {
    const char* desktop = nullptr;
    const char* wayland = nullptr;
    const char* gamescope_wayland = nullptr;
    const char* wine_host_desktop = nullptr;
};
// Proton inherits the Unix environment. Wine retains GAMESCOPE_WAYLAND_DISPLAY;
// newer Wine prefixes XDG_* with WINE_HOST_ and filters WAYLAND_DISPLAY.
// Hardware, Big Picture and compatibility-prefix flags do not identify a compositor.
inline const char* detect(Platform platform, Environment env) {
    if (platform == Platform::Other) return nullptr;
    auto contains = [](const char* v) {
        return v && std::string_view(v).find("gamescope") != std::string_view::npos;
    };
    if (contains(env.desktop)) return "XDG_CURRENT_DESKTOP";
    if (platform == Platform::Windows && contains(env.wine_host_desktop)) return "WINE_HOST_XDG_CURRENT_DESKTOP";
    if (contains(env.wayland)) return "WAYLAND_DISPLAY";
    if (env.gamescope_wayland && *env.gamescope_wayland) return "GAMESCOPE_WAYLAND_DISPLAY";
    return nullptr;
}
inline const char* reason() {
#if defined(__ANDROID__)
    constexpr auto platform = Platform::Other;
#elif defined(__linux__) || defined(WWHD_TEST_GAMESCOPE)
    constexpr auto platform = Platform::Linux;
#elif defined(_WIN32)
    constexpr auto platform = Platform::Windows;
#else
    constexpr auto platform = Platform::Other;
#endif
    return detect(platform, {std::getenv("XDG_CURRENT_DESKTOP"), std::getenv("WAYLAND_DISPLAY"),
                             std::getenv("GAMESCOPE_WAYLAND_DISPLAY"), std::getenv("WINE_HOST_XDG_CURRENT_DESKTOP")});
}
inline bool active() { return reason() != nullptr; }
// Mode indices are the display_modes.h order: Window=0, PiP=1.
struct Defaults { int mode; bool fullscreen; };
inline Defaults defaults(bool gaming, int mode, bool mode_chosen, bool fullscreen,
                         bool fullscreen_chosen) {
    return {gaming && !mode_chosen ? 1 : mode,
            gaming && !fullscreen_chosen ? true : fullscreen};
}
}
