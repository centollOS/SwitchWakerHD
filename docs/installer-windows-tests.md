# Installer: Windows test sheet

Manual tests of `SwitchWakerHD-Installer.exe` before a release (what changed: `installer-devkitpro-403-plan.md`,
"Hardening"). The `.exe` of a branch comes from the **Build Windows Installer** workflow run by hand on that branch
(Actions → Build Windows Installer → Run workflow; the `.exe` is the run's artifact, version 0.0.0).

For every test keep the `installer.log` of that run. It is in the working folder, not next to the `.exe`:
`%USERPROFILE%\SwitchWakerHD-Build\installer.log`, or `C:\SwitchWakerHD-Build\installer.log` when the user name has a
space, non-ASCII letters or is long (the window shows the path under "Full log"). Every run is appended after a
`==== <date> ====` line, so keep from the last one; copy it out before deleting or renaming the working folder.
**Pass** means it builds, or it stops with a message that names the cause and what to do, before wasting a download or
a build. Note anything unclear in the messages too.

Not supported, not tested: dragging files onto the window (the installer runs as administrator), mapped network
drives and network shares.

## A. Must pass (every release)

| # | Setup | Expected |
|---|---|---|
| 1 | Clean PC (or a fresh Windows user), `.wua` on the Desktop | Builds. Log: Python found or installed, "devkitPro tools ready", "Parallel compiles: N" |
| 2 | Run the `.exe` from Downloads, from the Desktop, and opened straight from inside its zip | Same as 1 |
| 3 | Disc image `Legend of Zelda, The - The Wind Waker HD (USA, Asia) (En,Fr,Es).wux` with its `.key` and `common.key` | Builds |
| 4 | EUR game (`.wua` or image) | Builds; log says "The Wind Waker HD (Europe)" |
| 5 | Windows user name with a space ("Test User") | Working folder `C:\SwitchWakerHD-Build`; builds (#30) |
| 6 | Re-run after a successful build | No new download; reuses the release; builds |
| 7 | Copy to a normal SD card | `switch/wwhd/` at the card's root; the game starts from the Homebrew Menu (title mode) |

## B. Keys and game files (stop before any download)

| # | Setup | Expected |
|---|---|---|
| 8 | Disc image without its `.key` | Game page and Install name the exact `…(En,Fr,Es).key` path |
| 9 | Key saved as `GAME.key` | "Rename GAME.key to …" |
| 10 | Key saved from Notepad as `….key.txt` (extensions hidden) | The ".txt" message, with how to show extensions |
| 11 | No `common.key`, or one with spaces in the hex | Exact message about `common.key` |
| 12 | Extracted game folder at a drive root (`E:\` with `code/`, `content/`, `meta/`) | Builds |

## C. PC setup

| # | Setup | Expected |
|---|---|---|
| 13 | User names "José", one in Cyrillic, one in Chinese, one of 20+ letters | Working folder `C:\SwitchWakerHD-Build`; builds |
| 14 | Game on a second drive (`D:\Games\…`) | Builds |
| 15 | devkitPro installed in `D:\devkitPro` (not `C:`) | Found; builds |
| 16 | Game image on an OneDrive Desktop, "online only" | Warning; builds |
| 17 | Less than 10 GB free on `C:` | Stops before any download |
| 18 | Only a 32-bit Python installed | "Not usable" in the log, Python 3.12 installed, builds |
| 19 | No winget (older Windows 10, or its alias off) and no Python | "this Windows has no winget", install Python from python.org |
| 20 | Defender "Controlled folder access" on, working folder not allowed | The access-denied / antivirus hint |

## D. devkitPro and network problems (#28)

| # | Setup | Expected |
|---|---|---|
| 21 | In the MSYS2 shell `pacman -R cmake` (#28's `cmake: command not found`) | Installs MSYS2's cmake again (if it then doesn't start: "updating MSYS2…", then "MSYS2 updated: the tools start"); builds |
| 22 | Block devkitPro: add `127.0.0.1 pkg.devkitpro.org` to `C:\Windows\System32\drivers\etc\hosts`, tools already installed | Warning; an installed cmake/make is left as it is (no `upgraded cmake` in the log); builds |
| 23 | Same block, and one devkitPro file missing (rename `C:\devkitPro\portlibs\switch\lib\liblz4.a`) | Stops in the devkitPro step with the network reason (undo both afterwards) |
| 24 | PC clock set a year ahead | Stops with the date/time message (or pacman's reason is the clock) |
| 25 | Break MSYS2's cmake: delete one of the DLLs `ldd /usr/bin/cmake.exe` lists whose package is already up to date (`pacman -Qo` names the package, `pacman -Qu` lists the outdated ones; an update reinstalls an outdated one) | "Updating MSYS2…" (pacman -Syu in the log, no `dkp-libs` in it); a deleted file is not brought back by an update, so it then stops with "updating MSYS2 did not fix it" and `ldd` in the log (if the update itself fails, e.g. HTTP 429, it says so instead, with the reason). Partial update (an older `jsoncpp` next to a newer `cmake`, as in #40): repaired, builds |
| 26 | Network unplugged on a re-run | Uses the release already downloaded ("using the one already downloaded") |

## E. Interruptions and the SD card

| # | Setup | Expected |
|---|---|---|
| 27 | Cancel during "Compiling for the Switch… 40%", then Retry | Builds (a link error from a cut-off file says so; the next Retry builds) |
| 28 | SD card with too little free space | "does not fit" |
| 29 | SD card without an `atmosphere/` folder | Warning |

## Results

Write the build or commit tested, then per test: pass / fail, and for a fail the message shown and `installer.log`.
