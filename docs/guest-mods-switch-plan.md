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
