# A one-file builder for players (plan)

Status: agreed 2026-10-08, not implemented.

## The problem

Players find the build steps of [INSTALL.md](../INSTALL.md) hard: Docker Desktop, WSL on Windows,
Python with `pycryptodome`, a 1 GB toolchain image and a terminal. Most reports from new players are
about that setup, not about the game.

The NRO cannot be published as it is: `build/gen` is the game's own PowerPC code translated to C
(about 280 MB, 2.8 million lines in 81 files, the `gamecode` target), so every NRO holds the game and
is built from the player's dump. What can change is how much the player has to install to build it.

## Options considered

| Option | Verdict |
|---|---|
| **Build on the Switch** at the first start (as people believe Dusklight does) | **Rejected.** Horizon has no C compiler for homebrew (porting gcc or clang is weeks of work). Even with one: 2.8 M lines at `-O3` on 3 usable A57 cores at 1 GHz, with ~3 GB in title mode while each generated file needs 1–2 GB of compiler memory, means hours and a likely out-of-memory. Dusklight does not do it either: it is a decompilation, its binary holds no game bytes and reads the disc at start-up; only shaders are prepared on the first run, which we already do |
| **One self-contained builder per desktop system** | **Chosen**, below |
| Prebuilt toolchain image on ghcr.io | Smaller help (still Docker and WSL); only worth it as a stopgap |

## The builder

One download per system, no Docker, no WSL, no Python install:

| System | Package |
|---|---|
| Windows 10/11 | `SwitchWakerHD-builder.exe` (or a zip with the exe next to its `toolchain/`) |
| Linux | `SwitchWakerHD-builder.AppImage` |
| macOS | `SwitchWakerHD-builder` (universal binary, or a `.app`) |

What it contains (no game data, so it can be published on GitHub Releases):

1. **The translator**: `tools/recomp/recomp.py` and the disc reader of `make_sd.py` (`.wux`/`.wud`
   decryption, version check) frozen with PyInstaller, so Python and `pycryptodome` come inside.
2. **A cross compiler for aarch64**: devkitA64's gcc/binutils repackaged for each host (devkitPro ships
   native builds for all three; GPL, so the source links go in THIRD_PARTY.md), or `zig cc` (one
   ~50 MB download per host) if the generated code builds cleanly with clang and links against the
   devkitA64 newlib/libstdc++ archives.
3. **Everything except the game code, already compiled for the Switch**: the runtime, cemu_latte,
   imgui, uam, deko3d, libnx and the C/C++ runtime as `.a` files, the linker script and crt files of
   the devkitA64 Switch target, and `elf2nro` (with the icon and NACP).

Flow for the player: open it, pick the `.wux`/`.wud` (with the keys) or the extracted folder, press
*Build*; it checks the version, translates, compiles only `build/gen` (the 81 files), links with the
prebuilt archives, runs `elf2nro` and writes the `sd/switch/wwhd/` folder (NRO + `game/`) ready to
copy. A small GUI (or a console window with progress) and the same error table as INSTALL.md.

Expected: 150–300 MB per system; the build itself a few minutes on a normal PC (only the generated
code is compiled). If that is still long, compile the generated files at `-O2` (measure the fps cost
on the console first).

## Work it needs

| Part | Notes |
|---|---|
| Split the CMake build: `gamecode` vs. everything else | CI builds the rest once per release as archives; the generated code must only need headers that ship with the builder |
| Pin the compiler | The prebuilt archives and `gamecode` must be built with the same devkitA64 gcc version and flags (`-fsigned-char`, `-O3`, the CMake defines of `WWHD_RENDERER=DEKO3D`) |
| Driver (Python, frozen) | Replaces `make_sd.py` + `build.sh`: extract/check, recomp, parallel compile with a memory-aware job count, link, `elf2nro`, SD folder |
| Packaging per host | PyInstaller + toolchain folder; AppImage on Linux; signing/notarization question on macOS (unsigned works with *Open anyway*) |
| CI | A job per host that builds the builder and tests it end to end with a dump on a private runner (the dump never in CI artifacts) |
| Docs | INSTALL.md rewritten around the builder; the Docker path stays for developers |

Estimate: 1–2 weeks. The risky parts are linking the player-built `gamecode` against archives built
elsewhere (same compiler, same flags) and Windows without WSL (paths, no `bash`).

## Related

SwitchWaker (the GameCube decompilation) takes the other road: it can publish the NRO itself once
the disc data is loaded at start-up (its `docs/RUNTIME_ASSETS.md`), the same model as Dusklight.
That is not possible here because the game code itself is translated from the dump.
