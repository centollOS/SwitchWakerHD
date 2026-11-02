// (this fork) The settings menu's texts: English (as upstream writes it), Spanish, Brazilian Portuguese
// (menu_i18n.h). One row per text; a format string keeps its conversions in the same order
// (runtime/tools/menu_i18n_check.py). The rows are split by where the texts are.
#include "menu_i18n.h"

namespace menu_i18n {

extern const Row kRows[] = {
// the window and the tab names (overlay.cpp settings_window, extensions.h, overlay_switch.cpp)
#include "menu_i18n_tabs.inc"
// overlay.cpp: Saves, Audio, Language / About, the bottom row
#include "menu_i18n_overlay.inc"
// the Mods tab and the dialogs (mods_tab.h, mods_model.h, quit / restart dialogs, built-in code mods)
#include "menu_i18n_mods.inc"
};
extern const int kRowCount = int(sizeof kRows / sizeof kRows[0]);

}  // namespace menu_i18n
