# Verifying the game files and the SD card copy (plan)

Branch `feature/verify-files` (on `feature/desktop-linux`, which has the installers' shared code). Issue #48, after
#46: a missing or damaged game file shows up only as a crash somewhere in the game, which is hard to tell apart
from a bug in the port.

## Goals

1. **The game against a list.** Each supported build (USA, EU) gets a list of content/'s files with size and
   SHA-256 (`tools/recomp/builds/<build>-files.txt`: names, sizes and hashes only, never game data).
   `make_sd.py` checks the game against it right after cking.rpx's version and says which files are missing, cut
   off or damaged. A **warning**, not a stop (user's call, 2026-10-19): whoever changed files on purpose still gets
   a build. The installers show it to the player.
2. **The SD card copy, read back.** Today a file already on the card with the same size (and date, on Windows) is
   not copied again, damaged or not. After the copy every file of switch/wwhd/ is read back from the card, past
   the system's cache, and compared with the build folder; a file that differs is copied again and checked once
   more; still different: the card may be damaged. Windows (after robocopy), macOS and Linux.

Not in it (user's call): the copy over USB with DBI (MTP, Windows and Linux) keeps its size check. A **Verify game
files** button on the console (#48's point 3) is for later.

## Steps (one commit each)

1. `tools/switch/game_files.py` (check, make), the USA and EU lists from known-good dumps, make_sd.py's warning.
2. The installers: the warning on the done page and in the log (shared code).
3. The SD read-back: desktop (macOS, Linux) CopyToSd, Windows after robocopy.
4. Docs (INSTALL.md, release checklist), tests.

## Status

- [x] 1. Lists and make_sd.py. USA 1007 files and EU 1009, from the decrypted USA and EU dumps (both cking.rpx
  hashes as in builds.py); a second USA copy (build/sd-game) matches. Check of 1.7 GB: about 4 s cold on this Mac.
  Tested on a clone with one file removed, one cut off and one byte changed: all three reported, extra files
  ignored; make_sd.py prints the warning in step 2 and goes on.
- [x] 2. Installers' warning: EngineBase reads make_sd's warning (count and lines), a warning in the log after the
  build, and on the done page (both windows) the count, three of the files and what to do. Tested with the engine
  harness and a stand-in make_sd.py printing the warning: count 3 and the three lines read, the log warning shown.
  (The installers run the release's make_sd.py: the check reaches players with the release that has it.)
- [ ] 3. SD read-back
- [ ] 4. Docs, tests
