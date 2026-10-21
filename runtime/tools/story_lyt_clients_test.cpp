// story_lyt_clients_test: the clients of story_lyt_screen (story_lyt_clients.h): which client a game read
// of a layout archive is for, and the order several clients' layouts are built and drawn in.
#include <cassert>
#include <cstdio>
#include <vector>

#include "mods/story_lyt_clients.h"

using namespace story_lyt_screen;

int main() {
    // the game reads Layout/<name>.szs from some folder; the match ignores case, not the folder boundary
    assert(is_layout_file("/vol/content/Common/Layout/StoryNotebook_00.szs", "StoryNotebook_00"));
    assert(is_layout_file("layout/storynotebook_00.SZS", "StoryNotebook_00"));
    assert(is_layout_file("Layout/StoryBag_00.szs", "StoryBag_00"));
    assert(!is_layout_file("/x/Layout/XStoryBag_00.szs", "StoryBag_00"));  // another layout ending in the name
    assert(!is_layout_file("/x/MyLayout/StoryBag_00.szs", "StoryBag_00"));
    assert(!is_layout_file("/x/Layout/StoryBag_00.szs.bak", "StoryBag_00"));
    assert(!is_layout_file("Layout/StoryBag_00.szs", ""));
    assert(!is_layout_file("bag_00.szs", "StoryBag_00"));

    // draw order: lower order first, registration order among equal ones
    assert((draw_order(std::vector<int>{}) == std::vector<int>{}));
    assert((draw_order(std::vector<int>{0}) == std::vector<int>{0}));
    assert((draw_order(std::vector<int>{0, 0, 0}) == std::vector<int>{0, 1, 2}));
    // the notebook (id 0, order 0) above the bag page (id 1, order -1)
    assert((draw_order(std::vector<int>{0, -1}) == std::vector<int>{1, 0}));
    assert((draw_order(std::vector<int>{5, -1, 5, 0}) == std::vector<int>{1, 3, 0, 2}));
    // a state load: kept only when built and in the loaded memory; else built with the loaded item screen
    assert(after_load(true, true) == AfterLoad::Keep);
    assert(after_load(true, false) == AfterLoad::BuildWithItemScreen);   // a full state from before the build
    assert(after_load(false, false) == AfterLoad::BuildWithItemScreen);  // never built (load from the title)
    assert(after_load(false, true) == AfterLoad::BuildWithItemScreen);

    // the item screen's projection and matrix from the GamePad's render layer, as the game picks them
    const uint32_t kDefP = 0x104A20FC, kDefM = 0x104A2098;
    LayerView l;
    l.own_matrix = 0x3B74D1E0;
    ScreenView v = item_screen_view(l, kDefP, kDefM);  // nothing set: the UI defaults
    assert(v.projection == kDefP && v.matrix == kDefM);
    l.projection = 0x3B74D128, l.view_matrix = 0x30000000;
    v = item_screen_view(l, kDefP, kDefM);  // the layer's pointers
    assert(v.projection == 0x3B74D128 && v.matrix == 0x30000000);
    l.flags = 0x1000 | 0xC0, l.internal_projection = 0x3B74D200;
    v = item_screen_view(l, kDefP, kDefM);  // its internal projection and its own view matrix
    assert(v.projection == 0x3B74D200 && v.matrix == 0x3B74D1E0);
    l.flags = 0x40;  // one of the two matrix flags is not enough
    v = item_screen_view(l, kDefP, kDefM);
    assert(v.projection == 0x3B74D128 && v.matrix == 0x30000000);
    std::puts("story_lyt_clients: ok");
    return 0;
}
