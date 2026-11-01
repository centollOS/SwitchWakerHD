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

// the buffer once, then recording on or off
void apply(bool on, const char* why) {
    if (availability() != Availability::kAvailable) return;
    if (on && !g_reserved) {
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
    if (!g_reserved) return;
    const Result rc = appletSetGamePlayRecordingState(on);
    g_recording = on && R_SUCCEEDED(rc);
    LOG("[clips] recording %s (%s): rc 0x%x", on ? "on" : "off", why, (unsigned)rc);
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
    if (g_enabled) apply(true, "saved option");
}

void set_enabled(bool on) {
    g_enabled = on;
    hostui::set(kKeyVideoClips, on ? "1" : "0");
    apply(on, "Picture tab");
}

}  // namespace video_clips
