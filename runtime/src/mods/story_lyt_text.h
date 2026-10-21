// Story layouts: setting the text of a text pane while the game runs, through the game's own text path.
//
// Our layouts' txt1 panes are built by the game's parts layout as its HD text panes (hdTextPane_c,
// FW-parts_text_valid / FW-parts_msg_base). Text goes in with hdTextPane_c::setStringPrepared (027046D4),
// the call the game's own screens use for runtime strings (e.g. the Miiverse comment count): the game's
// message conversion turns its tags into draw tags (colours), the pane keeps its capacity (longer text is
// cut off with the game's "Text Buffer Over" warning) and a pane with "Scalable" user data shrinks the text
// to its width. Line wrapping uses the game's own line breaker (hdTextBreak_wrap, 02706A68) measured with
// the pane's font (hdTextPane_c::getTextWidth, 02704820).
//
// All calls run on the game's UI thread (from a hook), with the layout built.
#pragma once
#include <cstdint>
#include <string>

#include "story_lyt_text_markup.h"

struct Cpu;

namespace story_lyt_text {

// a pane of a built layout by name (the layout object: hdPartsLayout_c +4), 0 if there is none
// a state was loaded: our guest buffers (made by the host) may hold the loaded state's memory now;
// they are made again with their next use
void state_loaded();
uint32_t find_pane(Cpu* c, uint32_t lyt, const char* name);
// is `pane` one of the game's text panes (hdTextPane_c)?
bool is_text_pane(uint32_t pane);

// the characters the pane holds. The game sizes every text pane's buffer itself when the layout is built
// (hdTextPane_c::setAutomaticCapacity: the pane width / the width of an "i", times 3), so the buffer size in
// the txt1 section does not count; longer text is cut off there.
uint32_t capacity(Cpu* c, uint32_t pane);
// a new buffer of `chars` characters (hdTextPane_c::setCapacity, as the game's own screens do once after
// building their layout); call it right after the layout is built, not while it draws
bool set_capacity(Cpu* c, uint32_t pane, uint32_t chars);

struct Options {
    bool wrap = false;  // break lines at the pane's width (or `width`) with the game's line breaker
    float width = 0;    // wrap width in layout units; 0: the pane's width
};
// set the pane's text (UTF-16 game text, see game_text()); false if `pane` is not a text pane
bool set_text(Cpu* c, uint32_t pane, const std::u16string& text, const Options& options = {});
// UTF-8 with markup ("\n", "{c:N}", "{c}", "{{")
bool set_text(Cpu* c, uint32_t pane, const std::string& utf8, const Options& options = {});
// the widest line of `text` in the pane's current font size (layout units)
float text_width(Cpu* c, uint32_t pane, const std::u16string& text);
// `text` with line breaks inserted by the game's line breaker so that every line fits `width`
std::u16string wrap(Cpu* c, uint32_t pane, const std::u16string& text, float width);

// test scene (WWHD_STORY_LYT_TEXT_DEMO=1): fills the demo layout's panes (tools/story_mods/lyt/examples/
// text.json); called once after the layout is built (draw 0) and on every draw of it
void demo(Cpu* c, uint32_t lyt, uint32_t draw);

}  // namespace story_lyt_text
