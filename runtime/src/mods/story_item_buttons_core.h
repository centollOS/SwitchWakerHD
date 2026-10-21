// Mod items on X, Y and R (story_item_buttons.h): the decisions, as pure code over plain values,
// unit-tested in runtime/tools/story_item_buttons_test.cpp.
//
// What a button holds is the game's save key (dSv_player_status_a_c::mSelectItem[5], an inventory
// slot) unless the story block says otherwise: record 13 (a mod item) or record 15 (the notebook,
// story_items_core.h). The port never writes the save key; it answers the game's reads instead:
//   - the play object's mSelectItem[btn] (the item number every actor reads) gets the item's pool
//     number, or 0xA0 for the notebook, once per frame;
//   - the HUD's command guide (hdCommandGuide_c::getButtonCommand, which reads the save key itself)
//     gets the same number.
// Taking the item off brings the game's item back (the save key was never touched).
#pragma once
#include <cstdint>

namespace story_item_buttons {

inline constexpr unsigned kButtons = 5;     // the play object's five item buttons (X, Y, R and the HD two)
inline constexpr unsigned kAssignable = 3;  // the item menu assigns X, Y and R (hdIngameUi_c::assignItemSlot slot 0..2)
inline constexpr uint32_t kNone = 0xFF;     // dItemNo_NONE / an empty save key
inline constexpr uint32_t kNotebookItem = 0xA0;

// What the story block puts on a button: the notebook, or an installed item with its pool number
struct Ours {
    bool notebook = false;
    uint16_t key = 0;     // item_key, 0: none
    uint8_t number = 0;   // its bound pool number
};
// The item number the game reads for the button, 0: no answer (the game's own item)
inline uint32_t answer(const Ours& o) {
    if (o.notebook) return kNotebookItem;
    return o.key && o.number ? o.number : 0;
}

// An item number the port answers with (a pool number or the notebook's): one the game never has
// itself, so a play byte holding one was written by the port
template <class IsPool>
inline bool ours_number(uint32_t n, IsPool&& is_pool) {
    return n == kNotebookItem || is_pool(n);
}

// The save's item for a save key (dComIfGs_getItem): the offset of its byte from the save base, or
// -1 for a key that holds nothing. Items 0x00..0x14, beasts 0x18..0x1F, baits 0x24..0x2B, the
// delivery bag 0x30..0x37 (d_item_local.h getItem_inl).
inline int save_item_offset(uint32_t key) {
    if (key < 0x15) return int(0x5C + key);
    if (key >= 0x18 && key < 0x20) return int(0x7E + key);
    if (key >= 0x24 && key < 0x2C) return int(0x7A + key);
    if (key >= 0x30 && key < 0x38) return int(0x76 + key);
    return -1;
}

// The value the play byte of a button should hold this frame: `write` false leaves it alone.
//   - ours on the button: its number;
//   - ours taken off (the byte still holds one of our numbers): the game's item for the save key;
//   - otherwise the game's own byte stays.
template <class IsPool>
inline uint32_t play_value(const Ours& o, uint32_t play_byte, uint32_t game_item, IsPool&& is_pool, bool& write) {
    const uint32_t a = answer(o);
    if (a) {
        write = play_byte != a;
        return a;
    }
    write = ours_number(play_byte, is_pool) && play_byte != game_item;
    return write ? game_item : play_byte;
}

// Where an X/Y/R assignment from the item menu goes, for the menu key the player lifted
enum class Route {
    Game,      // a game item: the game's own assignment (the save key)
    Notebook,  // the notebook's place: record 15
    Item,      // a mod item with a use action in one of the game's places: record 13
    Blocked,   // the carried letter, or a mod item without a use action: nothing happens
};
struct MenuKey {
    bool notebook = false;  // the place shows the notebook
    bool letter = false;    // the place shows the carried letter
    uint16_t item = 0;      // the mod item in the place (0: none)
    bool usable = false;    // that item has a use action
};
inline Route route(const MenuKey& k) {
    if (k.notebook) return Route::Notebook;
    if (k.letter) return Route::Blocked;
    if (k.item) return k.usable ? Route::Item : Route::Blocked;
    return Route::Game;
}

// After the game put one of its own items on button `slot` (and maybe swapped the previous one to
// another button): the buttons where the game's choice wins over ours. The target button always;
// any other button whose save key the game changed.
inline unsigned game_wins(const uint8_t before[kAssignable], const uint8_t after[kAssignable], unsigned slot) {
    unsigned mask = slot < kAssignable ? 1u << slot : 0;
    for (unsigned b = 0; b < kAssignable; b++)
        if (before[b] != after[b]) mask |= 1u << b;
    return mask;
}

}  // namespace story_item_buttons
