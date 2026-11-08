# Guest mods on the Switch: built into the NRO

## Request

#43 (josete2k): the GameCube sea minimap, `gc-minimap` in [ZeldaWWHDMods](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDMods)
(0.2.1), on the Switch. It is a Mod SDK v2 guest mod (docs/mod-sdk-v2.md): PowerPC code that upstream translates to C
and compiles into a library on the player's computer when the mod is installed, then loads with `dlopen`.

## What stands in the way (2026-10-16)

- **No loading.** Horizon homebrew has no `dlopen` and no compiler; `guest_mods.cpp` refuses native guest modules in this
  fork anyway (`WWHD_FORK_NO_NATIVE_GUEST_MODS`: upstream's modules read `g_core_preempt`, this fork's code `Cpu::preempt`).
- **No hook checks.** Hooks need `recomp.py --mod-hooks`: a flag check at the start of every game function, too costly on
  the console's main thread (~35 ms a frame in lava).
- **No guest memory for mods.** The region `0x7F000000-0x80000000` is not among the Switch's backed guest ranges
  (`core_switch.cpp` `kGuestRanges`).

What already works: this fork's `tools/guestmod/guestmod.py` translates the released `mod.elf` (3 functions, one return
hook on `daPy_Execute` `0240EBB0`, HUD/config/file services; 1,616 lines of C) without errors; the only difference from
this fork's code is the `PPC_ENTER` it writes (`g_host->core_preempt[c->core]`). The Switch build already has the HUD
service (`overlay/guest_hud.cpp`, drawn by the deko3d overlay, which takes ImGui's dynamic textures) and calls
`guestmods::init`/`frame`/`draw_frame`. The mod's map step (`tools/prepare.py`, the player's GameCube dump) runs on the
computer: 34 s and 1.7 MB from a USA ISO; 10 of the 49 sea sectors have verified map bounds (the mod's own limit: the
panel is hidden in the others, on every platform).

## Design

Guest mods are chosen when the NRO is built and compiled into it (the player built that NRO on their own computer from
the mod's package: that is their consent; no trust prompt on the console).

1. **Build tool** (`tools/switch/guest_mods.py`, fork): takes mod packages (catalogue `.zip` or folder, kind `guest`),
   runs their setup tools on the computer (`game_path` from `--gc-game`, `run_tool` with the host Python), assigns each a
   guest region from `0x7F000000` (64 KiB aligned: module + heap), translates it with `guestmod.py` and writes into
   `build/guest-mods/`: one C file per mod (init symbol renamed), `registry.c` (id, version, init, base, region size,
   heap size), `hooks.txt` (the hooked game functions, canonical addresses), and the files for the SD card
   (`sd/<id>/package/` manifest and images, `sd/<id>/data/` the setup's output, `options.json` from the manifest's defaults
   and `--option id.key=value`).
2. **Translator** (`guestmod.py`, marked fork change): `PPC_ENTER` reads `c->preempt`, as this fork's game code.
3. **Recompiler** (`recomp.py --mod-hooks-only FILE`, fork option): the flag check only in the listed functions; the flag
   and body tables as with `--mod-hooks`, and the list of checked functions in `table.c` so the runtime can refuse a hook
   on a function without its check.
4. **CMake** (`SwitchPort.cmake`): `build/guest-mods/registry.c` and the mods' C files go into the NRO when present
   (`WWHD_STATIC_GUEST_MODS`); without them the NRO is as today.
5. **Runtime** (`runtime/src/mods/guest_static.cpp`, fork; marked hooks in `guest_mods.cpp` and `core_switch.cpp`): the
   guest memory backs the registry's regions only when there are mods; `guestmods::init` loads the registry's modules
   through `load_one` with their init function instead of a library (same validation, chains, heap, HUD), with
   package/data paths `sdmc:/switch/wwhd/mods/<id>/` and the options from `options.json`.
6. **make_sd.py `--guest-mod PKG [--gc-game PATH] [--option ...]`**: the build tool before the recompile (step 3 gets
   `--mod-hooks-only`), the files copied to the SD folder.

## Steps

1. Translator and build tool on the computer: `build/guest-mods/` for gc-minimap (C compiles for the Switch: `aarch64-none-elf-gcc -c`).
2. Recompiler option: checks only in the listed functions (generated code otherwise byte-identical: `diff` of build/gen).
3. Runtime: memory, static loading, options; the desktop build unchanged (its refusal stays).
4. make_sd.py / build.sh wiring and the docs (INSTALL.md, switch-port.md, mod notes).
5. Console: Great Sea (panel, heading, sector changes), events/menus/pause (panel hidden), a sector without bounds
   (hidden), islands and interiors (hidden), frame time against the same NRO without the mod (in-boot toggle not
   possible: two boots, ≥4 interleaved).

## Results

- **Steps 1-2 (2026-10-16).** gc-minimap 0.2.1 from the catalogue zip and a USA GameCube ISO: maps prepared in 34 s,
  module 5,520 bytes at `7F000000` (heap 256 KiB, region to `7F050000`); its C compiles for the Switch without warnings.
  `WWHD_RECOMP_MOD_HOOKS_ONLY` with `0240EBB0`: build/gen differs from a plain recompile only in the check in that one
  function (`code_hot_007.c`) and the tables in `table.c`.
- **Step 3, console (USA, handheld).** The log: `return hook 0240EBB0`, `loaded gc-minimap (built into the NRO)`.
  Outset Island: the chart, with Link's marker on the pier; it moves and turns as he walks; 30 fps. A warp to
  Windfall: its chart (sector change). Hidden in the pause menu, in the Tower of the Gods' sector (no chart) and in
  Link's house.
  - Found on the way: every mod file read failed on the Switch (the chart never showed): upstream's `guest_files.h`
    opens through `openat`, which Horizon lacks (`posix_stubs_switch.cpp` returns `ENOSYS`). Fixed with a marked fork
    branch that opens the path directly (the SD card's FAT has no links to guard against).
- **Step 4.** The mod's setup tool refuses a data folder inside a Git checkout, so `guest_mods.py` writes the setup's
  output to a per-user folder (`--data`, default `~/Library/Application Support/SwitchWakerHD/guest-mods` on macOS,
  `%LOCALAPPDATA%` on Windows, XDG on Linux) and `make_sd.py` copies it into `OUT/switch/wwhd/mods/<id>/data/`.
- **Step 5, frame time (2026-10-16).** The same source with the bench commits (local throwaway branch), two NROs: A
  with gc-minimap (game code with the check in `daPy_Execute`), B without mods (plain recompile). `perf_test.py ab`,
  ABBA x 2 (4 boots each), Quest Log 2, 900 frames a scene, handheld, stock clocks. The panel is drawn in the
  Windfall scene (checked with a screenshot); lava and Dragon Roost are not on the sea (hook runs, panel hidden).

  | Scene | A (mod) fps | B fps | B - A fps (95% CI) | main ms A / B |
  |---|---|---|---|---|
  | lava | 27.88 | 27.60 | -0.28 (+-0.35) | 34.90 / 35.32 |
  | windfall | 28.06 | 27.89 | -0.17 (+-0.31) | 34.79 / 35.16 |
  | dragonroost | 28.43 | 28.48 | +0.05 (+-0.18) | 34.34 / 34.42 |

  No measurable cost: every difference is inside its interval (the mod's build is, if anything, ahead: code placement).
  Events: not checked one by one; the panel hid through every warp's fade and load, as the mod's event/next-stage
  test says.

## Follow-up: an easier install (2026-10-17)

What a player meets today: the Windows installer (the guide's recommended way) cannot add mods; a Dolphin `.rvz`
(Zstandard, Dolphin's default) needs Python 3.14 for the mod's map step (`tools/rvz.py`: standard-library
`compression.zstd`), which the guide does not say; changing an option means building again.

- **A. Docs.** INSTALL.md: the GameCube disc first; the `.rvz` and Python 3.14 (or an ISO made with Dolphin's *Convert
  File*); options edited on the console (C); the installer's mod box (D).
- **B. make_sd.py / guest_mods.py.** Before anything slow: the `--gc-game` path exists; a Zstandard RVZ (WIA/RVZ disc
  struct compression 5) with a Python older than 3.14 looks for a Python 3.14+ (`python3.14`..., Windows `py -3.14`) to
  run the mod's setup tools, else stops with what to do.
- **C. Options on the console.** `options.json` is already read at boot (guest_builtin.cpp); the Mods tab gets a fork
  section on the Switch, "Built into this game": each built-in guest mod with its manifest's options (the package copy
  on the SD card), written back to `options.json`; "Restart the game to apply" (the mod reads its options once; the
  Switch cannot relaunch itself).
- **D. Windows installer.** An optional "GameCube sea minimap" box: the mod's `.zip` (chosen or downloaded from the
  catalogue's release) and the GameCube disc; `make_sd.py --guest-mod ... --gc-game ...`; a `.rvz` gets Python 3.14
  from winget. Strings in both languages. Built by CI (release-installer.yml) or locally with the .NET SDK.

Steps: A+B (computer: a zstd RVZ with 3.12 and 3.14, an ISO), C (console: change size, restart, the panel's new size),
D (installer build; a Windows run by the user).

### Follow-up results

- **A+B (2026-10-17).** `guest_mods.py --check`, run by make_sd.py before the extraction. Checked with RVZ headers made
  for the test (compression 5 and 4), Python 3.12: the Zstandard one finds `/Library/Frameworks/.../python3.14`, which
  then runs the setup (the mod's reader reached, past its version check); with PATH without a 3.14, the message; LZMA
  and the ISO keep 3.12; a missing disc, no `--gc-game`, a non-guest package stop at once. Not tried: a real Zstandard
  RVZ (no Dolphin here).
- **C, console (USA, handheld).** Mods > Built-in, at the end: *Code mods in this build*, gc-minimap 0.2.1 with its five
  options at the values of `options.json`. A raises the slider's edit, Right x 8: size 240 -> 284, saved when let go;
  the style combo: `original`. After a restart, Windfall: the panel 225 px wide against 190 px at 240 (x1.18, as
  284/240) and the original style's tiles.
- **Catalogue ids (2026-10-17).** `--guest-mod gc-minimap`: the catalogue's entry (kind guest, its single download),
  SHA-256 checked (`5369bcac...`, as the zip used before), kept in `DATA/downloads/`; python.org's Python 3.14 has no
  certificates here: curl took over. A full guest_mods.py run by id: as from the zip.
- **D (2026-10-17).** The installer's game page has a "GameCube sea minimap (optional)" card (disc or folder); the
  build gets `--guest-mod gc-minimap --gc-game PATH`; a Zstandard RVZ installs Python 3.14 beside (winget, py launcher,
  not on PATH) when no Python has `compression.zstd`; a `guest_mods:` line goes into the error box with its hint; the
  done page says where the options are. Built on macOS with the .NET 8 SDK (`EnableWindowsTargeting`): no warnings or
  errors; `IsZstdRvz` run against the test headers and the ISO. Not run on Windows (the user's test).

## The HD minimap (issue #121, 2026-11-08)

`hd-minimap` 0.2.5 from the catalogue (upstream's code-mod port of Chironeyes' Minimap HD Cemu pack): no setup step,
no GameCube disc; it draws the HD game's own GamePad map art through the layout-art services (`WWHD_HUD_GAME`,
`layout-pane:` / `layout-texture:`, game_layouts.cpp) and the map/HUD bridge (docs/mod-sdk-map-hud.md).

- **Build.** `make_sd.py --guest-mod hd-minimap` needs no change: 29 functions, 2 return hooks (`0240EBB0`
  daPy_Execute and `02699094`, EU `0240EBB4` / `02699798`), module 81,536 bytes + 256 KiB heap at `7F000000-7F060000`.
  USA and EU NROs build and link (native devkitA64 GCC 16.1).
- **Console (EU, handheld, CPU 1020 / GPU 384 MHz, the `[dk] clocks` line checked; on battery).** The log:
  `loaded hd-minimap (built into the NRO) (translator guestmod-0.3)`, no HUD or texture errors. Windfall: the island
  map with Link and the wind arrow, the D-pad icons beside the HUD's D-pad; D-pad left hides it. Forbidden Woods and
  Dragon Roost Cavern: the floor plan with Link, the floor-change markers. Great Sea at the Tower of the Gods: the
  game's compasses (no map buttons in that sector, as the mod's README says). Pause menu: hidden. Sailing into the Tower
  (stage change, its cutscene): no error. Not checked: a treasure chart while sailing (no other sector reached by boat
  in that session).
- **Cost.** The user chose a single deploy (no `perf_test.py ab`), so it was measured inside the boot: a throwaway
  patch timed the hook chains (`ppc_mod_run`), the HUD callback (`draw_frame`) and the overlay's guest HUD
  (`overlay::guesthud::frame`) with `armGetSystemTick`, printed every 300 logic steps, during `perf_test.py run`
  (900 frames a scene, a build without PGO: the release profile is for GCC 15.2).

  | Scene | hooks µs/step | HUD callback µs | overlay µs | main ms (whole frame) |
  |---|---|---|---|---|
  | windfall (island map) | 105 | 120-135 | 33 | 33.1 |
  | sea_totg (compasses) | 103 | 40-46 | 0 | 18.7 |
  | forbidden (dungeon plan) | 85 | 177-185 | 62-68 | 17.2 |
  | lava (dungeon plan) | 85 | 188-204 | 71-74 | 33.2 |

  At most ~0.36 ms of a 33 ms main thread (~1%, about 0.3 fps where the main thread is the limit). The largest single
  step after a state load was 110-285 ms in the HUD callback: the island map's enlargement (below, fixed); in the
  recordings themselves no step went over 1 ms.
- **Stutters (the owner's report: "a stutter until it's drawn", and when the game starts).** Timed on the console
  (`[layouts-timing]`, a throwaway patch in game_layouts.cpp), all on the game's main thread in the port's layout-art
  loader, none in the mod: the first use read the whole 12 MB 2D pack through an `istreambuf_iterator` and copied every
  member (2313 ms, at the file select, for the port's own file-select quit button art); each island map's 3x Lanczos
  enlargement took 236-238 ms, again every time the mod showed the map (it releases the picture when hidden). Fixed in
  `game_layouts.cpp` (marked fork changes): only the pack's index is read (then each member from the file), the six
  Lanczos taps are computed once per phase and the sharpening blur is a running sum (the same picture within one level
  of rounding: 193 of 6.5 million bytes differ, by 1), and the finished pictures are kept (up to 4 MiB). On the console:
  pack index 1.3 ms, the file-select art ~65 ms in all, an island map 35.9 ms the first time (one 94 ms frame) and
  0.2 ms after; textures from the real EU pack are byte-identical to before except the enlarged ones.
- **Picture quality (the owner's report: "so low res").** Every island picture in the 2D pack is 100 x 100
  (`rXX_b`, the GamePad's `rXX_Full`, the treasure charts' `rXX_col_N`); dungeon plans are larger (up to 780 x 780).
  Compared on the console at Windfall: a smaller panel (150, 130) hid the steps but was too small for the owner; the
  mod's "Smooth island maps" (the port's 3x Lanczos with sharpening and a steeper alpha ramp) is what made the outline
  stair-stepped. The overlay draws mod pictures with a linear, clamped sampler (deko3d sampler 0), so with the option
  off the art keeps the game's soft outline: `guest_mods.py` writes `smooth: false` into hd-minimap's `options.json`
  (`SWITCH_DEFAULTS`; `--mod-option` still wins), sizes as the manifest (190 / 300).
- **Installers.** An "HD minimap (optional)" box on the game page (off by default; saved with the other choices),
  `--guest-mod hd-minimap`; strings in Spanish, English and Portuguese.
- **With gc-minimap.** Both hook daPy_Execute's return (chained) and get consecutive guest regions, but both default
  to the bottom-left corner (gc-minimap 0.3.1: x 6, y 18, size 225; hd-minimap: x 12, y 20, size 190) and both draw
  on the sea and in dungeons: on together they overlap unless one is moved in the options.
  The owner's choice (2026-11-08): one at a time. The installers clear the GameCube disc when the HD box is ticked and
  untick it when a disc is chosen; INSTALL.md says to give `make_sd.py` only one of them.
