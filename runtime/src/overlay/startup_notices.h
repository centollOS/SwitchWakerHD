#pragma once
// One-time notices for the player from start-up code that runs before the overlay exists (the
// renderer's GPU fallback, gfx/vulkan/gpu_start_guard.h): shown over the game once, with the mods'
// start-up notice (mods_tab.h startup_notice).
#include <mutex>
#include <string>

namespace startup_notices {
inline std::mutex& mutex() { static std::mutex m; return m; }
inline std::string& lines() { static std::string s; return s; }
inline void add(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex());
    std::string& s = lines();
    s += (s.empty() ? "" : "\n") + line;
}
inline std::string text() {
    std::lock_guard<std::mutex> lock(mutex());
    return lines();
}
}  // namespace startup_notices
