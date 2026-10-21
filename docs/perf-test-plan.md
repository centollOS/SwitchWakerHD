# Plan: the Switch performance test for every contributor

Status: design, waiting for review. Branch `feature/perf-test` (from main cfd44c0a).

## Why

The fork has a repeatable performance test (the debug server's `bench` command and `tools/switch/perf_test.py`),
built 2026-10-11. It found the gains that went into v0.4.4 and the loss of main against v0.4.7 that was fixed in
c2887fa. Until now it lived only on a local branch (`perf/benchmark`, rebased by hand onto each build under test).
Other contributors who work like the maintainer, through an AI agent with the console on the debug server, have no
way to answer "is my change faster or slower?".

## The use case

A contributor has changed something in their checkout, has a Switch on the local network with the debug server on,
and asks their agent: "compare my change against main" or "compare it against the last release". The agent builds
both NROs, runs the interleaved test on the console, and answers in the chat with a short table and a verdict
("+0.3 fps in the lava room, Windfall the same, within the noise at Dragon Roost"). The result stays in that chat:
no file to commit, nothing posted to a PR.

## Goals

1. `bench` and `perf_test.py` on main, documented in `docs/perf-test.md` and pointed to from `AGENTS.md`, so an agent
   that reads `AGENTS.md` finds the recipe without further instructions.
2. Two baselines:
   - **main** (`origin/main`): built from a temporary worktree.
   - **the last published tag**: from v0.4.8 on, the tag already contains `bench`, so it is built from a worktree
     like main. For **v0.4.7** (the last tag today, without `bench`), the test applies the `bench` code on top of
     the tag in a temporary worktree (the existing local branch `perf/v047-bench` is that code; it becomes a patch
     file in the repo).
3. A single entry point for the agent: `tools/switch/perf_test.py compare-to main|last-tag [--nro mine.nro]`
   (name left to the builder). It builds the baseline, builds or takes the contributor's NRO, runs `ab` with the fast
   defaults and prints the verdict. The low-level commands (`run`, `ab`, `toggle`, `show`, `compare`) stay.
4. Verdict rules written down in one place (docs and the script's output), so every agent reads the noise the same
   way: a difference smaller than the printed 95% interval (about 0.3 fps in the lava room and at Windfall with
   4 boots each) is "no measurable change".

## Non-goals

- Tags older than the last published one (v0.4.6 and before).
- Sharing or storing results: no PR comment, no results file in git, no upload.
- Regions other than USA (the port supports only the USA dump today; the states check the title id).
- PC, Android or emulator benchmarks.
- Windows as the contributor's computer (MSYS2 with native devkitPro): the baseline worktree shares `build/gen`
  through a symbolic link, which only the container build supports. The one-step command runs on macOS and Linux with
  the container build (podman or docker); the docs say Windows is not supported yet. (The maintainer's answer to the
  builder check.)
- A CI job: the test needs a real console.

## Anti-goals (a failure even if it works)

- Anything that changes the player's experience: `bench` ships in release builds but does nothing unless a debug
  server command calls it. The debug server stays off by default (AGENTS.md, "Defaults are for the player").
- Touching the player's data: the test never writes `settings.ini`, never saves the game, never writes the save
  slots or the `states/` folder (the scenes live in `sdmc:/switch/wwhd/bench/`). Already true of the local version;
  stays true.
- Using the console without warning: the docs tell the agent to tell the user before it deploys or restarts the
  game, and to stop and ask if the console does not answer.
- Changing the clocks: the test keeps the player's clock settings (stock by default); no overclock to "stabilise"
  numbers.
- Results that look precise but are noise: never report a single boot per build as a comparison.

## Constraints

- Fork code isolation: `bench` stays in its own module (`runtime/src/platform/bench_switch.{h,cpp}`) with small
  marked hooks in upstream files (`interp.cpp`, `gfx/deko/backend.cpp` `present()`, `platform/debug_switch.cpp`,
  `savestate.{h,cpp}`).
- The scenes are the 8 portable states in `tools/switch/perf_states/` (made from the maintainer's Quest Log 2 "Link";
  they hold progress and position only, no game code or data, so they can be in the repo). They load onto any Quest
  Log in memory; the contributor needs any save of their own. `perf_test.py --quest-log N` picks which Quest Log the
  file select starts (default 1).
- The console's address comes from the existing `build/switch_host.txt` / `WWHD_SWITCH_HOST` / `--host`.
- Fast defaults (agreed 2026-10-16): scenes `lava,windfall,dragonroost`, `--frames 900`, order `ABBA`, `--repeat 2`
  (~4 min a boot, ~35 min in total). The other scenes sit at the 30 fps cap and show only `main ms` / `render ms` /
  `gpu ms`.
- Building needs devkitPro (`tools/switch/build.sh`), as for any Switch build.

## What goes in (from the local branches)

Taken from `perf/fix-bench` (the most recent `bench` port, on main 279de25), minus what was specific to one
investigation:

- In: `bench` (states, clock hold at a time of day, per-frame recording, control check), `perf_test.py`
  (`run`, `ab`, `toggle`, `show`, `compare`, warm-up, DSP-kill count, `--quest-log`), the states, `docs/perf-test.md`.
- Out: the investigation-only switches and toggles (`perffix`, `drawopt`, `sched`, `sampler`, `pcprof`), the dynamic
  resolution window log lines, and the maintainer's dated result tables (the doc keeps one short table of reference
  numbers on stock settings so a contributor can tell their console behaves like the reference one).

## Left to the builder

- The exact name and flags of the one-step command, and how it finds the "last tag" (`git describe --tags --abbrev=0
  origin/main` or the GitHub latest release).
- Where the temporary worktrees and baseline NROs live (under `build/`, reused when the commit is the same).
- The form of the v0.4.7 `bench` code (patch file applied with `git apply`, or a script), as long as it does not need
  the maintainer's local branches. Once v0.4.8 is the last tag, it can be deleted.
- The step order and commits (one verified step per commit).

## How it is checked

- On the console: `compare-to main` with an unchanged checkout gives "no measurable change" in the three scenes.
- The v0.4.7 path: the baseline NRO it builds is compared (`ab`) with the NRO of the maintainer's local
  `perf/v047-bench` branch (the one behind the 2026-10-16 numbers) and gives "no measurable change". This checks the
  patch, not main against v0.4.7 (after c2887fa that difference is about the size of the noise, so it proves nothing).
  (Builder-check answer, the maintainer's call.)

## Which build the console is left on

At the end the console is left on the contributor's build (the one they are working on) and the verdict says so,
also when the run is interrupted (the script deploys it again on exit). Not the baseline: an old or patched build
left behind would be easy to forget. (Builder-check answer, the maintainer's call.)
- A release build with the debug server off: no new log line, no change in the settings menu, `bench` unreachable.
- A fresh agent session, given only "compare my change against main", finds the recipe through `AGENTS.md` and runs
  it without help (a dry check with a subagent that reads the docs and says which commands it would run).

## Found while planning

- `build/gen` cannot be shared between the baseline and the contributor's checkout: `tools/recomp` changed between
  v0.4.7 and main (the maintainer keeps `build/gen` and `build/gen-sync`). A baseline gets its own `build/gen`,
  generated by its own `tools/recomp/recomp.py` from the contributor's `cking.rpx` and cached by the git tree hash of
  the baseline's `tools/recomp` (`build/perf/gen/<hash>/`, linked as the worktree's `build/gen`: build.sh mounts a
  linked `build/gen` into the container). The contributor gives the `.rpx` once (`--rpx`, or `WWHD_RPX`).
- The baseline NRO keeps the default icon (no game files in the worktree); the icon does not change performance.

## Steps (one verified commit each)

1. **`bench` on main.** `runtime/src/platform/bench_switch.{h,cpp}` from `perf/fix-bench` without `perffix`
   (`g_perf_hle_guard`, `g_perf_slow_pane`); hooks in `interp.cpp` (`bench::game_frame`), `gfx/deko/backend.cpp`
   (`bench::present`), `debug_switch.cpp` (`bench::register_commands`), `savestate.{h,cpp}`
   (`request_save_portable_path`, `control_check`, `portable_arrivals`), `cmake/SwitchPort.cmake`. Not taken: `drawopt`,
   `sched`, `sampler`, the dynamic resolution window log lines. Check: Switch build and macOS build compile.
2. **`perf_test.py` and the scenes.** `tools/switch/perf_test.py` and `tools/switch/perf_states/*.wwstate` from
   `perf/fix-bench`; results under `build/perf/` (not in git). Check: `--help` of every subcommand, `show` and
   `compare` on saved result files.
3. **The v0.4.7 baseline.** `tools/switch/perf_baselines/v0.4.7.patch`: step 1's code on the v0.4.7 tag (same
   `bench_switch.cpp`, hooks fitted to v0.4.7). Check: `git apply --check` on v0.4.7, the patched tree builds.
4. **`compare-to main|last-tag`.** In `perf_test.py`: the last tag is `git describe --tags --abbrev=0 origin/main`;
   fetch first; a detached worktree in `build/perf/worktrees/<tag or commit>`; the patch from
   `perf_baselines/<tag>.patch` when the tag has no `runtime/src/platform/bench_switch.cpp`; `build/gen` per recomp tree
   hash; the NRO cached as `build/perf/nro/<commit>.nro`. The contributor's NRO: `--nro`, else
   `tools/switch/build.sh` in their checkout. Then `ab` with the fast defaults and `--final B` (the console is left
   on the contributor's build, also after Ctrl+C), and a verdict per scene from the 95% intervals. Refuses on
   Windows. Check: both baselines build on macOS.
5. **Docs.** `docs/perf-test.md` for contributors (requirements, the one command, how to read the verdict, what it
   never touches, a short reference table), an `AGENTS.md` section, a line in README's debug server section.
   Check: a fresh subagent given only "compare my change against main" names the right commands.
6. **On the console** (the user is told first): `compare-to main` with an unchanged checkout → no measurable change;
   the v0.4.7 baseline NRO against `perf/v047-bench`'s NRO → no measurable change; a release build without the debug
   server shows nothing new.
7. **Merge to main and push** (the user asked for it on 2026-10-21).
