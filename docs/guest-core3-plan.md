# The game's worker threads on core 3 too (plan)

Branch `feature/guest-core3`. Status: step 1 (2026-11-03).

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
3. If it gains: on by default when the process has core 3; check that the background shader compiles (also on
   core 3) do not cost the game frames.
