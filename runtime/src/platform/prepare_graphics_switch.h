// Prepare graphics (docs/prepare-graphics-plan.md): the console compiles the whole game's shaders once by warping
// through every Warp tab destination (mods/warps.h) and waiting in each until the shader worker has nothing pending.
// Runs from a game in progress (any Quest Log, or a new game that is never saved) or from the title screen (the
// game's own initial data, the first warp written here: the Warp tab's waits for a loaded file); save/user is copied
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
void request_at_title(); // start by itself once the title screen shows (the first-start notice's choice)

// for the menu (prepare_graphics_ui_switch.cpp)
struct Progress {
    size_t next = 0, total = 0;  // the next place to visit (0: from the start), of total
    bool complete = false;       // a sweep reached the end once
};
Progress progress();
// the line at the bottom of the screen: the progress while running, else for a few seconds after the restart that
// ends a sweep, "Graphics ready" ("" otherwise)
std::string screen_line();
// the Switch tab's section; true when the menu should close (a sweep was started)
bool ui_section();
void frame();            // the game's main thread, every frame (interp.cpp, next to the cheats)
}  // namespace prepare_graphics
