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

## devkitPro's 403 (investigated 2026-10-19)

Not an IP block: Cloudflare's bot rules in front of devkitpro.org, apt.devkitpro.org and pkg.devkitpro.org decide by
the client. From this network (Ubuntu 24.04 container and the Mac):

| client | user agent | answer |
|---|---|---|
| curl (Ubuntu 8.5, macOS) | any, also with --http1.1 | 403 |
| wget | its own (`Wget/…`), `Mozilla/5.0`, `Python-urllib/…` | 403 |
| wget | `dkp apt` (devkitPro's own script), `pacman/…`, `SwitchWakerHD-Installer/1.0` | 200 |
| Python urllib | its own, `curl/…`, `Wget/…` | 403 |
| Python urllib | `pacman/…` (Linux, Darwin, MSYS_NT), `Debian APT-HTTP/…`, `dkp apt` | 200 |
| apt, dkp-pacman (Linux), dkp-pacman (macOS) | their own | 200 |

So download tools' own user agents are refused and package managers' (and devkitPro's "dkp apt") pass; the curl
command line is refused whatever it sends. The first apt-route test failed only because our root script fetched
devkitPro's `install-devkitpro-pacman` with plain `wget`. Fix (LinuxEngine.DevkitProAptScript): do that script's
steps ourselves, the key with `wget -U 'dkp apt'` exactly as theirs, then apt and dkp-pacman. Tested: Ubuntu 24.04
as root, from this network, devkitpro-pacman 6.0.1 installed, `dkp-pacman -Sy` synced dkp-libs and dkp-linux,
115 MiB of Switch packages, then the build with minimap and forwarder: RUN OK.

The earlier conclusion in installer-devkitpro-403-plan.md ("for every URL and user agent, so here it looks
IP-based") came from curl probes, which are refused for any user agent: it does not hold. Windows' MSYS2 pacman
sends `pacman/… (MSYS_NT-…) libalpm/…`, which passes; the 403 in #28 is then not its user agent (its TLS client,
or that player's address, would be next).

Hardening (all three systems): when pacman's own downloads get 403, it runs again with a copy of its pacman.conf
whose XferCommand is `pacman-fetch.py` (EngineBase.WritePacmanFetch): the installer's Python with a pacman user
agent, which the rules let through; then, as before, the install from the package lists already there. Windows
does it after a 403 (InstallEngine.PacmanThroughPython, Python and script as /c/... paths for MSYS2's pacman);
the Mac and Linux root scripts as a shell function, dkp_xfer, between the two. Tested: Linux (devkitpro/devkita64,
the generated function on its own, package lists cleared): both lists and a 6 MiB package came through Python,
rc 0; the script's error path prints pacman's "returned error: 404" and leaves no partial file. Not tested: Windows
(MSYS2's path conversion of %o), the Mac root run.

#28 follow-up (v1.0.0 on an old devkitPro, pacman 6.0): pacman before 6.1 never asks an XferCommand for a
database's .sig, so the lists from Python were checked against the old signatures ("signature from fincs ... is
invalid"), and stayed broken for later runs. pacman-fetch.py now fetches `<db>.sig` with each `.db` (a 404 drops a
stale .sig; any other failure keeps the old list, which matches the old .sig). The same log showed Python's
"unable to get local issuer certificate" for MSYS2's mirrors: the script then retries with Windows' curl, as
setup.py does. Tested in devkitpro/devkita64 (dkp-pacman 6.0.2): with a mismatched dkp-libs.db.sig, main's script
fails -Syy with the player's error, the new one syncs (rc 0). Windows 11 (MSYS2 pacman 6.1.0, Python 3.13), the
XferCommand built as InstallEngine does, every .sig overwritten: -Syy rc 0 and valid lists, also with Python's
certificate store emptied (the player's "unable to get local issuer certificate"): curl.exe fetched MSYS2's and
devkitPro's lists. Not tested: an MSYS2 with pacman 6.0 on Windows.
