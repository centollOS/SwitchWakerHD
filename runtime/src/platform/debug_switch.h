// The game's side of the debug server (debug_server.h): the Switch tab's "Debug server (network)" setting
// (settings.ini switchDebugServer=1; off by default) starts it at boot on port 6543, with the commands info,
// warps, warp, shot, reload and quit (debug_switch.cpp).
#pragma once

namespace debug_switch {
void start();           // main(), first thing once the log is open
bool quit_requested();  // the host loop ends ("quit")
bool running();         // the server was started this session
int port();
}  // namespace debug_switch
