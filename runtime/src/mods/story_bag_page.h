// The Delivery Bag's extra pages (mod items, docs/mod-sdk-v2.md "Items"): what the pages show and how the
// player moves through them. Pure code: input in, state and actions out; unit-tested in
// runtime/tools/story_bag_page_test.cpp. The driver (story_bag_page.cpp) reads the save's items
// (story_items.h), carries the actions out and draws the state with the layout StoryBag_00
// (tools/story_mods/make_bag_page_layout.py).
//
// The pages lie over the game's Delivery Bag window, in its look: eight places in the window's three
// columns (places 0..7 as the game's L_BtnItemIcon_19..26, the menu's children 0x14..0x1B), the Back button in the last column's bottom
// row (index 8 of the grid), page dots above (the game's eight places are the first dot).
//
//   Game page  the game's own window. R opens the first extra page (only when Link has mod items).
//   Browse     an extra page. The cross moves the cursor, X / Y put the item on that button (an item with
//              a use action; story_items decides), L / R turn the pages (L on the first extra page
//              goes back to the game's page), B goes back to the game's page, A on an item opens its menu,
//              A on an empty place brings an item there from the game's places (when one holds a mod
//              item), A on Back goes back. Touch: a tap (released where it began; a drag is left to the
//              item buttons) selects, a tap on the selected place acts as A; a tap on a page dot turns
//              to that page.
//   Menu       Info, Use (items with a use action), Move to the first page (only into an empty place of the
//              game's eight), Cancel. Up / Down, A, B (back to Browse).
//   Pick       choosing one of the game's eight places: the extra page is hidden, the game's window shows
//              underneath with our cursor over its places (the same places as ours). Putting an item there
//              takes only an empty place; bringing one back takes a place that holds a mod item. A
//              chooses, B (or A on Back) goes back to Browse.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace story_bag {

inline constexpr unsigned kSlots = 8;
inline constexpr int kBack = 8;  // the grid's ninth cell: the Back button
inline constexpr uint32_t kPadA = 0x8000, kPadB = 0x4000, kPadX = 0x2000, kPadY = 0x1000, kPadL = 0x0020, kPadR = 0x0010,
                          kPadUp = 0x0200, kPadDown = 0x0100, kPadLeft = 0x0800, kPadRight = 0x0400;
inline constexpr uint16_t kGameItem = 0xFFFF;  // Model::game: one of the game's own items

// the window's places (and Back) relative to its centre, layout units (y up): MenuItem_00's bag window
inline constexpr float kPlaceX[9] = {-131, -175, -127, -3, -47, 1, 125, 81, 129};
inline constexpr float kPlaceY[9] = {130, -6, -142, 130, -6, -142, 130, -6, -142};
inline constexpr float kTapRadius = 62, kTapSlop = 24;
inline constexpr float kDotY = 210, kDotStep = 24;
inline constexpr float kMenuTop = 72, kMenuRow = 48, kMenuHalfWidth = 170;

// The three columns, top to bottom (the grid cell of each place)
inline int column_of(int cell) { return cell / 3; }
inline int row_of(int cell) { return cell % 3; }
inline int cell_at(int column, int row) { return column * 3 + row; }

struct Entry {
    uint16_t key = 0;   // 0: the place is empty
    uint8_t count = 0;
    bool usable = false;  // it has a use action
};

// What the save holds now (the driver builds it from story_items)
struct Model {
    std::vector<std::array<Entry, kSlots>> pages;  // the extra pages that exist (pages_shown)
    std::array<uint16_t, kSlots> game{};           // the game's places: 0 empty, kGameItem, else a mod item's key
    unsigned unplaced = 0;                         // owned items without a place (the pages are full)
    bool any() const {
        for (const auto& p : pages)
            for (const Entry& e : p)
                if (e.key) return true;
        return false;
    }
    bool game_has_empty() const {
        for (uint16_t g : game)
            if (!g) return true;
        return false;
    }
    bool game_has_mod_item() const {
        for (uint16_t g : game)
            if (g && g != kGameItem) return true;
        return false;
    }
};

// How many extra pages to show for the pages that hold items (page indices from 0): up to the last one
// that holds an item, at least one.
inline unsigned pages_shown(const std::vector<unsigned>& used_pages) {
    unsigned n = 1;
    for (unsigned p : used_pages) n = std::max(n, p + 1);
    return n;
}

enum class Sound { None, Cursor, Decide, Cancel, Open, Close, Turn, Error };

struct Action {
    enum Kind { None, Close, Info, Use, Put, Take, Assign } kind = None;
    uint16_t key = 0;
    int place = -1;   // Put: the game's place (0..7); Take: the game's place it comes from
    int slot = -1;    // Take: the page place it goes to (0..7); Assign: the item button (0 X, 1 Y)
    unsigned page = 0;
};

struct Input {
    uint32_t buttons = 0;  // held (VPAD bits)
    bool touch = false;
    float x = 0, y = 0;    // the touch relative to the window's centre, layout units (y up)
};

class Page {
public:
    enum class Mode { Closed, Browse, Menu, Pick };
    enum class Option { Info, Use, Move, Cancel };
    enum class PickKind { Put, Take };

    Mode mode() const { return mode_; }
    bool open() const { return mode_ != Mode::Closed; }
    unsigned page() const { return page_; }
    int cursor() const { return mode_ == Mode::Pick ? pick_cursor_ : cursor_; }
    int menu_cursor() const { return menu_cursor_; }
    const std::vector<Option>& options() const { return options_; }
    bool option_enabled(size_t i, const Model& m) const { return i < options_.size() && enabled(options_[i], m); }
    PickKind pick_kind() const { return pick_kind_; }
    uint16_t pick_key() const { return pick_key_; }
    int turned() const { return turned_; }  // -1 / +1: the page was turned this input (its animation), else 0

    // Opens an extra page (R in the game's window: the first; a tap on its dot). `held`: the buttons held
    // now (released first).
    void open(uint32_t held, unsigned page = 0) {
        mode_ = Mode::Browse, page_ = page, cursor_ = 0, prev_ = held, touch_prev_ = true, touch_moved_ = true;
    }
    void close() { mode_ = Mode::Closed, turned_ = 0; }

    Action input(const Input& in, const Model& m) {
        Action act;
        turned_ = 0;
        const uint32_t pressed = in.buttons & ~prev_;
        prev_ = in.buttons;
        // a tap: a touch released near where it began (a drag, e.g. towards the X / Y / R buttons, is none),
        // at the place it began
        if (in.touch && !touch_prev_) touch_x_ = in.x, touch_y_ = in.y, touch_moved_ = false;
        if (in.touch && std::hypot(in.x - touch_x_, in.y - touch_y_) > kTapSlop) touch_moved_ = true;
        const bool tap = !in.touch && touch_prev_ && !touch_moved_;
        touch_prev_ = in.touch;
        Input at = in;
        if (tap) at.x = touch_x_, at.y = touch_y_;
        sound_ = Sound::None;
        if (mode_ == Mode::Closed) return act;
        const unsigned pages = std::max<unsigned>(1, unsigned(m.pages.size()));
        if (page_ >= pages) page_ = pages - 1;
        switch (mode_) {
        case Mode::Browse: browse(pressed, tap, at, m, pages, act); break;
        case Mode::Menu: menu(pressed, tap, at, m, act); break;
        case Mode::Pick: pick(pressed, tap, at, m, act); break;
        case Mode::Closed: break;
        }
        return act;
    }
    Sound sound() const { return sound_; }

    // the entry under the cursor on the current page (Browse / Menu), key 0 if none
    Entry selected(const Model& m) const {
        if (mode_ == Mode::Pick || cursor_ >= int(kSlots) || page_ >= m.pages.size()) return {};
        return m.pages[page_][size_t(cursor_)];
    }
    // the place a tap at (x, y) hits (0..7, kBack), -1 if none
    static int hit(float x, float y) {
        for (int i = 0; i <= kBack; i++)
            if (std::hypot(x - kPlaceX[i], y - kPlaceY[i]) < kTapRadius) return i;
        return -1;
    }
    // the dot a tap hits (0: the game's page, 1.. the extra pages), -1 if none
    static int hit_dot(float x, float y, unsigned pages) {
        const unsigned dots = pages + 1;
        if (std::fabs(y - kDotY) > 16) return -1;
        for (unsigned i = 0; i < dots; i++)
            if (std::fabs(x - dot_x(i, dots)) < kDotStep / 2) return int(i);
        return -1;
    }
    static float dot_x(unsigned i, unsigned dots) { return (float(i) - float(dots - 1) / 2) * kDotStep; }
    // the menu's options (the panel in the window's centre, make_bag_page_layout.py: 4 rows of 48, 340 wide)
    static float option_y(int i) { return kMenuTop - kMenuRow * float(i); }
    static int hit_option(float x, float y, size_t n) {
        if (std::fabs(x) > kMenuHalfWidth) return -1;
        for (int i = 0; i < int(n); i++)
            if (std::fabs(y - option_y(i)) < kMenuRow / 2) return i;
        return -1;
    }

private:
    static bool enabled(Option o, const Model& m) { return o != Option::Move || m.game_has_empty(); }

    static int move(int cell, uint32_t pressed) {
        int c = column_of(cell), r = row_of(cell);
        if (pressed & kPadUp) r = std::max(0, r - 1);
        else if (pressed & kPadDown) r = std::min(2, r + 1);
        else if (pressed & kPadLeft) c = std::max(0, c - 1);
        else if (pressed & kPadRight) c = std::min(2, c + 1);
        return cell_at(c, r);
    }
    static constexpr uint32_t kCross = kPadUp | kPadDown | kPadLeft | kPadRight;

    void turn(int dir) { turned_ = dir, sound_ = Sound::Turn; }

    void browse(uint32_t pressed, bool tap, const Input& in, const Model& m, unsigned pages, Action& act) {
        if (tap) {
            const int dot = hit_dot(in.x, in.y, pages);
            if (dot == 0) return close_to_game(act);
            if (dot > 0 && unsigned(dot - 1) != page_) {
                turn(unsigned(dot - 1) > page_ ? 1 : -1);
                page_ = unsigned(dot - 1);
                return;
            }
            const int at = hit(in.x, in.y);
            if (at >= 0 && at != cursor_) {
                cursor_ = at, sound_ = Sound::Cursor;
                return;
            }
            if (at >= 0) pressed |= kPadA;  // a tap on the selected place
        }
        if (pressed & kPadL) {
            if (page_ == 0) return close_to_game(act);
            page_--, turn(-1);
            return;
        }
        if (pressed & kPadR) {
            if (page_ + 1 < pages) page_++, turn(1);
            else sound_ = Sound::Error;
            return;
        }
        if (pressed & kPadB) return close_to_game(act);
        if (pressed & (kPadX | kPadY)) {  // X / Y: the item goes on that button (R turns the pages here)
            const Entry e = selected(m);
            if (!e.key) {
                sound_ = Sound::Error;
                return;
            }
            act.kind = Action::Assign, act.key = e.key, act.slot = (pressed & kPadX) ? 0 : 1, sound_ = Sound::Decide;
            return;
        }
        if (pressed & kCross) {
            const int to = move(cursor_, pressed);
            if (to != cursor_) cursor_ = to, sound_ = Sound::Cursor;
            return;
        }
        if (!(pressed & kPadA)) return;
        if (cursor_ == kBack) return close_to_game(act);
        const Entry e = selected(m);
        if (e.key) {
            options_ = {Option::Info};
            if (e.usable) options_.push_back(Option::Use);
            options_.push_back(Option::Move);
            options_.push_back(Option::Cancel);
            menu_cursor_ = 0, mode_ = Mode::Menu, sound_ = Sound::Decide;
            return;
        }
        if (m.game_has_mod_item()) return start_pick(PickKind::Take, 0, m);
        sound_ = Sound::Error;
    }

    void menu(uint32_t pressed, bool tap, const Input& in, const Model& m, Action& act) {
        if (tap) {  // a tap on an option chooses it, one outside the menu closes it
            const int at = hit_option(in.x, in.y, options_.size());
            if (at < 0) pressed |= kPadB;
            else menu_cursor_ = at, pressed |= kPadA;
        }
        if (pressed & kPadB) {
            mode_ = Mode::Browse, sound_ = Sound::Cancel;
            return;
        }
        if (pressed & (kPadUp | kPadDown)) {
            const int n = int(options_.size());
            const int to = std::clamp(menu_cursor_ + ((pressed & kPadUp) ? -1 : 1), 0, n - 1);
            if (to != menu_cursor_) menu_cursor_ = to, sound_ = Sound::Cursor;
            return;
        }
        if (!(pressed & kPadA) || options_.empty()) return;
        const Entry e = selected(m);
        const Option o = options_[size_t(menu_cursor_)];
        if (!e.key || o == Option::Cancel) {
            mode_ = Mode::Browse, sound_ = Sound::Cancel;
            return;
        }
        if (!enabled(o, m)) {
            sound_ = Sound::Error;
            return;
        }
        switch (o) {
        case Option::Info: act.kind = Action::Info, act.key = e.key, mode_ = Mode::Browse, sound_ = Sound::Decide; break;
        case Option::Use: act.kind = Action::Use, act.key = e.key, mode_ = Mode::Browse, sound_ = Sound::Decide; break;
        case Option::Move: start_pick(PickKind::Put, e.key, m); break;
        case Option::Cancel: break;
        }
    }

    bool allowed(int place, const Model& m) const {
        if (place < 0 || place >= int(kSlots)) return false;
        const uint16_t g = m.game[size_t(place)];
        return pick_kind_ == PickKind::Put ? g == 0 : (g != 0 && g != kGameItem);
    }
    void start_pick(PickKind kind, uint16_t key, const Model& m) {
        pick_kind_ = kind, pick_key_ = key, mode_ = Mode::Pick, sound_ = Sound::Decide;
        pick_cursor_ = kBack;
        for (int i = 0; i < int(kSlots); i++)
            if (allowed(i, m)) {
                pick_cursor_ = i;
                break;
            }
    }
    void pick(uint32_t pressed, bool tap, const Input& in, const Model& m, Action& act) {
        if (tap) {
            const int at = hit(in.x, in.y);
            if (at >= 0 && at != pick_cursor_) {
                pick_cursor_ = at, sound_ = Sound::Cursor;
                return;
            }
            if (at >= 0) pressed |= kPadA;
        }
        if (pressed & kPadB) {
            mode_ = Mode::Browse, sound_ = Sound::Cancel;
            return;
        }
        if (pressed & kCross) {
            const int to = move(pick_cursor_, pressed);
            if (to != pick_cursor_) pick_cursor_ = to, sound_ = Sound::Cursor;
            return;
        }
        if (!(pressed & kPadA)) return;
        if (pick_cursor_ == kBack) {
            mode_ = Mode::Browse, sound_ = Sound::Cancel;
            return;
        }
        if (!allowed(pick_cursor_, m)) {
            sound_ = Sound::Error;
            return;
        }
        if (pick_kind_ == PickKind::Put) {
            act.kind = Action::Put, act.key = pick_key_, act.place = pick_cursor_, act.page = page_;
            mode_ = Mode::Closed, sound_ = Sound::Decide;  // the item shows in the game's window
        } else {
            act.kind = Action::Take, act.key = m.game[size_t(pick_cursor_)], act.place = pick_cursor_, act.slot = cursor_,
            act.page = page_;
            mode_ = Mode::Browse, sound_ = Sound::Decide;
        }
    }
    void close_to_game(Action& act) {
        act.kind = Action::Close, mode_ = Mode::Closed, sound_ = Sound::Close, turned_ = -1;
    }

    Mode mode_ = Mode::Closed;
    unsigned page_ = 0;
    int cursor_ = 0, menu_cursor_ = 0, pick_cursor_ = 0;
    std::vector<Option> options_;
    PickKind pick_kind_ = PickKind::Put;
    uint16_t pick_key_ = 0;
    uint32_t prev_ = 0;
    bool touch_prev_ = false, touch_moved_ = false;
    float touch_x_ = 0, touch_y_ = 0;
    int turned_ = 0;
    Sound sound_ = Sound::None;
};

}  // namespace story_bag
