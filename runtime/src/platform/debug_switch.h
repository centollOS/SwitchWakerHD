// The game's side of the debug server (debug_server.h): WWHD_DEBUG_SERVER in env.txt (1: port 6543, or a port
// number) starts it at boot, with the commands info, warps, warp, shot, reload and quit (debug_switch.cpp).
#pragma once

namespace debug_switch {
void start();           // main(), once env.txt is read
bool quit_requested();  // the host loop ends ("quit")
}  // namespace debug_switch
