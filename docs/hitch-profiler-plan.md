# Hitch profiler: what the frame threads did in the slow frames (plan)

Branch `feature/hitch-profiler`. Status: steps 1-2 done, checked on the console (2026-11-03).

## Why

Frame pacing reports (#23, #102) say "it stutters at a steady 30 fps". The log tells how many frames took longer
than 33 ms (`[dk] pacing`, `[hitch]` for frames over 55 ms, with the render thread's breakdown) but not what the
**main thread** was doing in them, and the sampler (`thread_sampler_switch.cpp`, the debug server's `profile`) needs
a computer on the same network, runs for seconds, and averages slow and normal frames together.

Idea from NFSMW-NX (`perfil_pilas.flag`: a flag file on the SD card turns on a 1 ms sampler for the session and logs
what each thread did in the frames over 45 ms). Ours reuses the existing sampler and the report tool.

## Design

- **Turned on by a file**: `sdmc:/switch/wwhd/logs/hitch_profile.flag`. Empty: frames over **40 ms** (a frame over
  33.4 ms stays on screen for three vblanks; 40 leaves out the steady 34-37 ms overrun of the heaviest places).
  Or one number, the threshold in ms (20-1000). No setting, no rebuild: a tester or a reporter can make it.
- **Started at the render thread's frame 120** (`backend.cpp swap()`, a marked hook), once the game's main thread
  and the render thread are registered. Runs until the game ends; `profile` refuses meanwhile ("already running").
- **The sampler thread** (core 0, priority 0x28, as `profile`) samples the frame threads every millisecond into a
  ring of the last 512 rounds (~0.5 s, ~530 KB for two threads).
- **The render thread's present** (`check_hitch`, a marked hook) passes each frame's start and end ticks
  (`thread_sampler::mark_frame`); frames over the threshold are queued (16 at most; the rest counted).
- **Each queued frame** is handed, once it has ended, to a writer thread (0x3B, core 0; 16 MiB queued at most) that
  appends it to the file and closes it: the sampler never waits for the SD card, and the file can be fetched during
  the session (the console's file system refuses to read a file open for writing). The records: a marker record (thread `0xFFFFFFFE`: start tick, end tick,
  frame number, rounds inside) and then the ring's samples inside the frame. File: `logs/hitch_<n>.samples`, the
  usual WWHDSAMP1 format, so the old readers skip the markers (no thread of that index). At most 2,000 frames.
- **A line a minute** in the log: frames written, frames longer than the ring, frames not queued, samples, failures,
  the pause per sample (the profiler's own cost).
- **Report**: `tools/switch/main_profile.py hitch_<n>.samples --hitches --thread all --elf <the build's ELF>`: all
  slow frames together (self and inclusive, per thread), then the longest frames one by one.

## Cost and caveats

- Sampling pauses each frame thread ~1000 times a second. While the flag is there the game runs a little slower,
  so it makes more slow frames than usual: compare shares, not counts or fps.
- The report needs the ELF of the build that ran. A player's build comes from their own recompile; whether the same
  recompiler, region and pinned toolchain give an identical ELF is not checked (the same question as PGO, #90).
  Guest frames (the game's functions, `f_XXXXXXXX`) only need any ELF of the same recompile.

## Steps

1. Hitch mode in the sampler, the two hooks, `--hitches` in the report (this branch).
2. Console check: flag on, a warp tour (warps make long frames), the file read with `--hitches`; the cost line.
3. Then decide: the audio thread too; the threshold; a note in `docs/reporting-bugs.md` for reporters.

## Console check (2026-11-03, USA, handheld, stock clocks, empty flag: 40 ms)

Warp tours of 10 places (`memory_tour.py --laps 1 --dwell 15 --warps 10`):

| Build | Slow frames | Without samples | Pause per sample |
|---|---|---|---|
| d809bc1e (the sampler wrote the file itself) | 136 | 34 (up to 462 ms) | ~10 us |
| fe76634b (writer thread) | 119 | **0** | 10.0-10.6 us (worst 4 ms) |

Two threads every millisecond at ~10 us each: about 1% of each thread. `--hitches` on the second file: the warps'
long frames are the game thread in `f_02759564` (the J3D draw-list loop, #64) and sleeping in `GX2WaitForVsync`, the
render thread waiting. One finding: frame 166 (418 ms, title screen), the render thread spent 61% in `log_flush`
called by the depth peek's one-time memory block creation (`depth_peek_dk.cpp:43`: the whole boot log written to the
SD card at once). One-off at boot, not a gameplay hitch; the other `log_flush` calls before deko3d creations on the
render thread could be checked the same way.
