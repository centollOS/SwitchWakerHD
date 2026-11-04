#include "file_select_quit.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <utility>

static void view_routing() {
    using namespace file_select_quit;
    // Normal and HiDPI logical canvases have the same normalized hit region.
    for (auto view : {View{1920, 1080, {0, 0, 1920, 1080}}, View{960, 540, {0, 0, 960, 540}}}) {
        assert(view.contains(.41f, .85f));
        assert(view.contains(.59f, .97f));
        assert(!view.contains(.4f, .91f));
        assert(!view.contains(.5f, .99f));
    }
    // A 4:3 window has bars above and below the TV. The old fixed-720
    // hit region missed the visible button and captured clicks in the lower bar.
    const View letterboxed{1280, 960, {0, 120, 1280, 720}};
    assert(letterboxed.contains(.41f, .76f));
    assert(letterboxed.contains(.59f, .85f));
    assert(!letterboxed.contains(.5f, .91f));
    // An ultrawide window must not capture input beside the narrower TV plate.
    const View ultrawide{2560, 1080, {320, 0, 1920, 1080}};
    assert(ultrawide.contains(.45f, .91f));
    assert(!ultrawide.contains(.41f, .91f));
    assert(!ultrawide.contains(.59f, .91f));
    // Android portrait and split-screen TV rectangles can occupy only part
    // of the host window; the lower GamePad/touch area remains outside Quit.
    const View portrait{1080, 1920, {0, 0, 1080, 607.5f}};
    assert(portrait.contains(.5f, .2875f));
    assert(!portrait.contains(.5f, .91f));
    const View split{1920, 1080, {0, 270, 960, 540}};
    assert(split.contains(.25f, .70f));
    assert(!split.contains(.75f, .70f));
    assert((!View{1280, 720, {0, 0, 0, 0}}.contains(.5f, .91f)));
}

static void dialog_routing() {
    using namespace file_select_quit;
    Routing routing;
    // With no Quit press, the real routing policy preserves every other field.
    input::PadState pad{input::kA | input::kB | input::kY, .25f, -.5f, .75f, 1, true, .4f, .6f};
    const auto original = pad;
    auto event = routing.route(pad, true, true, false);
    assert(!event.open && !event.dismiss && event.pressed == 0);
    assert(pad.buttons == original.buttons && pad.lx == original.lx && pad.ly == original.ly && pad.rx == original.rx &&
           pad.ry == original.ry && pad.touch == original.touch && pad.tx == original.tx && pad.ty == original.ty);
    pad = {};
    routing.route(pad, true, true, false);
    pad.buttons = input::kX;
    event = routing.route(pad, true, true, false);
    assert(event.open && pad.buttons == 0);
    pad.buttons = input::kB;
    event = routing.route(pad, true, true, true);
    assert(event.pressed == input::kB && pad.buttons == 0);
    // B that cancelled the dialog must not also go Back in the native menu.
    pad.buttons = input::kB;
    routing.route(pad, true, true, false);
    assert(pad.buttons == 0);
    // A hidden or disabled UI cannot leave that release gate capturing input.
    for (bool shown : {false, true}) {
        pad = original;
        pad.buttons |= input::kX;
        const auto before = pad;
        event = routing.route(pad, false, shown, true);
        assert(event.dismiss && !event.open && event.pressed == 0);
        assert(pad.buttons == before.buttons && pad.lx == before.lx && pad.touch == before.touch);
    }
    // A stale presentation must pass inputs unchanged without dismissing an
    // otherwise eligible confirmation (for example while saving a large PNG).
    pad = original;
    event = routing.route(pad, true, false, true);
    assert(!event.dismiss && !event.open && event.pressed == 0 && pad.buttons == original.buttons);
    pad = original;
    event = routing.route(pad, false, true, true);
    assert(event.dismiss && pad.buttons == original.buttons);
}


// Quit in the game's selection (issue #315): Down from Quest Log 3 and Up from Quest Log 1 select Quit, Down /
// Up from Quit go on to Quest Log 1 / 3, A on Quit asks, and the game sees none of those presses.
static void selector_routing() {
    using namespace file_select_quit;
    using R = Routing;
    auto step = [](Routing &r, uint32_t buttons, int focus, float ly = 0, bool shown = true, bool eligible = true,
                   bool confirming = false) {
        input::PadState pad{buttons, 0, ly};
        auto event = r.route(pad, eligible, shown, confirming, focus);
        return std::make_pair(event, pad);
    };
    Routing r;
    // the game moves between the slots itself
    auto [e, pad] = step(r, input::kDown, 0);
    assert(e.focus == R::kKeep && pad.buttons == input::kDown && !r.quit_selected());
    step(r, 0, 1);
    // Down on Quest Log 3: Quit, and the press stays from the game (no wrap to Quest Log 1)
    std::tie(e, pad) = step(r, input::kDown, 2);
    assert(e.focus == R::kClear && e.moved && r.quit_selected() && pad.buttons == 0);
    std::tie(e, pad) = step(r, input::kDown, 2);  // held, the guest side not there yet
    assert(e.focus == R::kKeep && r.quit_selected() && pad.buttons == 0);
    std::tie(e, pad) = step(r, 0, R::kClear);
    assert(r.quit_selected() && pad.buttons == 0);
    // A on Quit asks (and is the dialog's); B still goes back in the game's menu
    std::tie(e, pad) = step(r, input::kA, R::kClear);
    assert(e.open && pad.buttons == 0);
    std::tie(e, pad) = step(r, input::kA, R::kClear, 0, true, true, true);
    assert(!e.open && pad.buttons == 0);
    std::tie(e, pad) = step(r, 0, R::kClear, 0, true, true, true);
    std::tie(e, pad) = step(r, 0, R::kClear);  // answered No: Quit keeps the selector
    assert(r.quit_selected());
    std::tie(e, pad) = step(r, input::kB, R::kClear);
    assert(pad.buttons == input::kB && r.quit_selected());
    step(r, 0, R::kClear);
    // Down from Quit: Quest Log 1; the held press stays from the game until released
    std::tie(e, pad) = step(r, input::kDown, R::kClear);
    assert(e.focus == 0 && e.moved && !r.quit_selected() && pad.buttons == 0);
    std::tie(e, pad) = step(r, input::kDown, 0);
    assert(e.focus == R::kKeep && pad.buttons == 0);
    std::tie(e, pad) = step(r, 0, 0);
    // Up on Quest Log 1 (the stick too): Quit; Up from Quit: Quest Log 3
    std::tie(e, pad) = step(r, 0, 0, .9f);
    assert(e.focus == R::kClear && r.quit_selected() && pad.ly == 0);
    step(r, 0, R::kClear);
    std::tie(e, pad) = step(r, input::kUp, R::kClear);
    assert(e.focus == 2 && !r.quit_selected() && pad.buttons == 0);
    step(r, 0, 2);
    // a stick held down through the slots stops on Quest Log 3 (the game's repeat would wrap past Quit)
    std::tie(e, pad) = step(r, 0, 1, -.9f);
    assert(pad.ly < 0);
    std::tie(e, pad) = step(r, 0, 2, -.9f);
    assert(e.focus == R::kKeep && !r.quit_selected() && pad.ly == 0);
    step(r, 0, 2);
    // the game's selector taking a slot itself (a click on a Quest Log) ends Quit's selection
    std::tie(e, pad) = step(r, input::kDown, 2);
    assert(r.quit_selected());
    step(r, 0, R::kClear);
    std::tie(e, pad) = step(r, 0, 1);
    assert(!r.quit_selected() && e.focus == R::kKeep);
    // a presentation stall keeps Quit's selection and its inputs; losing the context gives the game its selector back
    step(r, input::kDown, 2);
    step(r, 0, R::kClear);
    std::tie(e, pad) = step(r, input::kDown | input::kA, R::kClear, 0, false);
    assert(r.quit_selected() && pad.buttons == 0 && !e.dismiss);
    std::tie(e, pad) = step(r, 0, R::kClear, 0, true, false);
    assert(e.dismiss && e.focus == 2 && !r.quit_selected());
    // without the selector's state (kUnknown) nothing changes: the X / mouse Quit only
    Routing plain;
    input::PadState down{input::kDown};
    assert(plain.route(down, true, true, false).focus == R::kKeep && down.buttons == input::kDown);
}

int main() {
    view_routing();
    dialog_routing();
    selector_routing();
    using namespace file_select_quit;
    for (bool setting : {false, true})
        for (bool idle : {false, true}) {
            Routing routing;
            input::PadState pad{
                input::kA | input::kB | input::kX | input::kY | input::kPlus, .25f, -.5f, .75f, 1, true, .4f, .6f};
            const auto original = pad;
            const bool visible = setting && idle;
            assert(routing.filter(pad, visible) == visible);
            auto expected = original;
            if (visible)
                expected.buttons &= ~input::kX;
            assert(pad.buttons == expected.buttons && pad.lx == expected.lx && pad.ly == expected.ly &&
                   pad.rx == expected.rx && pad.ry == expected.ry && pad.touch == expected.touch &&
                   pad.tx == expected.tx && pad.ty == expected.ty);
            pad = original;
            assert(!routing.filter(pad, visible));
            pad = {};
            assert(!routing.filter(pad, visible));
            pad = original;
            assert(routing.filter(pad, visible) == visible);
        }
    // A button held before the UI appears does not activate Quit.
    Routing routing;
    input::PadState pad{input::kX};
    assert(!routing.filter(pad, false));
    assert(pad.buttons == input::kX);
    assert(!routing.filter(pad, true));
    assert(pad.buttons == 0);
    // Turning the option off immediately restores every input, including X.
    pad.buttons = input::kX;
    assert(!routing.filter(pad, false));
    assert(pad.buttons == input::kX);
    puts("file_select_quit_test: passed");
}
