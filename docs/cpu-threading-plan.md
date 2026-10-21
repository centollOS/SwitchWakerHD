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
