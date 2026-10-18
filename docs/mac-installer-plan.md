# Mac installer: the Windows Easy Installer's pages and features on macOS (plan)

Branch `feature/mac-installer` (from main 093c96c). Afterwards the Linux installer (PR #16) is finished on the same
Avalonia app.

## Why

On a Mac, building SwitchWakerHD today means the Terminal, Docker Desktop and `make_sd.py` (INSTALL.md). The
Windows Easy Installer (`installer/src`, WPF) does it all from a window: Python, devkitPro, the latest release, the
build, the copy to the SD card. Players without a technical background on a Mac should get the same.

## Goals

- One app for macOS (Apple Silicon and Intel) with the Windows installer's pages: welcome (language), game (Wii U
  archive / disc image / folder, checks), the optional HOME-screen icon (`prod.keys`), the optional GameCube sea
  minimap, progress (steps, bar, log, cancel), done (copy to the SD card).
- Nothing for the player to type: no Terminal, no Homebrew, no Docker.
- The Linux installer (PR #16) can then use the same app with its own engine.

## Non-goals

- Signing and notarization with an Apple Developer ID (no account): the app is ad-hoc signed and INSTALL.md says
  how to open it the first time (System Settings > Privacy & Security > Open Anyway).
- MTP (DBI) on the Mac: macOS has no MTP. The copy goes to the mounted SD card (card reader, or hekate's USB mass
  storage).
- Changing what the Windows installer does.

## How

**Toolchain: native devkitPro, no Docker.** devkitPro ships a macOS installer (`devkitpro-pacman-installer.pkg`,
arm64 and x86_64 pacman, repositories `dkp-libs` and `dkp-osx`). Spike 2026-10-19 on this Mac (macOS 26, arm64):
the pkg plus `dkp-pacman -Sy --needed switch-dev switch-lz4 switch-zlib switch-cmake deko3d uam` installs.
Findings:
- `pacman` must be run through `/usr/local/bin/dkp-pacman` (it puts devkitPro's `gpg` on PATH); the bare binary
  fails with "GPGME error: Invalid crypto engine".
- Run the root steps from a readable directory (`cd /`): pacman warns "could not get current working directory".
- Both root steps go in one script run through `osascript ... with administrator privileges`: one password window.
- Host `cmake`: the `cmake` wheel from pip in the installer's Python (no Homebrew). `make`, `git` and a C compiler
  (the forwarder's hacBrewPack): Apple's Command Line Tools (`xcode-select --install`, Apple's own window).
- Native `make_sd.py` build in a clean environment (PATH without Homebrew/Docker, DEVKITPRO=/opt/devkitpro): see
  step 1.

**Python: a private Python 3.14** (python-build-standalone, pinned version and sha256) unpacked into the work folder,
no administrator: the Mac's own `python3` is Apple's 3.9 (needs the Command Line Tools) and the minimap's `.rvz`
needs 3.14. pip installs `pycryptodome`, `zstandard`, `Pillow`, `cmake`.

**UI: Avalonia 11** (as PR #16), .NET 8 self-contained, one `.app` per architecture (`osx-arm64`, `osx-x64`).
Shared C# in `installer/shared/` (the steps that do not depend on the system: preflight checks, release download,
build progress, failure hints, strings); Windows keeps its WPF window and its engine.

## Steps (one commit each; console/hardware tests where they apply)

1. **Native macOS build** with `make_sd.py` (clean environment, devkitPro from the pkg, pip cmake). Fix what
   macOS needs in fork scripts (bash 3.2, `native_toolchain_ok`, the forwarder script's MSYS2-only paths), and check
   the NRO on the console.
2. **Shared code:** `installer/src` -> `installer/windows`, the platform-free parts of `InstallEngine.cs` and
   `Strings.cs` to `installer/shared/` (EngineBase). Windows behaviour unchanged: the WPF project builds
   (EnableWindowsTargeting), `build.ps1` and `release-installer.yml` follow the paths.
3. **Avalonia app** `installer/desktop/`: the four pages ported from MainWindow.xaml, the same texts (Spanish /
   English), log file, cancel. Runs on the Mac with a stub engine.
4. **MacEngine:** preflight (shared), Python 3.14, Command Line Tools, devkitPro (pkg download + sha256, one admin
   prompt, packages), release (shared), build (shared; DEVKITPRO, PATH with the private Python first, WWHD_JOBS from
   the Mac's memory), optional forwarder and minimap.
5. **Done page on the Mac:** the SD volumes under /Volumes (removable or with `atmosphere/` / `switch/`), copy with
   progress, eject; hekate UMS how-to; "open in Finder" for a manual copy.
6. **Packaging:** `installer/build-mac.sh` makes `SwitchWakerHD Installer.app` (Info.plist, .icns from the crab
   icon), ad-hoc signed, zipped, arm64 and x64; `release-installer.yml` gets a macOS job.
7. **Docs:** INSTALL.md macOS section (download, first open, what it installs), installer README.
8. **End to end on this Mac:** the zipped app from step 6, quarantined like a download, a fresh work folder, a USA
   dump, minimap + forwarder; the NRO on the console. Then the Linux engine (PR #16) on the same app.

## Status

- [ ] 1. Native macOS build
- [ ] 2. Shared code
- [ ] 3. Avalonia app
- [ ] 4. MacEngine
- [ ] 5. Done page
- [ ] 6. Packaging
- [ ] 7. Docs
- [ ] 8. End to end
