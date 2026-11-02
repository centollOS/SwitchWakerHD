// (this fork) The settings menu's own texts (overlay.cpp and the tabs it shows) in Spanish or Brazilian Portuguese.
//
// Upstream's menu is written in English. Each text it shows goes through tr("English"), which looks the text up in
// the table (menu_i18n_table.cpp) for the language set_language() chose; a text the table does not have, or any text
// while the language is English (all platforms but the Switch, which sets it from ui_text::language()), is shown as
// written. "Label##id" / "Label###id": only the label is translated; the id stays, so ImGui's ids do not change.
// Format strings ("Slot %d") are translated as they are written; the table keeps their conversions in order
// (runtime/tools/menu_i18n_check.py checks it, and that every tr("...") in the sources has a row).
#pragma once

namespace menu_i18n {

enum Language { kEnglish, kSpanish, kPortuguese };

// a Wii U language code (5 Spanish, 9 Brazilian Portuguese: fan_translation.h; anything else English)
void set_language(long wii_u_code);
// the same, asked the first time a text is shown (the settings are read by then)
void set_language_source(long (*wii_u_code)());
Language language();

// the text in the chosen language; `english` itself when there is no translation. The pointer stays valid.
const char* tr(const char* english);

// tests: every row (English, Spanish, Portuguese)
struct Row {
    const char* en;
    const char* es;
    const char* pt;
};
const Row* rows(int* count);

}  // namespace menu_i18n
