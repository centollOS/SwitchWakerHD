# Profile-guided optimisation of the Switch build

The recompiled game is machine-translated PowerPC. Its branches carry no layout information, so GCC lays blocks out
and allocates registers from static guesses. A profile of the real game on the console tells it which paths run.
Measured results and the history of what failed: [pgo-progress.md](pgo-progress.md).

**A profile only fits the exact sources it was made from.** GCC compares a checksum of each function's flow graph and
ignores the profile of a function that changed (`-Wno-coverage-mismatch`). Make the profile again after a recompile with a
different recompiler, or after changing a profiled runtime file. It is derived from the player's own recompile, so it is
not committed. PGO is off by default and no release ships a PGO build; distributing profiles is issue #90.

## Result (stock clocks, console, 4 boots per build; full table in [switch-port.md](switch-port.md) "Round 49")

PGO with a game + runtime profile, on top of the recompiler changes: lava 28.36 -> 29.28 fps (+3.3%), windfall 29.22 -> 29.97
(+2.6%, the cap), dragonroost 28.98 -> 29.92 (+3.2%), outset_village 26.04 -> 27.98 (+7.5%); main thread -4.7% to -6.1%.
The scenes the profile was made on exclude those four. The runtime half of the profile (render thread, GX2 command path) is
what moves the Outset village.

## The three modes (`cmake -DWWHD_PGO=...`, `tools/switch/build.sh` passes it through)

| Mode | What | Use |
|---|---|---|
| `OFF` (default) | a normal build | releases |
| `GEN` | the hot game files (`code_hot_*.c`) and, with scope `runtime`, the runtime's own C++ are instrumented | training only, never released |
| `USE` | the same files compiled with the profile in `WWHD_PGO_DIR` | the optimised build |

`WWHD_PGO_SCOPE` is `game`, `runtime` or `game;runtime` (default both).

## Procedure

```sh
# 1. the training build, in its own folder so the normal build stays
WWHD_BUILD_DIR=build/switch-pgo-gen WWHD_PGO=GEN tools/switch/build.sh

# 2. train on the console (debug server on, game started from the HOME icon); it deploys the NRO, visits 26 scenes and
#    fetches the .gcda files (about 30 minutes; the instrumented game is slow, do not judge speed on it)
tools/switch/pgo_train.py train --nro build/switch-pgo-gen/wwhd.nro --out build/pgo

# 3. the optimised build
WWHD_BUILD_DIR=build/switch-pgo-use WWHD_PGO=USE WWHD_PGO_DIR=$PWD/build/pgo tools/switch/build.sh
```

`pgo_train.py` plans: `default` leaves out four scenes (lava, windfall, dragonroost, outset_village and warps 2, 3, 4) so
that the performance test can judge the result on places the profile never saw; `all` trains on everything.

## Traps found while making this work

- **No value profiling** (`-fno-profile-values`, set for GEN). libgcov's value-profiling counters are thread-local and it
  reads the hardware thread pointer `tpidr_el0`, which Horizon leaves 0 (libnx uses `tpidrro_el0`): the first indirect
  call in the instrumented game faults at a tiny address. After changing compile flags, delete the stale objects: `make`
  does not always notice, and an object compiled without the flag crashes the game.
- **Profile names come from the object's relative path** (`-fprofile-prefix-path` = the build folder). Compile from the
  build folder with relative `-o`, as CMake does, or the profile is not found (`-Wmissing-profile` says so).
- **GCC's default `-fprofile-use` options inflate the hot code 29%** (unrolling, peeling, tail duplication). The USE defaults
  turn them off and enable block partitioning (`cmake/SwitchPgo.cmake`); `WWHD_PGO_USE_FLAGS` appends more.
- `-fno-reorder-functions` keeps the sections named `.text.<function>`, which `tools/switch/hot_sections.ld` orders.
- Forcing night on the Outset benchmark states makes the game halt on its own assertion (`d_event_data.cpp:1103`); the
  trainer uses night only for states that survive it.
- Training needs the console at any clock, but compare speed only at stock clocks (sys-clk off, GPU profile default).

## Is the generated code the same on every player's computer? (issue #90)

Yes, for the same repository commit and the same region (Mac, 2026-11-03, main 8db8e2a6). The installer accepts a
`cking.rpx` only when its SHA-256 is one `tools/recomp/builds.py` knows, so every player translates the same bytes, and
the translation is deterministic:

| Comparison (`recomp.py`, the installer's options) | Files differing |
|---|---|
| USA, `PYTHONHASHSEED` 1 against 98765 (Python 3.12) | 0 of 85 |
| USA, Python 3.12 against 3.14 (the installers' Python) | 0 of 85 |
| USA, a fresh translation against an older `build/gen` of the same commit's recompiler | 0 of 85 |
| EUR, `PYTHONHASHSEED` 7 against 31337 | 0 of 85 |
| USA against EUR | 83 of 85 |

So a profile made by the maintainers fits players' builds, under these conditions:
- **One profile per region**: the USA and EUR code differ in almost every file.
- **Pinned to the commit**: the translation also reads the runtime's sources (`fork_passes.py runtime_abi_hooks`,
  `singleflow.py runtime_addresses`: sets, so the order of the directory walk does not matter), so a change to the
  recompiler, the hook lists or the runtime can change the generated code.
- **Same GCC**: the pinned toolchain (`docs/pinned-toolchain-plan.md`) gives every build the same compiler.
- Code mods (`--mod-hooks`) and the decomp build translate differently: a normal build for them.
