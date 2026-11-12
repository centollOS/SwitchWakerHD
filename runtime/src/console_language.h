#pragma once
#include <cstdint>
// The effective Wii U language selected at startup, including pack availability/source fallback.
uint32_t console_language();

// (this fork, issue #152) The host's own language as a Wii U console language code (0 ja, 1 en, 2 fr, 3 de,
// 4 it, 5 es, 6 zh, 7 ko, 8 nl, 9 pt, 10 ru, 11 zh-TW): on the Switch its system language (System Settings >
// System > Language); -1 when it cannot be read and on the other hosts. Read once, any thread
// (console_language_default.cpp).
int system_language();
// The console language when the player chose none (no WWHD_LANGUAGE, nothing saved in the Language tab, no fan
// translation): system_language() when the installed game has text for it (game_lang::usable()), else English (1)
int default_console_language();
