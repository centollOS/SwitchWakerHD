// SwitchWakerHD (this fork): "a new version is out". At start a background thread asks GitHub for the latest
// SwitchWakerHD release (HTTPS through libnx's ssl service, the console's own CA store; no curl) and compares its
// tag with build::version(); a newer one shows a line at the bottom of the picture for a few seconds (the frame
// layer's notice, overlay_switch.cpp) and in the Dev tab. Nothing is downloaded: each player builds the game
// from their own dump, so the line says to update with the installer (INSTALL.md, "Updating").
//
// Dev tab > "Check for updates at start" (settings.ini switchUpdateCheck, on by default) turns it off. A
// development build ("dev", no v* tag) never checks. [dev] WWHD_UPDATE_VERSION=v0.4.0 pretends an older build
// (tests).
#pragma once
#include <string>

namespace update_notice {
enum class State { kIdle, kOff, kDevBuild, kChecking, kUpToDate, kNewer, kFailed };
struct Info {
    State state = State::kIdle;
    std::string current;  // the version compared with
    std::string latest;   // the release's tag
    std::string title;    // the release's name
    std::string detail;   // why a check failed (English, for the log and the Dev tab)
};
constexpr const char* kKeyUpdateCheck = "switchUpdateCheck";  // "0" off; anything else (or none) on

void start();  // main_switch::start(), once: the check, when on
Info info();
bool check_enabled();
void set_check_enabled(bool on);  // saves it; checks now when turned on
// the frame layer (render thread, while the menu is closed): the notice line, for kShowSeconds from the first
// call that can show it (the caller leaves out the first-start card and Prepare graphics). False: none now.
bool notice(std::string& line);
constexpr double kShowSeconds = 12.0;
}  // namespace update_notice
