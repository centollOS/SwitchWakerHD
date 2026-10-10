# Prepare graphics: the console compiles the whole game's shaders once (plan)

Branch `feature/prepare-graphics`. Status: plan (2026-10-10).

## Why

A first start compiles each shader the first time it is drawn: on the owner's console from an empty cache, Outset
skipped 240 → 0 draws a frame over ~40-50 s before everything was there (title ~220 a frame), and every new place
does the same for a few seconds. `make_sd.py --shaders` (docs/shader-cache-from-dump-plan.md) needs a computer and
a recorded manifest first. Nothing derived from the game can be shipped, so the console has to make its own cache.

The measurement that makes this worth it: the warp sweep of 2026-10-10 recorded what the console compiled while
visiting every Warp tab destination, and those variants alone (no speculation, no computer) hold **93.5%** of the
vertex/pixel pairs of the console's whole-game harvest (95.2% of its shaders). The computer's guesses add 0.7%.

## What the player sees

Settings overlay, Switch tab: **Prepare graphics (once, ~30-40 min)**, with a short explanation.

1. Needs a game in progress: the player's own Quest Log, or a **new game that is never saved** (choose an empty Quest
   Log, enter a name, start). A save file on the SD card is not needed.
2. The game warps by itself through the 134 Warp tab destinations that work as warps (mods/warps.h), waits in each
   until no shader is pending (cap 60 s), then goes on. On screen: "Preparing graphics: 57/134 (Tower of the
   Gods). B: stop". Best docked (30-40 minutes of compiling).
3. During the sweep nothing is written to `save/` (writes refused at the file level), the HUD controls are ignored
   except stop.
4. At the end (or on stop) the game **restarts** (appletRestartProgram, as the debug server's reload): the game state
   the sweep touched in memory is gone, the Quest Log is as it was. What was compiled stays in
   `shadercache_dksh_local.bin` (and the manifest records it).
5. If the game closes during the sweep, what was compiled so far is kept; starting the sweep again continues from
   where it stopped (the index is kept in a small file next to the caches).

## How (fork code only, small hook points)

The runtime already has `WWHD_WARP_TOUR` (mods/cheats.cpp, upstream's desktop harvesting switch): a state machine
that requests each warp, waits for the arrival (no pending warp, no fade, the stage named) and dwells a fixed time.

1. **Sweep core** (new file, e.g. `mods/prepare_graphics.cpp`): the same states, but the dwell ends when the deko3d
   shader lane has 0 pending (a small accessor next to the existing stats) or after 60 s; a destination not reached
   within 30 s is skipped (E3ROOP/sea_T-like traps: then restart and continue from the next one). The list is
   kMainWarps + kAllWarps without duplicates. Debug server command `prepare start|stop|status` to test it without the
   menu.
2. **Save guard**: while the sweep runs, the save path's writes (hle/fs on `/vol/save`, savestate.cpp's writes) fail
   as a full card would; check what the game does then (it must not show a blocking dialog: if it does, block at the
   game's save request instead).
3. **Restart at the end** and the resume index file.
4. **Menu entry** in the Switch tab and the progress text (overlay).
5. Docs: INSTALL.md / README (an alternative to `make_sd.py --shaders` that needs no computer).

## Tests (console)

- From an empty cache: the sweep runs to the end unattended; count destinations reached, skipped, time; then a fresh
  start: Outset and a sample of places with (near) 0 skipped draws on arrival; code memory used.
- The Quest Log after the sweep: byte-identical cking.sav (and the save count) to before; a new unsaved game leaves
  the empty slot empty.
- Stop in the middle, close the game in the middle, continue.

## From the title screen (implemented, to test)

The Warp tab's warps wait for a loaded file (mods/cheats.cpp save_loaded excludes sea_T): that, not the game, is why
no warp worked after sea_T or E3ROOP in the warp sweep. The sweep writes every warp itself on the game's main thread
(prepare_graphics::frame, as cheats.cpp warp_service), and tops up Link's health itself, so it runs from the title
screen's placeholder save data too. The first-start notice offers A: prepare graphics now (the sweep starts once the
title screen has shown for 4 s) or B: play now.

## Findings on the console (2026-10-10)

- A game over stops the sweep (the next warp with Link dead stopped the game, c_xyz.cpp:285): Link's health is
  topped up every frame.
- Warping out a few seconds after arriving (M2tower -> M_DaiB after 10 s, Xboss1 -> Xboss2) stopped the game the same
  way: the next warp waits until Link has control for 1 s (the game's pause-menu conditions), at least 5 s, and
  M2tower stays 12 s.
- The Quest Log files were put back byte-identical after a cut sweep (the game closed), and the sweep continued where
  it stopped.
- About 12 s a place with nothing to wait for; ~25-35 min for the 127 places.
