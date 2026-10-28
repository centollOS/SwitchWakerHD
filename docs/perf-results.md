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
