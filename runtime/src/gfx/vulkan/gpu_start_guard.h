#pragma once
// A GPU chosen in Graphics > GPU that cannot show the picture must not crash every start (issue #309:
// on a Linux hybrid laptop without PRIME render offload the NVIDIA GPU starts fine, but the window's
// surface, owned by the AMD GPU that drives the screen, is lost at the first image acquire).
//
// Before the first picture of a start with a GPU chosen by the setting (not automatic, not
// WWHD_VK_DEVICE), a lost surface (or a similar presentation error) is retried once with a new surface
// and swapchain. When that fails too, the setting goes back to automatic, is saved, and the game
// restarts (Windows, which cannot restart itself, ends after a message) and tells the player once.
//
// Persisted keys (settings.ini on the SDL host, display.plist on the AppKit host), next to vkDevice:
//   vkDeviceStarting  the chosen GPU of a start that has not shown a picture yet; cleared at the first
//                     presented frame. Still there at the next start with the same choice: that start
//                     ended before any picture (a crash in the driver, for example), so the choice is
//                     dropped instead of failing at every start, when the menu cannot be reached.
//   vkDeviceFailed    the chosen GPU that could not show the picture; the next start tells the player
//                     once and clears it.
// WWHD_VK_DEVICE_FAILED (environment, set only for the game's own relaunch): the same GPU, in case the
// settings file could not be written; a choice it names is not used at that start.
//
// Pure decisions, without Vulkan or SDL (runtime/tools/gpu_start_guard_test.cpp); backend.cpp applies them.
// WWHD_TEST_GPU_SURFACE_LOSS=n (test aid): the first n image acquires of a start with a chosen GPU
// report a lost surface (1: the retry succeeds; 2: the fallback runs).
#include <cstdint>
#include <string>

namespace gfxvk::gpu_guard {

inline constexpr const char* kStartingKey = "vkDeviceStarting";
inline constexpr const char* kFailedKey = "vkDeviceFailed";
inline constexpr const char* kRelaunchEnv = "WWHD_VK_DEVICE_FAILED";
inline constexpr const char* kTestLossEnv = "WWHD_TEST_GPU_SURFACE_LOSS";
inline constexpr const char* kNotice =
    "Your chosen GPU couldn't show the picture, so the game is using the automatic choice again.";
inline constexpr const char* kNoticeUnfinished =
    "Your chosen GPU didn't show a picture at the last start, so the game is using the automatic choice again.";

struct StartInput {
    std::string saved;           // vkDevice ("": automatic)
    std::string starting;        // vkDeviceStarting
    std::string failed;          // vkDeviceFailed
    std::string relaunchFailed;  // WWHD_VK_DEVICE_FAILED
};
struct StartPlan {
    std::string use;             // the GPU setting for this start ("": automatic)
    bool resetSetting = false;   // save vkDevice as automatic
    bool clearFailed = false;    // vkDeviceFailed -> ""
    bool clearStarting = false;  // vkDeviceStarting -> "" (unless this start writes it again)
    std::string notice;          // once, for the player ("": none)
    std::string reason;          // for the log
};

inline StartPlan plan_start(const StartInput& in) {
    StartPlan p;
    p.use = in.saved;
    if (!in.failed.empty()) {
        p.clearFailed = true;
        p.notice = kNotice;
        p.reason = "the chosen GPU (" + in.failed + ") could not show the picture at the last start";
    }
    if (!in.saved.empty() && in.relaunchFailed == in.saved) {
        p.use.clear();
        p.resetSetting = true;
        p.notice = kNotice;
        p.reason = "the chosen GPU (" + in.saved + ") could not show the picture before this restart";
    } else if (!in.saved.empty() && in.starting == in.saved) {
        p.use.clear();
        p.resetSetting = true;
        if (p.notice.empty()) p.notice = kNoticeUnfinished;
        p.reason = "the last start with the chosen GPU (" + in.saved + ") ended before it showed a picture";
    }
    p.clearStarting = !in.starting.empty();
    return p;
}

// One start's first-picture guard (render thread).
struct FirstPicture {
    bool armed = false;          // a GPU chosen by the setting, before its first picture
    bool retried[2] = {};        // TV, GamePad: the new surface was tried
    bool fallingBack = false;    // the fallback is under way
};
enum class FailureAction {
    Report,    // not the guard's: the error is reported as before
    Retry,     // recreate this screen's surface and swapchain once
    FallBack,  // automatic GPU, save, restart (once)
    Wait,      // the fallback is under way: present nothing
};
// `surfaceClass`: a lost surface, an incompatible display, a window in use or an initialization error;
// `presented`: pictures presented so far (all screens).
inline FailureAction on_failure(FirstPicture& g, int screen, bool surfaceClass, uint64_t presented) {
    if (g.fallingBack) return FailureAction::Wait;
    if (!g.armed || !surfaceClass || presented) return FailureAction::Report;
    if (screen < 0 || screen > 1) screen = 0;
    if (!g.retried[screen]) {
        g.retried[screen] = true;
        return FailureAction::Retry;
    }
    g.fallingBack = true;
    return FailureAction::FallBack;
}

}  // namespace gfxvk::gpu_guard
