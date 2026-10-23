# CPU work for 30 fps at 1020 MHz: progress log (branch `perf/cpu-1020`, worktree SwitchWakerHD-perf)

Newest entries last. Each entry says what was done, the evidence, and what it means. Console numbers are from
192.168.0.27 (handheld, stock clocks, `tools/switch/perf_test.py`, ABBA order, 4 boots per build).

> **CORRECTION (user, 2026-10-23 ~13:40): sys-clk was on and the game's GPU profile was 460 MHz during everything below
> that has console timings** (the training session log shows `CPU 1224 MHz, GPU 460 MHz`). So the "Baseline", "Round A2" and
> "Round B" console numbers are NOT stock-clock numbers and are void; the static results (machine code, lockstep) and the PGO
> profile (counts per frame) are unaffected. GPU profile reset to `default` in settings.ini (only that line; the original file
> is build/console_settings.ini.orig-user); sys-clk off. Log now shows `CPU 1020 MHz, GPU 307 MHz, memory 1331 MHz`.
> Other settings on that console that differ from a fresh install and were left alone: Bloom off, Distance blur off, dynamic
> resolution off in handheld, dev options on, mods on (first person, ladder speed, move speed, target letterbox, wall climb).
> All timings are being redone from here.

## Baseline (main 113fa63, this console) [VOID: non-stock clocks]
lava 29.85 fps main 30.51 ms; windfall 29.97 / 29.85; dragonroost 29.97 / 29.28 (at the 30 fps cap, so main-thread ms is the
signal). Demanding Outset set, 1 boot: grass 29.97 (render 28.9 ms), village 28.57 (main 33.6, render 33.2, GPU 33.5 ms: all
three at the limit), lookout 28.77 (GPU 34.3 ms, behind 100%), cliff 29.95.

## Round A2: frame-free leaf functions + hidden visibility
- Machine code: functions with a stack frame 35,648 -> 24,696; GOT loads at indirect-call sites 26,203 -> 0.
- Console: main ms per frame lava -0.48 (+-0.45), windfall -0.19 (+-0.43), dragonroost -0.24 (+-0.46). fps unchanged (cap).

## Round B: write-back elision before calls (callee-saved + volatile registers by callee read/write sets)
- tools/recomp/calleeregs.py (new), leaflocal.py, fork_passes.py, ppc.h `PPC_RELOAD_SKIPPED`. Desktop keeps the old behaviour.
- Static: 33% of dirty volatile register stores before direct calls left out; stores per call 7.0 -> 5.3; hot code -1.9% instructions.
- Lockstep (macOS, Switch form, tools/gametest), 2 scenarios (20 s, 35 s): identical to A2 at equal boot alignment in Link,
  camera, rng, sound starts (1,230 / 2,150), all process hashes (40,280 / 70,430), save.
- Console: main ms lava -0.22 (+-0.51), windfall -0.48 (+-0.50), dragonroost -0.33 (+-0.69): same direction, inside noise.
- Reading: A2 + B are worth about 1-3% of main-thread time; verified and safe, not enough for 15%.

## PGO (GCC -fprofile-generate on the console, -fprofile-use for the final build)
- Plumbing: cmake/SwitchPgo.cmake, runtime/src/platform/pgo_switch.cpp (debug cmd `pgo dump|reset|status`),
  tools/switch/pgo_train.py, build.sh WWHD_PGO / WWHD_BUILD_DIR / WWHD_PGO_SCOPE.
- Attempt 1 (full scope, 366 files): black screen at launch. Attempt 2 (game code only): boots, then data abort in the main
  guest thread at frame 0.
- **Evidence (2026-10-23 ~13:30):** crash lr = the instruction after `bl __gcov_indirect_call_profiler_v4` in f_028EFFD0. libgcov's
  value-profiling counter is thread-local and libgcov reads `tpidr_el0`, which Horizon leaves 0 (libnx uses `tpidrro_el0`);
  fault address 0x2fd8 = the TLS offset. `-fno-profile-values` had been added but the objects were stale (built before the
  flag). A test compile with the flag has 0 value-profiler calls. The full-scope black screen is probably the same cause.
- Rebuilt game-only training build: 0 value-profiler calls, 0 `tpidr_el0` reads in the ELF, 3,669 counter arrays.
  Not known yet: whether libgcov's dump (pthread mutex, file I/O) works on Horizon.
- **Evidence (13:35):** corrected game-only training build deployed to 192.168.0.27: boots, stays up 40+ s, `pgo status` answers.
  `pgo reset` + `pgo dump` work: 13 .gcda files in sdmc:/switch/wwhd/pgo (58-68 KB each), game still alive. libgcov's pthread
  mutex and file I/O are fine on Horizon. Next: training run over the scenes (`pgo_train.py train --no-deploy`), held-out
  scenes lava / windfall / dragonroost / outset_village (+ warps 2, 3, 4) left out of the training.
- **Evidence (13:12):** training ran 13 dumps (outset, foresthaven, forbidden, sea_totg, hyrule at day and night; outset_grass day) with
  the game stable, ~55 s per scene. It then stopped at `outset_grass` with the clock forced to night (20 deg): the session log
  ends with the game's own assertion `PROGRAM HALT d_event_data.cpp:1103 "demo_data"` -> abort() (frame 21029). Not the
  instrumentation: the Outset benchmark states are made for the afternoon; night triggers an event their progress lacks.
  Fix: night variants only for the states that survived them; resume the plan after the 11 finished items (`--skip-first`).
- **Evidence (13:25):** resumed training finished: 15 of 15 scenes, no crash (warps 0,1,5-11,13-16 at afternoon; outset_lookout,
  outset_cliff). Total 26 scenes: outset/foresthaven/forbidden/sea_totg/hyrule day+night, outset_grass/lookout/cliff, 13 warps.
  13 .gcda files fetched to build/pgo (0.8 MB). Held out (never trained): lava, windfall, dragonroost, outset_village, warps 2/3/4.
- **Evidence (13:50), profile use on one file (code_hot_012):** GCC finds and matches the profile when compiled from the build
  directory layout (object path relative, `-fprofile-prefix-path` = build dir). With -fprofile-use's default options the object
  grows 29% (plain 40,180 -> 51,900 bytes): loop unrolling/peeling/tail duplication on code that is already larger than L2.
  `-fno-tracer -fno-unroll-loops -fno-peel-loops` -> 41,448 (+3%); adding `-freorder-blocks-and-partition` -> 41,824 with 11 cold
  parts split out. These are now the USE defaults (cmake/SwitchPgo.cmake).
- **Fetched main (13:55):** 113fa63 -> 754ab3b: #78 zcull on by default (+ dual-issue test switch), #80 NACP version. No recompiler or
  generated-code change, so the PGO profile (keyed to code_hot_*.c functions) stays valid. Plan: measure on 113fa63 as the
  common base, then rebase onto main and re-verify the build.
- **Evidence (13:55):** PGO USE build (game code only, 13 files, profile found for all: 0 "profile file not found" warnings)
  built: build/switch-pgo-use/wwhd.nro. Stock-clock baseline on this console: lava 27.85 fps / main 34.3 ms (maintainer's
  reference 27.9), windfall 28.61 / 33.5 ms.

## STOCK-CLOCK RESULTS (CPU 1020, GPU 307, memory 1331 MHz; 192.168.0.27; 4 boots per build, ABBA x2, 900 frames/scene)
- **Round B (A2 + write-back elision) vs main 113fa63, 14:30:** significant gains where the scene is CPU-bound:
  lava +0.51 fps (+-0.43, +1.8%, main thread 34.19 -> 33.37 ms, -2.4%); windfall +0.72 (+-0.40, +2.5%, -2.6%); dragonroost +0.51
  (+-0.38, +1.8%, -2.0%); outset_village +0.84 (+-0.38, +3.3%, main 37.73 -> 36.30 ms, -3.8%); outset_lookout and outset_grass
  fps unchanged (GPU-bound: 20.7 / 23.4) but main thread -1.13 ms (-3.5%) / -1.01 ms (-4.5%). 0 DSP kills.
  Lesson: the earlier "inside the noise" reading came from non-stock clocks where the scenes sat at the 30 fps cap.
- **Evidence (14:42):** clean full-scope training build (game + runtime, 366 files) built in build/switch-pgo-gen: 0 value-profiler
  calls, 0 tpidr_el0 reads, 57,756 counter arrays, bss 9.4 MB. Not deployed yet (console busy with the B vs B+PGO A/B, started 14:30).
- **B + PGO (game code only; profile from 26 scenes, held-out: lava, windfall, dragonroost, outset_village) vs B, 15:32, stock clocks,
  4 boots each:** main thread lava -0.59 ms (-1.8%), windfall -0.89 (-2.7%), dragonroost -0.45 (-1.4%), outset_village -0.75
  (-2.1%), outset_lookout -0.82 (-2.6%), outset_grass -0.38 (-1.8%); fps +0.37 / +0.56 / +0.35 / +0.11 in the four CPU-bound
  scenes. Same direction in all six, but each inside its 95% interval (the B boots spread 0.4-0.9 ms; the PGO boots only
  0.02-0.17: the profile-ordered code is also steadier from boot to boot). The held-out scenes gain as much as any, so it is not
  overfitting to the training scenes.
  Cumulative against main (different sessions, indicative): lava 27.94 -> 28.87, windfall 28.52 -> 29.94, dragonroost 28.61 ->
  29.57, outset_village 25.35 -> 26.25 fps.
- **Evidence (16:00):** full-scope training (game + runtime, clean build, value profiling really off) ran on the console at stock
  clocks: 26 of 26 scenes, no crash, ~62 s per scene; 49 .gcda files (1.2 MB) fetched to build/pgo-full. Confirms the black-screen
  diagnosis: the stale objects (compiled before -fno-profile-values) were the cause, not the runtime instrumentation itself.
- **16:25** full-scope PGO NRO built (build/switch-pgo-use-full): text +1.9% vs B, 4,895 cold parts split out, hot-function order intact. A/B vs round B started.
- **Worktree fast-forwarded to origin/main 59a4741 (16:10).** New since 113fa63: #78 zcull on, #80 NACP, #79 settings-menu rework, #85 the
  handheld GPU fixed at 384 MHz by default (the owner; 307 -> 384 +4.7 fps at the tall grass in their bench). None touches tools/recomp,
  runtime/include/ppc.h consumers or the generated code, so the rounds and the profile stay valid; GPU-bound scenes (lookout, grass)
  will move with #78/#85 independently of this work. All measurements here keep 113fa63 + my changes as one side and 113fa63 as the other.
- **B + full PGO (game + runtime profile, 26 scenes) vs B, 17:08, stock clocks, 4 boots each, all differences beyond the 95% interval
  in the CPU-bound scenes:** lava 28.36 -> 29.28 fps (+0.93 +-0.14, +3.3%; main thread 33.51 -> 31.72 ms, -5.4%); windfall 29.22 ->
  29.97 (+0.75 +-0.15, +2.6%, at the 30 fps cap; main -4.7%); dragonroost 28.98 -> 29.92 (+0.94 +-0.23, +3.2%; main -6.1%);
  outset_village 26.04 -> 27.98 (+1.95 +-0.30, +7.5%; main 36.44 -> 34.27 ms, -5.9%); outset_lookout and outset_grass fps unchanged
  (GPU-bound) but main thread -3.2% / -2.0%. 0 DSP kills. Boot-to-boot spread of the PGO build is tiny (sd 0.00-0.03 fps).
  The runtime half of the profile (render thread, GX2 command path) is what moved the Outset village (+1.95 fps vs +0.11 for the
  game-code-only profile).
- **Cumulative vs main 113fa63 (separate sessions, indicative):** lava 27.94 -> 29.28 (+4.8%), windfall 28.52 -> 29.97 (+5.1%),
  dragonroost 28.61 -> 29.92 (+4.6%), outset_village 25.35 -> 27.98 (+10.4%); main-thread time lava -7.2%, village -9.2%.
