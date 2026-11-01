// Save-state conveniences (save_convenience.h): slots, At start, Auto save timer, settings file.
// Pure logic; no game files.
#include "save_convenience.h"
#include <cassert>

using namespace ss::convenience;

namespace {
const auto yes = [] { return true; };
const auto no = [] { return false; };
// the frames until the player has had control long enough: true on the last one
bool settle(AutoSaveTimer& t, double now, int minutes, const char* stage = "sea") {
    for (int i = 1; i < kSettleFrames; i++)
        if (t.due(now, minutes, true, stage, yes)) return false;
    return t.due(now, minutes, true, stage, yes);
}
}  // namespace

int main() {
    // own slots: never the player's 1..5 or crash recovery's 101..103
    assert(is_convenience(kLastSessionSlot) && !is_auto_save(kLastSessionSlot));
    for (int s = 111; s <= 113; s++) assert(is_auto_save(s) && is_convenience(s));
    for (int s : {0, 1, 5, 101, 102, 103, 109, 114}) assert(!is_convenience(s));

    // At start: default intro; round trip; unknown text is the default
    assert(parse_start("") == StartChoice{} && parse_start("intro") == StartChoice{});
    assert(parse_start("last").what == Start::kLastSession);
    assert(parse_start("slot3").what == Start::kSlot && parse_start("slot3").slot == 3);
    for (const char* bad : {"slot0", "slot6", "slot", "slot12", "LAST", "x"}) assert(parse_start(bad) == StartChoice{});
    for (const char* s : {"intro", "last", "slot1", "slot5"}) assert(format_start(parse_start(s)) == s);

    // Auto save interval: off / 2 / 5 / 10; on (5) by default, "off" is the player's Off, and the
    // "0" older versions always wrote is the default
    assert(kDefaultAutoSaveMinutes == 5 && valid_auto_save_minutes(kDefaultAutoSaveMinutes));
    assert(parse_minutes("2") == 2 && parse_minutes("10") == 10 && parse_minutes("off") == 0);
    for (const char* other : {"0", "3", "-2", "", "5x", "60", "OFF"}) assert(parse_minutes(other) == 5);
    for (int m : kAutoSaveMinutes) assert(parse_minutes(format_minutes(m)) == m);
    assert(format_minutes(0) == "off");

    // the interval in effect: WWHD_AUTO_SAVE for a run, none for scripted runs, else the player's
    assert(effective_auto_save_minutes(5, nullptr, false) == 5 && effective_auto_save_minutes(0, "", false) == 0);
    assert(effective_auto_save_minutes(5, nullptr, true) == 0 && effective_auto_save_minutes(5, "", true) == 0);
    assert(effective_auto_save_minutes(0, "2", true) == 2 && effective_auto_save_minutes(5, "0", false) == 0);
    assert(effective_auto_save_minutes(5, "off", false) == 0);

    // the timer
    {
        AutoSaveTimer t;
        assert(!t.due(0, 0, true, "sea", yes));     // off
        assert(!t.due(0, 2, false, "sea", yes));    // not in gameplay
        assert(!t.due(10, 2, true, "sea", yes));    // gameplay begins: the interval starts now
        assert(!settle(t, 129, 2));
        assert(settle(t, 130, 2) && t.next == 1 && !t.area);  // due, after kSettleFrames of control
        assert(!t.due(131, 2, true, "sea", yes));   // one request at a time
        t.done(131, true);
        assert(t.next == 2 && !settle(t, 200, 2) && settle(t, 251, 2));
        t.done(251, false);                         // refused: retried after a short while
        assert(t.next == 2 && !settle(t, 260, 2) && settle(t, 266, 2));
        t.done(266, true);
        assert(t.next == 3);
        assert(settle(t, 386, 2)); t.done(386, true);
        assert(t.next == 1);                        // rotates through the three slots, oldest first
        assert(!t.due(400, 2, false, "sea", yes));  // leaving gameplay resets the interval
        assert(!t.due(401, 2, true, "sea", yes) && !settle(t, 520, 2) && settle(t, 521, 2));
        t.done(521, true);
        assert(!t.due(600, 0, true, "sea", yes) && !t.due(700, 5, true, "sea", yes) && settle(t, 1000, 5));  // off and on
        t.done(1000, true);
    }
    // waits for kSettleFrames frames in a row of control, and only asks while a save is due
    {
        AutoSaveTimer t;
        int asked = 0;
        auto counted = [&] { asked++; return true; };
        assert(!t.due(0, 2, true, "sea", counted));
        for (int i = 0; i < 100; i++) assert(!t.due(60, 2, true, "sea", counted));
        assert(asked == 0);                         // not due yet: control isn't checked
        for (int i = 1; i < kSettleFrames; i++) assert(!t.due(120, 2, true, "sea", counted));
        assert(asked == kSettleFrames - 1);
        assert(!t.due(120, 2, true, "sea", no));    // a cutscene: start over
        for (int i = 1; i < kSettleFrames; i++) assert(!t.due(121, 2, true, "sea", yes));
        assert(t.due(121, 2, true, "sea", yes));
    }
    // a new area: saved once kAreaGapSeconds passed since the last auto save (or gameplay began)
    {
        AutoSaveTimer t;
        assert(!t.due(0, 10, true, "sea", yes));
        assert(!t.due(1, 10, true, "M_NewD2", yes));   // entered a dungeon after a second: too soon
        assert(!settle(t, 59, 10, "M_NewD2"));
        assert(settle(t, 60, 10, "M_NewD2") && t.area);
        t.done(60, true);
        assert(!t.due(61, 10, true, "sea", yes));      // out again: within a minute of that save
        assert(!t.due(62, 10, true, "M_NewD2", yes));  // and in: still one pending area save
        assert(!settle(t, 119, 10, "M_NewD2") && settle(t, 120, 10, "M_NewD2") && t.next == 2);
        t.done(120, true);
        assert(!settle(t, 300, 10, "M_NewD2"));        // same stage: only the interval
        assert(settle(t, 720, 10, "M_NewD2") && !t.area);
        t.done(720, true);
        // a refused area save is retried after kRetrySeconds
        assert(!t.due(799, 10, true, "sea", no) && settle(t, 800, 10));
        t.done(800, false);
        assert(!settle(t, 810, 10) && settle(t, 815, 10) && t.area);
        t.done(815, true);
        // leaving gameplay (the title screen, a state being loaded) forgets the stage: no area save after
        assert(!t.due(900, 10, false, "", yes) && !t.due(901, 10, true, "Adanmae", yes));
        assert(!settle(t, 1000, 10, "Adanmae"));
    }

    // the settings file: round trip, unknown and bad lines ignored, defaults otherwise
    Options o;
    assert(parse_options("").start == StartChoice{} && parse_options("").autosave_minutes == 5);
    o.start = {Start::kSlot, 4};
    o.autosave_minutes = 2;
    Options r = parse_options(format_options(o));
    assert(r.start == o.start && r.autosave_minutes == 2);
    o.autosave_minutes = 0;  // the player's Off is kept
    assert(format_options(o).find("autosave=off\n") != std::string::npos && parse_options(format_options(o)).autosave_minutes == 0);
    r = parse_options("junk\r\nstart=last\r\nautosave=7\r\nother=1");
    assert(r.start.what == Start::kLastSession && r.autosave_minutes == 5);
    assert(parse_options("start=slot2\nautosave=0\n").autosave_minutes == 5);  // a file from an older version

    // Last session: on for players, off for scripted runs unless asked for
    assert(last_session_enabled(nullptr, false) && !last_session_enabled(nullptr, true));
    assert(!last_session_enabled("0", false) && last_session_enabled("1", true) && last_session_enabled("", false));
    return 0;
}
