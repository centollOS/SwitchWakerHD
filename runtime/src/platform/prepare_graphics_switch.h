// Prepare graphics (docs/prepare-graphics-plan.md): the console compiles the whole game's shaders once by warping
// through every Warp tab destination (mods/warps.h) and waiting in each until the shader worker has nothing pending.
// Needs a game in progress (any Quest Log, or a new game that is never saved); the Quest Log in save/user is copied
// before and put back after, and the program restarts at the end, so the sweep leaves no trace in the game. What the
// worker compiled stays in shadercache_dksh_local.bin.
#pragma once
#include <string>

namespace prepare_graphics {
// start: from the first destination, or from where a stopped sweep left off; "" or why it cannot start
std::string start();
void stop();             // stops after the current destination, then restarts the program
bool running();
std::string status();    // one line for the screen and the debug server ("" when not running)
void startup();          // main.cpp, once: puts the Quest Log back if a sweep was cut (the game closed during it)
}  // namespace prepare_graphics
