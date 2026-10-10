// Background shaders (docs/background-shaders-plan.md): the console reads its own game files' shader programs
// (shader_scan), speculates variants from its own recorded manifest (shader_list) and translates them; the renderer
// compiles them behind every draw's shaders and keeps them (gfx/deko translate_listed / queue_listed). Nothing of it
// is shipped.
//
// First start (no compiled shaders on the console): a loading card from the start. Behind it the game boots and its
// title screen runs, recording its states into the manifest; then the list is made and compiled at full speed
// (the frame rate does not matter behind the card). B on the card: play now, the rest goes on gently. Later starts:
// what is left goes on gently in the background, with no screen.
#pragma once
#include <cstddef>
#include <string>

namespace background_shaders {
void first_start();      // main.cpp: no compiled shaders yet (startup_checks::first_start)
void later_start();      // main.cpp: otherwise; continues an unfinished list in the background
std::string start();     // the debug server: a gentle run now ("" or why not)
bool running();
std::string status();    // one line: phase, counts, rate
// the first start's loading card (prepare_graphics_ui_switch.cpp draws it while `up`)
struct CardState {
    enum Phase { kStarting, kReading, kCompiling, kFinished };
    bool up = false;
    int phase = kStarting;
    size_t done = 0, total = 0;  // variants of the list translated (compiled right behind)
    int minutesLeft = -1;        // -1: not known yet
};
CardState card();
bool draw_screen(float width, float height);  // true when drawn (prepare_graphics_ui_switch.cpp)
}  // namespace background_shaders
