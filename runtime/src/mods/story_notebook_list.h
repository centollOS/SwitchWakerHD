// The story notebook's quest list: the quests that story mods register in a session, their state in
// the save file and the one tracked quest (docs/mod-sdk-v2.md, "The story notebook"). Pure code
// without game memory, unit-tested in runtime/tools/story_notebook_test.cpp.
//
// What the save file keeps (story_mail_book.h, the record shared with letters and charts):
//   - kind kRecordQuest (4), key quest_key(mod, quest): the step reached, 1..254, or 255 when the
//     quest is done. A quest that was never started has no record (it costs no room).
//   - kind kRecordTracked (5), key quest_key(mod, quest), value 1: the quest shown on the sea chart.
//     There is at most one such record; it is removed when the quest is done or untracked.
// The texts (title, next step, where, what is known, reward, start condition) and the place of the
// next step are not saved: the mod registers them in every session, like the letter texts.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "story_mail_book.h"

namespace story_notebook {

inline constexpr uint8_t kMaxStep = 254, kStepDone = 255;
inline constexpr size_t kMaxTitleBytes = 48, kMaxNextBytes = 120, kMaxWhereBytes = 48, kMaxRewardBytes = 96,
                        kMaxConditionBytes = 96, kMaxKnownBytes = 600, kMaxPortraitBytes = 96;
inline constexpr unsigned kMaxQuestsPerMod = 8, kMaxQuests = 32;

// the quest's key in the story record: the letters' hash, the record kind keeps the two apart
inline uint16_t quest_key(const std::string& mod, const std::string& quest) { return story_mail::letter_key(mod, quest); }

// A quest as a mod registered it in this session.
struct Quest {
    std::string mod, mod_name, id;
    uint16_t key = 0;
    std::string title, reward, condition;  // condition: empty when the quest can be started now
    std::string next, where, known;        // the current step: what to do next, the island, what is known so far
    std::string portrait;                  // package image (assets/... or textures/...), empty: the mod's emblem
    std::string hint;                      // set by the port: what the player has to do in the notebook (e.g. track it)
    bool hidden = false;                   // journal "hidden" / "on_trigger" (not yet revealed): not in the notebook
    bool texts_from_mod = false;           // the mod's code set the step texts this session (they win over the manifest's)
    bool has_place = false;                // the next step has a place on the sea chart
    float x = 0, z = 0;
    // the quest's steps as its manifest declares them (the notebook's progress line); empty when the
    // mod's code keeps its own steps
    std::vector<std::string> steps;        // step 1, 2, ...: what each asks for
    // the mod chart the current step points to ("find": "chart:<id>"), while Link owns it: the page
    // offers "Open chart" (the MAP screen's chart list with that chart selected)
    std::string chart_mod, chart;
};

enum class Status : uint8_t { InProgress, New, NotAvailable, Done };

// What the notebook shows for a quest, from its saved step (0: no record).
inline Status status_of(const Quest& q, uint8_t saved) {
    if (saved == kStepDone) return Status::Done;
    if (saved) return Status::InProgress;
    return q.condition.empty() ? Status::New : Status::NotAvailable;
}

inline uint8_t saved_step(const story_mail::Book& b, uint16_t key) { return b.get(story_mail::kRecordQuest, key); }

// The tracked quest's key in this save file, 0 when none.
inline uint16_t tracked_key(const story_mail::Book& b) {
    if (!b.usable()) return 0;
    for (unsigned r = 0; r < b.records(); r++)
        if (b.record_kind(r) == story_mail::kRecordTracked) return b.record_key(r);
    return 0;
}

// Tracks the quest (and untracks any other), or untracks it if it is the tracked one. A done quest
// can't be tracked. Returns whether the quest is tracked now; false also when the record is full.
inline bool toggle_tracked(story_mail::Book& b, uint16_t key) {
    const uint16_t now = tracked_key(b);
    if (now) b.erase(story_mail::kRecordTracked, now);
    if (now == key || saved_step(b, key) == kStepDone) return false;
    return b.set(story_mail::kRecordTracked, key, 1);
}

// Records "step N reached" (1..254); a quest never goes back and a done quest stays done. Returns
// the saved step afterwards, or -1 when the record is full or not ours.
inline int reach_step(story_mail::Book& b, uint16_t key, unsigned step) {
    const uint8_t now = saved_step(b, key);
    if (now == kStepDone || step == 0 || step <= now) return now;
    if (step > kMaxStep) return -1;
    if (!b.set(story_mail::kRecordQuest, key, uint8_t(step))) return -1;
    if (now == 0 && !b.has(story_mail::kRecordQuestOrder, key)) {  // started: note its place in the start order
        unsigned last = 0;
        for (unsigned r = 0; r < b.records(); r++)
            if (b.record_kind(r) == story_mail::kRecordQuestOrder && b.record_value(r) > last) last = b.record_value(r);
        b.set(story_mail::kRecordQuestOrder, key, uint8_t(last < 255 ? last + 1 : 255));
    }
    return int(step);
}
// the quest's place in the start order (1 first; 255 later or unknown)
inline unsigned start_order(const story_mail::Book& b, uint16_t key) {
    const uint8_t v = b.get(story_mail::kRecordQuestOrder, key);
    return v ? v : 255;
}

// Marks the quest done; it is no longer tracked. Returns kStepDone, or -1 when there is no room.
inline int finish(story_mail::Book& b, uint16_t key) {
    if (!b.set(story_mail::kRecordQuest, key, kStepDone)) return -1;
    b.erase(story_mail::kRecordTracked, key);
    return kStepDone;
}

struct Row {
    const Quest* quest;
    Status status;
    uint8_t step;  // saved step: 0 none, 1..254, 255 done
    bool tracked;
};

// The notebook's rows: the tracked quest first, then quests in progress, new ones, not yet
// available ones and done ones; within each group in the order the mods registered them.
inline std::vector<Row> rows(const std::vector<Quest>& quests, const story_mail::Book& b) {
    std::vector<Row> all;
    const bool ok = b.usable();
    const uint16_t tracked = ok ? tracked_key(b) : 0;
    for (const Quest& q : quests) {
        if (q.hidden) continue;
        const uint8_t step = ok ? saved_step(b, q.key) : 0;
        const Status s = status_of(q, step);
        all.push_back({&q, s, step, tracked && q.key == tracked && s != Status::Done});
    }
    std::vector<Row> out;
    for (const Row& r : all)
        if (r.tracked) out.push_back(r);
    for (Status s : {Status::InProgress, Status::New, Status::NotAvailable, Status::Done})
        for (const Row& r : all)
            if (!r.tracked && r.status == s) out.push_back(r);
    return out;
}

// The tracked quest's next step on the sea chart: null when no quest is tracked, it is done, or
// its current step has no place.
inline const Quest* tracked_place(const std::vector<Quest>& quests, const story_mail::Book& b) {
    for (const Row& r : rows(quests, b))
        if (r.tracked) return r.quest->has_place ? r.quest : nullptr;
    return nullptr;
}

}  // namespace story_notebook
