// SwitchWakerHD (this fork): the Switch parts of main() (main_switch.cpp), one call per marked hook in main.cpp.
// settings.ini's [dev] section goes into the environment before the static initialisers (a constructor there).
#pragma once

namespace main_switch {
void install_crash_handler();  // CPU exceptions, abort() and uncaught C++ exceptions into the session log
void start();                  // first in main(): sdmc:/switch/wwhd, session log, debug server, boot lines, saved settings
void choose_dirs();            // settings.ini [dev] WWHD_GAME_DIR / WWHD_SAVE_DIR (the console has no arguments)
void after_boot_line();        // the game's files, Prepare graphics, the first start's background shaders
void after_mods();             // the mods env.txt switched, through the mod manager
}  // namespace main_switch
