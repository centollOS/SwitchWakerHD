# PGO builds for players (plan, issue #90)

Branch `feature/pgo-release`. Status: steps 1-5 done (2026-11-03): +0.7% to +10.2% fps on scenes the profile never saw.

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

   **The toolchain's headers** (found with the pinned toolchain): both checkouts above used `/opt/devkitpro`. With the
   pinned toolchain, which is inside the checkout under `build/toolchain/<id>` (the id names its host system's
   files, so it differs between Windows, macOS and Linux), 36,245 of the 58,067 profiled functions come from its
   headers (the C++ library's templates, libnx), and GCC names them by absolute path even when its own path is
   relative. So a PGO build copies devkitPro's header folders (the compiler's implicit ones from CMake, libnx's,
   the portlibs'; 19 MB) into `<build>/pgo-sysinc/<path in devkitPro>`, and the launcher compiles the profiled files
   with `-nostdinc` and those copies, in the compiler's order (`pgo-sysinc.cmake`). Paths in the build folder (its
   generated headers) are given relative to it: the training build and the players' build have different names.

   | Two checkouts, each with its own pinned toolchain (GCC 15.2.0), build folders `switch-pgo-gen` and `switch-dk` | Functions | Differing |
   |---|---|---|
   | through `cmake/pgo_compile.cmake` (36,245 from `pgo-sysinc/`, 21,812 from `../../`) | 58,067 | 0 |
2. **The profile as a release asset.** `tools/switch/pgo_pack.py pack` writes `pgo-profile-<region>.zip` from the
   `.gcda` files `pgo_train.py` fetched, with a manifest: the commit, the region, the GCC version, the scope, the
   file count. Its `.sha256` next to it, as the shader template (#88). The counters hold no game code (function ids,
   checksums and arc counts).

   Done: the training build writes `pgo-build.json` (the compiler, the scope, the hot files' fingerprint), `pgo_pack.py
   pack|unpack` (`test_pgo_pack.py`, in CI: a round trip, a reproducible zip, and the zips it refuses: another region,
   a path in a name, a changed file, no manifest, a file list that differs, another format). A `WWHD_PGO=USE` build
   with a `manifest.json` in `WWHD_PGO_DIR` checks it (`cmake/SwitchPgo.cmake`); the boot log says `[boot] pgo: profile
   <region> <commit>`. Configured on the Mac against an unpacked made-up profile:
   - the training build's own compiler and code: 374 files profiled, the boot-log definition set;
   - another compiler in the manifest: a warning and a normal build (no file profiled);
   - other generated code: a note, the profile still used (the functions that did not change keep it).
3. **The installer builds with it** (`make_sd.py`): it downloads its release's profile for the dump's region, checks
   the `.sha256` and the manifest (region, GCC version of the toolchain it builds with) and builds with
   `WWHD_PGO=USE`; anything missing or not matching gives a normal build, with one line saying why. `--no-pgo` turns
   it off. Guest mods (the GameCube minimap) keep it: ~86% of the hot functions keep their profile.

   Done: `make_sd.py` (`release_pgo`) caches the zip as `build/pgo-profile-<tag>-<region>.zip` (seed it there to test
   an installer run before publishing, as the shader template) and unpacks it into `build/pgo-release`, passed
   relative to the checkout (`/src/...` in the container); `build_switch.py` (the pinned toolchain) and `build.sh`
   pass it to CMake. Mac: a seeded USA zip is used, EUR (no asset: HTTP 404) gives a normal build; a full native
   `build.sh` USE build with `WWHD_PGO_DIR=build/pgo-release` builds, 374 files profiled, the boot-log line in the NRO.
   The compiler matters: the pinned toolchain and the devkitpro/devkita64 image have GCC 15.2.0 (this Mac's devkitPro
   16.1.0), so the release's training build is made with `build_switch.py` (`WWHD_PGO=GEN`, `WWHD_BUILD_DIR`).
4. **Release checklist** (done, `docs/release-checklist.md` H1): from the tagged commit, train each region on the
   console (`pgo_train.py`, ~30 min each), pack, upload next to the template; an installer run checks the build used
   it (its log, `[boot] pgo: profile` in the NRO's boot log).
5. **Console**: re-time on current main at the default GPU profile (`perf_test.py ab`, PGO against no PGO), USA; the
   gain decides whether the release ships it.

   Console (2026-11-03, 4efd11a7, USA, handheld, charging, stock CPU, default GPU profile). Both builds made on the
   Mac with the pinned toolchain (GCC 15.2.0, `build_switch.py`). The profile: the training build in
   `build/switch-pgo-gen`, `pgo_train.py train --plan default` (26 of 26 scenes, 370 `.gcda` files, 2.5 MB; ~27 min),
   then the release path: `pgo_pack.py pack` (a 0.95 MB zip), `unpack` into `build/pgo-release`, and the PGO build in
   a third folder, `build/switch-pgo-use`. Every profiled file recompiled with the mismatch warnings on: no function
   with a different checksum; 222 small game functions (0.4%) have no entry (none in the training build's profile).
   `perf_test.py ab`, the four scenes the `default` plan leaves out, 900 frames, ABBA twice (4 boots per build):

   | Scene | No PGO fps | PGO fps | fps (95% CI) | Main thread ms |
   |---|---|---|---|---|
   | lava | 29.18 | 29.97 | +0.79 (+-0.07), +2.7% | 32.85 -> 30.55 (-7.0%) |
   | windfall | 29.95 | 29.97 | +0.02 (+-0.06), cap | 32.10 -> 30.68 (-4.4%) |
   | dragonroost | 29.76 | 29.97 | +0.20 (+-0.28), +0.7% | 32.34 -> 29.95 (-7.4%) |
   | outset_village | 26.24 | 28.90 | +2.67 (+-0.84), +10.2% | 35.88 -> 33.93 (-5.5%) |

   The render thread gains too (outset_village 38.7 -> ~31.8 ms per frame, one boot each). Three scenes reach the
   30 fps cap with PGO. Results: `build/perf/ab_base_vs_pgo_20261103_161419.*` (Mac). The console was left on main
   (0c16d6d).

## To decide

- EUR: a profile needs the EUR game on the console for training; without one, EUR players get a normal build.
