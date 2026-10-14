// The King of Red Lions' one-time line for the tracked quest (docs/mod-sdk-v2.md, "The King of Red
// Lions"): a quest step's `sail_line`, said once by the boat the next time Link boards it or walks up
// to it. Pure code, unit-tested in runtime/tools/story_notebook_test.cpp; the hooks are in
// story_notebook.cpp.
//
// The rules (user decisions 2026-10-13), enforced here and not by the mods:
// - only the TRACKED quest's current step can have a live line; untracked or without the mod nothing
//   changes;
// - "said" is remembered in the story block (kRecordSailLine: the step whose line was said), never in
//   a game event bit; a new step with its own line is said again once;
// - the game always comes first: the line is only ordered in a moment where the game itself would
//   have ordered one of the boat's forced lines (Link rides the boat, or stands close in front of it),
//   while the game has no forced line pending, no event runs and nothing else ordered an event, and a
//   little after the boat's last talk. The boat's own talk hint is never replaced: Link's own talks
//   to the boat are the game's.
// In HD the boat is daShip_c (d_a_ship): execute 02477A24 orders its forced lines after
// checkForceMessage 024731BC; a talk starts in procTalk_init 024749DC (tww: the same functions).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "story_mail_book.h"
#include "story_notebook_list.h"

namespace story_notebook {

using story_mail::kRecordSailLine;  // the step whose sail_line was said: key quest_key, value 1..254
inline constexpr size_t kMaxSailLineBytes = 240;  // one message box: up to three lines

// A step's line (manifest "sail_line"), for the quest key
struct SailLine {
    uint16_t quest = 0;
    uint8_t step = 0;
    std::string text;  // UTF-8, '\n' between lines
};

// The tracked quest's line that is due now, or null: its current step has one and it was not said.
inline const SailLine* due_sail_line(const std::vector<SailLine>& all, const story_mail::Book& b) {
    if (!b.usable()) return nullptr;
    const uint16_t tracked = tracked_key(b);
    if (!tracked) return nullptr;
    const uint8_t step = saved_step(b, tracked);
    if (!step || step == kStepDone || b.get(kRecordSailLine, tracked) == step) return nullptr;
    for (const SailLine& s : all)
        if (s.quest == tracked && s.step == step && !s.text.empty()) return &s;
    return nullptr;
}
// After the line was shown: remembered in the story record (one record per quest)
inline bool mark_sail_line_said(story_mail::Book& b, const SailLine& s) { return b.set(kRecordSailLine, s.quest, s.step); }

// The boat's moment, read after its execute (story_notebook.cpp)
struct ShipMoment {
    bool force_checked = false;  // the boat's execute asked checkForceMessage this step (it does in every mode but its talk)...
    bool force_pending = false;  // ...and the game has one of its forced lines pending
    uint8_t mode = 0;            // daShip_c::mCurMode
    bool event_running = false;  // dEvt_control_c::mMode != 0 (a talk, a cutscene, a get-item scene, ...)
    unsigned orders = 0;         // event orders placed this step so far (mOrderCount): the game's go first
    bool mini_game = false;      // dComIfGp_getMiniGameType() != 0
    bool riding = false;         // Link rides the boat (daPyStts0_SHIP_RIDE_e)
    bool close_in_front = false; // the game's walk-up test for its forced lines: |dy| < 50, XZ < 250, seen angle < 0x6000
    bool can_say = false;        // the port can show a replaced message (story_mail's text buffers)
    uint64_t steps_since_busy = 0;  // logic steps since the boat last talked or an event ran
};
inline constexpr uint64_t kSailQuietSteps = 60;  // two seconds at 30 steps a second after the boat's last talk or event
// the boat's modes in which it handles a talk and is not busy: waiting, steering, paddling
inline constexpr uint8_t kShipModeWait = 0, kShipModeSteer = 1, kShipModePaddle = 2, kShipModeTalk = 8;

inline const char* sail_refusal(const ShipMoment& m) {
    if (!m.can_say) return "no message buffers";
    if (!m.force_checked) return "the boat did not ask for its forced lines";
    if (m.force_pending) return "the game has a forced line pending";
    if (m.mode != kShipModeWait && m.mode != kShipModeSteer && m.mode != kShipModePaddle) return "the boat is busy";
    if (m.event_running) return "an event runs";
    if (m.orders) return "another event was ordered";
    if (m.mini_game) return "a mini game runs";
    if (m.steps_since_busy < kSailQuietSteps) return "the boat talked a moment ago";
    if (!m.riding && !m.close_in_front) return "Link is not on or at the boat";
    return nullptr;
}
inline bool may_order_sail_line(const ShipMoment& m) { return sail_refusal(m) == nullptr; }

}  // namespace story_notebook
