# Choose the build folder (#59)

From discussion #50: a player has no room on C: but has room on another drive (an SD card). Today the
Windows installer always builds in `%USERPROFILE%\SwitchWakerHD-Build` (or `C:\SwitchWakerHD-Build`, #30), and
the Mac/Linux installer in `~/SwitchWakerHD-Build`.

## What the player sees

The welcome page shows the work folder, the free space on its drive against what the build needs, and a
**Change folder...** button. Picking a folder uses it as is when it is already a work folder (named
`SwitchWakerHD-Build`, or holding `installer.log`; not a folder with a release unzipped by hand, like Downloads); otherwise a `SwitchWakerHD-Build` folder goes
inside it (picking `D:\` builds in `D:\SwitchWakerHD-Build`).

A folder that cannot work is refused on the welcome page, with the reason, and **Get started** stays off:

- spaces or non-ASCII letters in the path (the build tools, #30); on Windows also a drive letter path only
  (no `\\server\share`) and at most 45 characters (MAX_PATH, as `ChooseWorkDir`);
- a folder the installer cannot write to;
- a network drive (a mapped letter on Windows, an SMB/NFS/AFP mount on Mac/Linux: `DriveType.Network`): the build
  writes hundreds of thousands of small files and crawls or fails there, and a mapped letter is not there for the
  administrator devkitPro's installer runs as;
- Mac/Linux: a drive that cannot hold symbolic links or programs (Linux FAT/exFAT: the private Python is a
  tarball with symlinks, the build runs host tools). Checked by trying it in the folder. macOS FAT/exFAT pass
  (tested: the Python tarball unpacks and runs). On a Mac a disk's name is part of the path, so the Switch's card
  as `/Volumes/SWITCH SD` is refused for its space; the message says to rename the disk.

FAT32 and exFAT are allowed on Windows and macOS, with a note that the build is slower there: Windows' toolchain
fetch copies the archive's links instead of making them (tools/toolchain/fetch.py) and Windows' Python has none.

Not enough free space is a warning there (Preflight still stops the install, as now).

The installer's own folder (no pick) is never refused, only warned about: a player who does not choose a folder
gets the install exactly as before #59.

On a removable drive (Windows `DriveType.Removable`; Mac/Linux under `/Volumes`, `/media`, `/run/media`, `/mnt`)
a note: slower build, keep it connected, and when it is the Switch's own card it needs room for the work folder
and the finished game, and the work folder stays on the card (user, 2026-10-21: keep it, the next update reuses
it; the player can delete it).

The choice is remembered in `SwitchWakerHD-Installer/build-folder.txt` under the local application data folder
(`%LOCALAPPDATA%` on Windows, `~/Library/Application Support` on macOS, `~/.local/share` on Linux), written when
the install starts. Only a folder the player picked: the default is worked out again on every start (it can
change, #30). **Use the default folder** (shown while another one is chosen) goes back, and the file goes at the
next install. When the remembered folder's drive is gone, the default is used and the welcome page says so.

Update mode (#110): `installer-choices.txt` stays in the work folder, so the update offer is the chosen folder's.
Picking another folder loads that folder's last choices and offer when it has them (an older work folder on a
disk plugged in again), and *Update to vX* stays off while the folder is refused.

## Not changed

- devkitPro stays on the system drive on Windows (`C:\devkitPro`), `/opt/devkitpro` on Mac/Linux.
- The space numbers (10 GB first time, 5 GB on a re-run).

## Code

- `installer/shared/BuildFolder.cs`: remembered folder, folder for a pick, problems, drive and free space,
  removable check.
- Engines: `WorkDir` settable (Windows default stays `ChooseWorkDir`); Preflight's space check uses the drive
  the folder is on (Mac/Linux: the mount point, not `/`).
- Both welcome pages: folder, space, notes (FAT, removable), *Change folder...*, *Use the default folder*. The log
  path and the "Remove older builds" text (#120) follow the folder.
- Strings: Spanish/English in `Strings.cs` (and `DesktopStrings.cs` for Mac/Linux), Portuguese in `StringsPt.cs`
  and `DesktopStringsPt.cs`.

## Tests

- Mac, done (2026-11-08, again after the rebase onto #135/#136; through the built assembly): #120's older-build
  selection in a chosen folder on an exFAT image, and its free-space number from that volume; a path with a space and a read-only folder refused; FAT32
  and exFAT disk images accepted with both notes and their own free space; the pointer file written, read, gone
  after *Use the default folder*; a remembered folder on a missing volume falls back with the warning.
- Mac, to do on screen: pick an exFAT disk image, a path with a space (refused), a read-only folder
  (refused); install end to end from a folder on another volume; remembered on the next start.
- Linux (UTM VM): a FAT USB stick is refused (symlink check); an ext4 folder works.
- Windows (user's PC): pick `D:\` or an SD card, build end to end, re-run reuses it.
