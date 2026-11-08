// SPDX-License-Identifier: MPL-2.0
// Low memory (Android): the app asks the renderer to free what it can make again, so the system is less likely to
// end the game. MemoryPressure.java sends a level from onTrimMemory and from its own check of the system's free
// memory (Android 14 and newer no longer tell a running app that memory runs low); any thread may request, the
// render thread takes the request once per frame (gfx/vulkan backend.cpp, trim_memory) and logs what it freed.
//
// Light: what is cheap to make again and not needed now (textures unused for a while, compiled pipelines no draw has
// taken yet, dynamic resolution's images of other levels, free heap pages). Full: also every texture not drawn with
// in the last few frames.
#pragma once
#include <atomic>

namespace memory_trim {

enum Level : int { kNone = 0, kLight = 1, kFull = 2 };

inline std::atomic<int> g_requested{kNone};
inline std::atomic<int> g_source{0};  // the Android level (or other code) of the strongest request pending, for the log

// any thread: ask for a trim of at least `level`; requests before the next frame merge into the strongest
inline void request(Level level, int source) {
    int current = g_requested.load(std::memory_order_relaxed);
    while (level > current && !g_requested.compare_exchange_weak(current, level, std::memory_order_acq_rel)) {}
    if (level >= current) g_source.store(source, std::memory_order_relaxed);
}

// render thread, once per frame: the pending level (kNone: nothing to do), cleared
inline Level take(int& source) {
    const int level = g_requested.exchange(kNone, std::memory_order_acq_rel);
    source = g_source.load(std::memory_order_relaxed);
    return Level(level);
}

// Frames a texture must have gone unused for a light trim to free it, and for a full one (a full trim keeps only
// what the last few frames drew; the texture budget's pin is 3 frames).
constexpr unsigned kLightIdleFrames = 600;
constexpr unsigned kFullIdleFrames = 3;

}  // namespace memory_trim
