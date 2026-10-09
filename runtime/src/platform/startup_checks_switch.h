// Checks at start-up on the Switch, before the game loads (startup_checks_switch.cpp): a message on the console's
// screen (a text screen, libnx's console) instead of a crash report or a silent problem.
#pragma once
#include <string>

namespace startup_checks {
// The game's files in game_dir (code/cking.rpx, content/, meta/meta.xml): when one is missing, an error that says
// what to copy, and the app closes.
void game_files(const std::string& game_dir);
// No compiled shaders yet (no shadercache_dksh.bin, and none compiled on this console so far: the first start): a
// notice that some textures may look black at first, and how make_sd.py --shaders prepares the places not visited
// yet from the console's shader manifest; A continues.
void shader_cache();
}  // namespace startup_checks
