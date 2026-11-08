// Mod items and the story notebook on X, Y and R (docs/mod-sdk-v2.md "Items"; the decisions in
// story_item_buttons_core.h).
//
// The item menu puts a mod item with a use action, or the notebook, on a button like a game item; the
// assignment goes to the save's story block (records 13 and 15, story_items.h), never to the game's
// save key. In play the game reads the button's item number (the play object's mSelectItem, the HUD's
// command guide): the port answers with the item's pool number (its icon: the menu's itemForSlot,
// story_notebook.cpp) or 0xA0 for the notebook. Pressing the button runs the item's use action (or
// opens the notebook's page) instead of the game's item use (daPy_lk_c::checkNewItemChange), so Link
// never equips an item the game doesn't know.
#pragma once
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>

#include "story_item_buttons_core.h"

struct Cpu;

namespace story_item_buttons {

// What the story block puts on a button now (an uninstalled mod's item: nothing; the notebook only
// while the save owns it)
Ours ours(unsigned button);
uint32_t button_item(unsigned button);  // answer(ours(button)): the number the game reads, 0: the game's own

// The item menu (story_notebook.cpp's assignment hooks). assign_*: false when refused (the reason is
// shown as a caption and logged).
bool assign_notebook(unsigned button);
bool assign_item(uint16_t key, unsigned button);
void save_keys(uint8_t out[kAssignable]);                           // the save's keys of X, Y, R now
void game_assigned(const uint8_t before[kAssignable], unsigned slot);  // after the game's own assignment
void refresh_strip(Cpu* c);  // the item menu's X/Y/R strip shows the buttons again (after an assignment of ours)

// Once per frame on the game thread (the notebook's controller read): the play object's button bytes
void tick();

void set_game_calls(void (*original)(Cpu*, uint32_t), uint32_t (*call)(Cpu*, uint32_t, std::initializer_list<uint32_t>));
// After story_notebook::install: the item use, the HUD's button read and the item menu's X/Y/R strip (all or nothing)
void install(const std::function<std::string(uint32_t)>& replaced_by, const std::function<void(uint32_t, void (*)(Cpu*))>& replace);
bool installed();
void test_assign();  // research aid (WWHD_ITEMS_TEST_BUTTON=mod:item:button), once a save is loaded

namespace hooks {
void check_new_item_change(Cpu* c);  // daPy_lk_c::checkNewItemChange(btn)
void button_command(Cpu* c);         // hdCommandGuide_c::getButtonCommand(button)
void strip_refresh(Cpu* c);          // hdIconSlideArea_c::refreshItems(): the item menu's X/Y/R strip
void store_selection(Cpu* c);        // hdMenuItem_c::storeSelection(this, key, item)
void meter_probe(Cpu* c);            // dMeter_loadItemTextureHD(meter, btn): research aid, logs only
}  // namespace hooks

}  // namespace story_item_buttons
