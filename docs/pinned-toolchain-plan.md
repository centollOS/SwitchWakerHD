# Pinned build environment: the same tools on every player's computer

Branch `feature/pinned-toolchain`. Plan only; nothing here is built yet.

## Why

Players build the NRO themselves (it holds code translated from their own game), so whatever is installed on
their computer ends up in what runs on their console. Each difference has cost us an issue that was hard to
diagnose, because the player's build is not the one we test:

- **#98, #55**: a crash at start on HOS 22.1.0 with AMS 1.12.1, built on Windows, with v0.4.7 and v1.0.0 alike.
  The same v1.0.0 source built here runs on our console updated to HOS 22.1.0 (normal start, first start,
  forwarder and title mode). The reporter's code is larger than every v1.0.0 build we made (USA, EUR, GCC 15.2
  in Docker, GCC 16.1 native), so the crash addresses in the log cannot be mapped to a function. Suspect: an
  older devkitPro/libnx on their PC (waiting for `pacman -Q` from both reporters).
- **#28**: an old devkitPro (devkitA64 r19, libnx 4.2.1, pacman 6.0): database signatures not fetched, MSYS2
  mirrors failing certificate checks in the player's Python 3.10, then the devkitA64 metapackage conflict, then
  "MSYS2 tools not starting" (antivirus?).
- **#97**: a Windows `ninja.exe` on the player's PATH broke the prepared-graphics build.
- **The 403s** (docs/installer-devkitpro-403-plan.md): pkg.devkitpro.org's Cloudflare refuses some pacman users
  and every curl; on 2026-10-31 it refused every package URL from our Mac too.
- **Python**: the installer picked MSYS2's Python (no pip), then Python 3.15 through the py launcher (zstandard
  has no 3.15 wheel: pip needed Visual C++).
- **Docker present or not** changes which compiler `tools/switch/build.sh` uses (the container's GCC 15.2 or
  devkitPro's GCC 16.1), and so the NRO.

The goal: **the installer and `make_sd.py` build with tools we chose, pinned and checked, the same ones we
build and test with, whatever the player has installed.** A player's NRO is then the one we would build from
the same game files, and a crash address in a player's log maps to our ELF.

## Sources of variation and what replaces each

| Source | Today | Pinned |
|---|---|---|
| Python (version, packages, which one PATH finds) | the player's (Windows), python-build-standalone 3.14 (Mac/Linux installers) | python-build-standalone on every system, its packages as wheels pinned by hash |
| devkitPro toolchain and libraries (devkitA64, libnx, deko3d, uam, portlibs) | the player's install or `pacman -S` from pkg.devkitpro.org; Docker's image | our toolchain bundle, a release asset of ours |
| bash, make, cmake, ninja, host gcc for `tools/switch/dksh_cache` (Windows) | devkitPro's MSYS2 plus whatever is on PATH | a portable MSYS2 subset inside the bundle |
| Docker or Podman installed | `build.sh` prefers a container | the installer and `make_sd.py` use the bundle; the container stays a developer option |
| Environment (`DEVKITPRO`, `DEVKITARM`, `PATH`, `PYTHON*`, `CC`, ...) | inherited | each build process gets an environment built from scratch: the bundle, then the system's minimum |
| Game files (dump format, update, modified files) | checked by `tools/switch/game_files.py` (#48) | unchanged; stays a hard check |
| Network to pkg.devkitpro.org and MSYS2 mirrors | needed on every install | not needed: only github.com (already needed for the release zip and the graphics template) |

Not covered (out of our hands): the console's firmware and Atmosphère, antivirus behaviour, how files get to the
SD card (the log already records the first; the second and third get checks where we can, e.g. `._` files).

## Steps

### 1. Say what built it (small, first, also for v1.0.1)

Every NRO logs, next to the version, the toolchain that built it: GCC version (`__VERSION__`), libnx version
(its version header), deko3d and uam versions, the bundle id when built from the bundle, `docker`/`native`, the
host OS. One `[boot] toolchain ...` line. With it, #98's first log would have answered the question we are
now waiting for. Also in `installer.log`.

### 2. Portable Python on Windows (v1.0.1)

The Windows installer downloads python-build-standalone (the Mac/Linux installers' mechanism,
`installer/desktop/DesktopEngine.cs`), checks its sha256, unpacks it into the work folder and uses only that
Python; the packages come from a hash-pinned requirements file (`--require-hashes --only-binary :all:`), so pip
never builds anything. Replaces the PATH search (`FindPython`) and its patches on `fix/pacman-xfer-sig`
(MSYS2 Python, Python 3.15).

### 3. The toolchain bundle (v1.1.0)

A GitHub Actions workflow builds, for windows-x64, macos-arm64, macos-x64, linux-x64, linux-arm64:

- devkitA64 (binutils, GCC, newlib) from devkitPro's public `buildscripts` at a pinned commit, with their
  sources fetched from GNU's mirrors and GitHub (not pkg.devkitpro.org);
- libnx, deko3d, uam, switch-tools (elf2nro, nacptool), and the portlibs we link (zlib, lz4) from source at
  pinned commits;
- Windows only: a portable MSYS2 subset (bash, coreutils, make, cmake, ninja, gcc for the host tools), from a
  pinned msys2-base archive;
- a manifest (`toolchain.json`: every component, its version and commit, the bundle id) and the sources archive
  the GPL asks for.

It is published as release assets of a tools release (`toolchain-YYYYMMDD`), each with a `.sha256`. The repo
pins one bundle id per platform in a lock file (`tools/toolchain.lock`); a new bundle is a reviewed change of
that file, like any code change.

Our own builds use the same bundle (developer machines and CI), or the Docker image pinned by digest only if it
is proven to produce the same NRO.

### 4. Installers and `make_sd.py` use the bundle (v1.1.0)

- The installers download the bundle named in the release's `tools/toolchain.lock` from its direct release URL
  (`github.com/.../releases/download/<tag>/<file>`, like the release zip and the graphics template: not GitHub's
  API, whose asset endpoint counts against the 60 requests an hour an unauthenticated IP gets; resumable,
  sha256-checked),
  unpack it into the work folder (`toolchain/<bundle id>/`), keep one previous one, and never read or change the
  player's devkitPro, pacman, Docker or PATH.
- `tools/switch/build.sh` gets a bundle mode: an environment from scratch (`env -i` plus the bundle's paths and
  the system's minimum), `DEVKITPRO` pointing into the bundle. `make_sd.py` uses it by default and fetches the
  bundle itself; `--system-devkitpro` and `--docker` stay for developers.
- The devkitPro/pacman steps leave the installers (the 403 retries, `pacman-fetch.py`, the keyring hints, the
  package-conflict handling).

### 5. Prove it is the same build (v1.1.0)

- Build the same commit and game with the bundle on Windows (test PC), macOS and Linux and in our CI-like setup:
  `wwhd.elf`'s code must be identical (compare the text section; timestamps aside).
- A player's log (step 1) then names a bundle id; we keep the ELFs of each release built with it for crash
  addresses.

## Open questions (for the maintainers)

1. **devkitPro.** Redistributing a toolchain built from their buildscripts is allowed by the licences (GPL with
   sources), but devkitPro has asked projects not to rehost their tools, and already closes our PRs. Do we tell
   them first? Name the bundle so it is clearly ours (not "devkitPro"), and point support questions to us.
2. **Where the bundle lives**: this repo's releases (a `toolchain-*` pre-release) or a separate repo
   (`centollOS/switch-toolchain`) that SwitchWaker could also use.
3. **Size**: expected 300-500 MB download, 1-2 GB unpacked; acceptable?
4. **SwitchWaker**: it builds in Docker (and players do not build it). Use the same bundle there later?
