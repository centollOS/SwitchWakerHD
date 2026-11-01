#pragma once
// (this fork, #38) Video clips: holding the Capture button records the last 30 seconds. The forwarder's NACP
// allows video capture with VideoCapture = Manual (tools/switch/forwarder/make_nacp.py), so the software has to
// switch recording on itself: appletInitializeGamePlayRecording gives the system a 96 MiB buffer (0x6000000) out
// of our heap, kept until the program ends (libnx frees it at exit), then appletSetGamePlayRecordingState.
// Off by default (a Picture tab option, saved as switchVideoClips): the 96 MiB come out of the game's memory.
namespace video_clips {

enum class Availability {
    kAvailable,      // started from the SwitchWakerHD icon (the forwarder's title): the option works
    kNotApplication, // started from the Homebrew Menu as an applet: the system records no clips there
    kOtherTitle,     // started through another title (hbmenu on a game, "title takeover"): that game's own setting
    kUnsupported,    // the system says recording is not supported
};

Availability availability();
bool enabled();   // the option (what is saved)
bool recording(); // the system was asked to record and said yes
bool reserved();  // the 96 MiB are held (until the program ends)
bool failed();    // the system refused the buffer: see the log

void apply_at_start();     // settings_switch::apply_at_start: the saved option
void set_enabled(bool on); // the host loop's thread (hostui::post); saves it

constexpr const char* kKeyVideoClips = "switchVideoClips";
constexpr unsigned kBufferMiB = 96;  // libnx: 0x6000000 bytes

}  // namespace video_clips
