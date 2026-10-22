#include "platform/gaming_mode.h"
#include "gfx/display_modes.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
void log_msg(const char*, ...) {}
namespace input { void set_touch(bool, float, float) {} }
namespace aspect { void set_window_aspect(float) {} }
#ifdef _WIN32
static void setenv(const char* name, const char* value, int) {
    _putenv_s(name, value);
}
static void unsetenv(const char* name) { _putenv_s(name, ""); }
#endif
int main() {
    using namespace gaming_mode;
    for (auto platform : {Platform::Linux, Platform::Windows}) {
        assert(!detect(platform, {}));
        assert(detect(platform, {"gamescope", nullptr, nullptr}));
        assert(detect(platform, {"steam:gamescope", nullptr, nullptr}));
        assert(detect(platform, {nullptr, "gamescope-0", nullptr}));
        assert(detect(platform, {nullptr, nullptr, "gamescope-0"}));
        assert(!detect(platform, {"KDE", "wayland-0", ""}));
    }
    assert(detect(Platform::Windows, {nullptr, nullptr, nullptr, "gamescope"}));
    assert(!detect(Platform::Linux, {nullptr, nullptr, nullptr, "gamescope"}));
    assert(!detect(Platform::Other, {"gamescope", "gamescope-0", "gamescope-0"}));
    // Steam hardware/Big Picture/compatibility flags alone are deliberately ignored.
    unsetenv("XDG_CURRENT_DESKTOP"); unsetenv("WAYLAND_DISPLAY"); unsetenv("GAMESCOPE_WAYLAND_DISPLAY");
    unsetenv("WINE_HOST_XDG_CURRENT_DESKTOP");
    setenv("SteamDeck", "1", 1);
    assert(!active());
    setenv("SteamGamepadUI", "1", 1);
    setenv("STEAM_COMPAT_DATA_PATH", "/prefix", 1);
    setenv("SteamOS", "1", 1);
    unsetenv("XDG_CURRENT_DESKTOP"); unsetenv("WAYLAND_DISPLAY"); unsetenv("GAMESCOPE_WAYLAND_DISPLAY");
    assert(!active());
    auto first = defaults(true, gfx::kDrcWindow, false, false, false);
    assert(first.mode == gfx::kDrcPip && first.fullscreen);
    auto saved = defaults(true, gfx::kDrcWindow, true, false, true);
    assert(saved.mode == gfx::kDrcWindow && !saved.fullscreen);
    auto desktop = defaults(false, gfx::kDrcWindow, false, false, false);
    assert(desktop.mode == gfx::kDrcWindow && !desktop.fullscreen);
    for (int mode = 0; mode < gfx::kDrcModeCount; ++mode) {
        auto chosen = defaults(true, mode, true, true, true);
        assert(chosen.mode == mode && chosen.fullscreen);
    }
    assert(defaults(true, gfx::kDrcWindow, true, false, false).fullscreen);
    gfx::g_mode = first.mode;
    setenv("WWHD_DRC_MODE", "window", 1);
    gfx::display_env_overrides();
    assert(gfx::g_mode == gfx::kDrcWindow);
    setenv("WWHD_DRC_MODE", "off", 1);
    gfx::display_env_overrides();
    assert(gfx::g_mode == gfx::kDrcOff);
    setenv("WWHD_FULLSCREEN", "0", 1);
    assert(!gfx::display_start_fullscreen(first.fullscreen, false));
    setenv("WWHD_FULLSCREEN", "1", 1);
    assert(gfx::display_start_fullscreen(false, false));
    // Hidden runs record the full-screen decision without taking over the screen.
    assert(!gfx::display_start_fullscreen(true, true));
    setenv("WWHD_TEST_DRC_MODE", "0:window,4:pip", 1);
    assert(gfx::display_test_mode(0) == gfx::kDrcWindow);
    assert(gfx::display_test_mode(4) == gfx::kDrcPip);
    unsetenv("WWHD_DRC_MODE");
    gfx::g_mode = first.mode; gfx::g_shown = true; gfx::g_has_drc_window = false;
    auto plan = gfx::display_plan(true, 1280, 720, true, 854, 480, 1280, 800, 5);
    assert(plan.pip_on && !plan.drc_window && plan.pip.w > 0);
}
