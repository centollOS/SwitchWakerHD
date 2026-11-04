// SPDX-License-Identifier: MPL-2.0
// one_screen.h: the One screen (handheld) mode's rules.
#include "one_screen.h"

#include <cassert>
#include <cstdio>

using namespace one_screen;

int main() {
    // defaults: Android on, unless the player chose the Pro Controller; off elsewhere (an option)
    assert(default_on(true, false) && !default_on(true, true) && !default_on(false, false));
    // the screen mode: TV play asked for -> Off-TV Play; "both" stored -> Off-TV Play; only while active
    assert(requested_mode(true, kTvPlay) == kOffTv && requested_mode(true, kTvOnly) == kTvOnly);
    assert(requested_mode(true, kOffTv) == kOffTv && requested_mode(false, kTvPlay) == kTvPlay);
    assert(stored_mode(true, kBoth) == kOffTv && stored_mode(false, kBoth) == kBoth && stored_mode(true, kTvPlay) == kTvPlay);
    // Minus: never the game's; a short press is Plus when let go; a held one (Settings) is nothing
    {
        MinusAsPlus m;
        uint64_t t = 10;
        assert(m.filter(kMinus, t++) == 0);
        assert(m.filter(kMinus, t++) == 0);
        assert(m.filter(0, t++) == kPlus);   // let go after 2 steps
        assert(m.filter(0, t++) == kPlus);
        assert(m.filter(0, t++) == 0);
        for (uint64_t i = 0; i < kSettingsHoldSteps + 2; i++) assert(m.filter(kMinus | 0x8000, t++) == 0x8000);
        assert(m.filter(0, t++) == 0 && m.filter(0, t++) == 0);  // the Settings hold: no Plus
        assert(m.filter(kPlus, t++) == kPlus);  // Plus itself stays Plus
        assert(m.filter(kMinus | kPlus, t++) == kPlus);
    }
    // touches on the picture
    assert(picture_is_game_touch(true, true) && picture_is_game_touch(false, false) && !picture_is_game_touch(false, true));
    // the Options' order without Controller
    {
        int v[kOptionItems] = {10, 11, 12, 13, 14, 15};  // game order: ..., 13 Controller, 14 Gyroscope, 15 UI
        to_shown(v);
        const int shown[kOptionItems] = {10, 11, 12, 14, 15, 13};
        for (int i = 0; i < kOptionItems; i++) assert(v[i] == shown[i]);
        to_game(v);
        for (int i = 0; i < kOptionItems; i++) assert(v[i] == 10 + i);
        assert(kShownFromGame[kOptionItems - 1] == kControllerItem);
    }
    printf("one_screen_test: ok\n");
    return 0;
}
