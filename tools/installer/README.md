# Installer

The release packages contain no game files. The installer builds the game on the player's machine
from their own disc dump (or an extracted game folder): it extracts the game, translates its code
to C, compiles it with a pinned compiler, links it with the prebuilt runtime and creates the app or
shortcuts. Player instructions are in the main README ("Install (releases)").

## Pieces

| file | what |
|---|---|
| `setup.py` | all of the installation logic (Python 3.8+, standard library only) |
| `toolchains.json` | pinned compilers and Python downloads (URL + SHA-256); CI builds with the same ones |
| `gui/setup_gui.cpp` | **Wind Waker HD Setup**, the graphical front end (SDL3 + Dear ImGui) |
| `install-macos.command` | Terminal launcher, macOS (shipped as `Install Wind Waker HD.command`) |
| `install-windows.bat` + `bootstrap-windows.ps1` | Terminal launcher, Windows (fetches the pinned embeddable Python) |
| `install-linux.sh` | Terminal launcher, Linux (shipped as `install.sh`; falls back to a pinned standalone Python) |
| `test_setup.py` | unit tests of the helpers (`python3 tools/installer/test_setup.py`) |

In a release folder the graphical installer is `Wind Waker HD Setup.app` (macOS),
`Wind Waker HD Setup.exe` (Windows) or `wwhd-setup` plus `Wind Waker HD Setup.desktop` (Linux),
next to the Terminal launcher, which stays as the fallback.

## The graphical installer

It is only a front end: it runs the release's Terminal launcher with `--gui-protocol` as a child
process (so Python is found or fetched exactly as in the Terminal flow) and talks to `setup.py`
over its stdin/stdout. Screens: welcome (or, when installed: play / update / repair / reinstall /
import a save), choose the disc image or game folder (native file dialogs), keys (disc key found
next to the image or chosen; common key as a file or pasted into a hidden field), installation
(a bar per step, an overall bar, the log under "Details"), optional save import (an HD `cking.sav`
folder, or a GameCube `.gci` converted with `tools/savegame/gc2hd.py`), done (Play, Open folder,
Quit), and error screens with Retry and Copy log. On macOS it first checks for Apple's Command
Line Tools and offers Apple's installer, as the Terminal launcher does.

Build: `-DWWHD_SETUP_GUI=ON` adds the `wwhd-setup` target (`cmake/SetupGui.cmake`). SDL3 is linked
statically on macOS and Windows (pinned source, release toolchain); Linux uses the shared SDL3 the
release ships in `sdk/runtime`. The ImGui SDL3 and SDL_Renderer backends are the unmodified ones of
the vendored ImGui release.

Options: arguments it does not know are passed to `setup.py` (`--data-dir DIR`, `--app-dir DIR`,
`--jobs N`). For tests: `--self-test` (start setup.py, wait for its hello, render the first screen,
exit 0), `--automate SCRIPT.json` (scripted clicks, see the comment in `setup_gui.cpp`) and
`--screenshots DIR`. With `SDL_VIDEO_DRIVER=offscreen` nothing is shown on screen (software
rendering), which is how CI and local tests run it.

## `setup.py --gui-protocol`

One JSON object per line. Events on stdout all have `"event"`:

| event | fields |
|---|---|
| `hello` | `version`, `platform`, `data_dir`, `app_dir`, `log`, `installed` (state or null), `game_files`, `save_exists`, `toolchain` |
| `log` | `text` (also written to `setup.log`) |
| `plan` | `steps`: `[{id, title}]` of the installation that starts |
| `step` | `n`, `total`, `title`, `id` (`keys`, `folder`, `compiler`, `extract`, `copy`, `translate`, `compile`, `app`) |
| `progress` | `label`, `done`, `total`, `detail` |
| `reply` | `id` and `cmd` of the request, `ok`; on failure `problem` and `message` |
| `fatal` | `message` (setup cannot run, e.g. the wrong release for this system) |

Requests on stdin: `{"cmd": ..., "id": n, ...}`

| cmd | fields | reply |
|---|---|---|
| `probe` | `path` | `kind` (`image`/`folder`), `disc_key` (found next to the image), `common_key` (where one was found) |
| `check_keys` | `image`, optional `disc_key_file`, `common_key_file` or `common_key_hex` | `title_id`, `files`, `bytes`; problems `disc_key_wrong`, `common_key_wrong`, `wrong_title`, ... |
| `install` | `source` (`image`/`folder`/`installed`), `path`, optional `jobs` | streams `plan`/`step`/`progress`/`log`, then `app`, `exe`, `data_dir` |
| `import_save` | `kind` (`hd`/`gc`), `path`, optional `replace` | `message`; problem `exists` when a save is installed and `replace` is not set (with `replace`, the old save is moved to `save/user.backup-<time>` first) |
| `launch` | | starts the installed game |
| `quit` | | |

Keys: an image install uses the keys from the last successful `check_keys`; they stay in the
`setup.py` process's memory and are dropped when the installation starts extracting. Requests are
never logged or echoed, and no key is written to disk or passed on a command line.
