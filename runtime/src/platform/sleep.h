#pragma once
#include <chrono>
#include <thread>
#if defined(_WIN32) && defined(WWHD_SDL_HOST)
#include <SDL3/SDL_timer.h>
#endif

namespace host {
inline void sleep_until(std::chrono::steady_clock::time_point deadline) {
#if defined(_WIN32) && defined(WWHD_SDL_HOST)
    // SDL's Windows backend uses high-resolution waitable timers. Keep the
    // guest's steady-clock deadline instead of rounding it to milliseconds.
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining > 0) SDL_DelayNS(static_cast<uint64_t>(remaining));
#else
    std::this_thread::sleep_until(deadline);
#endif
}
}
