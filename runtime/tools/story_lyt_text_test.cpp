// Tests the story layouts' text markup (runtime/src/mods/story_lyt_text_markup.h): UTF-8 -> the game's
// UTF-16 pane text with line breaks and colour tags, invalid UTF-8, tag-aware element sizes.
#include <cassert>
#include <cstdio>
#include <string>

#include "mods/story_lyt_text_markup.h"

using namespace story_lyt_text;

int main() {
    // plain text, line breaks (also CR LF), accents, a character above U+FFFF
    assert(game_text("Hi\nyou") == u"Hi\nyou");
    assert(game_text("a\r\nb\rc") == u"a\nb\nc");
    assert(game_text("\xC3\xA9\xE2\x82\xAC") == u"é€");
    assert(game_text("\xF0\x9F\x98\x80") == std::u16string({0xD83D, 0xDE00}));
    // colour tags: group 0, tag 3, 2 parameter bytes, the colour number
    const std::u16string red = {kTagBegin, 0, 3, 2, 1};
    assert(colour_tag(1) == red);
    assert(game_text("{c:1}A{c}") == red + u"A" + colour_tag(0));
    assert(game_text("{c:12}") == colour_tag(12));
    // braces that are not markup stay; "{{" is a literal brace
    assert(game_text("{{c:1}") == u"{c:1}");
    assert(game_text("{x}") == u"{x}");
    assert(game_text("{c:}") == u"{c:}");
    assert(game_text("{c:1") == u"{c:1");
    // invalid UTF-8: lone continuation, overlong, truncated, surrogate
    assert(game_text("a\x80" "b") == u"a�b");
    assert(game_text("\xC0\xAF") == u"��");
    assert(game_text("\xE2\x82") == u"��");
    assert(game_text("\xED\xA0\x80") == u"���");
    // element sizes: a whole tag, an end tag, a character; a cut tag stops at the end
    const std::u16string t = game_text("x{c:2}y");
    assert(element_size(t, 0) == 1 && element_size(t, 1) == 5 && element_size(t, 6) == 1);
    assert(element_size(std::u16string({kTagBegin, 0, 3, 8}), 0) == 4);
    assert(element_size(std::u16string({kTagEnd, 0, 3}), 0) == 3);
    assert(strip_tags(t) == u"xy");
    assert(to_utf8(strip_tags(game_text("\xC3\xA9 \xF0\x9F\x98\x80{c:3}!"))) == "\xC3\xA9 \xF0\x9F\x98\x80!");
    // tags back into the breaker's output: inserted breaks, dropped spaces, the text's own breaks
    const std::u16string tagged = game_text("one {c:1}two three{c} four\nfive");
    assert(restore_tags(tagged, u"one two\nthree\nfour\nfive") ==
           u"one " + colour_tag(1) + u"two\nthree" + colour_tag(0) + u"\nfour\nfive");
    assert(restore_tags(game_text("{c:2}ab"), u"a\nb") == colour_tag(2) + u"a\nb");
    assert(restore_tags(u"abc", u"abc") == u"abc");
    std::puts("story_lyt_text_test: ok");
    return 0;
}
