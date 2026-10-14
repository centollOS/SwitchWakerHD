// Mods tab state model (overlay/mods_model.h) and the catalogue loader (overlay/catalogue_loader.h):
// row statuses, the Settings page's offers and explanations, the install flow, progress estimates,
// and the catalogue's cache / background check / offline fallback / once-per-session rule.
// No ImGui, no game, synthetic files only.
#include "overlay/catalogue_loader.h"
#include "overlay/mods_model.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace overlay::modsmodel;
namespace fs = std::filesystem;

static void rows() {
    Facts f;
    assert(row(f).status == Status::Off && row(f).text == "Off" && row(f).switch_enabled);
    f.enabled = true;
    assert(row(f).status == Status::On && row(f).text == "On" && row(f).switch_on);
    // switched on but loaded only after a restart
    f.pending_restart = true;
    assert(row(f).status == Status::RestartToApply && row(f).text == "Restart to apply");
    // switched off, still loaded until exit
    f = {};
    f.active = true;
    f.pending_restart = true;
    assert(row(f).status == Status::RestartToApply && !row(f).switch_on);
    f = {};
    f.needs_setup = true;
    assert(row(f).status == Status::NeedsSetup && row(f).text == "Needs setup" && row(f).switch_enabled);
    // an enabled mod whose receipt went stale (the source moved) is still On; its page says what is open
    f.enabled = true;
    assert(row(f).status == Status::On);
    f = {};
    f.working = true;
    f.percent = 45.4;
    // "Installing" only while downloading; "Setting up" once it is downloaded
    assert(row(f).status == Status::Installing && row(f).text == "Setting up… 45%" && !row(f).switch_enabled &&
           !row(f).switch_reason.empty());  // never a disabled control without the reason
    f.downloading = true;
    assert(row(f).text == "Installing… 45%");
    f = {};
    f.update_available = true;
    assert(row(f).status == Status::UpdateAvailable && row(f).text == "Update available");
    f.enabled = true;  // on: the switch matters more; the page offers the update
    assert(row(f).status == Status::On);
    f = {};
    f.error = "Missing dependency: x";
    assert(row(f).status == Status::Error && row(f).switch_enabled);
    f = {};
    f.compatible = false;
    assert(row(f).status == Status::Error && !row(f).switch_enabled && !row(f).switch_reason.empty());
    f.enabled = true;  // a broken mod that is on can always be switched off
    assert(row(f).switch_enabled);
    f = {};
    f.failed = true;
    assert(row(f).status == Status::Error);
    // working wins over everything else
    f.working = true;
    f.pending_restart = true;
    assert(row(f).status == Status::Installing);
}

static void pages() {
    PageFacts f;
    f.has_setup = true;
    f.setup_done = false;
    // off and not loaded: everything is offered, nothing is explained
    Page p = page(f);
    assert(!p.setup_locked && p.lock_line.empty() && p.offer_set_up && p.offer_run_again && p.offer_remove);
    assert(p.setup_summary == "Needs setup" && p.remove_line.empty());
    // on: one line instead of greyed buttons
    f.enabled = true;
    p = page(f);
    assert(p.setup_locked && p.lock_line == "Switch the mod off first to change its setup." && !p.offer_restart);
    assert(!p.offer_set_up && !p.offer_run_again && !p.offer_remove && !p.remove_line.empty());
    // switched off but still loaded in this session (mod.active): the restart is right there
    f.enabled = false;
    f.active = true;
    p = page(f);
    assert(p.setup_locked && p.lock_line == "Off — takes effect after a restart." && p.offer_restart && p.restart_hint.empty());
    assert(!p.offer_run_again && !p.offer_remove);
    f.can_restart = false;  // Android: say what to do instead
    p = page(f);
    assert(!p.offer_restart && !p.restart_hint.empty());
    // set up: the normal view says so, and offers no step action
    f = {};
    f.has_setup = true;
    f.setup_done = true;
    p = page(f);
    assert(p.setup_done && p.setup_summary == "Set up" && !p.offer_set_up && p.offer_run_again);
    // never "Ready:" anywhere
    assert(std::string(step_text(StepState::Done)) == "Done" && std::string(step_text(StepState::NotDone)) == "Not done yet");
    // while working: no actions, the reason instead
    f.working = true;
    f.update_available = true;
    p = page(f);
    assert(p.setup_summary == "Setting up…" && !p.offer_run_again && !p.offer_update && !p.update_line.empty() && !p.remove_line.empty());
    // update: offered while off and unloaded, or while on for a mod that can be switched off and on live
    f = {};
    f.update_available = true;
    assert(page(f).offer_update);
    f.enabled = true;
    assert(!page(f).offer_update && page(f).update_line == "Switch the mod off first to update it.");
    f.live = true;
    assert(page(f).offer_update && page(f).update_line.empty());
    f.enabled = false;
    f.active = true;  // off, still loaded until a restart (a mod loaded at start)
    f.live = false;
    assert(!page(f).offer_update && page(f).update_line == kOffAfterRestart);
    // built-in mods: no setup, no update, no remove
    f = {};
    f.builtin = true;
    p = page(f);
    assert(p.setup_summary.empty() && !p.offer_remove && !p.offer_update);
    // options: setup choices follow the lock; others stay editable
    assert(!option_state(true, true, false).shown);
    assert(option_state(true, false, false).editable);
    assert(option_state(false, true, false).editable && option_state(false, true, true).note == "Changes apply after a restart.");
}

static void flow() {
    Plan plan;
    plan.download = true;
    plan.download_bytes = 4 * 1024 * 1024;
    plan.backup = true;
    plan.prepare = {"Prepare island maps"};
    plan.support = true;
    plan.build = true;
    plan.restart = true;
    Flow f = Flow::make("gc-minimap", "Minimap", plan);
    assert(f.phase == Phase::Review && f.steps.size() == 7);
    const StepKind order[] = {StepKind::Download, StepKind::Check, StepKind::Backup, StepKind::Prepare, StepKind::Support,
                              StepKind::Build, StepKind::SwitchOn};
    for (size_t i = 0; i < 7; i++) assert(f.steps[i].kind == order[i]);
    assert(f.steps[3].title == "Prepare island maps");
    assert(f.overall() == 0 && !f.active());
    // review -> working
    f.accept(10);
    assert(f.phase == Phase::Working && f.current() == 0 && f.steps[0].state == StepState::Working && f.active());
    // download reports its bytes
    f.report(StepKind::Download, 0.5);
    assert(f.steps[0].measured && f.step_fraction() == 0.5);
    double half = f.overall();
    assert(half > 0 && half < 0.05);
    // measured remaining time: 2 s in for half of it -> about 2 s more for this step
    double left = f.remaining(12);
    assert(left > f.total_expected() - f.steps[0].expected - 1);
    // the check and the backup finish; marking a later step completes the earlier ones
    f.done(StepKind::Backup, 13);
    assert(f.steps[0].state == StepState::Done && f.steps[1].state == StepState::Done && f.steps[2].state == StepState::Done);
    assert(f.current() == 3 && f.steps[3].state == StepState::Working && f.steps[3].started == 13);
    // unmeasured steps estimate from time, never reaching 100%
    f.tick(13 + 30);
    assert(f.steps[3].fraction > 0.6 && f.steps[3].fraction < 0.65);
    f.tick(13 + 3000);
    assert(f.steps[3].fraction == 0.95);
    // failure: one sentence and the details; completed steps stay done
    f.fail("Preparing the mod's files failed.", "tool output");
    assert(f.phase == Phase::Failed && f.steps[3].state == StepState::Failed && f.current() == 3 && f.details == "tool output");
    assert(f.steps[2].state == StepState::Done);
    // try again: the failed step starts over
    f.retry(5000);
    assert(f.phase == Phase::Working && f.error.empty() && f.steps[3].state == StepState::Working && f.steps[3].fraction == 0 &&
           f.steps[2].state == StepState::Done);
    // the rebuild reports its own progress
    f.done(StepKind::Prepare, 5010);
    f.report(StepKind::Support, 0.25);
    assert(f.current() == 4 && f.steps[4].measured);
    f.tick(5020);
    assert(f.steps[4].fraction == 0.25);  // measured steps are not overwritten by the time estimate
    // cancel keeps what was finished
    Flow c = f;
    auto kept = c.cancel();
    assert(c.phase == Phase::Idle && kept.size() == 4 && kept[3] == StepKind::Prepare);
    // done, with the restart
    f.finish(true, 5100);
    assert(f.phase == Phase::Done && f.restart_needed && f.overall() == 1 && f.remaining(5100) == 0);
    // retry/fail/accept are ignored in the wrong phase
    f.retry(5200);
    f.fail("x", "y");
    assert(f.phase == Phase::Done);
    // an update ends in the mod's state before it: off stays off (no Switch on), on is switched off first and
    // on again at the end
    Plan off_update;
    off_update.download = true;
    off_update.prepare = {"Prepare"};
    off_update.keep_off = true;
    off_update.restart = true;
    Flow u = Flow::make("m", "M", off_update);
    assert(u.steps.size() == 3 && u.steps.back().kind == StepKind::Prepare && u.keep_off && !u.restart_needed);
    u.accept(0);
    u.finish(false, 1);
    assert(u.phase == Phase::Done && u.keep_off);
    Plan on_update;
    on_update.download = true;
    on_update.switch_off = true;
    Flow o = Flow::make("m", "M", on_update);
    assert(o.steps.size() == 4 && o.steps.front().kind == StepKind::SwitchOff && o.steps.back().kind == StepKind::SwitchOn && !o.keep_off);
    o.accept(0);
    assert(o.current() == 0 && o.steps[0].title == "Switch off for the update");
    // a mod that needs nothing: just Switch on
    Flow s = Flow::make("plain", "Plain", Plan{});
    assert(s.steps.size() == 1 && s.steps[0].kind == StepKind::SwitchOn && s.steps[0].title == "Switch on");
    s.accept(0);
    s.finish(false, 1);
    assert(s.phase == Phase::Done && !s.restart_needed);
    // the review's time estimate
    assert(duration_text(20) == "less than a minute" && duration_text(70) == "about 1 minute" && duration_text(400) == "about 7 minutes");
    assert(elapsed_text(75) == "1:15" && elapsed_text(9) == "9 s");
    assert(size_text(53 * 1024) == "53 KB" && size_text(3 * 1024 * 1024) == "3.0 MB");
    assert(first_sentence("A minimap. It shows islands.") == "A minimap." && first_sentence("No full stop") == "No full stop");
    assert(file_name("/Users/me/Games/Wind Waker (USA).rvz") == "Wind Waker (USA).rvz" && file_name("C:\\Games\\WW\\") == "WW");
    assert(percent_text(100.7) == "100%" && percent_text(-3) == "0%");
}

static void catalogue_policy() {
    CataloguePolicy p;
    assert(p.should_fetch(0));                    // never checked this session
    p.last_check = 0;
    assert(!p.should_fetch(10) && !p.should_fetch(3599) && p.should_fetch(3600));  // then hourly
    p.checking = true;
    assert(!p.should_fetch(10000));
    p = {};
    p.automatic = false;
    assert(!p.should_fetch(0));
    // the one quiet line
    assert(catalogue_line(true, true, false, true).text == "Checking for new mods…");
    assert(catalogue_line(true, false, true, true).retry && catalogue_line(true, false, true, true).text.find("last list") != std::string::npos);
    assert(catalogue_line(false, false, true, true).retry);   // first use, offline: one sentence and Retry
    assert(catalogue_line(true, false, false, true).text.empty());
    assert(catalogue_line(false, false, false, false).retry);  // automatic checks off, nothing cached: Refresh
}

static std::string index_json(const std::string& name) {
    return R"({"format_version":1,"mods":[{"id":"pilot","name":")" + name +
           R"(","description":"Synthetic.","version":"1.0.0","kind":"settings","authors":["Fixture"],"licences":["MPL-2.0"],)"
           R"("port_versions":{"minimum":"0.2.0"},"downloads":{"all":{"url":"https://example.invalid/p.zip","sha256":")" +
           std::string(64, 'a') + R"(","size":10}}}]})";
}
static void wait(overlay::CatalogueLoader& l) {
    for (int i = 0; i < 2000 && l.checking(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(!l.checking());
}

static void catalogue_loader() {
    auto root = fs::temp_directory_path() / ("wwhd-mods-model-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const std::string url = "https://example.invalid/index.json";
    std::atomic<int> calls{0};
    std::atomic<bool> online{true}, slow{false};
    std::string served = index_json("First");
    mods::catalogue::Fetch fetch = [&](const std::string& u, const fs::path& path, uint64_t) {
        ++calls;
        assert(u == url);  // only the index, nothing else
        while (slow) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!online) throw std::runtime_error("offline");
        std::ofstream(path, std::ios::binary) << served;
    };
    {
        // first use, no cache, no network: one plain line and Retry
        overlay::CatalogueLoader l;
        online = false;
        l.open(url, root, fetch, 0, true);
        wait(l);
        auto s = l.snapshot();
        assert(!s.have_list && s.failed && calls == 1);
        assert(catalogue_line(s.have_list, s.checking, s.failed, true).retry);
        // the next frames don't fetch again (once per session)
        l.open(url, root, fetch, 1, true);
        l.open(url, root, fetch, 100, true);
        assert(calls == 1 && l.fetches() == 1);
        // Retry: back online
        online = true;
        l.refresh(url, root, fetch, 101);
        wait(l);
        s = l.snapshot();
        assert(s.have_list && s.fresh && !s.failed && s.catalogue.index.entries.at(0).name == "First" && calls == 2);
    }
    {
        // a new session: the cached list shows at once, the check runs in the background without blocking
        overlay::CatalogueLoader l;
        served = index_json("Second");
        slow = true;
        auto start = std::chrono::steady_clock::now();
        l.open(url, root, fetch, 0, true);
        assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
        auto s = l.snapshot();
        assert(s.have_list && !s.fresh && s.checking && s.catalogue.index.entries.at(0).name == "First");
        assert(catalogue_line(s.have_list, s.checking, s.failed, true).text == "Checking for new mods…");
        slow = false;
        wait(l);
        s = l.snapshot();
        assert(s.fresh && s.catalogue.index.entries.at(0).name == "Second");
        // once per session, then again after an hour while the game runs
        int before = calls;
        l.open(url, root, fetch, 60, true);
        assert(calls == before);
        l.open(url, root, fetch, 3601, true);
        wait(l);
        assert(calls == before + 1);
        // offline later: the last list stays, one quiet line
        online = false;
        l.refresh(url, root, fetch, 3700);
        wait(l);
        s = l.snapshot();
        assert(s.have_list && s.failed && s.catalogue.index.entries.at(0).name == "Second");
        assert(catalogue_line(s.have_list, s.checking, s.failed, true).text == "Couldn't check for new mods — showing the last list.");
        online = true;
    }
    {
        // automatic checks off: the cache only, no network until Refresh
        overlay::CatalogueLoader l;
        int before = calls;
        l.open(url, root, fetch, 0, false);
        assert(l.snapshot().have_list && calls == before && !l.checking());
        l.refresh(url, root, fetch, 1);
        wait(l);
        assert(calls == before + 1);
    }
    {
        // another source has its own cache: nothing from the first one is shown
        overlay::CatalogueLoader l;
        online = false;
        l.open("https://example.invalid/other.json", root, [&](const std::string&, const fs::path&, uint64_t) { throw std::runtime_error("offline"); }, 0, true);
        wait(l);
        assert(!l.snapshot().have_list);
        online = true;
    }
    fs::remove_all(root);
}

int main() {
    rows();
    pages();
    flow();
    catalogue_policy();
    catalogue_loader();
    std::cout << "Mods tab model: rows, pages, install flow, progress and catalogue loading passed\n";
}
