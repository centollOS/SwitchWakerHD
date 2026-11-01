# Pinned build environment: the same tools on every player's computer

Branch `feature/pinned-toolchain`.

**Status (2026-10-31):** steps 1-4 done on the branch (not merged, not released): the toolchain line in the log,
the pinned Python in all installers, `tools/toolchain` (lock, fetch, weekly check), `build_switch.py` and
`dksh_cache/build.py` with zig, `make_sd.py --toolchain pinned` by default, the installers' Switch tools step.
Checked on macOS arm64: same NRO code as the Docker image's build (only address immediates differ), the same
`shadercache_dksh.bin` byte for byte, `make_sd.py` end to end under `env -i`. Linux arm64: `make_sd.py` end to end
in a clean Debian container (no system Python, gcc or make), the same NRO code as the Mac's (only address
immediates differ: the toolchain line and dates move read-only data). Left: a Windows run (test PC), a console
check of a pinned build.

**The HOME-screen forwarder too** (optional, 2026-11-01): with the pinned toolchain,
`build_forwarder_windows.py --toolchain pinned` (make_sd.py passes it) downloads nx-hbloader and hacBrewPack as
GitHub source archives checked by the SHA-256 of their file tree (forwarder.env), applies our patch without git,
builds nx-hbloader with the pinned devkitA64 (its Makefile's commands; same .text/.rodata/.data as `make
RELEASE=1`, only the build id differs, from the debug info's paths) and hacBrewPack with the pinned zig
(`tools/switch/forwarder/pinned_tools.py`). The installers no longer set up the computer's devkitPro for it.
Windows test PC (2026-11-01): the game's build needed two Windows-only fixes (cmake and ninja in the installer's
Python; devkitPro's CMake files refuse a native Windows CMake: a copy without that check) and uam's CMake mirror
had to follow meson.build on Windows (no _GNU_SOURCE there) for dksh_cache with zig. The prepared graphics came
out byte-identical to v1.0.0's; the game's code is equivalent to the Mac's (address immediates, and GCC on
Windows schedules ppc_fres differently).

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
| devkitPro toolchain and libraries (devkitA64, libnx, uam, portlibs) | the player's install or `pacman -S` from pkg.devkitpro.org; Docker's image | devkitPro's package files at locked versions and hashes, unpacked into our folder |
| bash, make, cmake, ninja, host compiler for `tools/switch/dksh_cache` | devkitPro's MSYS2 (Windows), Xcode / the distribution, whatever is on PATH | no bash or MSYS2 (a Python build driver); cmake and ninja from step 2; a pinned zig as host compiler |
| Docker or Podman installed | `build.sh` prefers a container | the installer and `make_sd.py` use the pinned folder; the container (pinned by digest) stays a developer option |
| Environment (`DEVKITPRO`, `DEVKITARM`, `PATH`, `PYTHON*`, `CC`, ...) | inherited | each build process gets an environment built from scratch: the pinned folder, then the system's minimum |
| Game files (dump format, update, modified files) | checked by `tools/switch/game_files.py` (#48) | unchanged; stays a hard check |
| Network to pkg.devkitpro.org and MSYS2 mirrors | pacman on every install, MSYS2 mirrors | pkg.devkitpro.org once for the locked files (kept in the work folder); no MSYS2 mirrors |

Not covered (out of our hands): the console's firmware and Atmosphère, antivirus behaviour, how files get to the
SD card (the log already records the first; the second and third get checks where we can, e.g. `._` files).

## Steps

### 1. Say what built it (small, first, also for v1.0.1)

Every NRO logs, next to the version, the toolchain that built it: GCC version (`__VERSION__`), libnx version
(its version header), deko3d and uam versions, the lock id when built from the pinned folder, `docker`/`native`, the
host OS. One `[boot] toolchain ...` line. With it, #98's first log would have answered the question we are
now waiting for. Also in `installer.log`.

### 2. Portable Python on Windows (v1.0.1)

The Windows installer downloads python-build-standalone (the Mac/Linux installers' mechanism,
`installer/desktop/DesktopEngine.cs`), checks its sha256, unpacks it into the work folder and uses only that
Python; the packages come from a hash-pinned requirements file (`--require-hashes --only-binary :all:`), so pip
never builds anything. Replaces the PATH search (`FindPython`) and its patches on `fix/pacman-xfer-sig`
(MSYS2 Python, Python 3.15).

### 3. Pinned devkitPro packages (v1.1.0)

**Decision (2026-10-31): we do not build or rehost a toolchain.** devkitPro's buildscripts say toolchains built
with them "are for personal use only and may not be distributed by entities other than devkitPro". Instead the
build uses **devkitPro's own package files, from devkitPro's server, at exact versions**, checked by SHA-256 and
unpacked into a folder of ours, without pacman and without the player's devkitPro.

- `tools/toolchain/lock.json`: for each platform (win-x64, osx-arm64, osx-x64, linux-x64, linux-arm64) the
  package files (`devkita64-gcc`, `-binutils`, `-newlib`, `-rules`, `-cmake`, `dkp-cmake-common-utils`,
  `switch-cmake`, `libnx`, `switch-tools`, `general-tools`, `uam`, `switch-zlib`, `switch-lz4`) with their path
  on pkg.devkitpro.org and SHA-256. The versions are the ones our Docker image has (devkitA64 r29.2: GCC
  15.2.0-7, binutils 2.45.1-2, newlib 4.6.0.20260123-4, libnx 4.12.0-1, ...), so a player's NRO is ours.
  deko3d and uam's compiler library are already built from our own copies (runtime/third_party).
- `tools/toolchain/fetch.py` (standard library only; Python 3.14 for zstd): downloads each file (devkitPro's
  Cloudflare refuses curl and Python's default client, and accepts pacman's user agent, as pacman-fetch.py
  already sends), checks the SHA-256, unpacks `opt/devkitpro/...` into `<dir>/devkitpro`, writes
  `toolchain.json` (the lock's id per platform: what step 1's log line names).
- `tools/toolchain/make_lock.py` (maintainers): writes the lock from a list of versions.
- On 2026-10-31 every r29.2 file was still there for Linux, Windows and macOS, though devkitPro's repository
  database already lists r30. pacman only installs what the database lists, which is why fetch.py asks for the
  exact files.
- A weekly GitHub Actions check downloads the locked files: if devkitPro removes one, we hear it before players
  do (and it reports newer versions to consider).

**Updating** is a reviewed change of the lock, like code: only for a reason (a firmware that needs a newer libnx,
a fixed bug, a measured improvement), built and checked on the console (start, memory tour, performance) before a
release. The Docker image used by `build.sh` is pinned by digest to the same versions.

### 4. The build without bash, MSYS2 or Docker (v1.1.0)

- `tools/switch/build_switch.py`: the parts of `build.sh` the players' build needs (icon, NACP, CMake configure,
  build), in Python, with the pinned cmake and ninja (step 2's packages) and the pinned devkitPro folder. Its
  environment is built from scratch: `DEVKITPRO` pointing at the pinned folder, `PATH` = its tools + the system's
  minimum; nothing inherited (`DEVKITPRO`, `DEVKITARM`, `CC`, ...). Windows needs no MSYS2: devkitPro's Windows
  packages are native programs.
- The host tool `tools/switch/dksh_cache` (prepared graphics) is compiled with a pinned zig (`zig c++`, one
  archive per platform from ziglang.org, SHA-256 in the lock): no Xcode, distribution gcc or MSYS2 gcc.
- `make_sd.py` uses this by default; `--system-devkitpro` and `--docker` stay for developers.
- The installers: the devkitPro step becomes "download the pinned toolchain" (no pacman, keyring, 403 retries
  through pacman, package conflicts). The 403 is still possible from devkitPro's server: the fetch retries, and
  the files stay in the work folder once downloaded.

### 5. Prove it is the same build (v1.1.0)

- Build the same commit and game with the pinned folder on Windows (test PC), macOS and Linux and in our CI-like setup:
  `wwhd.elf`'s code must be identical (compare the text section; timestamps aside).
- A player's log (step 1) then names a lock id; we keep the ELFs of each release built with it for crash
  addresses.

## Open questions (for the maintainers)

1. ~~devkitPro / where the bundle lives / size~~: settled by step 3's decision (their files, from their server;
   about 130 MB). Asking devkitPro for permission to rehost (which would also end the 403) stays possible later.
2. **SwitchWaker**: it builds in Docker (and players do not build it). Pin its image by digest too?
