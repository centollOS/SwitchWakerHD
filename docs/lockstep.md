# Lockstep frame hashes (Switch)

Two builds, or one build with an option on and then off, play the same frames from the same place, and the
console hashes each frame's picture. A host script compares the two lists and names the first frame that differs.
It catches a recompiler pass, an optimisation or a renderer change that changes the picture without crashing
(issue #67; the idea comes from bejeweled3-nx and sa2-nx).

- Console side: the debug server's `bench hash` command (`runtime/src/platform/bench_switch.cpp`) and
  `runtime/src/lockstep.{h,cpp}`, with small marked hooks in `savestate.cpp` (the fixed seeds and counters),
  `gx2/gx2_core.cpp` (the swap's tag), `gfx/deko/backend.cpp` (the present, dynamic resolution held) and
  `gfx/deko/capture.cpp` (the readback). It does nothing until the command is sent: a player's game pays one
  relaxed atomic load per swap and per present. The debug server itself is off unless the player turns it on.
- Computer side: `tools/switch/lockstep.py`, using the debug client and the scenes of the performance test
  ([perf-test.md](perf-test.md), `tools/switch/perf_states/`).

## What you need

As for the performance test: the console on the debug server ([debug-server.md](debug-server.md)), the game
started from the HOME-menu forwarder (the script reloads it), a Quest Log to play (`--quest-log N`, default 1), and
nobody touching the console during a run (the controller goes into the game).

## Use

```
# the determinism check: one build, every scene twice
tools/switch/lockstep.py run --nro build/switch-dk/wwhd.nro --runs 2 --scenes lava,windfall,dragonroost

# two builds: A is deployed and hashed, then B; the console is left on B
tools/switch/lockstep.py ab base.nro new.nro --labels base,new

# one build, a live switch (a debug-server command); --check runs A once more after B and weighs the
# difference against that repeat
tools/switch/lockstep.py toggle --cmd-a "bloom 1" --cmd-b "bloom 0" --check --no-boot

# two saved lists (build/lockstep/*.txt)
tools/switch/lockstep.py compare build/lockstep/A_lava_*.txt build/lockstep/B_lava_*.txt
```

Options: `--scenes a,b` (default `lava,windfall,dragonroost`), `--frames N` (default 300: 10 s of game),
`--skip S` (logic steps after the arrival before the first hashed frame, default 90; keep it above 27, see below),
`--time T` (time of day held, default 200), `--no-boot` (the game is already in a Quest Log), `--mem` (the
diagnostic below). The exit status is 0 when every comparison is identical; with the noise described under
"Determinism" that is rare today, so read the counts (and with `toggle --check`, the verdict line).

The report per scene:

```
windfall: A (bloom 1) against B (bloom 0): DIFFERENT
  DIFFERENT from step 91: pictures differ in 150 of 150 frames, game state (mTimer, cM_rnd) in 0
  the game state is the same in every frame: a difference of the picture alone (the renderer, or what the game draws from the same state)
  first picture difference, step 91: A 8F4E9407B12AC836 1280x720 draws 1771, B 9935A53660D8CCD8 1280x720 draws 1771
windfall: A against A again (repeatable): DIFFERENT
  DIFFERENT from step 96: pictures differ in 6 of 150 frames, game state (mTimer, cM_rnd) in 0
  ...
  verdict: A and B differ in 150 of 150 frames, A twice in 6: a real difference
```

"Game state differs" means the game itself went another way (its random generator or its gameplay frame counter
moved differently): a change to the game's logic, not to the renderer. Same game state with other pictures is the
renderer, or something the game draws from the same state.

For `ab`, get the noise of each build with `run --runs 2` first: a difference in every frame is real, a few frames
in a hundred may be the noise.

## What the console does

`bench hash NAME N [SKIP] [mem]` (one debug-server command; the reply is the list):

1. Arms the run (`lockstep::arm`). While armed, the renderer's dynamic resolution is held at the profile's top factor
   (a GPU-time decision would otherwise change the picture's size from run to run).
2. Loads `bench/NAME.wwstate` with the fixed seeds of `WWHD_TEST_FIXED_SEED` (`runtime/src/fixed_seed.h`): cM_rnd's
   three seeds, the particle system's emitter seed and `g_Counter.mCounter0` are set when the old stage's loading
   frames begin and again at the new stage's first gameplay frame. Without that, the game's state after a load
   depends on everything drawn before it (the title screen, how long the boot took), and two boots diverge from the
   first frame. The `[dev]` switch is not needed: the command arms it for this one load. A lockstep load also sets
   the counters that otherwise run on from the boot across loads: `g_Counter.mTimer` (to 0x1000, at the loading
   frames; the gate follows it) and `mCounter1` (0x101FF55C; at the loading frames and the arrival).
3. At the new stage's first gameplay frame, every looping HUD layout animation (nw::lyt, kept loaded across stages)
   is set back to its first frame: they run on from wherever the boot and the earlier stages left them. They are
   found by structure (an object pointing to a `pai1` block with the loop flag, its frame in the next word), which
   scans the game's memory once: about 2.5 s, the run waits.
4. Counts logic steps from that first gameplay frame. The game thread tags the swaps of steps SKIP+1 to SKIP+N
   (`gx2_core.cpp`: a host-only command `OP_LOCKSTEP` in the render queue right before the swap, so the tag
   travels with its frame however far the render thread is behind) with the step, `g_Counter.mTimer` and cM_rnd's
   seeds.
5. At each tagged present the render thread copies the picture the window shows back from the GPU after the
   frame's submit (it waits for the GPU) and hashes its stored bytes (64-bit, `lockstep::hash_bytes`). That is the
   GamePad picture in GamePad play (the default: the TV then shows the game's "you are using the Wii U GamePad"
   screen, which is not what is hashed), else the TV picture. It is the game's picture as the present pass samples
   it: the HUD is in it; the bars, the FPS counter, the settings overlay and the present pass's own effects (FSR 1)
   are not. A frame held for shader compiles (#49) is hashed too and marked.
6. Replies with one line per frame: `step timer rnd0 rnd1 rnd2 hash w h format draws held`.

The hashed frames are slower (each waits for the GPU and is read back): the game runs at about 14 frames a second
during a run. The steps are logic steps, so the frames compared are the same game moments.

For the first 27 logic steps after the arrival the HUD is not updated (the stage's fade-in): what it shows there is
left from before the load. The default skip (90) is past it.

### The diagnostic (`--mem`)

`bench hash ... mem` adds to the reply:

- `t STEP` and 64 small hashes per frame: the picture's 8x8 tiles, to see where two runs differ;
- at the first hashed frame, a snapshot of the game's memory (static data word by word, the rest in 4 KiB block
  hashes): `w ADDR OLD NEW` lines for the static words that differ from the run before, `b START END` for the blocks;
  from the third run on, `h ADDR OLD NEW` lines for the words of the blocks that differed the time before (small
  numbers only: a counter or an animation frame);
- the first hashed picture saved as `lockstep_0.png` .. `lockstep_3.png` in the game's folder (`wwhd_debug.py get`).

That is how the counters and the layout animations above were found: the run before is the reference, so compare
runs that differ by a known amount (a phase of the picture) and look for the word that differs by as much.

## Determinism

Measured 2026-11-09 on the handheld console (CPU 1020 MHz, GPU 384 MHz, stock clocks; settings.ini with no picture
options), the scenes run twice in a row, 300 frames after skipping 90 logic steps:

| scene | pictures identical | game state |
|---|---|---|
| windfall | 295 of 300 | identical |
| dragonroost | 279 of 300 (288 in a second pair) | identical |
| lava | 0 of 300 | differs from the first hashed frame (cM_rnd's seeds); draw counts differ in most frames |

A known change is told apart from that noise:

| change (`toggle --check`) | A against B | A against A again |
|---|---|---|
| `bloom 1` / `bloom 0`, windfall | 150 of 150 frames differ | 6 of 150 |
| `bloom 1` / `bloom 0`, dragonroost | 150 of 150 | 9 of 150 |
| `fxaa 0` / `fxaa 1`, windfall | 150 of 150 | (not run) |

What is still not repeatable:

- In windfall and dragonroost, single frames here and there (2 to 7 in a hundred) differ with the same game state
  and draw count. The tile hashes put them in the top left of the picture: the hearts and the magic bar. Not yet
  understood; a HUD animation stepped by the guest clock (real time: the run's frames take irregular times) would
  look like this.
- In lava the game's logic itself differs: cM_rnd's seeds part after some tens of steps (the exact step varies
  from run to run, 34 to 91), with the pictures still identical for a while. Something draws random numbers a
  varying number of times; not found yet.

## Limits

- No input: Link stands still. A recording of input to replay is not part of it yet.
- The picture must not depend on time: frame interpolation (the 60 fps modes) draws in-between pictures at times
  that differ from run to run; leave it off (the Switch default).
- Settings change the picture: both runs need the same `settings.ini` (resolution, shadow maps, ambient occlusion,
  bloom...). `ab` deploys only the NRO; the settings stay.
- Shaders compiled during a run can hold a frame (#49): the frame is hashed and marked `held`, and its picture is
  the same, but a first run after a new build may compile shaders the second does not. Run once to warm the cache
  when the report lists held frames.
- The hashed picture is the one the window shows: switching between the GamePad and the TV picture (Minus) between
  two runs compares different pictures.
