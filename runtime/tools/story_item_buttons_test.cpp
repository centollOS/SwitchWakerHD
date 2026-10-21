// Mod items and the story notebook on X/Y/R (runtime/src/mods/story_item_buttons*.{h,cpp}): the pure
// decisions, the item menu's assignment (records 13 and 15, never the save's key), the game's reads
// (the play object's button bytes, the HUD's command guide) and the item use in play, on a synthetic
// save. The game's own functions are stubbed; no game files are needed.
#include "mods/story_item_buttons.h"
#include "mods/story_items.h"
#include "mods/story_items_core.h"
#include "mods/story_mail_book.h"
#include "mods/story_manifest.h"
#include "mods/story_notebook.h"
#include "mods/story_store.h"
#include "input.h"
#include "runtime.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

using namespace story_item_buttons;

void log_msg(const char* fmt, ...) {
    (void)fmt;
}
namespace mods {
uint64_t step() { return 0; }  // story_item_buttons.cpp's research aid
}
namespace input {
bool pro_controller() { return false; }
}

namespace {

constexpr uint32_t kGlobals = 0x101F0000, kArena = 0x20000000, kSave = kArena;
constexpr uint32_t kPlaySelect = 0x1046F0B0 + 0x5BBB;  // g_dComIfG_gameInfo.play.mSelectItem
constexpr uint32_t kSaveSelect = kSave + 0x29;          // the save's button keys

void* map(uint32_t at, uint32_t size) {
#ifdef _WIN32
    return VirtualAlloc(mem::ptr(at), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    return mmap(mem::ptr(at), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif
}

std::vector<std::string> calls;
uint32_t command_answer = 0;
void original(Cpu* c, uint32_t fn) {
    switch (fn) {
    case 0x023EF7CC: calls.push_back("item change " + std::to_string(c->r[4])); c->r[3] = 1; break;
    case 0x0264A774: c->r[3] = command_answer; break;
    default: calls.push_back("other"); break;
    }
}
uint32_t game_call(Cpu*, uint32_t fn, std::initializer_list<uint32_t> args) {
    if (fn == 0x025E1988) calls.push_back("sound " + std::to_string(*args.begin()));
    return 0;
}
bool shown_ok = false;
std::vector<std::string> shown;
bool show(const std::string& title, const std::string& text, const char*) {
    if (!shown_ok) return false;
    shown.push_back(title + "|" + text);
    return true;
}
bool notebook_opens = true;
std::vector<std::string> opened;
bool open_notebook(const char* how) {
    opened.push_back(how);
    return notebook_opens;
}
int32_t reach(const std::string&, const std::string&, uint32_t) { return -1; }  // "Nothing happens."

uint32_t call(void (*hook)(Cpu*), uint32_t r3, uint32_t r4) {
    Cpu c{};
    c.r[1] = 0x2003F000;
    c.r[3] = r3, c.r[4] = r4;
    hook(&c);
    return c.r[3];
}

void test_core() {
    auto pool = [](uint32_t n) { return story_items::pool_number(n); };
    // the answer: the notebook, an item with its number, else none
    assert(answer({}) == 0 && answer({true, 0, 0}) == kNotebookItem && answer({false, 7, 0x1B}) == 0x1B && answer({false, 7, 0}) == 0);
    // the save keys' items (dComIfGs_getItem)
    assert(save_item_offset(0x00) == 0x5C && save_item_offset(0x14) == 0x70 && save_item_offset(0x15) == -1);
    assert(save_item_offset(0x18) == 0x96 && save_item_offset(0x24) == 0x9E && save_item_offset(0x30) == 0xA6 && save_item_offset(0x37) == 0xAD);
    assert(save_item_offset(0x38) == -1 && save_item_offset(0xFF) == -1);
    // the play byte: ours written; ours taken off: the game's item back; the game's own: left alone
    bool w = false;
    assert(play_value({false, 7, 0x1B}, 0x20, 0x20, pool, w) == 0x1B && w);
    assert(play_value({false, 7, 0x1B}, 0x1B, 0x20, pool, w) == 0x1B && !w);
    assert(play_value({true, 0, 0}, 0xFF, 0xFF, pool, w) == kNotebookItem && w);
    assert(play_value({}, 0x1B, 0x20, pool, w) == 0x20 && w);
    assert(play_value({}, kNotebookItem, 0xFF, pool, w) == 0xFF && w);
    assert(play_value({}, 0x20, 0x31, pool, w) == 0x20 && !w);  // the game's own byte (even mid-refresh)
    // the menu's routes
    assert(route({}) == Route::Game);
    assert(route({true, false, 0, false}) == Route::Notebook);
    assert(route({false, true, 0, false}) == Route::Blocked);
    assert(route({false, false, 9, true}) == Route::Item && route({false, false, 9, false}) == Route::Blocked);
    // the game's assignment wins on its button and on any button whose key it changed (a swap)
    const uint8_t before[3] = {0x01, 0x02, 0x03}, same[3] = {0x01, 0x02, 0x03}, swapped[3] = {0x02, 0x01, 0x03};
    assert(game_wins(before, same, 1) == 2u && game_wins(before, swapped, 0) == 3u && game_wins(before, same, 7) == 0u);
}

void clear_save() {
    std::memset(mem::ptr(kArena + 0xE000), 0, story_store::kMemoryBytes);
    for (unsigned i = 0; i < 8; i++) st8(kSave + 0xA6 + i, 0xFF);  // the game's delivery bag: empty
    for (unsigned b = 0; b < 5; b++) st8(kSaveSelect + b, 0xFF), st8(kPlaySelect + b, 0xFF);
}

void test_runtime() {
    clear_save();
    story_items::reset();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "wwhd_story_item_buttons_test";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "i.png", std::ios::binary) << "icon";
    using story_notebook::ManifestItem;
    ManifestItem lamp, rock, map;
    lamp.id = "lamp", lamp.name = "Lamp", lamp.description = "Bright.", lamp.icon = "i.png", lamp.use = ManifestItem::Use::Event;
    rock.id = "rock", rock.name = "Rock", rock.description = "Just a rock.", rock.icon = "i.png";  // no use action
    map.id = "map", map.name = "Map", map.description = "Old.", map.icon = "i.png", map.use = ManifestItem::Use::QuestStep,
    map.use_quest = "hunt", map.use_step = 2;
    std::string why;
    assert(story_items::define("mod", dir.string(), lamp, why) && story_items::define("mod", dir.string(), rock, why) &&
           story_items::define("mod", dir.string(), map, why));
    story_items::set_presenter(show);
    story_items::set_use_calls(open_notebook, reach);
    story_items::set_game_calls(original, game_call);
    set_game_calls(original, game_call);
    assert(story_items::give("mod", "lamp", 1, "test") == 1 && story_items::give("mod", "rock", 1, "test") == 1 &&
           story_items::give("mod", "map", 1, "test") == 1);
    const uint16_t kLamp = story_items::item_key("mod", "lamp"), kRock = story_items::item_key("mod", "rock"),
                   kMap = story_items::item_key("mod", "map");
    const std::vector<uint8_t> save_before(mem::ptr(kSave), mem::ptr(kSave) + 0x1000);

    // nothing of ours: the game's bytes are never touched
    st8(kSaveSelect + 0, 0x00), st8(kSave + 0x5C, 0x20);  // the telescope on X
    st8(kPlaySelect + 0, 0x20);
    tick();
    assert(ld8(kPlaySelect + 0) == 0x20 && ld8(kPlaySelect + 1) == 0xFF);

    // a mod item with a use action goes on Y (record 13, a pool number bound); one without can't
    assert(assign_item(kLamp, 1) && story_items::on_button(1) == kLamp);
    const uint32_t n = story_items::number(kLamp);
    assert(story_items::pool_number(n) && button_item(1) == n);
    assert(!assign_item(kRock, 0) && !story_items::on_button(0));
    assert(!assign_item(kLamp, 3));  // the menu assigns X, Y and R only
    // the game reads its number on Y; X keeps the telescope
    tick();
    assert(ld8(kPlaySelect + 1) == n && ld8(kPlaySelect + 0) == 0x20);
    // the game's refresh of the byte from the save key (dComIfGp_setSelectItem) is answered again next frame
    st8(kPlaySelect + 1, 0xFF);
    tick();
    assert(ld8(kPlaySelect + 1) == n);

    // the HUD: the command guide's Y (button 3) shows ours; X (2) the game's; a forced command stays
    command_answer = 0xFFFFFFFFu;
    assert(call(hooks::button_command, 0, 3) == n);
    command_answer = 0x20;
    assert(call(hooks::button_command, 0, 2) == 0x20);
    command_answer = 0x4C;  // an event's forced command on the button
    assert(call(hooks::button_command, 0, 3) == 0x4C);
    command_answer = 0x05;
    assert(call(hooks::button_command, 0, 1) == 0x05);  // the A button: not an item button

    // Y in play runs the use action (event: counted for wwhd_item_used); Link equips nothing
    calls.clear();
    assert(call(hooks::check_new_item_change, 0x1000, 1) == 0 && calls.empty());
    assert(story_items::used("mod", "lamp") == 1 && story_items::used("mod", "lamp") == 0);
    // X: the game's own item change
    assert(call(hooks::check_new_item_change, 0x1000, 0) == 1 && calls == std::vector<std::string>{"item change 0"});
    // a use that does nothing: the game's "can't" sound, still nothing equipped
    assert(assign_item(kMap, 2));
    calls.clear();
    assert(call(hooks::check_new_item_change, 0x1000, 2) == 0 && calls == std::vector<std::string>{"sound 2179"});

    // moving an item to another button takes it off the first, as the game moves its items
    assert(assign_item(kLamp, 0) && story_items::on_button(0) == kLamp && !story_items::on_button(1));
    tick();
    assert(ld8(kPlaySelect + 0) == n && ld8(kPlaySelect + 1) == 0xFF);  // Y: the game's (none) back

    // the notebook: assignable when owned (record 15), takes an item off the button; opens its page
    story_mail::Book b = story_store::book();
    assert(!assign_notebook(1));  // not owned
    assert(b.set(story_mail::kRecordItem, story_mail::kItemNotebook, 1));
    assert(assign_notebook(0) && story_items::notebook_on_button(0) && !story_items::on_button(0) && button_item(0) == kNotebookItem);
    tick();
    assert(ld8(kPlaySelect + 0) == kNotebookItem);
    // X in play: the page; the notebook isn't installed in this test, so it's refused with the game's
    // "can't" sound, and still nothing is equipped
    calls.clear();
    assert(call(hooks::check_new_item_change, 0x1000, 0) == 0 && calls == std::vector<std::string>{"sound 2179"});
    // an item put on the notebook's button takes it off
    assert(assign_item(kLamp, 0) && !story_items::notebook_on_button(0) && button_item(0) == story_items::number(kLamp));

    // the game puts its own item on a button of ours: the game wins there (and where it swapped)
    assert(assign_notebook(1));
    uint8_t before[kAssignable];
    save_keys(before);
    st8(kSaveSelect + 1, 0x00);  // the game's assignItemSlot: the telescope's key on Y (moved from X)
    st8(kSaveSelect + 0, 0xFF);
    st8(kPlaySelect + 1, 0x20);  // ... and its refresh of Y's play byte
    game_assigned(before, 1);
    assert(!story_items::notebook_on_button(1) && !story_items::on_button(0));
    tick();
    assert(ld8(kPlaySelect + 1) == 0x20 && ld8(kPlaySelect + 0) == 0xFF);  // the game's items, as the save says
    // the game assigning a button that isn't ours leaves ours alone
    assert(assign_item(kMap, 2));
    save_keys(before);
    st8(kSaveSelect + 0, 0x00);
    game_assigned(before, 0);
    assert(story_items::on_button(2) == kMap);

    // an uninstalled mod's item: hidden, the game's item answers (the record stays for a reinstall)
    story_items::reset();
    assert(!button_item(2) && !ours(2).key);
    tick();
    assert(ld8(kPlaySelect + 2) == 0xFF);

    // the game's own save bytes: only the keys this test wrote itself changed, nothing of ours
    const std::vector<uint8_t> save_after(mem::ptr(kSave), mem::ptr(kSave) + 0x1000);
    for (uint32_t i = 0; i < 0x1000; i++)
        if (i != 0x29 && i != 0x2A && i != 0x5C) assert(save_before[i] == save_after[i]);
}

}  // namespace

int main() {
    assert(map(kGlobals, 0x10000) && map(kArena, 0x10000) && map(0x10470000, 0x10000));
    story_store::set_memory(kArena + 0xE000);
    st32(0x101F84DC, kSave);
    test_core();
    test_runtime();
    std::puts("story_item_buttons_test: ok");
    return 0;
}
