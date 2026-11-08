// Story layouts: text for the game's own text panes (host side, no guest memory). See story_lyt_text.h.
//
// The game's pane text is UTF-16 with its message tags: 0x000A is a line break, 0x000E opens a tag
// (group u16, tag u16, parameter size u16 in bytes, parameters), 0x000F closes one (group, tag).
// A colour is tag group 0 / tag 3 with one u16 parameter: the colour number 1-8 of the game's message
// colour table (the conversion turns it into an RGBA tag, hdTextUnit_c::colorTag); any other number
// returns to the pane's own colour.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace story_lyt_text {

constexpr char16_t kNewLine = 0x000A, kTagBegin = 0x000E, kTagEnd = 0x000F;

// UTF-8 with a little markup -> the game's UTF-16 text:
//   "\n" line break; "{c:N}" colour N (1-8) of the game's message colours; "{c}" back to the pane's
//   colour; "{{" a literal "{". Invalid UTF-8 bytes become U+FFFD; characters above U+FFFF surrogate pairs.
std::u16string game_text(const std::string& utf8);
// a colour tag (group 0, tag 3, colour number)
std::u16string colour_tag(uint16_t number);
// the number of code units of the element at `i`: a whole tag, or one character
size_t element_size(const std::u16string& s, size_t i);
// the text without its tags (for logs and tests)
std::u16string strip_tags(const std::u16string& s);
std::string to_utf8(const std::u16string& s);
// put the tags of `tagged` back into `wrapped`, its tag-free text after line breaking (line breaks inserted,
// spaces at the breaks dropped): the game's line breaker reads a tag's group 0 as the end of the text
std::u16string restore_tags(const std::u16string& tagged, const std::u16string& wrapped);

}  // namespace story_lyt_text
