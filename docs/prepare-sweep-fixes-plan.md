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

1. **Places left out** (`kLeftOut`): M2tower, M_DaiB, Mjtower. The player's first visit prepares them, as without the
   sweep. A detour through the sea was tried first: M2tower -> sea -> M_DaiB passed once, but M_DragB -> sea ->
   Mjtower crashed the same way (the state that breaks Mjtower comes from the places before, not the warp), so the
   places go instead. The list is now 124 places (state.txt of a sweep in progress may move by up to 3 places).
2. **The sweep needs a game in progress.** Starting from the title screen goes: the settings menu and `prepare start`
   already asked for a game in progress outside the title (the title start was for the first-start notice, which the
   background-shaders card replaced; nothing asked for it any more). A sweep that continues after a restart waits for
   a Quest Log to be loaded (Link in control), showing `waitingForGame` ("choose your Quest Log ... and it begins by
   itself") on the screen until then. The title-screen warp in frame() goes with it.

## Results (console, 2026-10-18, USA, handheld, Quest Log 2)

- Main's list from place 1 with a Quest Log: places 1-64 passed, then M2tower -> M_DaiB stopped the game with the
  issue's `c_xyz.cpp:285 isNearZeroSquare` (reproduced; from a fresh load at M2tower it had passed 3 of 3).
- With the three places left out: places 64-124 in one go, "complete: 61 places in 7 min, 0 skipped" (with 1-64 of
  the run before, the whole list has passed once).
- Fix 2: state.txt 63 + resume.txt, restart: "[prepare] continues once a Quest Log is loaded", the title screen shows
  the waiting line, no warp; Quest Log loaded: the sweep began by itself at 64/124 (M_DaiMB, M_Dra09, M_DragB,
  Msmoke), stopped with `prepare stop`.
- Once, the restart that ends a sweep hit the known boot crash (ResMgrThread, f_0275D21C reading NULL+0x14 at frame
  ~165, seen 2026-10-13 and -15 on other builds): unrelated.
- Quest Log files (41) and prepare_graphics/ byte-identical to before the tests.
