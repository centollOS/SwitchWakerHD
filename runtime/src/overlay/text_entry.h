// On-screen text entry: when the game asks for text (the Wii U software keyboard, hle/swkbd.cpp; the
// name at the start of a new game) a window of the settings overlay shows over the game picture: the
// game's hint, an input field with caret and character counter, an on-screen keyboard for controllers
// and the mouse, and OK / Cancel. Keyboard typing (with the host's text input: dead keys, IME) goes
// straight into the field.
//
// Threads: the game asks on a guest thread (start); the hosts feed keys and typed text on their main
// thread (overlay::key forwards keys here); the window is built on the render thread inside
// overlay::frame (draw), which also finishes the prompt, so the answer (`done`) runs there.
//
// While the prompt shows, overlay::blocks_input() keeps every button from the game; after it closes the
// game sees nothing until all controller buttons are released (no stray A press confirms something).
//
// Hosts: the SDL host turns SDL text input on while active() (platform/input_sdl.cpp); the AppKit host
// hands key presses to an NSTextInputContext (gfx/text_input_appkit.mm). Where the overlay cannot show
// (no renderer drawing it), start() returns false and the caller uses the host's own prompt
// (input::prompt_text: window title on SDL, a sheet on macOS).
//
// Test switches (scripted runs, see README "Testing"):
//   WWHD_TEST_PAD=1600-1603:A,...   host controller inputs (overlay.cpp) also drive the on-screen keyboard
//   WWHD_TEST_POST_KEYS=1600:Text=Tetra,1650:Return   typed text and keys through the host's real path
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "game_font.h"

namespace text_entry {

// what the game asks for
struct Request {
    std::u16string initial;  // text already in the field
    std::u16string hint;     // the game's guide text ("" if none)
    int max_len = 0;         // in UTF-16 units, as the game counts
    int mode = 0;            // swkbd keyboard mode: 0 full, 1 numbers only, 2 UTF-8, 3 Nintendo Network ID
    int language = 1;        // swkbd language: 0 Japanese (kana pages), 1 English, 2 French, ...
    // called (render thread) with the text after each edit, so the game's own name field shows it
    // as it is typed, in the game's font (the real keyboard tells the game's receiver the same way)
    std::function<void(const std::u16string&)> changed;
    std::shared_ptr<const game_font::Glyphs> glyphs;  // set by start(): the name font's characters
};
using Done = std::function<void(bool ok, std::u16string text)>;

// Any thread. Shows the prompt; false when the overlay cannot show it (the caller falls back). Only
// characters the game's name font has (game_font.h) are offered and taken.
bool start(const Request& r, Done done);
// Any thread. The game took its keyboard away: close without an answer.
void dismiss();
bool active();  // any thread

// ---- host input (main thread)
// Keys (macOS virtual key codes, as overlay::key): editing and navigation only (Backspace, Delete,
// arrows, Home, End, Return = OK, Escape = Cancel); characters come through text().
void key(int code, bool down, bool repeat);
void text(const char* utf8);     // typed (committed) text
void preedit(const char* utf8);  // input method composition, shown at the caret ("" ends it)

// ---- render thread (overlay::frame, inside an ImGui frame)
// Builds the window. `pad` are the host controller inputs (input_map::kPadCount values, 0..1).
// Returns true when the prompt closed in this frame (the overlay then waits for released buttons).
bool draw(const float* pad);

}  // namespace text_entry
