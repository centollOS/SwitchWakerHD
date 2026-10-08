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

### Debug server (developers)

For development there is a debug server: from the computer, over the local network, you can
deploy a new build and restart the game without touching the console, follow the log as it is
written, take screenshots, press buttons and warp. It is **off by default**: players never need it
and the game opens no network port unless it is turned on in the options menu (**Switch** tab,
**Debug** section, *Debug server (network)*; it starts at the next start). It has no password, so use
it only on your own network.

```sh
echo <console ip> > build/switch_host.txt                  # once; the log prints the address
tools/switch/build.sh && tools/switch/wwhd_debug.py deploy  # upload, check, restart (~10 s)
tools/switch/wwhd_debug.py log                               # the log, live
tools/switch/wwhd_debug.py shot                              # PNG of the next frame
tools/switch/wwhd_debug.py press A                           # also hold, release, stick, warp
```

`deploy` restarts the game, so start it from the HOME-screen icon (forwarder). Every command and the
protocol: [docs/debug-server.md](docs/debug-server.md).

Developer and test variables (`WWHD_*`: traces, renderer switches) go in a `[dev]` section at the end
of `settings.ini`, one `NAME=value` per line, read at the next start; the menu keeps that section as it
is. Over the debug server: `get settings.ini`, edit, `put` it back and `reload`
([docs/debug-server.md](docs/debug-server.md)).

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
  Pro Controller when the game asks.

### Saves, settings and updates

- Saves: `sdmc:/switch/wwhd/save/`. Back up this folder.
- Save states: `sdmc:/switch/wwhd/states/`.
- Settings from the options menu: `settings.ini`. An `env.txt` from an earlier build is converted into
  it at the first start and renamed `env.txt.old`.
- To update, replace `wwhd.nro` (and `shadercache_dksh.bin` if you rebuilt it). Saves, settings and
  caches stay.
- Logs, if something goes wrong: `logs/`, one `wwhd_<date>_<time>.log` per session (the newest is this
  run); the 10 most recent are kept.

## Options

Hold **Minus** for half a second while playing to open the options menu (a short press still goes
to the game). **L / R** change tabs, **B** or **Minus** closes it. Everything you choose is saved
and comes back at the next start.

| Tab | What you'll find there |
|---|---|
| **Saves** | Five save-state slots: save where you stand, come back to that spot in seconds |
| **Switch** | Resolution, picture, performance, frame-rate counter, rumble and gyro aiming |
| **Warp** | Travel straight to any island, dungeon or room |
| **Mods** | Built-in gameplay and camera mods, and cheats |
| **Language / About** | The game's language, version information |

### Controls and gyro aiming

- **Play with what you have.** Joy-Con, a Pro Controller or the console in handheld mode all
  work. They act as a Wii U Pro Controller, so the map, items and menus all sit on one screen.
- **Rumble:** the Joy-Con or the Pro Controller vibrates whenever the game asks for it, as the Wii U
  controllers did. One switch turns it off.
- **Gyro aiming**, as on the Wii U GamePad: aim the bow, hookshot, boomerang, grappling hook,
  telescope and Picto Box by moving the Joy-Con, the Pro Controller or the whole console. Tune it
  to your hands:
  - how turning works: *Player space* (turn however you hold it, recommended), *Yaw* (as if the
    controller lay flat) or *Roll* (tilt it like a steering wheel);
  - sensitivity left/right and up/down, each one invertible, with a *Default* button;
  - *Recalibrate* if the view drifts while you hold still.

  The game's own *Options > Gyro* switch still applies, and the right stick takes over while you
  push it.

### Picture

- **Resolution for handheld and for docked, each its own:** 720p, 900p or 1080p. It switches by
  itself when you dock or undock. *Dynamic resolution* lowers it only for the moments the console
  can't hold 30 fps, and raises it again right after.
- **Picture adjustments:** exposure, contrast, saturation and gamma, with *Original colours* and
  *Vivid* presets. Tame a glaring sea and sky, or make the colours pop.
- **16x anisotropic filtering:** sharper ground, sand and water textures seen at an angle. It costs
  some GPU time, so it starts off; try it, especially docked.
- **Frame-rate counter** in the corner: off, frame rate, or frame rate and load.

### Performance

The game is **perfectly playable at the console's stock clocks**, which are the default (CPU
1020 MHz, GPU 307 MHz in handheld): 30 fps on the opening island, a smooth 26-29 fps in the busiest
scenes. If you want those scenes at a steady 30, choose the mild **1224 MHz CPU** step, marked
*Recommended*: it stays well clear of any heat or battery issue. Higher CPU steps (up to 1785 MHz),
Nintendo's other handheld GPU profiles and a 614 MHz GPU overclock are there too; they drain the
battery faster and warm the console. If you use sys-clk, give this game no profile there, or the
two fight over the clocks.

### Save states

Save in any of five slots and load it whenever you like: Link is back on that spot, with the hearts,
items, rupees and progress he had, and on his boat if he was sailing. A state is a small file of a
few KB. Enemies and cutscenes start fresh when you load. You can save whenever you control Link, so
not during a cutscene, a dialogue, a menu or a scene change; a notice tells you when it has to wait.

### Warp

Jump to any of the main places, or to any of the game's stages and rooms, from a list. Visiting a
place before the story gets there can break its events, so try it on a copy of your save.

### Mods and cheats

All built-in mods start off; turn on the ones you like.

| Mod | What it does |
|---|---|
| Direct right-stick camera | The camera turns at once with the right stick, without the original easing; adjustable speed |
| First-person shortcut | Look in first person with R3 |
| Climb any wall | Grab and climb walls, with a stamina wheel; A or B lets go |
| Quick doors | Doors open and close four times faster |
| Fast scene changes | Quicker fades and transitions; gameplay keeps its normal speed |

**Cheats:** all items; the Master Sword and Mirror Shield; 20 hearts, double magic and 5,000
rupees; infinite health, magic, arrows and bombs. Save in game to keep what they give you. The
story cheats (all songs, all Triforce shards, a dungeon's map, compass and boss key, a small key)
can break story events, so use a spare save file.

### Language

The game speaks the console language you choose, from the languages your copy of the game carries
(it applies at the next start).

## Status

Boots and plays on a Switch in handheld mode, with sound, controllers, saves, **working save
states**, the options menu and mods. Played by hand mostly on the opening island, Dragon Roost and the sea
around it, and areas reached with the Warp tab. **It is perfectly playable at the console's stock
clocks** (CPU 1020 MHz, GPU 307 MHz): 30 fps on the opening island, and a smooth 26-29 fps in the
busiest scenes, such as sailing near Dragon Roost and the volcano island. The recommended CPU
setting of about 1.2 GHz (1224 MHz, *Recommended* in the Switch tab) is only for extra stability
there, holding a steady 30. CPU and GPU run at their stock clocks by default.
Shader and texture stutters on first use are much reduced, and shaders seen in an earlier session
no longer stutter. Docked it renders at 1080p (picture profile per mode in the Switch tab: handheld
1x, docked 1.5x, with dynamic resolution), tested on a TV at a median of 29.9 fps. Known issues and
open work: [docs/switch-port.md](docs/switch-port.md) (latest: "Round 47") and
[docs/deko3d-plan.md](docs/deko3d-plan.md).

## Changelog

### 2026-10-08: post office letters (round 47)

- Fixed: in the Rito post office's letter-sorting game the letters were black, so it could not be
  played. Models without normals (like the letters) are now lit the way the Wii U's GPU does it.
  Confirmed on the console.

### 2026-10-08: settings.ini replaces env.txt

- Every option lives in the options menu (`settings.ini`). New in the **Switch** tab's **Debug** section:
  *Debug server (network)* (off by default, applies at the next start), and the main-thread sampler is
  now saved. Developer variables go in a `[dev]` section of `settings.ini`. An existing `env.txt` is
  converted at the first start and renamed `env.txt.old`.
- Old test switches removed (the `WWHD_GL_*` names, the finished grass bisection, rollbacks of
  long-confirmed renderer paths, the test pattern and a model probe); nothing changes for players.

### 2026-10-08: debug server for development

- **Network debug server** for developers, **off by default** (then turned on with `WWHD_DEBUG_SERVER=1`
  in `env.txt`; now in the options menu). `tools/switch/wwhd_debug.py` deploys a new build and
  restarts the game, and it streams the log, fetches the session logs and Atmosphère's crash reports, takes screenshots,
  injects button presses and sticks, and warps. That removes the reboot to hekate for every test.
  Tested on the console: a 39 MiB build is uploaded, checked and running again in under 10 s.
  [docs/debug-server.md](docs/debug-server.md).

### 2026-10-08: save states work, rumble, upstream v0.2.8 (round 46)

- **Save states work on the Switch** (confirmed on the console): five slots in the Saves tab. They
  are now upstream's *portable* save states, a small file with your progress and Link's place (the
  boat too), saved and loaded with the game's own save functions. The old full save states never
  worked on the console and are gone there, with Crash Recovery, which used them.
- **Gyro aiming in Pro Controller mode** (the default), a lower default sensitivity, a choice of
  turning axis and *Recalibrate* for drift (upstream #45, #71).
- **Rumble**: the Joy-Con and Pro Controller now vibrate when the game asks (Switch tab > Rumble).
- **16x anisotropic filtering** in the Switch tab's Picture section (off by default).
- Fixed: a crash with quick doors in Tingle's jail on Windfall (upstream #61).
- The recompiled game code re-reads memory in its wait loops, so a thread waiting for another core
  can never hang there (upstream #62).
- Also from upstream: fan translations (including right-to-left scripts) as content mods.

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

**Clocks**
- The GPU now runs at the console's stock clocks by default (was Nintendo's 460.8 MHz profile with
  memory 1600 MHz); a choice saved in the menu is kept.
- The CPU now runs at the stock 1020 MHz by default (was 1224 MHz); a choice saved in the menu is
  kept. The 1224 MHz step is marked *Recommended*: the game is perfectly playable at stock clocks,
  and this mild overclock only steadies the frame rate in a few busy scenes.

**Notes**
- At the fully stock CPU (1020 MHz) the busiest scenes, such as sailing near Dragon Roost and the
  volcano island, run at a smooth 26-29 fps (there the game's own code is the limit, not the
  renderer); the recommended 1224 MHz holds a steady 30 fps.

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
