// story_bag_page_test: the Delivery Bag's extra pages (runtime/src/mods/story_bag_page.h): paging with L / R,
// the cursor in the window's three columns, the item menu, choosing one of the game's places (only empty
// ones to put an item, only mod items to bring one back), touch, and the page count.
#include <cassert>
#include <cstdio>

#include "mods/story_bag_page.h"

using namespace story_bag;

namespace {
Model model(unsigned pages, unsigned items) {
    Model m;
    m.pages.resize(pages);
    for (unsigned i = 0; i < items; i++) m.pages[i / kSlots][i % kSlots] = {uint16_t(0x100 + i), uint8_t(1 + i % 3), i % 2 == 0};
    m.game = {kGameItem, kGameItem, 0, kGameItem, 0, kGameItem, kGameItem, kGameItem};
    return m;
}
Sound g_sound = Sound::None;  // the sound of the last press
// one press (the button down, then up)
Action press(Page& p, const Model& m, uint32_t button) {
    const Action a = p.input({button}, m);
    g_sound = p.sound();
    p.input({0}, m);
    return a;
}
Action tap(Page& p, const Model& m, float x, float y) {
    Input in;
    in.touch = true, in.x = x, in.y = y;
    p.input(in, m);
    return p.input({}, m);  // a tap acts when it is released
}
}  // namespace

int main() {
    // page count: up to the last page that holds an item, at least one
    assert(pages_shown({}) == 1);
    assert(pages_shown({0, 0}) == 1);
    assert(pages_shown({0, 2}) == 3);
    assert(pages_shown({5}) == 6);  // beyond a lowered limit: still shown (never lose an item)

    // grid: three columns of three, Back in the last column's bottom row
    assert(column_of(kBack) == 2 && row_of(kBack) == 2);
    assert(cell_at(1, 2) == 5);

    Model m = model(2, 12);
    Page p;
    assert(!p.open());
    assert(p.input({kPadA}, m).kind == Action::None);  // closed: nothing

    // opened with R held: the held R is not a press on the page
    p.open(kPadR);
    assert(p.open() && p.page() == 0 && p.cursor() == 0);
    assert(p.input({kPadR}, m).kind == Action::None && p.page() == 0);
    p.input({0}, m);
    // R turns, at the last page it stays (error sound)
    press(p, m, kPadR);
    assert(p.page() == 1);
    p.input({kPadR}, m);
    assert(p.page() == 1 && p.sound() == Sound::Error);
    p.input({0}, m);
    // L turns back, on the first extra page L goes back to the game's page
    p.input({kPadL}, m);
    assert(p.page() == 0 && p.turned() == -1 && p.sound() == Sound::Turn);
    p.input({0}, m);
    Action a = press(p, m, kPadL);
    assert(a.kind == Action::Close && !p.open());

    // B goes back to the game's page
    p.open(0);
    assert(press(p, m, kPadB).kind == Action::Close && !p.open());

    // the cursor: down the column, right across, not out of the grid
    p.open(0);
    press(p, m, kPadUp);
    assert(p.cursor() == 0);
    press(p, m, kPadDown);
    press(p, m, kPadDown);
    press(p, m, kPadDown);
    assert(p.cursor() == 2);
    press(p, m, kPadRight);
    press(p, m, kPadRight);
    press(p, m, kPadRight);
    assert(p.cursor() == kBack);
    assert(press(p, m, kPadA).kind == Action::Close);  // A on Back

    // X / Y on an item: it goes on that button; on an empty place nothing
    p.open(0);
    a = press(p, m, kPadY);
    assert(a.kind == Action::Assign && a.key == 0x100 && a.slot == 1 && p.mode() == Page::Mode::Browse);
    press(p, m, kPadRight);
    press(p, m, kPadRight);
    press(p, m, kPadDown);
    press(p, m, kPadDown);  // Back
    assert(press(p, m, kPadX).kind == Action::None && g_sound == Sound::Error);
    p.close();

    // A on an item: its menu (Info, Use for usable items, Move, Cancel)
    p.open(0);
    press(p, m, kPadA);
    assert(p.mode() == Page::Mode::Menu);
    assert(p.options().size() == 4 && p.options()[1] == Page::Option::Use);  // item 0 is usable
    a = press(p, m, kPadA);
    assert(a.kind == Action::Info && a.key == 0x100 && p.mode() == Page::Mode::Browse);
    press(p, m, kPadDown);  // item 1: not usable
    press(p, m, kPadA);
    assert(p.options().size() == 3 && p.options()[1] == Page::Option::Move);
    press(p, m, kPadB);
    assert(p.mode() == Page::Mode::Browse);

    // Move: choosing one of the game's places; only the empty ones take the item
    press(p, m, kPadA);
    press(p, m, kPadDown);
    press(p, m, kPadA);
    assert(p.mode() == Page::Mode::Pick && p.pick_kind() == Page::PickKind::Put && p.pick_key() == 0x101);
    assert(p.cursor() == 2);  // the first empty place
    press(p, m, kPadUp);      // place 1 holds a game item
    assert(press(p, m, kPadA).kind == Action::None && g_sound == Sound::Error && p.mode() == Page::Mode::Pick);
    press(p, m, kPadDown);
    a = press(p, m, kPadA);
    assert(a.kind == Action::Put && a.key == 0x101 && a.place == 2 && !p.open());  // back to the game's window

    // with no empty game place, Move is shown but refused
    Model full = m;
    full.game.fill(kGameItem);
    p.open(0);
    press(p, full, kPadA);
    press(p, full, kPadDown);
    press(p, full, kPadDown);
    assert(p.options()[2] == Page::Option::Move && !p.option_enabled(2, full));
    press(p, full, kPadA);
    assert(p.mode() == Page::Mode::Menu && g_sound == Sound::Error);
    press(p, full, kPadDown);
    press(p, full, kPadA);  // Cancel
    assert(p.mode() == Page::Mode::Browse);

    // A on an empty place brings a mod item back from the game's places (only places holding one)
    Model back = model(2, 12);
    back.game[4] = 0x1FF;  // a mod item in the game's place 4
    p.open(0);
    press(p, back, kPadR);  // page 2: places 4..7 empty
    press(p, back, kPadRight);
    press(p, back, kPadDown);  // cell 4
    assert(p.selected(back).key == 0);
    press(p, back, kPadA);
    assert(p.mode() == Page::Mode::Pick && p.pick_kind() == Page::PickKind::Take && p.cursor() == 4);
    a = press(p, back, kPadA);
    assert(a.kind == Action::Take && a.key == 0x1FF && a.place == 4 && a.slot == 4 && a.page == 1);
    assert(p.mode() == Page::Mode::Browse);
    // without mod items in the game's places, A on an empty place does nothing
    press(p, m, kPadA);
    assert(p.mode() == Page::Mode::Browse && g_sound == Sound::Error);
    // B leaves the choosing
    press(p, back, kPadA);
    assert(p.mode() == Page::Mode::Pick);
    press(p, back, kPadB);
    assert(p.mode() == Page::Mode::Browse && p.open());

    // touch: a tap selects a place, a second tap acts as A; a tap on a dot turns to its page
    p.close();
    p.open(0);
    p.input({}, m);  // the touch that was down at the open is released
    tap(p, m, kPlaceX[3], kPlaceY[3]);
    assert(p.cursor() == 3 && p.mode() == Page::Mode::Browse);
    tap(p, m, kPlaceX[3], kPlaceY[3]);
    assert(p.mode() == Page::Mode::Menu);
    tap(p, m, 0, Page::option_y(0));  // the menu's first option: Info
    assert(p.mode() == Page::Mode::Browse);
    tap(p, m, Page::dot_x(2, 3), kDotY);  // dot 2: the second extra page
    assert(p.page() == 1);
    a = tap(p, m, Page::dot_x(0, 3), kDotY);  // dot 0: the game's page
    assert(a.kind == Action::Close && !p.open());
    // a drag (towards the item buttons) is no tap: nothing happens on the page
    p.open(0);
    p.input({}, m);
    {
        Input down;
        down.touch = true, down.x = kPlaceX[0], down.y = kPlaceY[0];
        p.input(down, m);
        down.x += 120;
        p.input(down, m);
        assert(p.input({}, m).kind == Action::None && p.mode() == Page::Mode::Browse && p.cursor() == 0);
    }
    // a touch held while the page opens is no tap when it is released
    p.close();
    {
        Input held;
        held.touch = true, held.x = kPlaceX[2], held.y = kPlaceY[2];
        p.open(0);
        p.input(held, m);
        p.input({}, m);
        assert(p.cursor() == 0 && p.mode() == Page::Mode::Browse);
    }
    p.close();
    assert(Page::hit(1000, 1000) == -1);
    assert(Page::hit(kPlaceX[kBack], kPlaceY[kBack]) == kBack);

    // a model that lost a page (items moved away): the page index follows
    p.open(0);
    press(p, m, kPadR);
    assert(p.page() == 1);
    Model one = model(1, 3);
    p.input({0}, one);
    assert(p.page() == 0);

    std::puts("story_bag_page: ok");
    return 0;
}
