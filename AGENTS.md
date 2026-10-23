# Notes for agents

## Reaching the Switch: the debug server

The console runs a TCP debug server (port 6543) when **Debug server (network)** is ticked in the settings
menu (Minus, **Dev** tab: tick **Show developer options**, then **Debug server**); it starts at the next start of the game. Full reference:
[docs/debug-server.md](docs/debug-server.md).

- The console's address: `build/switch_host.txt` (first line), else `export WWHD_SWITCH_HOST=<ip>`, else
  `--host <ip>`. `build/` is not in git, so a fresh checkout needs the file written again.
- Check that it answers: `tools/switch/wwhd_debug.py ping` (or `info`: build, frame, stage, heap). If it does
  not answer, ask the user to start the game (and turn the server on if it is off); do not guess.

## Update the console

```sh
tools/switch/build.sh && tools/switch/wwhd_debug.py deploy
```

`deploy` uploads `build/switch-dk/wwhd.nro`, checks its CRC32, restarts the game and waits until it answers.
The restart (`reload`) needs the game started from the HOME-menu forwarder; from hbmenu, `deploy --no-reload`,
then ask the user to start it again.

## Debug

```sh
tools/switch/wwhd_debug.py log --save build/session.log &   # the log as it is written
tools/switch/wwhd_debug.py warps                            # the Warp tab's destinations
tools/switch/wwhd_debug.py warp 4 && sleep 10 && tools/switch/wwhd_debug.py shot build/shot.png
tools/switch/wwhd_debug.py press MINUS 600                  # buttons as if from the controller
tools/switch/wwhd_debug.py get settings.ini build/settings.ini   # SD files, relative to sdmc:/switch/wwhd
```

After a crash the server is gone with the game: ask the user to start it again, then
`tools/switch/wwhd_debug.py lastlog` and `tools/switch/wwhd_debug.py crashes --fetch build/crashes`.

Options and A/B switches for a test go into the in-game menu (Dev tab, behind its "Show developer options" checkbox), not into
`settings.ini`'s `[dev]` section; `[dev]` is for developer variables read at start (docs/switch-port.md,
"Rule for test builds").

The Switch-port agent's design and rules: [.github/agents/switch-port.agent.md](.github/agents/switch-port.agent.md).

## Performance: is a change faster or slower?

[docs/perf-test.md](docs/perf-test.md). With the console on the debug server (above):

```sh
tools/switch/perf_test.py compare-to main       # the checkout against origin/main
tools/switch/perf_test.py compare-to last-tag   # against the last release (the newest tag on origin/main)
```

It builds the baseline in its own worktree under `build/perf/` (the first time it also needs the user's
`cking.rpx`: `--rpx PATH`), builds the checkout, runs 4 interleaved boots of each (~35 minutes of console time)
and prints a verdict per scene; the console is left on the checkout's build. macOS and Linux only. Tell the user
before it starts (the game restarts several times; nobody should touch the console meanwhile), report the verdict
lines as they are, and never call a difference inside the printed interval a gain or a loss.

## Defaults are for the player

A fresh install (no `settings.ini`) must start with player defaults: debug server off, frame-rate counter
and every diagnostic, perf or capture option off, stock CPU clock and the fixed GPU profile (384 MHz), mods off, no `[dev]` section. A new
diagnostic or test option ships off and goes in the Dev tab (its content is hidden until "Show developer options"; that checkbox is off by default). Check the fresh-install
defaults again whenever the settings code changes; a player's saved `settings.ini` keeps what they chose.

## Clocks

The game runs at the stock CPU 1020 MHz and, in handheld, always at Nintendo's 384 MHz GPU profile, one of its
handheld profiles for games (the owner's choice, 2026-10-23, from `perf_test.py gpu-profile`: at Outset's tall grass
307 -> 384 MHz is +4.7 fps, 384 -> 460 MHz +0.5 fps; docs/perf-test.md, docs/switch-port.md "Settings"). It is the
same for every player: the menu offers no clock or GPU profile choice (a profile saved by an older version is reset
to 384 at start); only tests set another (`WWHD_TEST_GPU_PROFILE` in `[dev]`). The port sets no overclock (no CPU
above 1020, no GPU above Nintendo's profiles; sys-clk is the player's own): do not propose one as a fix; speed comes
from the code. Performance tests state the GPU profile they ran at, and check the `[dk] clocks` lines: sys-clk
overrides the port's clocks. The target is 720p
(1280x720 internal) at 30 fps.

## The shader template (issue #88)

Releases attach `shader-template.sealed` from the private repository `centollOS/SwitchWakerHD-shader-manifest`,
pinned by `tools/switch/shader_template.ref`. After the test console has played, `tools/switch/update_shader_template.sh
GAME_DIR` says whether its recording adds shaders (and makes the new sealed template); `attach_shader_template.sh pin`
/ `TAG` pin and attach it. Never commit the sealed or an unsealed template, a console's `shader_manifest.bin` or any
`shadercache_*.bin` to this repository (`guard.py` refuses them). A change to the shader translation (the Latte
decompiler, translation keys) means distilling the recordings again. [docs/shader-manifest-release-plan.md](docs/shader-manifest-release-plan.md).

## The SD card without the debug server

- Best: hekate's USB mass storage (UMS). The SD mounts at `/Volumes/SWITCH SD` (FAT32): plain `cp`, then
  `sync`; the user ejects it before leaving UMS. The port's files are in `/Volumes/SWITCH SD/switch/wwhd/`.
- MTP is fragile: one operation at a time, `killall icdd` first (Image Capture grabs the device), never kill
  an MTP process mid-session, list one folder rather than the whole card. If a session fails twice, stop and
  ask the user to restart the MTP app on the console.

## Checks

Per change, run the checks that change touches (its build, the affected scenes, its own verification).
Full regressions only at checkpoints (every few integrated changes, or at the end of a milestone); bisect
if a checkpoint fails.
