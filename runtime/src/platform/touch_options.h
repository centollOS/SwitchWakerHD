// SPDX-License-Identifier: MPL-2.0
// The Android touch-control options (touch_style.h) as the running game keeps them: one copy, read by
// any thread (the swipe camera, the JNI poll of TouchControls), changed on the main thread by the
// Settings window and the game-style settings page, saved to settings.ini (hostui::set). Built on every
// platform (the swipe camera reads it); only Android shows the options.
#pragma once
#include <map>
#include <string>

#include "touch_style.h"

namespace touch_options {
touch_style::Options get();
// applies and saves (main thread); values are sanitized first
void set(const touch_style::Options& o);
// start-up: the saved values (settings.ini keys touchStyle, touchShowSticks, touchFollow, touchRight,
// touchSwipeSpeed, touchSwipeBoat); missing keys keep the defaults
void load(const std::map<std::string, std::string>& saved);
// the first-start question (touch_style.h ask_first_start): a style was saved, the question was answered;
// answering sets the style and remembers it (settings.ini touchStyleAsked=1); main thread
bool style_saved();
bool first_start_asked();
void answer_first_start(bool flexible);
}  // namespace touch_options
