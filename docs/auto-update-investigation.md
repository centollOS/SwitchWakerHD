# Auto-update from inside the game: investigation (#84)

Question (#84, after the update notice of #83): can the game update itself, instead of the player running the
installer on a computer again? Short answer: **not the game code**, so the update stays a computer build. What
can change is how little the player has to do for it: one click on the computer, and no SD card shuffle.

## What stops a real in-game update

- `wwhd.nro` holds the game's code, recompiled from the player's own dump (`tools/recomp/recomp.py`). It must not
  be distributed (INSTALL.md, "for your own console only"), so there is no prebuilt NRO to download.
- The console cannot build one: the recompiler (Python) and devkitPro's toolchain do not run on Horizon, and a
  build takes 10-30 minutes and several GB on a computer.
- The runtime and the generated code are one program. The generated code includes `ppc.h` (the `Cpu` layout,
  the memory and call macros), calls `hook_X` functions the runtime defines, and is compiled with the runtime's
  flags (hidden symbols, the recompiler passes, PGO, #87). Most runtime releases touch one of these.

## What already exists

- **The notice** (`update_notice_switch.cpp`, docs/update-notice-plan.md): at start the game asks GitHub's API
  for the latest release over HTTPS (libnx's `ssl` service, certificates checked; no switch-curl) and says
  "vX is out: update it with the installer".
- **The installers already update**: running one again reuses its work folder (`EngineBase.cs`: 5 GB free
  instead of 10, most of the first run's files reused), downloads the latest release, rebuilds, and copies
  to the card only what changed (same size and date are skipped), then reads every file back (`VerifyCard`).
  What it does not keep: **the player's choices**. The dump, its format (`--image`/`--wua`/`--game-dir`), the
  forwarder keys, the GameCube game for the minimap, the PT-BR download and "prepare the graphics" are asked
  again every time.
- **Moving files without the card**: the debug server's `put` (`debug_server.cpp`: to a `.part` file, CRC32,
  then `rename`, so the NRO is never half written) and `reload` (`appletRestartProgram`; the forwarder loads the
  NRO again; from hbmenu the player restarts by hand). The NRO uses no romfs, so it can be replaced while the
  game runs. The debug server is a developer tool: no authentication, off by default; it is not the way for
  players.

## The options

### 1. Update from the computer, in one step (recommended)

**1a. Update mode in the installers.** The installers remember the player's choices in the work folder
(`installer-choices.txt`, one `key=value` a line: the dump path and format, the optional files, the shader choice; no keys' contents,
only their paths) after a successful build. At the next start, when the work folder has a build and a newer
release is out, the first page offers **"Update to vX (same game and options)"**: one click runs the same
steps with the saved choices (each path checked again; a missing one goes back to the normal pages), then the
Done page as today. The in-game notice then says "run the installer and choose Update".

- Where: `installer/shared/EngineBase.cs` (save and load the choices, checked like the pages check them), the
  Windows UI (`installer/windows/MainWindow.xaml.cs`) and the desktop UI (`installer/desktop/MainWindow.axaml.cs`,
  macOS and Linux), `Strings.cs` (+ PT), `ui_text_switch.cpp` (the notice text, six languages).
- Cost: **about 2-3 days** with the three platforms' tests. No console work.

**1b. Send it to the console over Wi-Fi.** The step players find hardest is the card: out of the console, into a
reader or hekate's UMS, back. Instead, Dev tab > Updates > **"Receive an update from the computer"** opens a
receiver while that screen is open: the console shows its address and a 6-digit code; the installer's Done page
gets **"Send to the console"** (address and code typed once, remembered). The installer sends only the files that
differ (the NRO, the shader cache when it changed; `game/` only when it is missing), each to a `.part` file with
its SHA-256 checked before the rename, as `put` does; then the game restarts into the new NRO from the forwarder,
or asks the player to start it again from hbmenu.

- Safety: no listener unless the player opened that screen; one connection; the code (shown on the TV, never
  sent in clear: an HMAC challenge) authenticates it; only files under `sdmc:/switch/wwhd/`, no `..`; a size cap.
- Where: a small receiver in the runtime (the debug server's `put` code as the model, not the debug server
  itself), the menu entry and its texts, a sender in `EngineBase.cs` (the same file list `VerifyCard` uses), the
  Done pages of both UIs.
- Cost: **about 3-5 days**, including console tests (Wi-Fi drops mid-file, a full card, the game killed during
  the transfer: the old NRO must stay).

1a alone already makes the update one click plus the card copy; 1b removes the card. 1b also needs the player's
computer and console on the same network (the card stays the fallback).

### 2. Partial in-game updates (not now)

Only files that hold no game code could be downloaded from the game, with the notice's HTTPS code. Today there are
almost none: the menu texts and the guest mods are compiled into the NRO, and what is on the card besides it is the
player's own (`game/`, `save/`, `settings.ini`) or made from their files (the shader cache from the game's shaders,
`translation/pt-BR` from their pack, the minimap mod's data from their GameCube game). **Not worth building now.**
Worth it once something distributable moves out of the NRO into files (language packs, a mod catalogue list); the
download would then be a GitHub release asset (a redirect to `objects.githubusercontent.com`, which the notice's
HTTP code does not follow yet), its SHA-256 from the release, and a `.part` + rename.

### 3. A prebuilt runtime that loads the player's game code (no)

A distributable runtime NRO would load the player-built game code at start. Horizon homebrew has no dynamic
loader: `ldr:ro` needs an NRR (signed, or Atmosphère's patch); a hand-written loader (code memory through libnx's
`jit`, relocations against the runtime's symbols) is possible but large. And it costs what the port fights for:
the game code could no longer be built with the runtime (no hidden symbols, no cross-module inlining, a call
through a table for every hook and HLE function, PGO only per module). At 1020 MHz the main thread is at its limit
(31-36 ms of a 33.3 ms frame in the lava room), and #87's gains came from exactly these. Most releases change
`ppc.h`, the hooks or the recompiler anyway, so the game module would need a rebuild on the computer for them.
**Not worth it.**

### 4. Rebuild on the console (no)

The recompiler and the toolchain do not run on Horizon; memory and time are far over budget.

## Recommendation

**1a now, then 1b.** 1a is computer-only, cheap, and makes the notice's "update it with the installer" a single
click. 1b is the bigger change for players (no card), but needs console tests and a new network receiver, so it
comes second and ships off unless the player opens it. Options 2-4: not now (2 can come back with files to
update).

Follow-up issues: #110 for 1a (the installers' update mode, with the notice text), #111 for 1b (the Wi-Fi transfer).
