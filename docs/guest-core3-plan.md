# The game's worker threads on core 3 too (plan)

Branch `feature/guest-core3`. Status: steps 1-2 done (2026-11-03): +1.9% fps at the Outset village on top of PGO; step 3 next.

## Why

A main-thread profile of the Outset village with the PGO build (`feature/pgo-release`, USA, 20 s, 7,819 samples of
the main thread) has one function far ahead of the rest: `f_02759564`, 7.5% self, 26% inclusive (the next one is 2%).
Its hot part is a polling loop: it calls a virtual getter again and again until the getter's count reaches the sum
of three counters of the object (PowerPC `L_02759634`-`L_027596BC`; on ARM most samples sit right after the indirect
call and in the sum). That is the main thread waiting for the game's other threads to get through their jobs.

With `WWHD_CORE_LAYOUT` the main thread has host core 1 to itself, and the game's threads of emulated cores 0 and 2
(WorkerMgr/Worker1 and Worker2, update_ubo, the resource and audio threads) run on host cores 0 and 2, together with
the render thread and the host service threads, which have a higher priority. Since the forwarder of
`docs/core3-plan.md` the process also has core 3, which runs only the background shader work.

## Steps

1. A switch (`cores::place_guest_thread`, a marked hook in `threads.cpp core_acquire`): the game's threads of emulated
   cores 0 and 2 may also run on host core 3 (mask 0xD instead of 0x5). Off by default; `WWHD_GUEST_CORE3=1` or the
   debug server's `guestcore3 0|1`, applied by each thread the next time it takes its emulated core, so it can be
   switched inside one boot.
2. Console: `perf_test.py toggle --cmd-a "guestcore3 0" --cmd-b "guestcore3 1"` on the four scenes, then the main
   thread's time in `f_02759564` with it on. Without the PGO build first (the branch is on main).
   Console (2026-11-03, USA, handheld, charging, stock CPU, GPU 384; the forwarder with cores 0-3), `perf_test.py
   toggle` inside one boot, 900 frames, ABBA twice (4 recordings each):

   | Build | Scene | Off fps | On fps | fps (95% CI) | Main thread ms |
   |---|---|---|---|---|---|
   | main + this (7c1b80be) | lava | 29.28 | 29.38 | +0.10 (+-0.16) | 32.60 -> 32.16 |
   | | windfall | 29.96 | 29.87 | -0.09 (+-0.13) | 32.24 -> 32.52 |
   | | dragonroost | 29.78 | 29.56 | -0.22 (+-0.23) | 31.71 -> 32.56 (+-2.1) |
   | | outset_village | 26.43 | 26.34 | -0.09 (+-0.21) | **36.17 -> 34.71 (-4.0%, +-0.5%)** |
   | `feature/pgo-release` + this, the USA profile | lava | 29.97 | 29.95 | cap | 30.37 -> 30.27 |
   | | windfall | 29.97 | 29.97 | cap | 31.03 -> 31.18 |
   | | dragonroost | 29.97 | 29.97 | cap | 29.89 -> 30.71 (+-1.5) |
   | | outset_village | 28.69 | **29.24** | **+0.55 (+-0.11), +1.9%** | 34.17 -> 33.47 (-2.1%) |

   Without PGO the Outset village is held by the render thread (~37.8 ms), so the main thread's 1.5 ms do not show in
   the frame rate; with PGO the main thread is the limit there and the frame rate follows it. Together with PGO the
   village goes from 26.2-26.5 fps (no PGO, off) to 29.2. Results: `build/perf/toggle_off_vs_core3_20261103_194505.*`,
   `build/perf/toggle_pgo-off_vs_pgo-core3_20261103_201252.*` (Mac).
3. If it gains: on by default when the process has core 3; check that the background shader compiles (also on
   core 3) do not cost the game frames.
