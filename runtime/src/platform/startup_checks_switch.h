// Checks at start-up on the Switch, before the game loads (startup_checks_switch.cpp): a message on the console's
// screen (a text screen, libnx's console) instead of a crash report or a silent problem.
#pragma once
#include <string>

namespace startup_checks {
// The game's files in game_dir (code/cking.rpx, content/, meta/meta.xml): when one is missing, an error that says
// what to copy, and the app closes.
void game_files(const std::string& game_dir);
// No compiled shaders yet (no shadercache_dksh.bin, and none compiled on this console so far: the first start): a
// notice that some textures may look black at first, with the choice to prepare the graphics now (A: true, Prepare
// graphics then starts at the title screen) or to play (B: false). With graphics already compiled (an update), the
// same choice once, as "new in this version" (until Prepare graphics completes).
bool shader_cache();
}  // namespace startup_checks
