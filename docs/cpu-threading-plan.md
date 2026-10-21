# CPU and threading: using the three cores (plan)

Branch `plan/cpu-threading`. Status: plan (2026-10-21). Follows issue #49 (shader freezes) and the retrospective of
rounds 39-45 (docs/switch-port.md).

## Where we are (measured)

The game gets three of the Switch's four cores (core 3 is the system's). The layout is deliberate
(`WWHD_CORE_LAYOUT`, rounds 10-11):

| Core | Who | Load (stock CPU, measured) |
|---|---|---|
| 1 | the game's main thread (emulated core 1) alone | 64% (Forest Haven, 2026-10-21) to 96-98% (at sea, Dragon Roost, the volcano) |
| 2 | the render thread (host priority 0x2C), part of the shader worker | 33-36%; at Outset's heaviest view the render thread alone is ~1000 ms/s |
| 0 | the game's other threads (JASThread 15-30%, WorkerMgr workers 8-13%, update_ubo 12%), audio, the shader worker's background jobs | 27-33% |

There are **two limits**, in different places (retrospective, round 45):

- **Dense island views** (Outset, 6,900-7,300 draws): the **render thread** (core 2). Rounds 40-45 took it from 22
  to ~29 fps.
- **Sailing, Dragon Roost, the volcano** (2,000-3,400 draws): the **main thread** (core 1), ~90% of it the game's
  own recompiled code, ~10% runtime calls. The render thread has headroom there; no renderer round moved it.

And a third problem that is not frame rate: **freezes and missing pieces** when a shader is new (#49). Compiling
is one shader at a time on one worker thread; an item-get on a cold cache waited 11 shaders, 1.2 s, while cores 0
and 2 were ~30% busy.

So in total about 1.3 of the 3 cores are free in the main-thread-limited places, but the work that sets the frame
rate is a single sequential stream. Using the free cores means **taking work off that stream** (or off the render
thread), not adding threads beside it.

## Goals

| Where (stock CPU 1020 MHz, GPU 307 MHz) | Now | Goal |
|---|---|---|
| Sailing near Dragon Roost / the volcano | 25.7-29.5 fps | 30 fps, 1st percentile frame time under 40 ms |
| Outset's heaviest view | ~29 fps | 30 fps with margin (render thread under 850 ms/s) |
| First item-get on a cold cache (#49 test) | 1.2 s freeze | under 0.3 s |
| Everything else | | no regression: frame rate, audio gaps, `kill DSP channel`, battery |

No clock changes (AGENTS.md "Clocks"): the gains come from the code.

## Rules for this plan (from rounds 39-48)

1. **Measure the limit before choosing the target.** Each step names the thread it relieves and the place where
   that thread is the limit.
2. **One switch per change**, in the Switch tab's Debug section, logged with its frame (docs/switch-port.md, "Rule
   for test builds"); A/B in the same session, the same spot.
3. **Kept only with hardware evidence.** Estimates in the round notes were often too optimistic.
4. **Correctness is checked by machine**, not by playing: `tools/verify` for single functions, a lockstep
   determinism check (step 0.4) for anything that changes when game code runs.
5. Fork code in fork-owned files, reached through small marked hook points (round 48's rule).

## Phase 0: measurement (first; everything else depends on it)

**0.1 Benchmark routes on the console.** `tools/switch/bench.py`: through the debug server, load a portable save
state, wait for the arrival, hold a fixed camera input for 30 s, collect the 5 s `[dk]`, `[cpu]`, `[main]` and
`[sched]` lines and the frame times, write JSON. Routes: Outset's heaviest view (render-limited), sailing near Dragon
Roost and the volcano (main-limited), Windfall, Forest Haven, a cold-cache item-get (#49). A/B mode: the same route
with a debug switch off and on, interleaved (A B B A), as `tools/bench/run_bench.py` does on the desktop. Done when
two runs of the same build agree within 0.3 fps.

**0.2 A profiler for the main thread's game code on the console.** The `[main]` sampler names runtime calls only
(10% of the thread); the other 90% is unnamed.
- First try: a sampler thread reading the main thread's PC with `svcGetThreadContext3` 1,000 times a second, the
  PC mapped to recompiled functions through the ELF's symbols. Horizon documents the call as exact only for a
  thread that is not running: check whether the PCs of a running thread are usable (compare with 0.3).
- Fallback: a profiling build of the recompiler (`WWHD_RECOMP_PROFILE=1`) that stores the current function's id in
  a global at each function entry and after each call returns (main thread only); the sampler reads it. Its own
  cost is measured and reported with the profile.
- Output: `tools/switch/main_profile.py`: functions by self time and by inclusive time, with names from the decomp
  (`tools/verify/funcdb.py`) where known.

**0.3 Desktop cross-check.** The same save states on the macOS (Apple silicon) build with Instruments' Time
Profiler, and Windows on ARM with `thread_sampler`. Same game code, a similar ARM64 compiler: the hot list should
look alike; where the console differs, the console wins.

**0.4 Lockstep determinism check (desktop).** Scripted input (`WWHD_PRESS`, gametest scenarios), a hash of MEM2's
game heap and of the main actors' state every N frames, written to a file. Two runs of the same build must match
(find and mask what is not deterministic: timers, audio). Then any change that moves game code (phases 2 and 3)
must keep the hashes equal. This is what makes phase 3 safe enough to try.

**0.5 Core 3 probe.** Can an application thread run on core 3, and how much time does it get? A test thread
spinning on core 3 for 10 s, logging its CPU time. If it gets a meaningful share, background jobs (the shader
worker's background list, the log writer) could go there. Likely answer "little or none": an hour to know.

**Exit of phase 0:** a profile of the main thread at sea and of the render thread at Outset, on the console, that
names the top 50 functions and their shares; the benchmark reproducible. **This decides the order of phases 2-4.**

## Phase 1: runtime threading (contained, measurable)

**1.1 Shader freezes: compile a moment's shaders in parallel** (#49). While draws wait (the item-get option,
start-up, `WWHD_DK_SHADER_BUDGET=0`): the first frame of the moment collects every pending shader its draws need
(skipped, not waited), the frame is presented again unchanged, and those shaders compile on up to three worker
threads (cores 0 and 2: the main thread waits for the frame anyway). Then the frame is drawn whole. Outside a wait
nothing changes: one worker, as today. Measure: the #49 chest on a cold cache (1.2 s now), Forest Haven with
"Always" (7.5 s now), and that the first frames after a wait are not slower (the render thread on core 2 must not
compete with a still-running compile). Check `uam` is safe to run in several threads (its state is per compiler
object; if not, separate processes are not an option on the Switch, so one compiler object per thread).

**1.2 Placement audit.** With `[sched]` and the phase 0 profile: which guest threads run on core 2 next to the
render thread at Outset, and on core 0 at sea. Candidates: keep the render thread's core free of the game's worker
threads in render-limited views; check JASThread's 15-30% (the sound mixing) for cost that belongs to the runtime
(the AX HLE). Each move behind a switch, measured on the routes.

**1.3 Not doing** (measured before): moving the update_ubo jobs the main thread runs itself (at most ~2%, backlog
risk, round 16); a copy helper thread (starved the game's sound) and render-thread priority changes (both tried in
round 43, removed in round 44).

## Phase 2: the main thread's game code (the sea limit)

Chosen from phase 0's profile; expected shape (desktop profiles so far): math leaves (matrix and vector
functions, paired-single heavy), J3D model calculation (joint matrices, envelopes, skinning), collision checks,
particle (JPA) updates, the game's memory and list routines.

**2.1 Native replacements of hot leaf functions.** For each function in the top of the profile that is a leaf (or
close to one): a native C++ version (NEON where it pays) behind a hook (`tools/recomp/hooks_perf.txt`), checked with
`tools/verify` against the recompiled original (generated inputs and recorded ones, `mutate.py` for the test's
sensitivity), then on the lockstep check (0.4). Float rules: the game's results must be bit-identical, or the
difference shown harmless (paired singles round to single precision at each step; fused multiply-add changes
results: off unless the original fuses).

**2.2 Recompiler output for hot functions.** Where the profile shows the recompiled form itself is the cost
(byte-swapped loads and stores of the same field, paired-single pairs done as two scalars, condition-register
traffic, calls that could be inlined): a fork pass (`tools/recomp/fork_passes.py`) limited to `hot_functions.txt`,
checked byte-for-byte on everything else and with `tools/verify` on the changed functions.

Measure: main thread's share at sea (`[cpu]` core 1, `[sched]`), fps on the sea routes. Stop rule: when the top
functions left are each under 1% of the main thread, phase 2 is done.

## Phase 3: game work on the free cores (the large step)

The Wii U game was written for three cores, but most of its per-frame work runs on its main thread. Some of that
work is a loop over independent items: actors, models, particle emitters. Such a loop can run on several cores
**within the frame**: the main thread hands out the items, runs its share, and waits for the others before going
on (fork-join). Nothing outside the loop sees a difference in order.

**3.1 Find the candidates** (phase 0's profile, inclusive time): loops that take more than ~5% of the main thread
and call the same function per item. Likely: J3D's per-model matrix calculation, skinning and envelope matrices,
particle emitter updates, per-actor culling.

**3.2 Prove independence** before writing any parallel code: record what each iteration reads and writes over a
few thousand frames of the routes. The verify taps (`runtime/src/verify_tap.cpp`) already record a tapped
function's own loads and stores; every function the iteration calls needs a tap too (or the taps learn to follow
calls).
Iterations that write only their own item's memory and read only memory no iteration writes are candidates; shared
counters, allocators (the game's heap), and calls into the sound or resource systems rule a loop out, or need the
part that touches them kept on the main thread.

**3.3 Run them in parallel.** A small worker pool on cores 0 and 2 (host priority above the game's background
threads, below the render thread's waits), the loop's hook splitting its items. Guest state per worker: each
worker runs guest code with its own register file and stack (the runtime's threads already do), and the emulated
core it acts for stays the main thread's (it holds core 1's identity for the duration). Behind a Debug switch per
loop.

**3.4 Check:** the lockstep hashes (0.4) equal with the switch off and on, over the gametest scenarios; then the
console routes. A race shows as a hash difference long before it shows as a crash.

Risk: the highest of the plan. A loop that looks independent but is not corrupts state rarely and far from the
cause. The proof of 3.2 and the hashes of 3.4 are the safeguards; a loop without both stays sequential.

## Phase 4: the render thread (dense views)

At Outset the render thread is ~1000 ms/s: it decodes the game's command stream, looks up shaders, textures and
vertex layouts, and records deko3d commands. Split it into two stages on two cores: a **decode stage** (commands to
resolved draw records: state, shader and texture lookups done) and a **record stage** (deko3d command buffers from
the records). A queue between them, one frame's worth of records at most. The deko3d calls stay on one thread.
Measure: the render thread's ms/s and fps at Outset; latency: at most one extra stage per frame, inside the same
frame. Only worth doing if phase 0 confirms the render thread still limits Outset after phases 1-2.

## Phase 5: #49's root cause (shader coverage)

Prepare graphics with an **item gallery**: after its warps, the sweep shows each item-get model once in one place,
through the game's own item creation for the get-item demo (found in the decomp), so every item's shaders are
compiled before a player meets them. The sweep restores the Quest Log and restarts at the end, so nothing stays. A
research step first: the function, whether it can be called for every item id safely, and how long the gallery
takes.

## Order and checkpoints

| Step | Checkpoint |
|---|---|
| Phase 0 (0.1-0.5) | console profiles of both limits; benchmark reproducible |
| 1.1 parallel compile in waits | #49 chest under 0.3 s |
| 1.2 placement | per move, on the routes |
| Phase 2 | each function: verify + lockstep; sea fps |
| Phase 3 | per loop: proof, hashes, console |
| Phase 4 (only if still needed) | Outset render thread |
| Phase 5 | the sweep's item gallery on the console |

Full regression (every route, both games, audio, a sweep) at the end of each phase; bisect if it fails
(AGENTS.md "Checks").

## Open questions

- Does `svcGetThreadContext3` give usable PCs for a running thread (0.2)? If not, the profiling build's cost.
- Is `uam` thread-safe across compiler objects (1.1)?
- How deterministic is the game on the desktop under scripted input (0.4)? What has to be masked.
- Does core 3 give an application any time (0.5)?

## Progress

Newest last. Each entry: what was done, what was measured, what it decided.

- 2026-10-21: plan written.
- 2026-10-21, **0.2 done (first version): the console profiles the main thread.** `thread_sampler_switch.cpp`
  (the Switch backend of `thread_sampler.h`): the debug server's `profile <s>` pauses each registered thread
  (`svcSetThreadActivity`), reads it (`svcGetThreadContext3`), resumes it; pc, lr and the frame-pointer chain
  (mean 8-11 frames: inclusive times work). `tools/switch/main_profile.py` names samples from the ELF and the
  game's functions from the decomp's `/* ADDR */` lines. Cost: 9.8 us a pause, ~590 samples/s per thread, 0
  failures in 11,720 samples; the frame rate did not move (29.3-30.0 during, 29.6-30.0 around). So the PCs of a
  running thread are exact: no recompiler profiling build needed.
- 2026-10-21, **first profile: sea at Dragon Roost** (EUR game, Quest Log 1, handheld, CPU 1224 MHz; 29.3-30 fps,
  core 1 93-94%, render thread 742-773 ms/s, 3,300 draws). Main thread: 85.5% game code, 14.5% runtime; waiting
  only ~5.5% (GX2DrawDone 3.0%, vsync 2.4%). Flat: the top function is 4.1%. Inclusive: the process manager
  (`fpcM_Management`) 32%, of which the draw (`dScnPly_Draw`) 21%. Self-time leads:
  - `dKyr_drawStar` 4.1% self / 5.9% incl (the star field; the fork hooks it);
  - process searches by ID: `fpcSch_JudgeByID` 2.3% + `cNdIt_Judge` / `cTgIt_JudgeFilter` (linear walks of the
    process lists) ~4.7% together;
  - `dComIfGp_get` 1.8% self (a one-line accessor);
  - grass binding (`Shape_bind`, `ModelBinding_build`, `Texture_bind`) ~4%; sea, weather, clouds, particles
    (`JPAEmitterManager::calc` 2.9% incl) a few % each.
- 2026-10-21, **five places compared** (`main_profile.py --compare`; same session, CPU 1224 MHz, warps from the
  Warp tab, 10 s each, text dismissed). The main thread sleeps (waiting for the next frame, `svcSleepThread`)
  **25.9% at Outset, 41.7% at Windfall, 25.1% in Forest Haven, 45.0% in the Tower of the Gods, 2.4% at sea at
  night** (Dragon Roost, the arrival event). So by day in those places it has room; the night at sea is where it
  is the limit. What the night adds: the star field. `dKyr_drawStar` 4.1% self and, per star, `mDoLib_project`
  1.5%, `dComIfGp_get` 1.8%, `hdGameInfo_dComIfGd_getProjViewMtx` 1.1%, `mDoLib_projectViewport` 0.5% (all near 0
  by day): **~9% of the main thread at night**. The HD projection reads the view-projection matrix through
  `getProjViewMtx` five times per point (each through `dComIfGp_get`). The process searches by ID
  (`fpcSch_JudgeByID`) are 2.3% at night at sea, under 0.2% elsewhere (the event's searches). Everything else is
  flat: no game function above ~1.5% self in daytime places.
  Next: benchmark routes (0.1) for the main-thread-limited cases (night at sea; sailing by day; Dragon Roost with
  bombs; the volcano), then the first candidate, the star field's projection (2.1: native `mDoLib_project` and
  accessors, checked with tools/verify and on the lockstep check).
  Note for the routes: these warps arrive at the save's time of day, and the first sea profile ran during an
  arrival event; a route needs a fixed time and no event.
- 2026-10-21, **the same five places at stock CPU 1020 MHz** (GPU 307, handheld; sys-clk had the CPU at 1224
  before). Sea at Dragon Roost at night, no event (Link standing in the shallows): **27.0-27.7 fps, core 1
  94-95%, main thread never idle** (0% `svcSleepThread`). Outset 29.0-30.0 fps (core 1 80%, idle 16%), Forest
  Haven 29.0-29.9 (81%, 15%), Windfall 29.6-30.0 (61%, 35%), Tower of the Gods 29.2-30.0 (55%, 42%). The night
  at sea is the main-thread-limited case at stock clocks; there the star field again: `dKyr_drawStar` 2.7% self,
  `dComIfGp_get` 2.1%. The rest stays flat (no game function above ~2%).
  **First benchmark route:** warp 3 (sea, room 13: Dragon Roost) at night, Link standing, no event; ~2.5 fps under
  30. Done when its numbers repeat (next session).
- 2026-10-21, **the night-sea route repeats** with the time of day fixed (save `dSv_info_c` + 0x44 = 15.0, 01:00,
  poked before the warp; tools: a route script with the debug server). After the warp the draws climb from
  ~2,600 to ~2,850 over ~15 s (the night sky fills in): only the steady part counts. Two runs: 26.9 and 27.5 fps
  (core 1 95-96%); by day at the same spot 28.6-29.3. So the night sky costs ~1.5 fps there.
- 2026-10-21, **first optimization: native fast paths for two accessors** (`native_helpers_switch.cpp`, phase 2.1):
  `dComIfGp_get` (the game-info singleton) and `hdGameInfo_dComIfGd_getProjViewMtx` (the HD view-projection
  matrix) reached through `-Wl,--wrap` (14,408 call sites redirected, 84 same-file calls stay direct), no change to
  the generated code; Switch tab Debug "Native game helpers (A/B test)", debug server `native 0|1`, off by
  default. **Night route, A B B A: off 27.52 / 27.33, on 27.77 / 27.79 fps: +0.35 fps (+1.3%).** Profile:
  `dComIfGp_get` 2.00% -> 0.33% of the main thread, `getProjViewMtx` gone from the top. Less than the two
  functions' share: the callers still pay for each call (registers written back and read again around it).
- 2026-10-21, **next target found: the compiler's FPR save/restore helpers** (`f_028F6xxx`, the `_savefpr` /
  `_restfpr` family; 66 entry points): **3.4-3.9% of the main thread by day at Outset and at night at sea**
  (self time only). Float-heavy game functions call them in their prologue and epilogue; recompiled, each saved
  register is two byte-swapped guest stores (ps0 as 8 bytes, ps1 as 4), and since recomp.py lists them as
  functions that change callee-saved registers (`find_saved_clobbers`), every caller writes all its register
  locals back to the Cpu before the call and reads them again after it. Plan (2.2): a recompiler pass that keeps
  the saved FPRs in host locals at the save and puts them back at the restore, without the helper call or the
  guest-memory traffic; generated into its own folder (~46 s for the EU build), checked function by function
  against the current output, then the lockstep check and the routes.
  Process note: a profile must be read with the ELF of the build that ran (symbols move between builds): each
  profile's ELF is kept next to its samples from now on.
- 2026-10-21, **2.2 first pass: dead tails** (`fork_passes.py trim_dead_tail`, `WWHD_RECOMP_DEAD_TAIL=0` off).
  Why the helpers cost so much: they are entry points into one block of code, and each entry's generated body
  ran on past its `blr` into the next entries' code. That code never runs from the entry, but the
  register-locals pass saw it, so a helper that saves one register (f31) loaded f14-f31 from the Cpu (36
  doubles) and wrote them all back. The pass cuts a body at its first unconditional `blr` / `bctr` / `b` with no
  branch target after it (the labels translation collects, jump tables included). Checked: the recompiler run
  from this branch without the pass reproduces `build/gen` byte for byte; with it, of 39,713 functions 39,660
  differ only in the unreachable fall-through tail call (now `__builtin_unreachable()`) and 14 in trimmed code,
  each read: 13 helpers and padding after returns, and `f_028F5E98` (a float routine next to the helpers that
  loaded and stored f14-f31 on every call). Console A/B: next entry.
  The bigger part stays for a second pass: callers still write all their registers back and read them again
  around each helper call (`find_saved_clobbers`), and the save itself still stores each register to guest
  memory.
- 2026-10-21, **2.2 second pass: save/restore helpers inlined** (`fork_passes.py fork_inline_helper`, hooks in
  recomp.py's `call()` / `branch()`; `WWHD_RECOMP_HELPER_INLINE=0` off). A call of a helper becomes the
  helper's straight-line run of loads and stores (through the next entries, to its `blr`; `isync` allowed) in
  the caller; a tail jump takes the `blr` too and returns from the caller. EU build: 305 call sites (sqrtf's
  save of f31 among them); 145 functions change; the 158 calls left go to larger routines with branches in the
  same address range.
  **Console, night route, stock CPU 1020 MHz (every session's log checked for the clock), native helpers off,
  mean of the steady 5 s reports per run:**

  | Build | Runs (fps) | Mean | vs A |
  |---|---|---|---|
  | A: main + profiler + native switch | 27.52, 27.33, 27.40, 27.50 | 27.44 | |
  | B: + dead tails | 27.46, 27.67 | 27.57 | +0.13 (noise) |
  | **C: + helpers inlined** | **28.22, 28.17** | **28.20** | **+0.76 (+2.8%)** |
  | C with native helpers on | 28.27, 28.32 | 28.30 | +0.86 |

  C's slowest run is above A's fastest. B alone is noise: GCC had already removed most of the dead tails'
  register traffic itself (`f_028F5E98` came out the same size); the gain is the callers no longer writing back
  and re-reading every register around a helper call. The native helpers add ~0.1 on top of C (+0.35 on A).
  Checks so far: the generated code compared function by function (above); C warped through 8 places (Outset,
  Link's house, Windfall, Dragon Roost Cavern, Forest Haven, the Tower of the Gods, Hyrule, the Earth Temple):
  pictures normal, no new log errors. Not yet: the lockstep check (0.4), which this kind of change needs before
  it ships, and a longer play session.
- 2026-10-21, **0.4 lockstep check: it already exists** as `tools/gametest` (run_scenario.sh + compare.py, made
  for the decomp build): a headless desktop run with scripted input that traces Link, the camera, the random
  numbers, sound starts, a 64-bit hash of every process's memory after each execute, and the save data, per
  logic step. Used here on macOS (Apple silicon, Metal, Release), a test bench only: the same generated game code
  as the Switch, compiled with clang instead of GCC; the console runs cover the Switch's compiler, and every
  performance number comes from the console. (Local link workaround, not committed: `-Wl,-U,_f_027F4D5C_orig`,
  the weak reference branch `fix/macos-link-weak-orig` is about.)
  Method: `ALIGN=link` (the boot's length varies by a step or two; aligned on Link's first execute, Link, camera,
  random numbers and sounds match from the first step). Baseline against itself (A1 / A2, origins 85 / 87): Link,
  camera, 1,230 sound starts and the save identical; process hashes differ in 17 of 601 steps from 4.2 s, and
  the random numbers in the last 0.4 s: what the runner's mask of uninitialised bytes does not cover yet.
  **Result, scenario: Quest Log 1, walk 20 s (`WWHD_TEST_STICK=2-12:0:1`), A = both passes off, C = dead tails +
  helpers inlined: A2 / C2 (same boot alignment) identical in everything, all 40,280 process hashes over 601
  steps; A1 / C1 identical but for 3 steps of process hashes; the cross pairs show exactly the baseline's
  noise.** The two passes do not change what the game does.
- 2026-10-21, **profile of build C** (night route and Outset at night, 1020 MHz): the star field still leads
  (`dKyr_drawStar` 3.1-3.5% self, `mDoLib_project` 1.8%), then flat. Instruction level (the sampler's PCs
  through `addr2line` on the ELF): the hottest spots in the star code follow an `fdiv` and the return from
  `sqrtf`; and the generated C showed why the function is slow at all: **it is in struct form**, every register
  access a load or store on the Cpu, because the locals passes skipped hooked functions (`not hooked` in
  `fork_locals`; no reason found in the docs or history). The hooked originals (`f_X_orig`) are **9.4-10.1% of
  the main thread** (46-48 functions: the star field, `f_028766CC`, `wave_move`, the sea's `fillVtxBuf`,
  `settingTevStruct` ...). A native port of the star function was ruled out: it would be the decomp's code
  (Nintendo's) in the published runtime; generic recompiler work helps every function instead.
- 2026-10-21, **2.2 third pass: hooked originals keep registers in locals** (`WWHD_RECOMP_HOOKED_LOCALS=0` off).
  EU build: 180 more functions with locals (52 leaves, 128 with calls); the 173 functions that change are all
  `f_X_orig`; `dKyr_drawStar`'s Cpu accesses 683 -> 53. Build D = C + this; console A/B and lockstep next.
- 2026-10-21, **build D (C + hooked locals) measured.** Night route, 1020 MHz, native helpers off: 28.33, 28.68
  fps (mean 28.5; C 28.20 in an earlier session: +0.3). Profile, same route: the hooked originals 10.14% -> 8.42% of
  the main thread; `dKyr_drawStar` itself only 3.14% -> 2.97%: struct form was not its main cost (its time is the
  divides, the per-star projection and its loads). Lockstep (macOS): A2 / D2 identical in everything (40,280
  process hashes), A1 / D1 the baseline's 3-step noise. Kept.
  Restarts: the debug server's `reload` failed 3 times in ~14 today, never in the game's code: `vi` and
  `overlayDisp` (12:57, the console went down), `hbloader` (sphaira) and `omm` (14:51), the forwarder
  ("Application", `nn::diag::detail::Abort` in nnSdk, 15:27, the console rebooted). A separate issue; until it is
  understood, comparisons that need a restart are done one build per session, and A/B within a session uses
  runtime switches.
- 2026-10-21, **the standard performance test** (Pulpparty's `tools/switch/perf_test.py`, merged into this branch):
  `ab` of `main` + the region change (its states were made on a USA console: `bench load` now accepts the other
  region's) against this branch (build E = D + the region change), ABBA twice, 4 boots each, 900 frames per scene,
  Quest Log 1, stock 1020 MHz, the console started from a forwarder (restarts safe again; the earlier reload
  crashes were the title override, PR #56):

  | scene | main fps | this branch | fps (95% CI) | main thread ms (95% CI) |
  |---|---|---|---|---|
  | lava | 27.23 | 27.38 | +0.15 (+-0.33): no measurable change | -0.24 (+-0.42) |
  | windfall | 27.56 | 28.11 | **+0.54 (+-0.29), +2.0%: faster** | -0.67 (+-0.40) |
  | dragonroost | 28.01 | 28.32 | +0.31 (+-0.33): no measurable change | -0.37 (+-0.41) |

  The main thread's time per frame is lower in all three; these scenes are only partly main-thread bound (GPU
  31-36 ms, render thread 26-35 ms a frame), so the fps moves less than at night at sea, where the main thread is
  the only limit.
- 2026-10-21, **1.1 shaders on several threads: uam made reentrant, but 3 threads are slower on the Switch.**
  Branch `feature/parallel-shaders` (from main): uam patch 10 (the `gl_context` one per thread, the log callback
  and the fatal jump thread-local) and `dksh_cache stress`. On the Mac, 24,417 of the game's GLSL sources
  (translated from the console's manifest and the game files): 3 threads give the same DKSH bytes as one, 2.51x as
  fast; ThreadSanitizer 0 reports (46 and an abort without the patch). Then deko3d helper threads (up to 2, only
  jobs a draw asked for; Debug "Shader compile threads", default 1).
  **Console, cold cache (a test folder, no first-start card, the background list off), 1020 MHz:**

  | | 1 thread | 3 threads |
  |---|---|---|
  | uam time per 5 s window with a backlog | ~5,000 ms | ~15,000 ms (three threads busy) |
  | shaders finished per 5 s window | 43 | 21 |
  | mean uam time per shader | 104 ms | 598 ms |

  Each compile takes ~6x as long, so the throughput halves. Suspects: cores 0 and 2 only (shared with the render
  thread and the game's threads; three compilers at the lowest priority time-slice), and Mesa's mutexes (its type
  tables) contended through the kernel. The default stays 1 thread (as before). Next: 2 threads, and the sampler on
  the compiler threads to see where the time goes. (Time-to-settle comparisons were useless: the title's attract
  sequence draws more places the longer it runs.)
  Also: one restart through the forwarder failed at 17:40 (`vi` crashed, then the new process aborted in libnx's
  `__nx_win_init`, getting no window): rarer than through the title override, but not gone.
