// Restarting the program (appletRestartProgram): the debug server's reload and the end of Prepare graphics.
//
// appletRestartProgram starts the *title* the process runs as again, not the NRO. From the HOME-menu forwarder
// (tools/switch/forwarder) that title is the forwarder, which loads wwhd.nro again. Started through Atmosphere's
// title override (R held on a game's icon, hbmenu or sphaira in application mode) the process is also an
// Application, but the title is that game: a restart starts the game itself, without the override, and on
// 2026-10-21 the borrowed game (a Chowdren one) aborted on every one of 14 reloads, the system's vi with it 3 times.
// So a restart is only done from the forwarder, recognised by its RomFS (romfs:/nextNroPath, the SAK /
// nsp-forwarder convention our forwarder follows).
#pragma once

#include <cstdint>
#include <string>

namespace restart_switch {
bool from_forwarder();     // started from a HOME-menu forwarder: a restart loads the NRO again
uint64_t program_id();     // the title the process runs as (0 when unknown)
std::string launch_text(); // how the program was started, for the log and the debug server's info
// appletRestartProgram from the forwarder; otherwise nothing is done and false comes back (why in the log)
bool restart(const char* arg);
}  // namespace restart_switch
