# Performance and memory results per release

The record of the release checklist's sections F (memory) and G (performance), one section per release, newest
first. The files are in `perf/results/`:

- `<release>-vs-<previous>.json` / `.csv`: `perf_test.py compare-to <previous>` (the summary per scene and boot;
  `perf_test.py show <file>` prints its table). The per-frame times (`_frames.json`) stay in `build/perf/`.
- `<release>-memory.txt`: the memory tour's warps and its `[mem]` line at the end of each lap.

**How to use them.** A number measured on one console says little about another, or about the same console with
other settings, a different charge state or firmware. The comparison that decides is always the A/B in one session
on one console: `compare-to <last release>` builds the last release again and alternates the boots. What is kept
here is the record (what was measured, how, and what came out) and a sanity check: `perf_test.py ... --expect
perf/results/<release>-vs-<previous>.json` warns when a scene comes out clearly below what that release measured,
which on the same console means something got slower, or the console's conditions changed.

From v1.0.1 on, `perf_test.py` keeps the conditions of each boot in the result file (`meta.boots`: the version, the
console mode and charge, the clocks the game logged, the picture settings that change the load).

## v1.2.0, 2026-11-04

Console: the maintainers' Switch, HOS 22.1.0, handheld, charging; the player's settings as in v1.1.0's run (dynamic
resolution on, internal resolution 1.0 handheld, FSR 1 on, FPS counter on). CPU at the stock 1020 MHz, GPU at
384 MHz in both builds. Measured on 8f5fe253, built as a player gets it: with a PGO profile (#90) trained on this
console from that commit (`pgo_train.py train --plan default`). The tagged commit adds the installers' update mode
(#113) and the update notice's text, nothing the game runs per frame.

### G. Performance: v1.2.0 against v1.1.0

`perf_test.py ab build/v110.nro <v1.2.0 PGO build> --scenes lava,windfall,dragonroost,outset_grass_us,outset_lookout_us
--frames 900 --order ABBA --repeat 2 --expect perf/results/v1.1.0-vs-v1.0.0.json` (`ab` with v1.1.0's own NRO,
since `compare-to` needs a container build on this Mac): 4 boots of each, interleaved, time of day held at 200. File:
[`perf/results/v1.2.0-vs-v1.1.0.json`](../perf/results/v1.2.0-vs-v1.1.0.json).

| Scene | v1.1.0 fps | v1.2.0 fps | Change (95% interval) | Main thread |
|---|---|---|---|---|
| lava (`M_NewD2` 2) | 29.13 | 29.92 | +0.78 (+-0.05) | 33.10 -> 31.21 ms (-5.7%) |
| windfall | 29.89 | 29.97 | +0.08 (+-0.05): at the 30 fps cap | 32.49 -> 31.33 ms (-3.6%) |
| dragonroost | 29.50 | 29.96 | +0.46 (+-0.15) | 32.89 -> 31.00 ms (-5.8%) |
| outset_grass_us | 29.93 | 29.97 | +0.04 (+-0.15): at the cap | 20.77 -> 19.44 ms (-6.4%) |
| outset_lookout_us | 29.97 | 29.97 | 0.00: at the cap | 31.93 -> 31.17 ms (-2.4%) |

The main thread is faster everywhere (PGO and the fourth core); `--expect`: every scene as v1.1.0's record or above.

### F. Memory

Two laps of the 40-warp tour (`perf/results/v1.2.0-memory.txt`), no crash:

| | End of lap 1 | End of lap 2 |
|---|---|---|
| malloc in use | 2503 MiB | 2567 MiB (+64) |
| Image heap | 3 chunks, 192 MiB (15.3 MiB free) | 4 chunks, 256 MiB (27.5 MiB free) |
| Shader variants alive | 1287 (5588 freed) | 1288 (12115 freed) |

The +64 MiB of lap 2 is the image heap's fourth chunk, nothing else. It came from fragmentation, the case F3
allows: early in lap 2 a 10.7 MiB image found no free run long enough after the evictions (largest free 3.5 MiB),
so the heap took a chunk (`creating a memory block: image heap chunk 3`). With the room, idle textures stay
resident longer before the heap fills, so its use rises to ~250 MiB in places, and no fifth chunk came. v1.1.0's
tour stayed at 3 chunks; v1.2.0 is 31 MiB higher at the end of lap 1 (not looked into: within the heap's
589 MiB never used).

## v1.1.0 (68358e90), 2026-11-02

Console: the maintainers' Switch, now on HOS 22.1.0 (v1.0.0's record was on 19.0.1), handheld, charging. Settings
as the player's (the same as v1.0.0's run): dynamic resolution on, internal resolution 1.0 handheld, FSR 1 on, FPS
counter on. CPU at the stock 1020 MHz, GPU at 384 MHz in both builds.

### G. Performance: v1.1.0 against v1.0.0

`perf_test.py compare-to v1.0.0 --scenes lava,windfall,dragonroost,outset_grass_us,outset_lookout_us --expect
perf/results/v1.0.0-vs-v0.4.7.json`: 4 boots of each, interleaved (ABBAABBA), 900 frames per scene, time of day
held at 200, on v1.1.0-rc.2 (the tagged commit). File:
[`perf/results/v1.1.0-vs-v1.0.0.json`](../perf/results/v1.1.0-vs-v1.0.0.json).

| Scene | v1.0.0 fps | v1.1.0 fps | Change (95% interval) | Main thread |
|---|---|---|---|---|
| lava (`M_NewD2` 2) | 29.20 | 29.22 | +0.01 (+-0.21): no measurable change | 32.84 -> 32.83 ms |
| windfall | 29.91 | 29.93 | +0.02 (+-0.13): at the 30 fps cap | 32.30 -> 32.31 ms |
| dragonroost | 29.66 | 29.75 | +0.09 (+-0.41): no measurable change | 32.48 -> 32.30 ms |
| outset_grass_us | 29.54 | 29.97 | +0.43 (+-0.50): no measurable change | 20.59 -> 20.65 ms |
| outset_lookout_us | 29.97 | 29.97 | 0.00: at the 30 fps cap | 31.84 -> 31.69 ms |

Every scene came out as v1.0.0's record (`--expect`, within 0.5 fps), so HOS 22 did not move the numbers.

### F. Memory

Two laps of the 40-warp tour (`perf/results/v1.1.0-memory.txt`), no crash:

| | End of lap 1 | End of lap 2 |
|---|---|---|
| malloc in use | 2472 MiB | 2472 MiB (+0) |
| Image heap | 3 chunks, 192 MiB | 3 chunks, 192 MiB |
| Shader variants alive | 1291 (5592 freed) | 1275 (12123 freed) |

The first tour, on v1.0.1-rc.1, grew 60 MiB in lap 2: every auto save (on by default since this release) kept its
writer thread's 4 MiB stack, since a detached thread is never reclaimed on the Switch. Fixed before the tag
(8618a1ff: the writer thread is joined); the tour above is with the fix.

## v1.0.0 (62bee4fb), 2026-10-28

Console: the maintainers' Switch (Atmosphère), handheld, charging (the first attempt, on battery at 12%, lost the
Wi-Fi at boot 6 of 8 and was run again from the start). Settings as the player's: dynamic resolution on, internal
resolution 1.0 handheld, FPS counter on. CPU at the stock 1020 MHz in both. GPU at 384 MHz in both: v1.0.0 always
runs the handheld GPU there (#85), and v0.4.7 took the profile saved in the console's settings.ini
(`switchGpuProfile=384`). So this compares the CPU-side work of v1.0.0, not the GPU profile (+4.7 fps in Outset's
tall grass over the stock 307 MHz, measured on its own with `perf_test.py gpu-profile`). The console's settings.ini,
read after the runs, had FSR 1 on (`switchFsr=1`, the player's choice): v1.0.0 was measured with it (handheld:
RCAS sharpening in the present pass), v0.4.7 has no FSR.

### G. Performance: v1.0.0 against v0.4.7

`perf_test.py compare-to v0.4.7 --scenes lava,windfall,dragonroost,outset_grass_us,outset_lookout_us`: 4 boots of
each, interleaved (ABBAABBA), 900 frames per scene, time of day held at 200. File:
[`perf/results/v1.0.0-vs-v0.4.7.json`](../perf/results/v1.0.0-vs-v0.4.7.json).

| Scene | v0.4.7 fps | v1.0.0 fps | Change (95% interval) | Main thread |
|---|---|---|---|---|
| lava (`M_NewD2` 2) | 28.11 | 29.22 | **+1.11** (+-0.29), +4.0% | 34.58 -> 32.82 ms |
| windfall | 28.43 | 29.91 | **+1.49** (+-0.12), +5.2% | 34.51 -> 32.21 ms |
| dragonroost | 28.54 | 29.55 | **+1.01** (+-0.38), +3.5% | 34.34 -> 32.73 ms |
| outset_lookout_us | 28.68 | 29.97 | **+1.29** (+-0.23), +4.5% | 34.24 -> 31.90 ms |
| outset_grass_us | 28.86 | 29.52 | +0.66 (+-0.87): no measurable change | 21.69 -> 20.71 ms |

v1.0.0 kept the full internal resolution (1.00) in every scene; v0.4.7's dynamic resolution went down to 0.75-0.85
and it was still slower. Outset's tall grass is bound by the GPU (about 34 ms), not the main thread.

### F. Memory

Two laps of the 40-warp tour (`perf/results/v1.0.0-memory.txt`), no crash:

| | End of lap 1 | End of lap 2 |
|---|---|---|
| malloc in use | 2474 MiB | 2476 MiB (+2) |
| Image heap | 3 chunks, 192 MiB | 3 chunks, 192 MiB |
| Shader variants alive | 1290 (5595 freed) | 1275 (12112 freed) |
