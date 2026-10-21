# Performance test (Switch)

A fixed, repeatable benchmark for comparing builds on the console: the same places, the same time of day, the same
camera and Link standing still, a fixed settle time, and per-frame measurements taken on the console. It answers
"is my change faster or slower than main / than the last release?" and is meant to be run by you or by your AI
agent through the debug server ([debug-server.md](debug-server.md)).

- Console side: the debug server's `bench` command (`runtime/src/platform/bench_switch.{h,cpp}`, this fork; small
  marked hooks in `interp.cpp`, `gfx/deko/backend.cpp` `present()`, `platform/debug_switch.cpp` and
  `savestate.{h,cpp}`). It is in every build, release ones too, and does nothing until a debug-server command calls
  it; the debug server itself is off unless the player turns it on.
- Computer side: `tools/switch/perf_test.py` (the runs and the tables) and `tools/switch/perf_baseline.py` (builds
  the baseline to compare against), using the debug client `tools/switch/wwhd_debug.py`.
- Scenes: portable save states in `tools/switch/perf_states/` (copied to `sdmc:/switch/wwhd/bench/` when they are
  missing or have a different size).

## What you need

- A Switch with the game installed from a USA dump, on your local network, the game started from the HOME-menu
  icon (forwarder; the test restarts the game) and **Debug server (network)** on (Minus, **Switch** tab, **Debug**
  section; it starts at the next start). The console's address in `build/switch_host.txt` (or
  `WWHD_SWITCH_HOST`, or `--host`).
- Any save of your own: the scenes load onto whatever Quest Log the file select starts (`--quest-log N`, default
  1; that Quest Log must exist) in memory only.
- macOS or Linux with podman or docker (the container build of `tools/switch/build.sh`), and your `cking.rpx`: the
  baseline gets its own generated code from it (`--rpx PATH` or `WWHD_RPX`; looked for in `game/code/` and
  `Rom/Decrypted/*/code/` otherwise). Windows is not supported by `compare-to` yet: build both NROs yourself and
  use `ab`.
- Time: the first `compare-to` of a baseline generates its code and does a full build (kept under `build/perf/`);
  then each comparison is ~35 minutes of console time, during which nobody should touch the console.

## Compare your change (the one command)

```
tools/switch/perf_test.py compare-to main        # your checkout against origin/main
tools/switch/perf_test.py compare-to last-tag    # against the newest tag on origin/main (the last release)
```

What it compares: `main` is `origin/main` right after the fetch; `last-tag` is the newest tag reachable from it
(today v0.4.7). Your side is your checkout as it is on disk, uncommitted changes included. For a second comparison
right after the first, pass the NRO already built (`--nro build/switch-dk/wwhd.nro`) instead of building again.

What it does: fetches `origin`, builds the baseline in a worktree of its own (`build/perf/worktrees/`; your
checkout is not touched; v0.4.7, which has no `bench`, gets it from `tools/switch/perf_baselines/v0.4.7.patch`),
builds your checkout with `tools/switch/build.sh` (or takes `--nro mine.nro`), then deploys the two in the order
ABBA twice (4 boots each), measures the lava room, Windfall and Dragon Roost for 30 s each per boot, and prints the
table and a verdict per scene. **The console is left on your build**, also when the run is stopped or fails.
Results: `build/perf/ab_<baseline>_vs_mine_<time>.json` (not in git).

### Reading the verdict

The verdict compares the means of the 4 boots of each build against the 95% interval of their spread (Welch's t
for few runs; it includes the boot-to-boot spread below):

- `no measurable change (+0.12 fps +-0.40)`: the difference is inside the noise. With 4 boots each the test
  resolves about 0.3-0.4 fps in the lava room and at Windfall and about 0.05-0.1 fps at Dragon Roost; anything
  smaller is not a result. Do not report it as a gain or a loss.
- `faster: +0.55 fps (+-0.35, +1.9%)` / `slower: ...`: a difference larger than the noise. Three scenes are judged
  at once, so one of them now and then lands just outside its interval by chance: a single scene barely outside,
  with the other two unchanged, is worth a second run (`--repeat 3`) before calling it.
- `at the 30 fps cap; main thread -0.80 ms a frame (+-0.30): more headroom`: both builds hold 30 fps there, so the
  fps cannot move; the main thread's time per frame shows the change instead.
- `not enough runs`: a scene failed (no control after loading, a crash): read the log lines above it.

More boots narrow the interval: `--repeat 3` (6 boots each, ~50 minutes).

## Rules for agents

- Check first that the console answers and runs the USA game (`tools/switch/wwhd_debug.py info`).
- Tell the user before the first deploy (the game restarts) and say how long it will take. If the console does not
  answer, ask them to start the game with the debug server on; do not guess.
- Report the verdict lines and the table; never a single boot per build as a comparison, never a difference inside
  the interval as a gain or a loss.
- The test never changes `settings.ini`, never saves the game, never writes the save slots or the `states/` folder,
  and keeps the player's clocks: do not "fix" noise by changing any of those.
- A change that can be switched live (a debug-server command of the test build) is measured better inside one boot:
  `toggle` below (~15-20 minutes, no boot-to-boot spread).

## The other commands

```
# one build, every scene 3 times (deploys the NRO first; without --nro the game is reloaded)
tools/switch/perf_test.py run --nro build/switch-dk/wwhd.nro --runs 3 --label mybuild

# interleaved A/B of NROs you built yourself: ABBA order run twice = 4 runs per build
tools/switch/perf_test.py ab base.nro new.nro --labels base,new --order ABBA --repeat 2 \
    --scenes lava,windfall,dragonroost --frames 900

# a live switch inside one boot (no boot-to-boot spread): ABBA x 3 recordings per scene
tools/switch/perf_test.py toggle --nro some.nro --cmd-a "mycmd 0" --cmd-b "mycmd 1" --scenes lava,windfall --frames 900

# tables of saved results
tools/switch/perf_test.py show build/perf/mybuild_YYYYMMDD_HHMMSS.json
tools/switch/perf_test.py compare build/perf/a.json build/perf/b.json
```

Options: `--scenes a,b` (default: the 7 standard ones; `all` adds the extras; `compare-to`: lava, windfall,
dragonroost), `--frames N` (default 1800 = 60 s at 30 fps; `compare-to`: 900), `--settle S` (default 20 s after
arriving), `--time T` (time of day held, default 200 degrees: the afternoon), `--no-boot` (the game is already in a
Quest Log: no reload), `--quest-log N`.

Each `run` / `ab` writes `build/perf/<label>_<time>.json` (summary per scene and run), `.csv` (the same) and
`_frames.json` (every recorded frame). `ab` also saves a `_partial` file after every build so an interrupted
session keeps its results.

How a run gets to gameplay: it reloads, presses A on the title screen and the file select until a Quest Log is
being played with the player in control. Then, for each scene: `bench clock freeze 200` → `bench load NAME` → waits
until Link has arrived → waits the settle time → checks the player has control → `bench rec N`. The scene order
shifts by one between runs, so a warm-up or thermal effect does not always land on the same scene.

A portable state's progress goes only into the Quest Log in memory, and nothing writes it to `cking.sav` unless
someone saves the game in the game. The states are kept in `bench/`, not in `states/`, so they do not show in the
Saves tab.

## The scenes

All were made from Quest Log 2's progress ("Link": 13 hearts, three pearls, Swift Sail), so arriving starts no story
cutscene and no dialogue. Link stands still, the camera is behind him, and the time of day is held at 200.

| State | Place | What it stresses |
|---|---|---|
| `outset` | Outset Island, the lookout platform (warp 0) | far view: island, trees, sea; ~3460 draws; at the 30 fps cap |
| `windfall` | Windfall, the pier facing the town (warp 2, turned around) | NPCs, grass; ~1100-1200 draws, below 30 fps |
| `dragonroost` | Dragon Roost Island beach (warp 3) | ~2500 draws, below 30 fps |
| `lava` | Dragon Roost Cavern, the lava room (`warp M_NewD2 2`) | ~4900 draws: main-thread bound (~36 ms of main-thread CPU a frame) |
| `foresthaven` | Forest Haven interior (warp 6) | foliage, ~3800 draws: main and render thread |
| `forbidden` | Forbidden Woods entrance (warp 7) | dungeon, ~2500 draws |
| `sea_totg` | the open sea on the boat before the Tower of the Gods (warp 8) | waves, light load (~960 draws) |
| `hyrule` (extra) | Hyrule Castle (warp 12) | ~1700 draws, at the cap; not in the default set |

Considered and left out: Quest Log 1 ("Pulpo", early game) as the progress, because warps from it start story events
(warp 3 started a dialogue). Windfall's other spawn points face a house or the sea. A boss room was left out because
the fight is random. Full save states (exact snapshots) do not work on the Switch (`savestate.cpp`), so the scenes
are portable states. These are not exact: the actors, the RNG and the waves start fresh, but the place, Link's
position and heading, the camera, the progress, the date and the time of day are the same every time.

To make a new scene: play to the place (any Quest Log; the state takes its progress), stand still with control,
then `wwhd_debug.py raw bench save NAME`, `wwhd_debug.py get bench/NAME.wwstate tools/switch/perf_states/`, and add
it to `SCENES` in `perf_test.py`.

## What is measured (per presented frame, on the console)

`bench rec N` returns one line per presented frame:

| Field | Meaning |
|---|---|
| `dt_us` | time since the previous present |
| `render_us` | render thread busy in that interval (the interval minus its wait for GX2 commands) |
| `main_us` | the game's main thread CPU time since the previous present (`svcGetInfo` thread ticks; the bench's own control check is left out) |
| `gpu_us` | GPU time of the frame without its start's idle (GPU pass timestamps: every 30th frame, otherwise -1) |
| `passes` | game frames (logic passes) since the previous present |
| `draws` | draws executed |
| `behind` | the GPU had not finished the previous frame at present |
| `control` | the player had control (no cutscene, text box, menu, scene change) |
| `res` | internal resolution x 100 (dynamic resolution changes it) |

`perf_test.py` turns them into: fps (frames / time), the frame time's p50/p95/p99, the 5th percentile of 1-second fps
windows, slow frames (> 34.5 ms), mean main-thread ms per frame and per pass, mean render-thread ms, median sampled GPU
ms, % GPU behind, median draws, mean internal resolution, and checks without control (should be 0; a scene with any
is suspect).

At the 30 fps cap the fps cannot show a gain: then compare `main ms` / `render ms` / `gpu ms`, which still show the
headroom. Dynamic resolution (the player's setting, not changed by the test) lowers the internal resolution when
the GPU is behind, so a GPU-side change can appear as `res` instead of fps. Read both.

## Comparing builds: boot-to-boot spread and the in-boot toggle

**Within one boot** the same scene measures almost the same every time: the fps standard deviation of 60 s
recordings is 0.00-0.08 (main thread: 0.01-0.20 ms).

**Between boots** of the same NRO it is much larger in the scenes below 30 fps. In the lava room and at Windfall
a boot is about 0.2 fps sd (range 0.4) in one session, and over 6 boots in two sessions the sd is 0.37-0.39
(range ~1.1). Dragon Roost (0.02-0.05 sd) barely varies. Each boot seems to land in a slightly faster or slower
state (memory layout, thread placement?), and there is also drift over a session: the base build was ~0.5 fps
slower at the end of a 70-minute session than at its start. So:

- Different NROs: interleave (`ab`, the default order ABCCBA ..., `--repeat 2` or more) and give each build at least
  4 boots. The 95% interval printed is from the run spread, so it already includes the boot spread. 4 boots per
  build resolve ~0.3-0.4 fps in the lava room and at Windfall and ~0.05-0.1 fps at Dragon Roost.
- A switch that can be flipped live (a debug-server command): use `toggle`, which never reboots. It loads the scene
  once and alternates A and B recordings (ABBA repeated). 30 s recordings at Windfall resolve ~0.1 fps.

`--warmup 8` (the default) visits each scene once after a boot before measuring. Without it, the first scene
measured after a boot was often the slowest.

Other columns: `kills` counts the game's `[game] kill DSP channel` log lines during the recording (issue #13: the
game's audio DSP-overload guard; read from the log stream). The `behind` per-frame flag is "GPU still busy with
the previous frame **at present**", so it is ~0% here. The log's `[dk] ... GPU behind at N% of presents` is counted
at the frame's first command instead and is usually 100% in these scenes.

Anything that samples or traces the game while it runs (a profiler, a trace option) slows the game itself: a
main-thread PC sampler took the lava room from ~28 to ~26-27 fps. Measure fps with such options off.

## Reference numbers

The maintainer's console (handheld, charging, stock 1020 MHz CPU and GPU profile, the player's settings with
dynamic resolution on), v0.4.7 with the bench command, mean of 4 boots, 60 s per scene (2026-10-16):

| scene | fps | main thread ms |
|---|---|---|
| lava | 27.9 | ~34 |
| windfall | 28.0 | ~35 |
| dragonroost | 28.5 | ~34 |

A console within a few tenths of these behaves like the reference one. The other four scenes sit at the 30 fps
cap. Much lower numbers usually mean an overclock or dynamic-resolution setting off its default, a hot console, or
something else running.
