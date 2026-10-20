// Wait for shaders (issue #49): a draw whose shader is still compiling is skipped (gfx/deko draw.cpp), so a model
// seen for the first time "builds itself" piece by piece: the item Link holds up, his new clothes. With this option
// such draws wait for the compiler instead: a short freeze the first time, the whole model at once. Switch tab, Debug
// section (a test option, off by default; saved: switchShaderWait).
#pragma once

namespace shader_wait {
enum Mode : int {
    kOff,          // draws of a shader still compiling are skipped (the default)
    kItemGets,     // they wait while Link holds up an item, opens a chest or salvages one
    kItemsEvents,  // ... and during any event (cutscenes, talks, doors)
    kAlways,       // they always wait (new places freeze for a moment instead of drawing late)
    kModes
};
const char* label(int mode);
int mode();
void set_mode(int mode);  // any thread (the menu, at start from settings.ini, the debug server)
void frame();             // the game's main thread, every frame (interp.cpp): whether draws wait now
}  // namespace shader_wait
