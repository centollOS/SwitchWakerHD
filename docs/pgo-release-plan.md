# PGO builds for players (plan, issue #90)

Branch `feature/pgo-release`. Status: step 1 done (2026-11-03).

## Why

PGO gave +2.6% to +7.5% fps at stock clocks (`docs/pgo.md`), but only a maintainer's build has it: the profile comes
from a training run on a console. Players build their own NRO from their own dump, so the release has to ship the
profile and the installer has to build with it.

What decides the design (measured, `docs/pgo.md` "Is the generated code the same on every player's computer?"):
- the generated code is the same for every player of a region at one commit, so one profile per region fits;
- GCC drops a function's profile when its `lineno_checksum` differs, and that checksum hashes the source file's path
  as the compiler was given it. CMake gives absolute paths and every player's checkout is somewhere else: 3,859 of
  3,894 hot functions would drop their profile. `-fprofile-prefix-map` and `-ffile-prefix-map` do not change the
  checksum; a relative file name (or a `#line` with one) does (0 of 192 differ);
- the GCC version has to be the profile's: the pinned toolchain (`docs/pinned-toolchain-plan.md`, the installers'
  default) gives every player the same one, inside the checkout (`build/toolchain`).

## Steps

1. **Path-independent profiled compiles.** In a `WWHD_PGO=GEN|USE` build, every profiled file is compiled with the
   checkout's paths relative to the build folder, `../../runtime/...` (`cmake/pgo_compile.cmake`, a compiler launcher run with `cmake -P`:
   every environment that builds has CMake, unlike bash or Python on the pinned Windows build). It changes nothing
   for files the profile does not cover, nor for `WWHD_PGO=OFF`. Check (Mac): the `.gcno` checksums of the profiled
   files from two checkouts in different folders are identical (`gcov-dump`).

   Mac (2026-11-03, native devkitA64 GCC 16.1, `WWHD_PGO=GEN`, `~/Documents/SwitchWakerHD` and a second checkout in
   `~/Documents/SwitchWakerHD-pgopath`, the same `build/gen`): both builds link (440 s and 460 s, 374 profiled files).
   Each profiled file compiled again from its build command with `-ftest-coverage` (the same object name, relative
   to the build folder, as in the build), `gcov-dump` of the `.gcno`:

   | Profiled files compiled | Functions | Whose checksums differ between the two checkouts |
   |---|---|---|
   | as CMake writes them (absolute paths) | 54,740 | 21,806 |
   | through `cmake/pgo_compile.cmake` | 54,740 | 0 |

   A function's id also hashes the object's name for `static` functions; the object names are relative to the
   build folder (`CMakeFiles/...`), the same for every player.
2. **The profile as a release asset.** `tools/switch/pgo_pack.py pack` writes `pgo-profile-<region>.zip` from the
   `.gcda` files `pgo_train.py` fetched, with a manifest: the commit, the region, the GCC version, the scope, the
   file count. Its `.sha256` next to it, as the shader template (#88). The counters hold no game code (function ids,
   checksums and arc counts).
3. **The installer builds with it** (`make_sd.py`): it downloads its release's profile for the dump's region, checks
   the `.sha256` and the manifest (region, GCC version of the toolchain it builds with) and builds with
   `WWHD_PGO=USE`; anything missing or not matching gives a normal build, with one line saying why. `--no-pgo` turns
   it off. Guest mods (the GameCube minimap) keep it: ~86% of the hot functions keep their profile.
4. **Release checklist**: after the last change to the recompiler or a profiled runtime file, train each region on
   the console (`pgo_train.py`, ~30 min each), pack, upload with the template; an installer run checks the build
   used it (its log, the `[pgo]` line in the NRO's boot log).
5. **Console**: re-time on current main at the default GPU profile (`perf_test.py ab`, PGO against no PGO), USA; the
   gain decides whether the release ships it.

## To decide

- EUR: a profile needs the EUR game on the console for training; without one, EUR players get a normal build.
