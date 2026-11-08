// (this fork) Brazilian Portuguese from the player's own fan translation (docs/ptbr-translation-plan.md).
//
// The installers (make_sd.py --ptbr, tools/switch/ptbr_pack.py) rebuild Triforce-Heroes' PT-BR translation, which
// the player downloads from their page, on the player's own English 2D pack and put it next to the game folder:
// <game folder's parent>/translation/pt-BR/permanent_2d_ptBR.pack (sdmc:/switch/wwhd/translation/pt-BR/ on the
// Switch; WWHD_PTBR_PACK points at another file). With it there, console language 9 (Wii U "pt") is
// "Português (Brasil)": the game itself runs in English (its own code, region and options) and its English pack
// (permanent_2d_UsEnglish.pack / EuEnglish) is read from the translation's file. Nothing else changes.
//
// Hooks: game_languages.cpp (name, is_available), hle/coreinit_misc.cpp (console_language), hle/fs.cpp
// (read_path), overlay/overlay.cpp (Language tab), platform/ui_text_switch.cpp (the fork's own texts).
#pragma once
#include <string>

namespace fan_translation {

constexpr int kLanguage = 9;  // the Wii U console language code it is offered under
extern const char* const kName;  // "Português (Brasil)"

// the translation's pack is there (a file starting with "SARC"; looked up once, any thread)
bool present();
const std::string& pack_path();

// The language the game is told for a wanted one, when the console language is read (once per start): the
// translation's language with the translation present -> English (1), and the translation is used from then on.
int game_language(int wanted);
// true once game_language() chose the translation
bool active();
// the language this start runs with as the Language tab shows it (kLanguage while active)
int shown_language(int started);

// The language when none is saved: the translation's when present (the player chose it in the installer), else
// `fallback`.
int default_language(int fallback);

// The host file for a guest path: the translation's pack for the game's English 2D pack while active (any case,
// any folder: ".../Pack/permanent_2d_UsEnglish.pack"); "" otherwise.
std::string redirect(const std::string& guest);

// tests: forget what was looked up and chosen
void reset_for_test();

}  // namespace fan_translation
