# Debug server (Switch)

A TCP port on the console for the development machine: the log as it is written, files on the SD card,
controller presses, warps, screenshots, and a new build deployed and restarted without touching the console.
It is off unless the settings menu turns it on. There is no password, so use it only on your local network.

## Turning it on

Hold Minus for the settings menu, **Switch** tab, **Debug** section: tick **Debug server (network)**. It is
saved (`switchDebugServer=1` in `sdmc:/switch/wwhd/settings.ini`) and the server starts at the **next** start
of the game, on port 6543. At start the log says
`[debug] server listening on <ip>:6543`. A fixed address for the console (a DHCP reservation in the router)
saves looking it up each time.

On the Mac, tell the client where the console is, either with `export WWHD_SWITCH_HOST=<ip>` or with the
address on the first line of `build/switch_host.txt` (`build/` is not in git).

## Developer variables: the `[dev]` section of `settings.ini`

The `WWHD_*` variables for development and tests (traces, renderer switches, A/B tests: the README lists
them) go at the end of `sdmc:/switch/wwhd/settings.ini`, under a `[dev]` header, one `NAME=value` per line:

```
# Wind Waker HD settings
switchCpuClock=1224
switchDebugServer=1

[dev]
# the passes of two frames in the log
WWHD_DK_TRACE_FRAMES=2500,2501
WWHD_SCHED_STATS=2
```

They are read at the next start, before anything else, and the log lists each one (`[settings] [dev] ...`).
The menu rewrites only its own lines and keeps the `[dev]` section as it is. A variable the menu has a
setting for (`WWHD_CPU_CLOCK`, `WWHD_GPU_PROFILE`, `WWHD_DEBUG_SERVER`, `WWHD_MAIN_SAMPLER`, `WWHD_MOD_*`,
`WWHD_FPS`, the picture's, `WWHD_RES_SCALE`...) is ignored there, with a log line: use the menu.

With the server running, edit it from the Mac:

```
tools/switch/wwhd_debug.py get settings.ini build/settings.ini
# edit build/settings.ini: add or change lines under [dev]
tools/switch/wwhd_debug.py put build/settings.ini settings.ini && tools/switch/wwhd_debug.py reload
```

An `env.txt` from earlier builds is converted once at start: its variables with a menu setting become that
setting, the others go into `[dev]`, and it is renamed `env.txt.old` (the log lists every line).

## The client: `tools/switch/wwhd_debug.py`

| Command | What it does |
|---|---|
| `info` | build, frame, stage, heap, applet type, address |
| `log [--all] [--save F] [--grep RE] [--seconds N]` | the log as it is written; `--all` starts with the last 2 MiB |
| `deploy [NRO] [--no-reload]` | uploads `build/switch-dk/wwhd.nro`, checks its CRC32, restarts the game, waits until it answers again |
| `shot [OUT.png] [--game]` | PNG of the next frame: the window as shown, or only the game's picture with `--game` |
| `press A [B ...] [ms]` | buttons pressed together (`A+B` is the same); 120 ms by default; replies once released |
| `hold X`, `release [X]` | held until released (`release` alone lets go of everything) |
| `stick L\|R x y [ms]` | stick override, -1..1, y up; `stick L 0 0` lets go |
| `warps`, `warp N`, `warp STAGE [ROOM] [POINT]` | the Warp tab's destinations, by number or by name |
| `get`, `put`, `ls`, `rm`, `mkdir` | SD card files; paths are relative to `sdmc:/switch/wwhd` unless they start with `/` |
| `logs`, `lastlog [LOCAL]` | the session logs in `logs/`; download the newest one. The running session's file cannot be opened while the game writes it (its listed size stays 0), so `lastlog` then takes the text the console keeps in memory (`logtext`: the last 2 MiB; the server starts before the first log line) |
| `crashes [--fetch DIR]` | Atmosphère's crash reports (`/atmosphere/crash_reports`) |
| `wait`, `quit`, `reload`, `ping`, `help`, `raw CMD ...` | `raw logtext` prints the kept log text |

Button names: `A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LS RS`. Presses go in where the controller is
read (`platform/input_switch.cpp`), as if they came from the controller, so they reach the settings menu too:
`press MINUS 600` opens it.

A typical loop:

```
tools/switch/build.sh && tools/switch/wwhd_debug.py deploy
tools/switch/wwhd_debug.py log --save build/session.log &
tools/switch/wwhd_debug.py warp 4 && sleep 10 && tools/switch/wwhd_debug.py shot
```

`reload` restarts the application (`appletRestartProgram`), so it only works when the game was started from
the HOME-menu forwarder. Started from hbmenu, use `quit`, then start it again. A game that crashed takes the
server down with it: start the game again from the console, then fetch `lastlog` and `crashes`.

## Protocol

Plain TCP, one command per line: `name arg arg ...`. An argument with spaces goes in double quotes. Every
reply is `ok <n>\n` or `err <n>\n` followed by n bytes (text, or a file's contents).
`put <path> <size>\n` is followed by the file's bytes and replies `crc32 <hex> size <n>`. The file is
written to `<path>.part` and then renamed, so a broken upload never replaces a good file. `log` replies `ok 0`,
then streams the log's text until the client closes the connection. At most 4 connections are open at once,
each with its own thread.

## Code

- `runtime/src/platform/debug_server.{h,cpp}`: the server. It knows nothing about the game, and it builds on
  the Switch and on POSIX hosts, so the same pair can go into SwitchWaker.
- `runtime/src/platform/debug_switch.{h,cpp}`: the game's commands (`info`, `warps`, `warp`, `shot`, `reload`,
  `quit`) and the start from the saved setting (`settings.ini` `switchDebugServer`).
- The hooks:
  - `core.cpp` `log_flush`: the log's text goes to `log` streams.
  - `input_switch.cpp`: the injected buttons and sticks.
  - `gfx/deko/backend.cpp`: the host loop ends on `quit`, and `request_pictures` arms the capture of the
    next frame's pictures (`gfx/deko/capture.cpp`).
- `tools/switch/debug_server_host_test.cpp`: the server on the Mac with stand-in commands, for testing the
  protocol and the client without a console. The build command is at the top of the file.
