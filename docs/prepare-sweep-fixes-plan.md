# Prepare graphics: sweep fixes (#18)

Branch `fix/prepare-sweep`, from main bed2af7. Code: runtime/src/platform/prepare_graphics_switch.cpp only.

## What the console showed (main bed2af7, USA, handheld, a Quest Log at Tower of the Gods, 2026-10-17/18)

- **M2tower -> M_DaiB** (the issue's `c_xyz.cpp:285` halt): not seen in 2 passes. Both places end with "Link had no
  control for 20 s" (an arrival event, `event 2`) and the sweep goes on.
- **M_DragB -> Mjtower** (place 69): the game crashed 2 of 2 times, a data abort in Link's own code
  (`daPy_Execute` -> `daPy_lk_c::dProcTool` -> `setSeAnime`, a null pointer: Link in an event procedure whose
  animation/sound data is not there). **sea -> Mjtower** passes, and the sweep then went to the end (59 places in
  8 min, 0 skipped).
- **Continuing from the title screen** (the issue's problem 2, forced with state.txt 65 + resume.txt): the first warp
  from the title, to M_DaiMB room 12, stopped the game (`d_stage.cpp:4871 stageRsrc != 0`). In the issue it was "Link
  had no control" in every place (no Link without a Quest Log, `player 00000000`) and one place per restart. Either
  way, the sweep does not work without a game in progress.

## Fixes

1. **Places entered from the sea.** The places whose arrival from another stage stopped or crashed the game
   (Mjtower after M_DragB, M_DaiB after M2tower) are entered from the sea: the sweep first warps to the sea (the list's
   first place, sea room 44), waits for the arrival and Link's control as for any place, then goes on. The place list
   and its numbering do not change (state.txt stays valid).
2. **The sweep needs a game in progress.** Starting from the title screen goes: the settings menu and `prepare start`
   already ask for a game in progress outside the title (the title start was for the first-start notice, which the
   background-shaders card replaced; nothing asks for it now). A sweep that continues after a restart waits for a
   Quest Log to be loaded (Link in control), showing `waitingForGame` ("choose your Quest Log ... and it begins by
   itself") on the screen until then. The title-screen warp in frame() goes with it.

## Tests (console)

- Fix 1: state.txt 63, a Quest Log loaded: M2tower -> sea -> M_DaiB -> ... -> M_DragB -> sea -> Mjtower, no crash,
  then on to the end. 2 passes.
- Fix 2: state.txt 65 + resume.txt, restart: the title screen shows the waiting line and nothing warps; load the Quest
  Log: the sweep begins by itself at place 66 and goes on.
- The Quest Log files and prepare_graphics/ put back as before the tests.
