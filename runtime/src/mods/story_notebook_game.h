// The story notebook's page and the game's own resources (story_notebook.cpp): the game's menu font
// and button icons, its window textures and its menu sounds, read and played at run time from the
// player's game (nothing of the game is stored by the port). Only in the game binary; the unit tests
// run the page without these services (story_notebook::set_game_ui is never called there).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../game_glyph.h"

struct Cpu;

namespace story_notebook {

namespace font { struct Face; struct Prompts; }
namespace style { struct Art; }

struct GameUi {
    // The glyphs of `cps` from the game's font slot (0 the menu font CKingMain, 1 the message font
    // CKingMsg, 4 the picture font CKingPic) into `face`; characters the font lacks are marked missing.
    // False if the font isn't loaded (yet).
    bool (*glyphs)(Cpu* c, int slot, const std::vector<uint32_t>& cps, font::Face& face) = nullptr;
    // The game's named sound effect (its menu sounds: SE_MENU_CURSOR, ...)
    void (*sound)(Cpu* c, const char* label) = nullptr;
    // The menu art (story_notebook_style.h) from the item menu's layout that is open now (`menu`:
    // hdMenuMain_c); false (with the reason) if a piece can't be read
    bool (*art)(uint32_t menu, style::Art& out, std::string& why) = nullptr;
    // The sea chart's own Link icon (and its ship) from the chart menu's layout (`chart`:
    // hdSeaChartMenu_c, story_chart::sea_chart_menu()) into `out`; false if they can't be read
    bool (*chart_icons)(uint32_t chart, style::Art& out) = nullptr;
    // How the game's button prompts look now (input_map::prompt_style, as button_glyphs.cpp)
    font::Prompts (*prompts)() = nullptr;
    // research aid (WWHD_NOTEBOOK_PANES=1): the menu layout's panes and their textures, in the log
    void (*dump)(Cpu* c, uint32_t menu, const std::string& dir) = nullptr;
};
const GameUi& game_ui();  // story_notebook_game.cpp
void set_game_ui(const GameUi* ui);  // story_notebook.cpp

}  // namespace story_notebook
