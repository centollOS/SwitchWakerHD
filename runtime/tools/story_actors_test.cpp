#include "mods/story_actors_core.h"
#include <array>
#include <cassert>
#include <cstdio>
int main() {
    // Flag names are scoped to their mod, persist through copying the extension, and fail
    // cleanly when the extension has no free record. Native save isolation is tested separately.
    std::array<uint8_t,1024> extension{};
    story_mail::Book b(extension.data(),255);
    assert(!story_actors::flag(b,"mod-a","chest"));
    assert(story_actors::set_flag(b,"mod-a","chest"));
    assert(story_actors::flag(b,"mod-a","chest"));
    assert(!story_actors::flag(b,"mod-b","chest"));
    assert(story_actors::set_flag(b,"mod-a","sign"));
    assert(story_actors::set_flag(b,"mod-a","chest",false));
    assert(!story_actors::flag(b,"mod-a","chest"));
    assert(story_actors::flag(b,"mod-a","sign"));
    auto restored=extension;story_mail::Book loaded(restored.data(),255);
    assert(story_actors::flag(loaded,"mod-a","sign"));
    std::array<uint8_t,8> small{};story_mail::Book full(small.data(),1);
    assert(story_actors::set_flag(full,"a","chest"));
    assert(!story_actors::set_flag(full,"a","sign"));
    assert(!story_actors::set_flag(full,"a",""));
    std::puts("actor mod-owned flag mapping passed");
}
