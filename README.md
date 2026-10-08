# SwitchWakerHD

## What it is

SwitchWakerHD is an unofficial native port of a 2013 Wii U adventure game (title ID
`00050000-10143500`, USA, version 0) to the Switch (homebrew). The game's PowerPC code is
translated to C ahead of time by a static recompiler, the Cafe OS libraries the game uses are
reimplemented natively, and its GX2 graphics are drawn with deko3d, the Switch's own GPU API. There
is no CPU emulation and no GPU command emulation.

It is a fork of [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp), which runs
the same recompilation on macOS, Linux, Windows and Android. Those platforms still build from this
repository; their instructions are in the upstream README, kept as
[docs/upstream-README.md](docs/upstream-README.md). How the recompilation works:
[docs/how-it-works.md](docs/how-it-works.md).

## Standing on the shoulders of others

SwitchWakerHD would not exist without these projects. The hard part, recompiling the game and
reimplementing its system, is their work; this repository adds the layer that runs it on the
Switch.

- **[ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp)** (Lukas S and its
  contributors: Sean13128, rhemfur, resadent, arcadematicas and others): the static recompiler, the
  Cafe OS and GX2 runtime, the Metal and Vulkan renderers, the settings overlay, the mods and the
  tools. Almost everything outside the Switch layer is theirs.
- **[Cemu](https://github.com/cemu-project/Cemu)**: its GPU address library and shader decompiler
  (vendored), whose GLSL the deko3d renderer compiles; parts of the OS layer follow it.
- **[zeldaret/tww](https://github.com/zeldaret/tww)**: the GameCube decompilation the function
  names and the 60 fps work are matched against.
- **[devkitPro](https://devkitpro.org)**, **[libnx](https://github.com/switchbrew/libnx)**,
  **[deko3d](https://github.com/devkitPro/deko3d)** and **[uam](https://github.com/devkitPro/uam)**
  (fincs and the devkitPro team): the Switch toolchain, the GPU API and its shader compiler, which
  this port runs on the console too.
- **[nx-hbloader](https://github.com/switchbrew/nx-hbloader)**,
  **[hacBrewPack](https://github.com/TooTallNate/hacBrewPack)** and
  **[hactool](https://github.com/SciresM/hactool)**: the HOME-menu forwarder.

If you enjoy this port, the projects above are where the credit belongs. Every component and its
license: [THIRD_PARTY.md](THIRD_PARTY.md).

## How it was made: AI use

Be aware of this before you use or build on this repository:

- **This fork's own code was written with AI.** Nearly all of the Switch work (the libnx platform
  layer, the deko3d renderer, the uam integration and its patches, the Switch options menu, the
  tools under `tools/switch/`), its documentation and the commit messages were written by an AI
  coding agent, Anthropic's Claude through Claude Code. The human authors chose what to build, set
  the priorities, played and tested every build on a real Switch, reported the bugs and decided
  what went in. Commits written with the agent say so in a `Co-Authored-By: Claude` line.
- **The upstream project** describes its own authorship in its README
  ([docs/upstream-README.md](docs/upstream-README.md)); its Vulkan renderer, for example, is
  credited there to OpenAI Codex.
- **How it is checked**: builds of the NRO and of the desktop targets the changes touch, play on
  hardware (handheld, stock and official clock profiles), on-console frame captures compared
  against the game's own textures, and the logs of every session. Bugs can still slip through;
  reports are welcome.

## What you need

- **Your own legally obtained copy of the game**: the USA version, title `00050000-10143500`,
  version 0 (the disc or eShop release without the update), dumped from your own Wii U. The
  runtime checks `code/cking.rpx` against that version at startup.
- **This repository contains no game code, assets, keys or data.** The NRO you build contains the
  game's code recompiled from your dump, and the shader caches are made from the game's shaders:
  they are for your own use only; do not share them.
- **No release binaries are provided** for the Switch, precisely because they carry the
  recompiled game.

## Build for the Switch

**The short way** (step-by-step guide for players: [INSTALL.md](INSTALL.md)):
`python3 tools/switch/make_sd.py --image game.wux` (or `--game-dir <extracted game>`) checks the
game version, recompiles, builds the NRO in Docker and lays out `build/sd/switch/wwhd/` for the SD
card. The steps it runs, by hand:

Requirements: Python 3 with `pycryptodome` (for the extractor), and Docker or Podman (the Switch
toolchain runs in the `devkitpro/devkita64` image). On the Mac also the Xcode command line tools.

```sh
git clone https://github.com/centollOS/SwitchWakerHD switchwakerhd && cd switchwakerhd
# 1. extract your dump into game/ (game.wux with game.key next to it, plus your common key;
#    a Cemu .wua or an already extracted folder works too: docs/upstream-README.md)
python3 tools/wudextract.py game.wux extract game     # -> game/code, game/content, game/meta
# 2. recompile the game code to C (build/gen/, stays on your machine)
python3 tools/recomp/recomp.py game/code/cking.rpx build/gen
# 3. build the NRO
tools/switch/build.sh                                 # -> build/switch-dk/wwhd.nro
```

`WWHD_JOBS=<n>` limits parallel compiles (the generated code needs a lot of memory per file);
`WWHD_DEKO3D_DEBUG_LIB=ON` links deko3d's debug library (`wwhd_dk_debug.nro`: every GPU call
checked, readable errors). Details, architecture and the history of the port:
[docs/switch-port.md](docs/switch-port.md); the renderer: [docs/deko3d-plan.md](docs/deko3d-plan.md).

### The shader cache

The renderer compiles each game shader with uam the first time it is drawn (about 70 ms on the
console, in a background thread; the object it draws appears a few frames late) and keeps the
result in `shadercache_dksh_local.bin`, so a shader is compiled once per console. To start without
those first-time hitches, `tools/switch/dksh_cache` compiles a whole list of shaders on the
computer into `shadercache_dksh.bin`:

```sh
tools/switch/dksh_cache/build.sh build <shadercache_gl.bin> build/shadercache_dksh.bin
```

`shadercache_gl.bin` is the list of shader sources the console writes as it plays
(`sdmc:/switch/wwhd/`); copy it back to the computer from time to time. A complete list was made by
touring every stage with a headless desktop build that was removed with the OpenGL renderer (in git
history at `207349b`; [docs/switch-port.md](docs/switch-port.md), "Harvesting the shader cache").

## Install on the Switch

### What you need

- A Switch running custom firmware (Atmosphère) with the Homebrew Menu.
- The NRO you built and, optionally, the shader cache.
- Your extracted game (`code/`, `content/`, `meta/`).

### Copy the files

The SD card must end up like this:

```
sdmc:/switch/wwhd/
├── wwhd.nro                  the game
├── shadercache_dksh.bin      compiled shaders (optional, see above)
├── game/                     your extracted game: code/, content/, meta/
├── env.txt                   optional: WWHD_* options, one NAME=value per line
└── (created by the game: save/, settings.ini, logs/, shader caches, captures/)
```

Put the SD card in the computer (or mount it over USB with hekate's *Tools → USB Tools → SD Card*)
and copy the files.

### Start the game

- Open the Homebrew Menu **in title mode**: hold **R** while starting any installed game, then pick
  **SwitchWakerHD**. Opened from the album (applet mode) the game has far too little memory.
- Or install the HOME-screen icon (forwarder), which always starts it in title mode:
  [tools/switch/forwarder/INSTALL.md](tools/switch/forwarder/INSTALL.md).
- Controllers act as a Wii U Pro Controller, so the game draws everything on one screen: pick the
  Pro Controller when the game asks. The menu's Switch tab makes them the GamePad instead (choose it
  in the game's options too): **ZL + ZR + Minus** then switches between the TV picture and the
  GamePad screen (items, map; the touch screen works on it), and gyro aiming can be turned on.

### Saves, settings and updates

- Saves: `sdmc:/switch/wwhd/save/`. Back up this folder.
- Settings from the options menu: `settings.ini`. `env.txt` values win over it at every start.
- To update, replace `wwhd.nro` (and `shadercache_dksh.bin` if you rebuilt it). Saves, settings and
  caches stay.
- Logs, if something goes wrong: `logs/`, one `wwhd_<date>_<time>.log` per session (the newest is this
  run); the 10 most recent are kept.

## Options

- In-game options menu: **Minus held half a second** (a short Minus goes to the game); B or Minus
  closes it, L / R change tabs. Tabs: Saves (save states), Switch (CPU clock, GPU profile, picture,
  frame-rate counter, controller, gyro aiming, debug options), Warp (teleport to any stage), Mods,
  Language / About.
- **Performance**: the CPU runs at 1224 MHz by default; the menu offers every step of the console's
  CPU table from the stock 1020 up to 1785 MHz. The handheld GPU uses Nintendo's official profiles
  (460.8 MHz with memory 1600 MHz by default), plus a 614 MHz overclock. Higher clocks drain the
  battery faster and warm the console; if sys-clk has its own profile for this title, the two
  fight over the clocks.
- **Frame captures** for bug reports: turn on *Capture a frame with both sticks clicked* in the
  Switch tab's Debug section; **L3 + R3** then writes the frame and its render targets as PNG files
  to `captures/<frame>/` (they contain game imagery: keep them to yourself).
- **Test options** in the Switch tab's Debug section, switched while the game runs and not saved:
  on/off switches for each draw-path optimization (A/B tests), performance diagnostics for the log
  (the main thread's runtime calls, every game thread's CPU use), and *Frame rate (test)*: 30 fps
  (the default), 60 fps interpolation or the experimental true 60. The 60 fps modes need far more
  CPU and GPU than the console has to spare; they are there to try out, not to play with.
- `WWHD_*` options in `env.txt`: [docs/switch-port.md](docs/switch-port.md) and the comments next to
  their code (grep for `WWHD_` in `runtime/src`).

## Status

Boots and plays on a Switch in handheld mode, with sound, controllers, saves, save states, the
options menu and mods. Played by hand mostly on the opening island, Dragon Roost and the sea
around it, and areas reached with the Warp tab. Handheld at the console's stock clocks (CPU 1020
MHz, GPU 307 MHz): about 30 fps on the opening island (~29 in its heaviest view), 26-29 fps
sailing near Dragon Roost and the volcano island, where the game's own code on its main thread is
the limit; at a CPU of 1122 MHz those places hold 30. The menu's default CPU clock is 1224 MHz.
Shader and texture stutters on first use are much reduced, and shaders seen in an earlier session
no longer stutter. Docked it renders at 1080p (picture profile per mode in the Switch tab: handheld
1x, docked 1.5x, with dynamic resolution), tested on a TV at a median of 29.9 fps. Known issues and
open work: [docs/switch-port.md](docs/switch-port.md) (latest: "Retrospective (rounds 39-45)") and
[docs/deko3d-plan.md](docs/deko3d-plan.md).

## Changelog

### 2026-10-08: performance rounds 39-45 (since "Player install kit")

Measured on a Switch at stock clocks (CPU 1020 MHz, GPU 307 MHz); details, numbers and the
retrospective in [docs/switch-port.md](docs/switch-port.md).

**Faster**
- The opening island's heaviest view went from 22 to about 29 fps: the renderer's per-draw work on
  its render thread dropped by about a fifth (unchanged state is skipped, render-target and texture
  lookups are cached, shader data is laid out for the cache, half as many GX2 commands are
  processed, GPU work is submitted in larger batches, and a profiler that only the desktop reports
  no longer runs on the Switch).
- Shadow-map draws whose pixel shader has no effect skip it.

**Fewer stutters**
- Shader variants seen in an earlier session are rebuilt from a new cache file,
  `shadercache_dk_translations.bin`, instead of being translated again: effects and arrivals that
  stuttered every session now only do so the first time.
- New textures are unpacked about three times faster (about 1.7 ms instead of 5.5 ms each).

**Menu** (Switch tab, Debug)
- On/off switches for each renderer optimization, for A/B tests while the game runs.
- Performance diagnostics for the log: the main thread's runtime calls and every game thread's CPU
  use.
- *Frame rate (test)*: 60 fps interpolation and the experimental true 60, to try out (not saved;
  the game starts at 30 fps as before).

**Logs**
- One log per session in `logs/` (`wwhd_<date>_<time>.log`, the newest is the current run); the
  10 most recent are kept, and there is no `wwhd.log` next to the `.nro` any more.

**Known**
- Sailing near Dragon Roost and the volcano island stays at 26-29 fps at stock clocks: there the
  game's own code on its main thread is the limit, not the renderer (30 fps at a CPU of 1122 MHz).

## License

- The project's code, upstream's and this fork's: **Mozilla Public License 2.0**
  ([LICENSE](LICENSE)), as upstream licenses it.
- Vendored third-party code keeps its own license (Cemu MPL-2.0, uam zlib / MIT, {fmt} MIT,
  Dear ImGui MIT, xxHash BSD-2-Clause, metal-cpp Apache-2.0): [THIRD_PARTY.md](THIRD_PARTY.md).
- Contributing: [CONTRIBUTING.md](CONTRIBUTING.md).

## Disclaimer

Independent project, not affiliated with or endorsed by any video game company. Trademarks
mentioned or alluded to belong to their respective owners. No game content is included or
distributed.

---

## Authors

Pulpparty, depende3000, and the SwitchWakerHD contributors, with an AI coding agent (see "How it
was made: AI use"), on top of ZeldaWWHDRecomp by Lukas S and its contributors. Thanks to everyone
behind the projects in "Standing on the shoulders of others".
