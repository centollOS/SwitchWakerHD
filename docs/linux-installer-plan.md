# Linux installer: the desktop app (installer/desktop) with a Linux engine (plan)

Branch `feature/desktop-linux` (from `feature/mac-installer`). It finishes PR #16 (centollosdev-debug's Avalonia
Linux installer, based on the Windows installer of 2026-10-12) on the app the Mac installer now uses, instead of
a second copy of the window.

## Goals

- Linux x86-64 and arm64, one self-contained file each: the Mac installer's pages (game, GameCube sea minimap,
  HOME-screen icon, progress, done), plus PR #16's DBI transfer over MTP through `gio`.
- No Terminal unless the system cannot show a password window: root steps through `pkexec`; without a polkit
  agent, the notice shows one `sudo sh '...'` command with a Copy button (PR #16).

## How (what changes from PR #16)

- **One window for macOS and Linux.** `DesktopEngine` (installer/desktop) holds what both share: the work folder
  `~/SwitchWakerHD-Build`, the private Python 3.14 (python-build-standalone, pinned per system and architecture:
  no `python3-venv`, no PEP 668 trouble, and the minimap's `.rvz` reads with it), processes and their cancel, the
  copy to a mounted card. `MacEngine` and `LinuxEngine` bring the toolchain, the card list, eject and "open".
- **Toolchain on Linux** (PR #16's order): devkitPro already installed with the Switch packages -> native build;
  else a Docker/Podman the user may use -> container build; else install: Debian/Ubuntu devkitPro's apt route
  (their `install-devkitpro-pacman`) plus `make`, `gcc` and `git` for the build and the forwarder; Fedora, Arch,
  openSUSE Podman. Host `cmake` comes from pip, as on the Mac.
- **make_sd.py honours WWHD_NATIVE=1**: today it picks the container whenever docker or podman is on PATH, so with
  devkitPro installed *and* Docker present the forwarder (native only) was skipped.
- **Running as root** (a container, or `sudo` the whole app): root steps run directly.
- Strings: PR #16's `lx_*` texts in `LinuxStrings.cs` over the shared ones, as `MacStrings.cs` does.

## Steps (one commit each)

1. Plan; make_sd.py: WWHD_NATIVE=1 forces the native build.
2. `DesktopEngine` from MacEngine's shared parts; the Mac installer unchanged (engine harness rerun).
3. `LinuxEngine`, `GioMtp`, `LinuxStrings`; the window picks the engine by system and shows the DBI card on Linux.
4. Packaging: `installer/build-linux.sh` (linux-x64, linux-arm64, single file), a job in release-installer.yml.
5. Docs: INSTALL.md Linux section.
6. Tests in containers on this Mac (Ubuntu 24.04 arm64): the engine as root (apt route, devkitPro, build with
   minimap and forwarder), as a user without a password window (the sudo notice), Fedora (Podman route, to the
   point Podman needs a real system); the window under Xvfb (screenshot). Not testable here: pkexec on a real
   desktop, MTP with DBI, x86-64 hardware.

## Status

- [x] 1. Plan, make_sd.py
- [x] 2. DesktopEngine
- [x] 3. LinuxEngine
- [x] 4. Packaging
- [x] 5. Docs
- [~] 6. Tests (below)

Tests (2026-10-19, containers on this Mac, arm64; a console harness running the engine without the window):
- Mac engine after the DesktopEngine split: end to end again, RUN OK (minimap + forwarder).
- devkitpro/devkita64 (Debian 12, devkitPro installed), as root: native route, Python 3.14 downloaded, release
  verified, build with the GameCube sea minimap and the forwarder (533,720-byte NSP): RUN OK.
- Ubuntu 24.04 as root: the apt route ran apt-get (make, gcc, git, wget...), then wget of
  apt.devkitpro.org/install-devkitpro-pacman got **403 from Cloudflare** (this network is blocked for every client
  and user agent, as pkg.devkitpro.org was for players in #28): now explained as dkp_why_403 (wget -nv, "ERROR 403").
  The devkitPro apt install itself is therefore not tested here.
- Ubuntu 24.04 as a user without a password window: the notice with `sudo sh '~/SwitchWakerHD-Build/root-step.sh'`.
- Fedora 41 as root: Podman route, `dnf install -y podman` completes; podman cannot start inside an unprivileged
  container (expected here), so the build is not reached.
- The window: the linux-arm64 single file under Xvfb (Debian 12 + libice6/libsm6/fontconfig), Spanish from LANG.
Not tested: pkexec on a real desktop, DBI over MTP, x86-64, the apt route from an unblocked network.
