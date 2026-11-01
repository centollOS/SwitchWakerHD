// SPDX-License-Identifier: MPL-2.0
// Save-state conveniences (savestate.cpp): the "Last session" state taken when the game is quit (and
// on Android when the app goes to the background), the "At start" choice, and Auto save every few
// minutes and on entering a new area. All three use portable states (a few KB, no game data) in their
// own slots, so they never touch the player's slots 1..5 or crash recovery's automatic full states
// (slots 101..103).
// Pure logic, so save_convenience_test checks it without the game.
// Idea from GreenNaugahyde/ZeldaWWHDRecompAndroid (MPL-2.0).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ss::convenience {

constexpr int kLastSessionSlot = 110;  // <states>/last_session.wwstate
constexpr int kAutoSaveBase = 110;     // Auto save n: slot 110 + n, <states>/autosave/autosave<n>.wwstate
constexpr int kAutoSaves = 3;
inline bool is_auto_save(int slot) { return slot > kAutoSaveBase && slot <= kAutoSaveBase + kAutoSaves; }
inline bool is_convenience(int slot) { return slot == kLastSessionSlot || is_auto_save(slot); }

// ---- At start ----
enum class Start { kIntro, kSlot, kLastSession };
struct StartChoice {
    Start what = Start::kIntro;  // default: the intro and title screen as usual
    int slot = 0;                // kSlot: 1..5
    bool operator==(const StartChoice&) const = default;
};
// "intro", "last", "slot1".."slot5"; anything else is the default
inline StartChoice parse_start(const std::string& s) {
    if (s == "last") return {Start::kLastSession, 0};
    if (s.size() == 5 && s.compare(0, 4, "slot") == 0 && s[4] >= '1' && s[4] <= '5') return {Start::kSlot, s[4] - '0'};
    return {};
}
inline std::string format_start(const StartChoice& c) {
    if (c.what == Start::kLastSession) return "last";
    if (c.what == Start::kSlot && c.slot >= 1 && c.slot <= 5) return "slot" + std::to_string(c.slot);
    return "intro";
}

// ---- Auto save ----
// On by default (like Breath of the Wild): a player who never opens the Saves tab still has
// something to load after a crash, and auto saves never touch cking.sav.
constexpr int kAutoSaveMinutes[] = {0, 2, 5, 10};  // 0: off
constexpr int kDefaultAutoSaveMinutes = 5;
inline bool valid_auto_save_minutes(int m) {
    for (int v : kAutoSaveMinutes)
        if (v == m) return true;
    return false;
}
// "off" is the player's Off; "2", "5", "10" an interval. Anything else, and "0" (written by older
// versions whether or not the player chose Off, when Off was the default), is the default.
inline int parse_minutes(const std::string& s) {
    if (s == "off") return 0;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return end != s.c_str() && *end == '\0' && v > 0 && valid_auto_save_minutes(int(v)) ? int(v) : kDefaultAutoSaveMinutes;
}
inline std::string format_minutes(int m) { return m > 0 ? std::to_string(m) : std::string("off"); }

// The timer. Two reasons to save: the interval passed since the last auto save (or since gameplay
// began), or Link entered a new stage, at most once per kAreaGapSeconds so going in and out of doors
// doesn't use up the three slots. Either way it waits until the player has had control of Link for
// kSettleFrames frames in a row, so it doesn't save the moment Link arrives or a cutscene ends. A
// refused save is tried again after kRetrySeconds. Writes rotate through the three slots, oldest first.
constexpr double kRetrySeconds = 15;
constexpr double kAreaGapSeconds = 60;
constexpr int kSettleFrames = 90;  // ~3 s at 30 fps
struct AutoSaveTimer {
    double since = -1;   // time the interval counts from (-1: not in gameplay yet)
    double retry_at = -1;
    int next = 1;        // the slot number (1..kAutoSaves) the next write goes to
    bool pending = false;
    std::string stage;   // the stage seen on the previous frame
    bool area_pending = false;  // a new stage was entered since the last auto save
    bool area = false;          // the request in flight is for a new area (else the interval)
    int control_frames = 0;     // frames in a row with the player in control, while a save is due

    // per frame: whether to request an auto save now. `has_control()` (the player controls Link) is
    // only asked while a save is due.
    template <class Control>
    bool due(double now, int minutes, bool gameplay, const std::string& stage_now, Control&& has_control) {
        if (minutes <= 0 || !gameplay) {
            since = -1;  // the interval starts again with the next gameplay
            retry_at = -1;
            stage.clear();
            area_pending = false;
            control_frames = 0;
            return false;
        }
        if (since < 0) {
            since = now;
            stage = stage_now;
        } else if (stage_now != stage) {
            stage = stage_now;
            area_pending = true;
        }
        if (pending) return false;
        bool timer_due = now - since >= minutes * 60.0;
        bool area_due = area_pending && now - since >= kAreaGapSeconds;
        if (retry_at >= 0 && now < retry_at) timer_due = area_due = false;
        if (!timer_due && !area_due) {
            control_frames = 0;
            return false;
        }
        if (!has_control()) {
            control_frames = 0;
            return false;
        }
        if (++control_frames < kSettleFrames) return false;
        control_frames = 0;
        area = area_due && !timer_due;
        pending = true;
        return true;
    }
    // the requested save finished
    void done(double now, bool ok) {
        pending = false;
        if (ok) {
            since = now;
            retry_at = -1;
            area_pending = false;
            next = next % kAutoSaves + 1;
        } else {
            retry_at = now + kRetrySeconds;
        }
    }
};

// ---- the auto save interval in effect: WWHD_AUTO_SAVE=0|2|5|10 sets it for a run; scripted runs
// (WWHD_NO_HOST_INPUT) have none unless that asks for one; otherwise the player's choice ----
inline int effective_auto_save_minutes(int chosen, const char* auto_save_env, bool scripted) {
    if (auto_save_env && *auto_save_env)
        return std::strcmp(auto_save_env, "0") == 0 ? 0 : parse_minutes(auto_save_env);
    return scripted ? 0 : chosen;
}

// ---- Last session: on unless WWHD_LAST_SESSION=0; scripted runs (WWHD_NO_HOST_INPUT) only with
// WWHD_LAST_SESSION=1 ----
inline bool last_session_enabled(const char* last_session_env, bool scripted) {
    if (last_session_env && *last_session_env) return std::strcmp(last_session_env, "0") != 0;
    return !scripted;
}

// ---- the settings file (<states>/save_options.cfg): "start=...", "autosave=..." lines ----
struct Options {
    StartChoice start;
    int autosave_minutes = kDefaultAutoSaveMinutes;
};
inline Options parse_options(const std::string& text) {
    Options o;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            if (key == "start") o.start = parse_start(value);
            else if (key == "autosave") o.autosave_minutes = parse_minutes(value);
        }
        at = end + 1;
    }
    return o;
}
inline std::string format_options(const Options& o) {
    return "start=" + format_start(o.start) + "\nautosave=" + format_minutes(o.autosave_minutes) + "\n";
}

}  // namespace ss::convenience
