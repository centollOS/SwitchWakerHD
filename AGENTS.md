# Notes for agents

## Reaching the Switch: the debug server

The console runs a TCP debug server (port 6543) when **Debug server (network)** is ticked in the settings
menu (Minus, **Switch** tab, **Debug** section); it starts at the next start of the game. Full reference:
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

Options and A/B switches for a test go into the in-game menu (Switch tab, Debug section), not into
`settings.ini`'s `[dev]` section; `[dev]` is for developer variables read at start (docs/switch-port.md,
"Rule for test builds").

The Switch-port agent's design and rules: [.github/agents/switch-port.agent.md](.github/agents/switch-port.agent.md).

## Defaults are for the player

A fresh install (no `settings.ini`) must start with player defaults: debug server off, frame-rate counter
and every diagnostic, perf or capture option off, stock clocks, mods off, no `[dev]` section. A new
diagnostic or test option ships off and goes in the Switch tab's Debug section. Check the fresh-install
defaults again whenever the settings code changes; a player's saved `settings.ini` keeps what they chose.

## Clocks

The defaults are the stock CPU 1020 MHz and the stock GPU profile (docs/switch-port.md, "Settings"). The
higher CPU steps (up to 1785) and GPU profiles are opt-in menu choices for players who want them; do not
make one the default or propose an overclock as a fix: speed comes from the code. The target is 720p
(1280x720 internal) at 30 fps.

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
