// Background shaders (docs/background-shaders-plan.md): the console reads its own game files' shader programs
// (shader_scan), speculates variants from its own recorded manifest (shader_list) and translates them on a low-
// priority thread; the renderer compiles them behind every draw's shaders and keeps them (gfx/deko
// translate_listed / queue_listed). Nothing of it is shipped.
#pragma once
#include <string>

namespace background_shaders {
std::string start();     // "" or why not (already running)
bool running();
std::string status();    // one line: phase, counts, rate
}  // namespace background_shaders
