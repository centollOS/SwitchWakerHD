# Background shaders: the console builds its own shader list and compiles it while you play (plan)

Branch `feature/background-shaders` (from `feature/prepare-graphics`). Status: plan + step 1 measured (2026-10-10).

## Why

Prepare graphics (docs/prepare-graphics-plan.md) controls the game: warps, story events on a new game's data, a
loading screen for 30-60 minutes. The alternative needs no control of the game: the same speculation as
`make_sd.py --shaders` (docs/shader-cache-from-dump-plan.md), on the console, from the console's own game files
and the register states the console recorded itself. Nothing derived from the game is shipped.

Measured on the computer: speculation seeded **only from the title screen's manifest** (1,031 recorded variants)
covers 73.5% of the vertex/pixel pairs of the console's whole-game harvest (74.2% of its shaders): 18,935 sources.
A few minutes of play: 76%. The warp sweep: 93.5%.

## The first start (no warning, no choice)

1. A "preparing graphics" bar while the title screen runs hidden behind it (its draws record states into the
   manifest and compile the title's own shaders), about a minute.
2. The scan of the game's shader programs (step 1 below) and the speculation: the list.
3. The real title screen. The list compiles in the background while the player plays, behind what the game asks
   for (the worker's priority), into shadercache_dksh_local.bin: about 19,000 shaders, 15-20 minutes of the
   worker's time at ~50 ms each.
4. Later starts: the remaining list continues in the background; the manifest keeps growing as the player plays,
   and the list can be refreshed from it.

Prepare graphics (the sweep) stays as the option for the rest (to ~93%).

## Steps

1. **Scan** (done, `platform/shader_scan_switch.cpp`): tools/shaderprep.py's game_shaders in C++. On the console
   (title screen running): 818 files, 1,408 MB read in 24 s, 3,419 MB inflated in 37 s, **64 s**; 29,967 distinct
   programs (11,755 vertex, 18,212 pixel), the same as the Python extractor. Debug server: `scanbench`.
2. **Speculation** on the console: shader_manifest.py speculate (own_block families with the SQ_VTX_SEMANTIC_CLEAR
   fix, up to 2 states per program) over the recorded manifest.
3. **Translation from the list**: the runtime's translate path takes guest registers and a program in guest memory;
   a list entry has its program bytes and registers: copy the program into a guest buffer and translate as
   dksh_cache translate does (same decompiler, same GLSL hashes), queue the GLSL to the worker at the lowest priority.
4. **The first-start bar** and the hidden title, the list kept on the SD card (programs + list: the console's own).
5. Measure: coverage after the first start + N minutes, the game's frame rate while the list compiles.
