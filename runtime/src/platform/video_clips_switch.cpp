// (this fork, #38) Video clips (video_clips_switch.h).
#include "video_clips_switch.h"

#include <switch.h>

#include <atomic>
#include <string>

#include "../overlay/hostui.h"
#include "../runtime.h"

namespace video_clips {
namespace {

// the forwarder's title (tools/switch/forwarder/forwarder.env TITLE_ID), whose NACP has VideoCapture = Manual
constexpr u64 kForwarderTitle = 0x01FF575748440000ull;

std::atomic<bool> g_enabled{false}, g_recording{false}, g_reserved{false}, g_failed{false};

Availability find_availability() {
    const AppletType type = appletGetAppletType();
    if (type != AppletType_Application && type != AppletType_SystemApplication) return Availability::kNotApplication;
    u64 title = 0;
    if (R_FAILED(svcGetInfo(&title, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) || title != kForwarderTitle)
        return Availability::kOtherTitle;
    bool supported = false;
    if (R_FAILED(appletIsGamePlayRecordingSupported(&supported)) || !supported) return Availability::kUnsupported;
    return Availability::kAvailable;
}

const char* availability_name(Availability a) {
    switch (a) {
    case Availability::kAvailable: return "available";
    case Availability::kNotApplication: return "not available: started as an applet (Homebrew Menu)";
    case Availability::kOtherTitle: return "left to the title the game was started through";
    case Availability::kUnsupported: return "not supported by the system";
    }
    return "?";
}

// the buffer once, then recording on. Never off while the game runs: a start that initialized recording after an
// earlier process had switched it off hung inside appletInitializeGamePlayRecording until the console was reset
// (2026-11-01). Turning the option off is saved and applies at the next start (no buffer, no recording).
void start_recording(const char* why) {
    if (availability() != Availability::kAvailable || g_recording) return;
    if (!g_reserved) {
        const size_t before = heap_never_used_mib();
        const Result rc = appletInitializeGamePlayRecording();
        if (R_FAILED(rc)) {
            g_failed = true;
            LOG("[clips] could not give the system the %u MiB video buffer (%s): rc 0x%x; heap never used %zu MiB",
                kBufferMiB, why, (unsigned)rc, before);
            return;
        }
        g_reserved = true;
        LOG("[clips] %u MiB video buffer given to the system (%s): heap never used %zu -> %zu MiB", kBufferMiB, why,
            before, heap_never_used_mib());
    }
    const Result rc = appletSetGamePlayRecordingState(true);
    g_recording = R_SUCCEEDED(rc);
    LOG("[clips] recording on (%s): rc 0x%x", why, (unsigned)rc);
}

}  // namespace

Availability availability() {
    static const Availability a = find_availability();
    return a;
}
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }
bool recording() { return g_recording.load(std::memory_order_relaxed); }
bool reserved() { return g_reserved.load(std::memory_order_relaxed); }
bool failed() { return g_failed.load(std::memory_order_relaxed); }

void apply_at_start() {
    std::string v;
    g_enabled = hostui::get(kKeyVideoClips, v) && v == "1";
    LOG("[clips] video clips: %s, %s", g_enabled ? "on" : "off (the default)", availability_name(availability()));
    if (g_enabled) start_recording("saved option");
}

void set_enabled(bool on) {
    g_enabled = on;
    hostui::set(kKeyVideoClips, on ? "1" : "0");
    if (on) start_recording("Picture tab");
    else if (g_recording) LOG("[clips] option off (Picture tab): recording stays on until the game restarts");
}

}  // namespace video_clips
