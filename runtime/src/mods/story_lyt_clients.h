// Story UI layouts: the clients of story_lyt_screen (story_lyt_screen.h), several layouts shown with the
// game's item screen at once (the notebook's page, the Delivery Bag's extra pages). Pure code, unit-tested
// in runtime/tools/story_lyt_clients_test.cpp: which client a game file read is for, the order the
// clients are built and drawn in, and what a state load does to a client's layout.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace story_lyt_screen {

// Does the game's read of `guest` ask for Layout/<layout>.szs (any folder before it, any case)?
inline bool is_layout_file(const std::string& guest, const std::string& layout) {
    const std::string want = "layout/" + layout + ".szs";
    if (layout.empty() || guest.size() < want.size()) return false;
    const std::string tail = guest.substr(guest.size() - want.size());
    for (size_t i = 0; i < want.size(); i++)
        if (std::tolower((unsigned char)tail[i]) != std::tolower((unsigned char)want[i])) return false;
    return guest.size() == want.size() || guest[guest.size() - want.size() - 1] == '/';
}

// The clients' indices (their ids) in draw order: lower `order` first (drawn first, so below the
// others), registration order among equal ones. Built in the same order.
template <class Orders>
std::vector<int> draw_order(const Orders& orders) {
    std::vector<int> ids(orders.size());
    for (size_t i = 0; i < ids.size(); i++) ids[i] = int(i);
    std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) { return orders[size_t(a)] < orders[size_t(b)]; });
    return ids;
}

// After a state load: a client's layout is kept when it was built and its objects are in the loaded
// memory (a portable state, a full state saved after the build); otherwise (not there, or never built,
// e.g. a load from the title screen) it is built for the loaded game's item screen when that is next
// drawn: the loaded game keeps the item screen it was saved with and builds none until the next stage
// or controller change.
enum class AfterLoad { Keep, BuildWithItemScreen };
inline AfterLoad after_load(bool built, bool objects_there) {
    return built && objects_there ? AfterLoad::Keep : AfterLoad::BuildWithItemScreen;
}

// The item screen's projection and matrix, as the game's screen creation takes them from the GamePad's
// render layer (hdLayerBase_c; HD decomp hd_scene_ingame_ui.cpp): the internal projection with flag
// 0x1000, else the layer's projection pointer, else the UI default; the layer's own view matrix with
// both flags 0xC0, else its view matrix pointer, else the UI default.
struct LayerView {
    uint32_t flags = 0, projection = 0, internal_projection = 0, view_matrix = 0, own_matrix = 0;
};
struct ScreenView {
    uint32_t projection = 0, matrix = 0;
};
inline ScreenView item_screen_view(const LayerView& l, uint32_t default_projection, uint32_t default_matrix) {
    ScreenView v;
    v.projection = (l.flags & 0x1000) ? l.internal_projection : l.projection ? l.projection : default_projection;
    v.matrix = (l.flags & 0xC0) == 0xC0 ? l.own_matrix : l.view_matrix ? l.view_matrix : default_matrix;
    return v;
}

}  // namespace story_lyt_screen
