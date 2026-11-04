// SPDX-License-Identifier: MPL-2.0
// Android touch-control options and the rules that follow from them (pure: touch_style_test).
// The C++ side owns these options (the Settings window and the game-style settings page change them,
// settings.ini keeps them, platform/touch_options.cpp); the Java side (TouchControls) polls them with
// nativeTouchOptions and only draws and routes fingers. Per-control size, shape, opacity and fill are
// the layout's (TouchLayout.java), not options.
//
// Styles:
//   Classic   fixed sticks and buttons where the layout puts them; a finger on no control goes to the game
//   Flexible  the buttons stay where the layout puts them, but a finger landing on no control becomes a
//             stick under the thumb: on the left half the movement stick, on the right half the camera
//             (a floating right stick, or the swipe camera). The sticks can be invisible. A finger on
//             the game's own touch areas (the view button, the GamePad picture) still goes to the game.
// Flexible with the swipe camera is the default (user decision 2026-10-28, after the phone session);
// a player who picked Classic keeps it (settings.ini touchStyle=0).
//
// Idea from the GreenNaugahyde Android fork (GreenNaugahyde/ZeldaWWHDRecompAndroid, MPL-2.0), the
// "Flexible" controls of ControlsView; reimplemented for our design.
#pragma once
#include <cmath>
#include <cstdint>

#include "../mods/camera_swipe.h"

namespace touch_style {

enum Style : int { Classic = 0, Flexible = 1 };
enum RightSide : int { RightSwipe = 0, RightStick = 1 };

struct Options {
    int style = Flexible;
    bool show_sticks = true;  // Flexible: draw the floating sticks under the thumbs
    bool follow = true;       // the left stick follows a thumb that slides past its edge (both styles)
    int right = RightSwipe;   // Flexible: what the right half is
    float swipe_speed = mods::camera_swipe::kSpeedDefault;
    float swipe_boat = mods::camera_swipe::kBoatDefault;
    bool operator==(const Options& o) const {
        return style == o.style && show_sticks == o.show_sticks && follow == o.follow && right == o.right &&
               swipe_speed == o.swipe_speed && swipe_boat == o.swipe_boat;
    }
};

inline Options sanitize(Options o) {
    o.style = o.style == Classic ? Classic : Flexible;  // unknown values: the default
    o.right = o.right == RightStick ? RightStick : RightSwipe;
    o.swipe_speed = mods::camera_swipe::speed(o.swipe_speed);
    o.swipe_boat = mods::camera_swipe::boat(o.swipe_boat);
    return o;
}

// What a finger landing on no control does. x: 0..1 of the view's width; on_game_ui: the point is on
// the game's own touch areas (view button, GamePad picture); talk: a talk, a choice, a shop... claims the
// TV picture now (mods/touch_talk.h claims(): the finger goes there, TouchControls.nativeTalkFinger), in
// either style.
enum Free : int { FreeGame = 0, FreeLeftStick = 1, FreeRightStick = 2, FreeSwipe = 3, FreeTalk = 4 };
inline Free free_touch(const Options& o, float x, bool on_game_ui, bool talk = false) {
    if (on_game_ui || !std::isfinite(x)) return FreeGame;
    if (talk) return FreeTalk;
    if (o.style != Flexible) return FreeGame;
    if (x < 0.5f) return FreeLeftStick;
    return o.right == RightStick ? FreeRightStick : FreeSwipe;
}

// Whether a finger at a point is the game's own touch, as the renderer routes a primary-screen finger
// (gfx/vulkan/backend.cpp android_display_touch_event; nativeFreeTouch asks this first):
//   one screen: the view button or the GamePad picture (picture-in-picture or GamePad alone);
//   two screens, roles swapped (the phone shows the GamePad): the GamePad picture on the phone;
//   two screens, not swapped (the phone shows the TV): nothing on the phone is the GamePad's.
struct GameTouch {
    bool dual = false, swapped = false;  // a second display; its roles swapped
    bool view_button = false, pip = false;  // one screen: on the view button / the GamePad picture
    bool primary_gamepad = false;           // swapped: on the GamePad picture shown on the phone
};
inline bool game_touch(const GameTouch& g) {
    if (g.dual) return g.swapped && g.primary_gamepad;
    return g.view_button || g.pip;
}

// The first-start question (controls plan P8): "Use the new touch controls?" (Flexible, recommended) or the
// classic ones, asked once on a touch device with the on-screen controls on, unless the player already chose a
// style (settings.ini touchStyle) or answered it (touchStyleAsked). Flexible is the default either way; the
// question tells the player, and lets them keep the classic fixed sticks. Idea from the GreenNaugahyde Android
// fork (StarterQuestions), reimplemented.
inline bool ask_first_start(bool touch_device, bool controls_on, bool style_saved, bool asked) {
    return touch_device && controls_on && !style_saved && !asked;
}
// the answer: the new controls (Flexible with the swipe camera) or the classic ones
inline Options first_start_answer(Options o, bool flexible) {
    o.style = flexible ? Flexible : Classic;
    if (flexible) o.right = RightSwipe;
    return o;
}

// For Java (TouchControls.nativeTouchOptions): ints, speeds in percent.
//   [0] version  [1] style  [2] show sticks  [3] follow  [4] right side  [5] swipe speed %  [6] boat %
constexpr int kPackVersion = 1, kPackLen = 7;
inline void pack(const Options& in, int32_t* out, int n) {
    const Options o = sanitize(in);
    const int32_t v[kPackLen] = {kPackVersion, o.style, o.show_sticks, o.follow, o.right,
                                 int32_t(std::lround(o.swipe_speed * 100)), int32_t(std::lround(o.swipe_boat * 100))};
    for (int i = 0; i < n && i < kPackLen; i++) out[i] = v[i];
}

}  // namespace touch_style
