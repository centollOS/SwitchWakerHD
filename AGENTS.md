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
