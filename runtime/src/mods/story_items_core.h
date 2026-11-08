// Mod items: what a save file keeps of them (docs/mod-sdk-v2.md, "Items"). Pure code over the story
// record block (story_mail_book.h), unit-tested in runtime/tools/story_items_test.cpp.
//
// A mod item lives only in the save's story block, never in the game's item bytes or bits:
//   kRecordItemCount   (10) key item_key(mod, item): how many Link has, 1..254 (no record: none)
//   kRecordItemPlace   (11) key item_key: where it sits in the Delivery Bag: 1..8 one of the game's
//                           own places (the player chose a free one; shown by the port, the game's
//                           byte stays 0xFF), kFirstPagePlace.. a place on the bag's extra pages
//                           (kFirstPagePlace + page * 8 + slot, page and slot from 0); no record:
//                           not placed (the bag was full)
//   kRecordItemNumber  (12) key item_key: the game item number from the port's pool bound to it
//                           (only while it is needed: a game place, a button, the game's floating icon)
//   kRecordItemButtons (13) key item_key: bit n = on the item button n (X, Y, R, and the two HD buttons)
// Removing the mods leaves these records unread: the game's bag places and buttons are empty.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "story_mail_book.h"

namespace story_items {

inline constexpr uint8_t kRecordItemCount = 10, kRecordItemPlace = 11, kRecordItemNumber = 12, kRecordItemButtons = 13;
// a manifest quest's reward item was given (key quest_key, value 1): once, even if Link gives it away later
inline constexpr uint8_t kRecordQuestRewardItem = 14;
// the story notebook on item buttons (it is no mod item: its number 0xA0 never enters kRecordItemNumber):
// one record, key kNotebookRecordKey, value bit n = on button n
inline constexpr uint8_t kRecordNotebookButtons = 15;
inline constexpr uint16_t kNotebookRecordKey = 1;
inline constexpr uint8_t kMaxCount = 254;
inline constexpr unsigned kGamePlaces = 8, kPageSlots = 8;
inline constexpr unsigned kPages = 4;      // the default soft cap: 4 pages of 8 (Bag's limit, story_items::set_page_limit)
inline constexpr unsigned kMaxPages = 16;  // places of up to 16 pages are valid in a save (a lower limit never loses an item)
inline constexpr uint8_t kFirstPagePlace = 9;
inline constexpr unsigned kButtons = 5;

// The item numbers the port owns for mod items: numbers the game has no handling for (the 2026-10-14
// audit of the item tables, the code's item-number compares and ranges, and the game's stage, event
// and message data; docs/mod-sdk-v2.md lists the findings). 0xA0 / 0xA1 (the notebook, a carried
// letter) are the port's own and not in the pool.
inline constexpr std::array<uint8_t, 19> kPool = {0x1B, 0x1C, 0x1D, 0x40, 0x41, 0x73, 0x74, 0x75, 0x76, 0x81,
                                                  0x8B, 0xA8, 0xA9, 0x84, 0x85, 0x86, 0x87, 0x88, 0x8A};
inline bool pool_number(uint32_t number) {
    for (uint8_t n : kPool)
        if (n == number) return true;
    return false;
}

// FNV-1a over "mod id" NUL "item id", folded to 16 bits (as letter_key); 0 is never used
inline uint16_t item_key(const std::string& mod, const std::string& item) { return story_mail::letter_key(mod, item); }

inline bool game_place(uint8_t place) { return place >= 1 && place <= kGamePlaces; }
inline bool page_place(uint8_t place) { return place >= kFirstPagePlace && place < kFirstPagePlace + kMaxPages * kPageSlots; }
inline uint8_t page_place(unsigned page, unsigned slot) { return uint8_t(kFirstPagePlace + page * kPageSlots + slot); }

// The items of one save file (a view of its record block)
class Bag {
public:
    // `pages`: how many pages new items fill (the soft cap, 1..kMaxPages)
    explicit Bag(story_mail::Book book, unsigned pages = kPages) : b_(book), pages_(pages < 1 ? 1 : pages > kMaxPages ? kMaxPages : pages) {}

    uint8_t count(uint16_t key) const { return b_.get(kRecordItemCount, key); }
    uint8_t place(uint16_t key) const { return b_.get(kRecordItemPlace, key); }
    uint8_t number(uint16_t key) const { return b_.get(kRecordItemNumber, key); }
    uint8_t buttons(uint16_t key) const { return b_.get(kRecordItemButtons, key); }
    // the item a place holds (0: none)
    uint16_t at(uint8_t place) const { return key_with(kRecordItemPlace, place); }
    // the item bound to a pool number (0: none)
    uint16_t with_number(uint8_t number) const { return key_with(kRecordItemNumber, number); }
    // the item on a button (0: none)
    uint16_t on_button(unsigned button) const {
        for (unsigned r = 0; r < b_.records(); r++)
            if (b_.record_kind(r) == kRecordItemButtons && (b_.record_value(r) >> button & 1)) return b_.record_key(r);
        return 0;
    }
    // every item the save has (count records), in record order
    std::vector<uint16_t> owned() const {
        std::vector<uint16_t> out;
        for (unsigned r = 0; r < b_.records(); r++)
            if (b_.record_kind(r) == kRecordItemCount) out.push_back(b_.record_key(r));
        return out;
    }

    // Adds n (up to `stack`). A new item goes to the first free place of the extra pages; `placed`
    // tells whether it has one (false: the pages are full, it is kept without a place). Returns the
    // new count, or -1 when the record block has no room.
    int give(uint16_t key, unsigned n, uint8_t stack, bool& placed) {
        placed = place(key) != 0;
        const unsigned have = count(key), cap = stack ? (stack < kMaxCount ? stack : kMaxCount) : 1;
        const unsigned now = have + n < cap ? have + n : cap;
        if (now == have) return int(have);
        if (!b_.set(kRecordItemCount, key, uint8_t(now))) return -1;
        if (!have) placed = place_on_pages(key);
        return int(now);
    }
    // Takes n away; the last one removes every record of the item (its place, number and buttons
    // become free). Returns the new count, or -1 when Link has fewer than n.
    int take(uint16_t key, unsigned n) {
        const unsigned have = count(key);
        if (n > have) return -1;
        if (n == have) {
            forget(key);
            return 0;
        }
        b_.set(kRecordItemCount, key, uint8_t(have - n));
        return int(have - n);
    }
    void forget(uint16_t key) {
        for (uint8_t kind : {kRecordItemCount, kRecordItemPlace, kRecordItemNumber, kRecordItemButtons}) b_.erase(kind, key);
    }

    // owned items with no place (the pages were full when Link got them)
    std::vector<uint16_t> unplaced() const {
        std::vector<uint16_t> out;
        for (uint16_t k : owned())
            if (!place(k)) out.push_back(k);
        return out;
    }
    // The first free place of the extra pages within the limit (0: none)
    uint8_t free_page_place() const {
        for (unsigned p = 0; p < pages_ * kPageSlots; p++)
            if (!at(uint8_t(kFirstPagePlace + p))) return uint8_t(kFirstPagePlace + p);
        return 0;
    }
    bool place_on_pages(uint16_t key) {
        const uint8_t p = free_page_place();
        return p && b_.set(kRecordItemPlace, key, p);
    }
    // Moves an owned item to a place: one of the game's own places (only an empty one: `game_empty`
    // is what the game's save holds there) or a free place of the pages. False when the place is taken.
    bool move(uint16_t key, uint8_t to, bool game_empty) {
        if (!count(key) || !(game_place(to) || page_place(to))) return false;
        const uint16_t there = at(to);
        if (there && there != key) return false;
        if (game_place(to) && !game_empty) return false;
        return b_.set(kRecordItemPlace, key, to);
    }

    // The pool number of an item, bound now if it has none: a free one, else one taken from an item
    // that needs none right now (no game place, no button; `in_use` names the numbers the game shows
    // at the moment, e.g. the floating icon). 0 when every number is in use.
    template <class InUse>
    uint8_t bind(uint16_t key, InUse&& in_use) {
        if (const uint8_t n = number(key)) return n;
        for (uint8_t n : kPool)
            if (!with_number(n) && b_.set(kRecordItemNumber, key, n)) return n;
        for (uint8_t n : kPool) {
            const uint16_t other = with_number(n);
            if (!other || other == key || game_place(place(other)) || buttons(other) || in_use(n)) continue;
            b_.erase(kRecordItemNumber, other);
            if (b_.set(kRecordItemNumber, key, n)) return n;
        }
        return 0;
    }
    uint8_t bind(uint16_t key) {
        return bind(key, [](uint8_t) { return false; });
    }

    // the notebook's buttons (bit n = button n)
    uint8_t notebook_buttons() const { return b_.get(kRecordNotebookButtons, kNotebookRecordKey); }
    // Puts the notebook on a button (taking any item off it), or takes it off
    bool set_notebook_button(unsigned button, bool on) {
        if (button >= kButtons) return false;
        if (on) clear_button(button, 0);
        const uint8_t now = uint8_t(on ? notebook_buttons() | 1u << button : notebook_buttons() & ~(1u << button));
        return now ? b_.set(kRecordNotebookButtons, kNotebookRecordKey, now) : (b_.erase(kRecordNotebookButtons, kNotebookRecordKey), true);
    }
    // Puts an item on a button (taking any other item and the notebook off it), or takes it off (on false)
    bool set_button(uint16_t key, unsigned button, bool on) {
        if (button >= kButtons || !count(key)) return false;
        if (on) {
            clear_button(button, key);
            if (const uint8_t nb = notebook_buttons() & ~(1u << button); nb != notebook_buttons())
                nb ? b_.set(kRecordNotebookButtons, kNotebookRecordKey, nb) : b_.erase(kRecordNotebookButtons, kNotebookRecordKey);
        }
        const uint8_t now = uint8_t(on ? buttons(key) | 1u << button : buttons(key) & ~(1u << button));
        return now ? b_.set(kRecordItemButtons, key, now) : (b_.erase(kRecordItemButtons, key), true);
    }

private:
    // takes every item but `keep` off a button
    void clear_button(unsigned button, uint16_t keep) {
            for (unsigned r = 0; r < b_.records(); r++)
                if (b_.record_kind(r) == kRecordItemButtons && b_.record_key(r) != keep && (b_.record_value(r) >> button & 1)) {
                    const uint16_t other = b_.record_key(r);
                    const uint8_t rest = uint8_t(b_.record_value(r) & ~(1u << button));
                    if (rest) b_.set(kRecordItemButtons, other, rest);
                    else b_.erase(kRecordItemButtons, other);
                }
    }
    uint16_t key_with(uint8_t kind, uint8_t value) const {
        if (!value) return 0;
        for (unsigned r = 0; r < b_.records(); r++)
            if (b_.record_kind(r) == kind && b_.record_value(r) == value) return b_.record_key(r);
        return 0;
    }
    story_mail::Book b_;
    unsigned pages_;
};

}  // namespace story_items
