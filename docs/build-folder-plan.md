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
- Mac/Linux: a drive that cannot hold symbolic links or programs (Linux FAT/exFAT: the private Python is a
  tarball with symlinks, the build runs host tools). Checked by trying it in the folder. macOS FAT/exFAT pass
  (tested: the Python tarball unpacks and runs).

Not enough free space is a warning there (Preflight still stops the install, as now).

On a removable drive (Windows `DriveType.Removable`; Mac/Linux under `/Volumes`, `/media`, `/run/media`, `/mnt`)
a note: slower build, keep it connected, and when it is the Switch's own card it needs room for the work folder
and the finished game, and the work folder stays on the card (user, 2026-10-21: keep it, the next update reuses
it; the player can delete it).

The choice is remembered (`SwitchWakerHD-Installer/build-folder.txt` under the local application data folder;
only a folder the player picked, the default is worked out again on every start)
so a re-run reuses the build. When the remembered folder's drive is gone, the default is used and the welcome
page says so.

## Not changed

- devkitPro stays on the system drive on Windows (`C:\devkitPro`), `/opt/devkitpro` on Mac/Linux.
- The space numbers (10 GB first time, 5 GB on a re-run).

## Code

- `installer/shared/BuildFolder.cs`: remembered folder, folder for a pick, problems, drive and free space,
  removable check.
- Engines: `WorkDir` settable (Windows default stays `ChooseWorkDir`); Preflight's space check uses the drive
  the folder is on (Mac/Linux: the mount point, not `/`).
- Both welcome pages: folder, space, note, button. The log path follows the folder.

## Tests

- Mac: the desktop app builds; pick an exFAT disk image, a path with a space (refused), a read-only folder
  (refused); install end to end from a folder on another volume; remembered on the next start.
- Linux (UTM VM): a FAT USB stick is refused (symlink check); an ext4 folder works.
- Windows (user's PC): pick `D:\` or an SD card, build end to end, re-run reuses it.
