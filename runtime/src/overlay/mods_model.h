// The Mods tab's state model: what a mod's row says, what its Settings page offers and explains, the
// install flow (review -> progress -> done / failed) and its progress estimate. Plain data, no ImGui and
// no mod-manager calls, so every state can be unit tested (runtime/tools/mods_model_test.cpp). The overlay
// gathers the facts (mods/packages.h, the setup run, the catalogue) and draws what this decides.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace overlay::modsmodel {

// ---------------------------------------------------------------- words (same everywhere)
inline constexpr const char* kSwitchOffFirst = "Switch the mod off first to change its setup.";
inline constexpr const char* kOffAfterRestart = "Off — takes effect after a restart.";
inline constexpr const char* kSetUpDone = "Set up";  // drawn with a check mark

// ---------------------------------------------------------------- row status
enum class Status { On, Off, NeedsSetup, Installing, UpdateAvailable, RestartToApply, Error };

struct Facts {
    bool builtin = false;
    bool enabled = false;           // wanted in this profile (the switch)
    bool active = false;            // loaded in this session (files stay loaded until exit)
    bool pending_restart = false;   // wanted != loaded, or settings that apply at the next start
    bool compatible = true;
    bool needs_setup = false;       // a required setup step is not done, or the GameCube game is missing
    bool working = false;           // install / setup of this mod is running
    bool downloading = false;       // ... and it is being downloaded (Installing), not set up (Setting up)
    double percent = 0;             // 0..100 while working
    bool failed = false;            // its install / setup stopped with an error
    std::string error;              // load / package problem (empty: none)
    bool update_available = false;  // the catalogue has a newer version
};

struct Row {
    Status status = Status::Off;
    std::string text;       // "On", "Needs setup", "Installing… 45%", "Setting up… 45%", ...
    bool switch_on = false;
    bool switch_enabled = true;
    std::string switch_reason;  // why the switch can't be used (shown next to it), empty when it can
};

inline std::string percent_text(double percent) {
    char b[16];
    snprintf(b, sizeof b, "%d%%", int(std::clamp(percent, 0.0, 100.0)));
    return b;
}

inline Row row(const Facts& f) {
    Row r;
    r.switch_on = f.enabled;
    if (f.working) {
        r.status = Status::Installing;
        r.text = (f.downloading ? "Installing… " : "Setting up… ") + percent_text(f.percent);
        r.switch_enabled = false;
        r.switch_reason = "Wait until it is installed.";
    } else if (f.failed || !f.error.empty() || !f.compatible) {
        r.status = Status::Error;
        r.text = "Error";
        // a broken mod can always be switched off; switching it on again asks for setup or shows the problem
        r.switch_enabled = f.enabled || f.compatible;
        if (!r.switch_enabled) r.switch_reason = "It can't run on this version of the game.";
    } else if (f.pending_restart) {
        r.status = Status::RestartToApply;
        r.text = "Restart to apply";
    } else if (!f.enabled && f.needs_setup) {
        r.status = Status::NeedsSetup;
        r.text = "Needs setup";
    } else if (f.update_available && !f.enabled && !f.active) {
        r.status = Status::UpdateAvailable;
        r.text = "Update available";
    } else {
        r.status = f.enabled ? Status::On : Status::Off;
        r.text = f.enabled ? "On" : "Off";
    }
    return r;
}

// ---------------------------------------------------------------- the mod's Settings page
enum class StepState { Done, NotDone, Working, Failed };
inline const char* step_text(StepState s) {
    switch (s) {
    case StepState::Done: return "Done";
    case StepState::NotDone: return "Not done yet";
    case StepState::Working: return "Working…";
    default: return "Failed";
    }
}

struct PageFacts {
    bool enabled = false, active = false;
    bool builtin = false;
    bool has_setup = false;      // the mod has setup steps
    bool setup_done = true;      // every required step is done
    bool working = false;        // its install / setup is running
    bool can_restart = true;     // the game can relaunch itself (not on Android)
    bool update_available = false;
    bool live = false;           // the mod applies without a restart (native code, settings): an update can
                                 // switch it off, update it and switch it on again in one go
};

struct Page {
    // one line instead of greyed setup buttons while the mod is on or still loaded
    bool setup_locked = false;
    std::string lock_line;
    bool offer_restart = false;      // "Restart now" right next to the line
    std::string restart_hint;        // where the game can't relaunch itself
    // setup summary in the normal view
    std::string setup_summary;       // "Set up" (with a check mark), "Needs setup", "Setting up…"; empty: no setup
    bool setup_done = false;
    bool offer_set_up = false;       // say how to set it up (switching it on opens the review)
    // troubleshooting (collapsed)
    bool offer_run_again = false;    // per step "Run again", only while off and not loaded
    // update / remove
    bool offer_update = false;
    std::string update_line;         // why Update isn't offered right now (empty: nothing to say)
    bool offer_remove = false;
    std::string remove_line;
};

inline Page page(const PageFacts& f) {
    Page p;
    p.setup_locked = f.enabled || f.active;
    if (f.enabled) p.lock_line = kSwitchOffFirst;
    else if (f.active) {
        p.lock_line = kOffAfterRestart;
        p.offer_restart = f.can_restart;
        if (!f.can_restart) p.restart_hint = "Restart the game to finish.";
    }
    if (f.has_setup) {
        p.setup_done = f.setup_done;
        p.setup_summary = f.working ? "Setting up…" : f.setup_done ? kSetUpDone : "Needs setup";
        p.offer_set_up = !f.working && !f.setup_done && !p.setup_locked;
        p.offer_run_again = !f.working && !p.setup_locked;
    }
    if (!f.builtin) {
        if (f.update_available) {
            // an update ends in the mod's state before it: off stays off; on stays on where the mod can be
            // switched off and on again while the game runs
            p.offer_update = !f.working && (!p.setup_locked || (f.enabled && f.live));
            if (p.offer_update) p.update_line.clear();
            else if (f.enabled) p.update_line = "Switch the mod off first to update it.";
            else if (f.active) p.update_line = kOffAfterRestart;
            else if (f.working) p.update_line = "Wait until the current setup has finished.";
        }
        p.offer_remove = !p.setup_locked && !f.working;
        if (f.enabled) p.remove_line = "Switch the mod off first to remove it.";
        else if (f.active) p.remove_line = kOffAfterRestart;
        else if (f.working) p.remove_line = "Wait until the current setup has finished.";
    }
    return p;
}

// An option on the page: setup choices (they change prepared files) follow the setup lock; other
// options stay editable, live or at the next start.
struct OptionState { bool shown = true, editable = true; std::string note; };
inline OptionState option_state(bool setup_option, bool locked, bool applies_after_restart) {
    OptionState o;
    if (setup_option && locked) { o.shown = false; o.editable = false; return o; }  // the lock line explains it
    if (applies_after_restart) o.note = "Changes apply after a restart.";
    return o;
}

// ---------------------------------------------------------------- install flow
enum class StepKind { SwitchOff, Download, Check, Backup, Prepare, Support, Build, SwitchOn };
enum class Phase { Idle, Review, Working, Done, Failed };

struct FlowStep {
    StepKind kind = StepKind::Download;
    std::string title;
    StepState state = StepState::NotDone;
    double fraction = 0;       // of this step, 0..1 (estimated when the step reports nothing)
    bool measured = false;     // fraction comes from the step itself (download bytes, rebuild units)
    double expected = 1;       // seconds, for the overall bar and the estimate
    double started = -1;
};

// Nominal durations: the estimate before a step reports its own progress.
inline double expected_seconds(StepKind k, uint64_t download_bytes = 0) {
    switch (k) {
    case StepKind::SwitchOff: return 2;
    case StepKind::Download: return 2 + double(download_bytes) / (2.0 * 1024 * 1024);
    case StepKind::Check: return 1;
    case StepKind::Backup: return 3;
    case StepKind::Prepare: return 30;
    case StepKind::Support: return 300;
    case StepKind::Build: return 45;
    default: return 1;
    }
}
inline const char* default_title(StepKind k) {
    switch (k) {
    case StepKind::SwitchOff: return "Switch off for the update";
    case StepKind::Download: return "Download";
    case StepKind::Check: return "Check";
    case StepKind::Backup: return "Back up saves";
    case StepKind::Prepare: return "Prepare";
    case StepKind::Support: return "Add code-mod support (restarts the game)";
    case StepKind::Build: return "Build code mod";
    default: return "Switch on";
    }
}

// What a flow will do, decided when the review opens.
struct Plan {
    bool download = false;             // a catalogue install or update
    uint64_t download_bytes = 0;
    bool backup = false;               // experimental: back up saves first
    std::vector<std::string> prepare;  // titles of the mod's preparation steps still to do
    bool support = false;              // code-mod support must be added (one rebuild, restart)
    bool build = false;                // a code mod is built for this installation
    bool restart = false;              // the mod applies at the next start
    bool switch_off = false;           // update of a mod that is on: switch it off first (it is switched on again)
    bool keep_off = false;             // update of a mod that is off: it stays off (no Switch on step)
};

class Flow {
public:
    std::string id, name;
    Phase phase = Phase::Idle;
    std::vector<FlowStep> steps;
    std::string error, details;   // one plain sentence; the tool output behind "Show details"
    bool restart_needed = false;  // Done: the mod becomes active after a restart
    bool keep_off = false;        // Done: the mod was updated (and prepared) and stays off
    double started = -1;

    static Flow make(std::string id, std::string name, const Plan& plan) {
        Flow f;
        f.id = std::move(id);
        f.name = std::move(name);
        auto add = [&](StepKind k, std::string title = {}) {
            FlowStep s;
            s.kind = k;
            s.title = title.empty() ? default_title(k) : std::move(title);
            s.expected = expected_seconds(k, plan.download_bytes);
            f.steps.push_back(std::move(s));
        };
        if (plan.switch_off) add(StepKind::SwitchOff);
        if (plan.download) { add(StepKind::Download); add(StepKind::Check); }
        if (plan.backup) add(StepKind::Backup);
        for (const auto& title : plan.prepare) add(StepKind::Prepare, title);
        if (plan.support) add(StepKind::Support);
        if (plan.build) add(StepKind::Build);
        if (!plan.keep_off) add(StepKind::SwitchOn);
        f.keep_off = plan.keep_off;
        f.restart_needed = plan.restart && !plan.keep_off;
        f.phase = Phase::Review;
        return f;
    }
    bool active() const { return phase == Phase::Working || phase == Phase::Failed || phase == Phase::Done; }
    // Review -> Working: Install pressed.
    void accept(double now) {
        if (phase != Phase::Review) return;
        phase = Phase::Working;
        started = now;
        begin_next(now);
    }
    // The current step (Working or Failed); steps.size() when none.
    size_t current() const {
        for (size_t i = 0; i < steps.size(); i++)
            if (steps[i].state == StepState::Working || steps[i].state == StepState::Failed) return i;
        return steps.size();
    }
    // A step's own progress (download bytes, rebuild units).
    void report(StepKind k, double fraction) {
        for (auto& s : steps)
            if (s.kind == k && s.state == StepState::Working) { s.fraction = std::clamp(fraction, 0.0, 1.0); s.measured = true; return; }
    }
    // Mark a step done from the outside truth (a setup receipt, the installed version, ...). Steps are
    // done in order; marking a later one also completes everything before it.
    void done(size_t index, double now) {
        if (index >= steps.size() || phase != Phase::Working) return;
        for (size_t i = 0; i <= index; i++) { steps[i].state = StepState::Done; steps[i].fraction = 1; }
        begin_next(now);
    }
    void done(StepKind k, double now) {
        for (size_t i = steps.size(); i-- > 0;)
            if (steps[i].kind == k) { done(i, now); return; }
    }
    void fail(std::string sentence, std::string output) {
        if (phase != Phase::Working) return;
        size_t i = current();
        if (i < steps.size()) steps[i].state = StepState::Failed;
        error = std::move(sentence);
        details = std::move(output);
        phase = Phase::Failed;
    }
    // Try again: completed steps are kept, the failed one starts over.
    void retry(double now) {
        if (phase != Phase::Failed) return;
        error.clear();
        details.clear();
        for (auto& s : steps)
            if (s.state == StepState::Failed) { s.state = StepState::NotDone; s.fraction = 0; s.measured = false; }
        phase = Phase::Working;
        begin_next(now);
    }
    // Cancel: the flow ends; whatever finished stays finished (the steps' own records keep it).
    std::vector<StepKind> cancel() {
        std::vector<StepKind> kept;
        for (const auto& s : steps) if (s.state == StepState::Done) kept.push_back(s.kind);
        phase = Phase::Idle;
        return kept;
    }
    // The mod is switched on (or will be after the restart).
    void finish(bool restart, double now) {
        if (phase != Phase::Working) return;
        for (auto& s : steps) { s.state = StepState::Done; s.fraction = 1; }
        restart_needed = restart;
        phase = Phase::Done;
        (void)now;
    }

    // ---- progress
    // Update a step's estimated fraction from elapsed time (steps that report nothing themselves).
    void tick(double now) {
        size_t i = current();
        if (i >= steps.size() || steps[i].state != StepState::Working || steps[i].measured || steps[i].started < 0) return;
        // approaches but never reaches the end: 63% at the nominal time, capped at 95%
        double t = (now - steps[i].started) / std::max(steps[i].expected, 0.1);
        steps[i].fraction = std::min(0.95, 1 - std::exp(-t));
    }
    double overall() const {
        double total = 0, done = 0;
        for (const auto& s : steps) {
            total += s.expected;
            done += s.expected * (s.state == StepState::Done ? 1.0 : s.fraction);
        }
        return total > 0 ? std::clamp(done / total, 0.0, 1.0) : 0.0;
    }
    double step_fraction() const {
        size_t i = current();
        return i < steps.size() ? steps[i].fraction : phase == Phase::Done ? 1.0 : 0.0;
    }
    double elapsed(double now) const { return started < 0 ? 0 : std::max(0.0, now - started); }
    // Seconds left: the current step's remainder (from its measured rate when it reports one) plus the
    // nominal time of the steps after it.
    double remaining(double now) const {
        if (phase == Phase::Done) return 0;
        double left = 0;
        size_t i = current();
        for (size_t j = 0; j < steps.size(); j++) {
            const auto& s = steps[j];
            if (s.state == StepState::Done) continue;
            if (j == i && s.started >= 0) {
                double spent = std::max(0.0, now - s.started);
                if (s.measured && s.fraction > 0.05) left += spent / s.fraction - spent;
                else left += std::max(s.expected - spent, s.expected * (1 - s.fraction));
            } else left += s.expected;
        }
        return std::max(0.0, left);
    }
    double total_expected() const {
        double t = 0;
        for (const auto& s : steps) t += s.expected;
        return t;
    }

private:
    void begin_next(double now) {
        for (auto& s : steps) {
            if (s.state == StepState::Done) continue;
            if (s.state != StepState::Working) { s.state = StepState::Working; s.started = now; s.fraction = 0; s.measured = false; }
            return;
        }
    }
};

// "about 2 minutes", "less than a minute"
inline std::string duration_text(double seconds) {
    if (seconds < 50) return "less than a minute";
    int minutes = int(std::lround(seconds / 60));
    if (minutes <= 1) return "about 1 minute";
    char b[32];
    snprintf(b, sizeof b, "about %d minutes", minutes);
    return b;
}
inline std::string elapsed_text(double seconds) {
    int s = int(seconds);
    char b[32];
    if (s < 60) snprintf(b, sizeof b, "%d s", s);
    else snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}
inline std::string size_text(uint64_t bytes) {
    char b[32];
    if (bytes < 1024) snprintf(b, sizeof b, "%llu bytes", (unsigned long long)bytes);
    else if (bytes < 1024 * 1024) snprintf(b, sizeof b, "%llu KB", (unsigned long long)((bytes + 1023) / 1024));
    else if (bytes < 1024ull * 1024 * 1024) snprintf(b, sizeof b, "%.1f MB", bytes / (1024.0 * 1024));
    else snprintf(b, sizeof b, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    return b;
}
// The first sentence of a description, for cards and rows.
inline std::string first_sentence(const std::string& text, size_t limit = 160) {
    size_t end = std::string::npos;
    for (size_t i = 0; i + 1 < text.size(); i++)
        if ((text[i] == '.' || text[i] == '!' || text[i] == '?') && (text[i + 1] == ' ' || text[i + 1] == '\n')) { end = i + 1; break; }
    std::string s = end == std::string::npos ? text : text.substr(0, end);
    if (auto nl = s.find('\n'); nl != std::string::npos) s.resize(nl);
    if (s.size() > limit) {
        size_t cut = s.rfind(' ', limit);
        s = s.substr(0, cut == std::string::npos ? limit : cut) + "…";
    }
    return s;
}
// A path as the player sees it: the file or folder name (the full path is the tooltip).
inline std::string file_name(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// ---------------------------------------------------------------- catalogue loading policy
// Get mods shows the cached list at once and checks for new mods in the background: at most once per
// session, again when the last check is older than an hour while the game runs, and on Refresh.
struct CataloguePolicy {
    static constexpr double kMaxAge = 3600;
    bool automatic = true;    // "Check for new mods automatically"
    double last_check = -1;   // when the last fetch finished (success or failure), session clock
    bool checking = false;
    bool should_fetch(double now) const {
        if (!automatic || checking) return false;
        return last_check < 0 || now - last_check >= kMaxAge;
    }
};

// The one quiet line under Get mods.
struct CatalogueLine {
    std::string text;
    bool retry = false;   // offer Retry next to it
};
inline CatalogueLine catalogue_line(bool have_list, bool checking, bool failed, bool automatic) {
    if (checking) return {have_list ? "Checking for new mods…" : "Loading the mod list…", false};
    if (failed && have_list) return {"Couldn't check for new mods — showing the last list.", true};
    if (failed) return {"Couldn't load the mod list. Check your internet connection.", true};
    if (!have_list) return {automatic ? "Loading the mod list…" : "Press Refresh to see the available mods.", !automatic};
    return {};
}

}  // namespace overlay::modsmodel
