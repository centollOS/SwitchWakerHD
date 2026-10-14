#pragma once
#include <atomic>
#include <cstdlib>
#include <string_view>

struct ImDrawData;

namespace overlay::window_target {
enum class Window { None, TV, GamePad };
// Published by the SDL main thread; consumed by input transitions and the render thread.
inline std::atomic<Window> selected{Window::TV};
inline bool gamescope_session() {
#if !defined(__linux__) && !defined(WWHD_TEST_GAMESCOPE)
    return false;
#else
    auto contains = [](const char* name) {
        const char* value = std::getenv(name);
        return value && std::string_view(value).find("gamescope") != std::string_view::npos;
    };
    // SteamDeck=1 alone also occurs on the desktop: it must not change desktop behavior.
    return contains("XDG_CURRENT_DESKTOP") || contains("WAYLAND_DISPLAY") ||
           std::getenv("GAMESCOPE_WAYLAND_DISPLAY") != nullptr;
#endif
}
inline Window choose(bool follow_focus, bool two_windows, Window focus, bool tv_visible, bool drc_visible) {
    if (!follow_focus || !two_windows) return Window::TV;
    if (focus == Window::TV && tv_visible) return Window::TV;
    if (focus == Window::GamePad && drc_visible) return Window::GamePad;
    return Window::None;
}
// Shared by live presentation and present dumps: the menu and toast use the same draw data.
inline bool draws_into(Window target, bool gamepad) {
    return target == (gamepad ? Window::GamePad : Window::TV);
}
struct DrawFrame {
    Window target = Window::TV;
    ImDrawData* draw = nullptr;
    ImDrawData* for_window(bool gamepad) const { return draws_into(target, gamepad) ? draw : nullptr; }
    // Stereo is TV-only, even when menu/toast draw data belongs to GamePad this frame.
    bool stereo_packed(bool enabled, bool right_eye) const { return target == Window::TV && enabled && right_eye; }
};
struct Destination {
    float width, height;
    bool drawable;
};
// The Vulkan swap path and the SDL integration test use this same frame builder. A frame
// is built exactly once at its destination's size; menu and toast share its draw data.
template<class Render, class Close>
DrawFrame build_frame(Window requested, bool separate, bool require_drawable,
                      Destination tv, Destination drc, Render render, Close close) {
    if (requested == Window::GamePad && !separate) requested = Window::None;
    const Destination destination = requested == Window::GamePad ? drc : tv;
    if (requested == Window::None || (require_drawable &&
        (!destination.drawable || destination.width < 16 || destination.height < 16))) {
        close();
        return {Window::None, nullptr};
    }
    ImDrawData* draw = render(destination.width, destination.height);
    if (require_drawable && !draw) {
        close();
        return {Window::None, nullptr};
    }
    return {requested, draw};
}
}
