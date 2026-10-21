// After a save-state load the game must play with the controller the player uses now.
//
// The host's choice (input::pro_controller, Input menu / Controls) is never taken from a state. The
// game keeps its own choice in hdInputManager::mMode (singleton 101F5088 +0x1D0, wwhd_src
// d/hd_input_ctrl.cpp): 0 Pro Controller (TV menus), 1 GamePad, 2 GamePad Off-TV, 3 both (boot,
// title, file select), 4 none. A full state brings back the mode it was saved with; a progress state
// keeps the running one. When it does not match the host, the game reads a controller that is idle
// (Link does not move), so the mode is switched once the player is in play again, with the steps of
// the game's own in-game controller change (Options, StateID_ExchangeProMode) that concern play:
// hdInputManager::setMode and, for the GamePad, its touch pointer. Nothing is shown and no input is
// made. Only if the game refuses the mode directly does the change run through the pause menu (the
// Options screen's controller change, menu closed). Same-controller loads change nothing.
#pragma once
#include <cstdint>

struct Cpu;

namespace controller_reapply {
// The Options category the game has to switch to (0 Pro Controller, 1 GamePad), or -1: nothing to
// do (the game already uses the host's controller, or it is in a scene that picks for itself).
constexpr int needed_category(int game_mode, bool host_pro) {
    if (game_mode == 0) return host_pro ? -1 : 1;
    if (game_mode == 1 || game_mode == 2) return host_pro ? 0 : -1;
    return -1;
}

// a state was loaded (any thread): check the game's controller once it is playing again. A change of
// the host's choice (input::set_pro_controller) requests the same by itself. While the game's mode does
// not match, it waits for the play state (cutscenes and menus finish first).
void request(const char* kind);
// at the frame boundary (ss::service)
void service(Cpu* c);
// the game is being switched from the Pro Controller to the GamePad: its Pro Controller stays
// connected (idle) until the switch is done (padscore)
bool holds_pro_controller();
}  // namespace controller_reapply
