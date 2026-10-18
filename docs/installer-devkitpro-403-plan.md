# Installer: devkitPro 403 and the missing cmake (issue #28)

Branch `fix/installer-cmake`. To be tested on Windows before it goes to main.

## The problem

From the reporter's `installer.log` (v0.4.6, Windows 10):

```
>> > C:\devkitPro\msys2\usr\bin\pacman.exe -Sy --needed --noconfirm switch-dev deko3d uam switch-lz4 switch-zlib switch-cmake
error: failed retrieving file 'dkp-libs.db' from pkg.devkitpro.org : The requested URL returned error: 403
error: failed to synchronize all databases (failed to retrieve some files)
[!!] pacman reported errors; carrying on in case the packages were already installed.
...
[4/5] building the Switch homebrew (10-20 minutes the first time)
tools/switch/build.sh: line 67: cmake: command not found
```

Two causes:

1. **pkg.devkitpro.org answers 403.** Cloudflare in front of devkitPro blocks some pacman users by mistake
   (devkitPro is fighting LLM scraper bots): [devkitPro/pacman#63](https://github.com/devkitPro/pacman/issues/63),
   [#62](https://github.com/devkitPro/pacman/issues/62). On 2026-10-12 it also answered 403 to us, for every
   URL and user agent, so here it looks IP-based. (2026-10-19: not so; those probes used curl, which is refused
   for any user agent: docs/linux-installer-plan.md, "devkitPro's 403".) A failed `-Sy` installs nothing at all.
2. **MSYS2's `cmake` was never requested.** No devkitPro package brings it (`switch-cmake` only brings
   `Switch.cmake`), so a machine without it failed in stage 4 even when pacman worked. The reporter had
   `Switch.cmake` (build.sh's check passed) but no `cmake`.

The installer then carried on, downloaded the release and ran stages 1 to 3 (a few minutes) before failing with
a message that does not say what is wrong.

## The fix

- `installer/src/InstallEngine.cs`: request `cmake make` too; when `-Sy` fails, retry `-S` with the package
  lists already on disk (the ones that did refresh, MSYS2's in the log above, and older ones); then
  `CheckDevkitTools()` checks the files the build needs and stops the devkitPro step with a message naming
  what is missing:
  `msys2\usr\bin\cmake.exe`, `msys2\usr\bin\make.exe`, `devkitA64\bin\aarch64-none-elf-gcc.exe`,
  `libnx\lib\libnx.a`, `cmake\Switch.cmake`, `portlibs\switch\lib\libz.a`, `portlibs\switch\lib\liblz4.a`
  (paths checked against the `devkitpro/devkita64` image). Without MSYS2's `gcc` the forwarder is skipped,
  as without Git.
- `installer/src/Strings.cs`: `dkp_missing`, `dkp_tools_ok`, `fwd_gcc_fail`; `dkp_pkg_warn` reworded.
- `tools/switch/build.sh`: a native build without `cmake` says how to install it.
- `INSTALL.md`, `tools/switch/make_sd.py`: the package list includes `cmake make`.
- `.github/workflows/release-installer.yml`: `workflow_dispatch` builds the `.exe` from any branch as a run
  artifact (version 0.0.0); only a published release attaches it to the release.

## Getting the test installer

Actions > *Build Windows Installer* > *Run workflow* on `fix/installer-cmake`, or
`gh workflow run release-installer.yml -R centollOS/SwitchWakerHD --ref fix/installer-cmake`; the `.exe` is the
run's `SwitchWakerHD-Installer` artifact (`gh run download <run id> -R centollOS/SwitchWakerHD`).

The installer downloads the latest **release** zip, so the game code it builds is v0.4.6's: the `build.sh`
message is not part of this test, everything in the installer is.

## Reproducing the error (in devkitPro's MSYS2 window: Start menu > devkitPro > MSYS2)

**A. Missing cmake (cause 2).** Works whether or not devkitPro answers 403:

```
pacman -Q cmake make        # note what is installed
pacman -R cmake             # remove MSYS2's cmake
```

- Old installer (v0.4.6 release `.exe`): with a 403, reaches `[4/5]` and fails with `cmake: command not found`
  (the issue). Without a 403 it reinstalls nothing either, since it never asks for `cmake`, so it fails the same way.
- New installer: without a 403 it installs `cmake` and builds. With a 403 the retry installs `cmake` from
  MSYS2's list (that one refreshes; only devkitPro's server blocks).

**B. 403 from devkitPro (cause 1).** Check whether this machine is blocked:

```
curl -s -o /dev/null -w "%{http_code}\n" https://pkg.devkitpro.org/packages/dkp-libs.db
```

`403` = blocked (as in the issue); `200` = not blocked. To get the 403 without being blocked, make
`pkg.devkitpro.org` unreachable for a moment: in an administrator Notepad add `127.0.0.1 pkg.devkitpro.org`
to `C:\Windows\System32\drivers\etc\hosts` (pacman then fails the same way, with a connection error instead of
a 403), and **remove that line afterwards**.

**C. Missing devkitPro tools (the new early stop).** With B in place, rename `C:\devkitPro\portlibs\switch\lib\liblz4.a`
to `liblz4.a.bak` (the retry cannot download it again): the new installer must stop in the devkitPro step with
"Some devkitPro tools are missing: switch-lz4", before downloading the release. Rename it back afterwards.

**D. A Windows user name with a space (issue #30).** Sign in with a local account whose name has a space (for
example a new local user "Test User"): the new installer must work in `C:\SwitchWakerHD-Build` (its welcome page
and log say so) instead of `C:\Users\Test User\SwitchWakerHD-Build`, and build, even from v0.4.6's release zip
(the release is unpacked and built inside that folder, so no path has a space; the CMake quoting fix of #30 on
fix/build-path-spaces covers manual builds).

## Expected results

| Case | Old installer (v0.4.6) | New installer |
| --- | --- | --- |
| A, no 403 | fails in `[4/5]`: `cmake: command not found` | installs `cmake`, builds |
| A + B | fails in `[4/5]`: `cmake: command not found` | warning, retry installs `cmake`, builds |
| B + C | fails in `[4/5]` (missing lz4) | stops in the devkitPro step: `switch-lz4` missing |
| Nothing broken | builds | builds; log shows "devkitPro tools ready." |
| D, user name with a space | works in `C:\Users\Test User\...`, fails at 26 % | works in `C:\SwitchWakerHD-Build`, builds |

Restore afterwards: `pacman -S --needed cmake make`, remove the `hosts` line, rename `liblz4.a.bak` back.

## Hardening (branch `fix/installer-hardening`, on top of this one)

From the players' `installer.log`s (#28, #30) and a review of every step. Only `installer/src` changes what the
`.exe` does; `make_sd.py` and `build.sh` reach players with the next release zip.

- **Failures say why.** The error box carries make_sd's own reason, the first compiler error and a hint (disc or
  common key missing, wrong key, disk full, out of memory, path with a space, interrupted build, missing tool,
  access denied / antivirus, and build.sh stopping with no output, #28's last attempt). File and network
  exceptions name the step; pip and a missing winget get their own messages.
- **Checked before anything is installed:** the game file or folder; for a disc image, `<image name>.key` and
  `common.key` next to it (16 bytes or 32 hex digits), naming the exact file, the `.key` to rename or the hidden
  `.txt`; free space (10 GB, 5 GB on a re-run); the PC's clock against GitHub's (pacman's "certificate has
  expired" on every mirror in a #30 log); an OneDrive online-only image (warning). The game page shows the same
  key check, with the right file name (it asked for `GAME.key`, which make_sd never reads).
- **devkitPro:** pacman's output gives the reason (403, certificate/clock, lock, network with one retry,
  signatures) that goes with the "tools missing" stop; MSYS2's bash then runs `cmake`, `make` and
  `aarch64-none-elf-gcc` the way make_sd does, so a tool that cannot start stops the step with "run
  `pacman -Syu`" (and `ldd` in the log). `bash.exe` is checked too.
- **MSYS2's `cmake`, `make` (and `gcc`) only when missing.** Asked for with `--needed`, an installed one was still
  upgraded to the package list's version without its libraries: in the Windows tests (case 22) cmake 4.0.2 became
  4.4.4 next to `jsoncpp` 1.9.6-3 (`msys-jsoncpp-27.dll => not found`), and every later run stopped with "run
  `pacman -Syu`". A partial update the installer itself caused, with or without devkitPro's 403.
- **A partial update is mended, not left to the player.** When the tools are there but don't start, the installer
  runs `pacman -Syu` itself (up to three passes: a core update ends the first one) with a copy of `pacman.conf`
  without devkitPro's `[dkp-*]` repositories, so their 403 can't stop it (none of the broken programs come from
  them), then checks the tools again; only if they still don't start does it stop, with pacman's output and `ldd`
  in the log. This also covers a missing `cmake` installed next to an older `jsoncpp` (case 21).
- **Paths:** every path goes through `Quote()` (a trailing backslash, as in a game folder at a drive root, no
  longer eats the closing quote); a profile path over 45 characters also moves the working folder to
  `C:\SwitchWakerHD-Build` (MAX_PATH for devkitPro's gcc).
- **Re-runs:** the latest release comes from the API, else the `releases/latest` redirect, else the newest one
  already downloaded (it was a hard-coded v0.4.3); a damaged zip is deleted for Retry; a link error from a
  truncated object (interrupted build) or a CMake cache from another folder deletes `build/switch-dk`.
- **Python:** 64-bit 3.8+ only (else 3.12 is installed). **Build:** `WWHD_JOBS` from the PC's memory (~1.5 GB a
  compile). **SD copy:** free space, FAT32's 4 GB limit, a card without `atmosphere/` (warning).
- **make_sd / build.sh:** the failing command, its line and exit code (an ERR trap; hex for a Windows status).

Checked on macOS: the whole `.exe` project compiles (net48 + WPF, `EnableWindowsTargeting` and the
`Microsoft.NETFramework.ReferenceAssemblies` package in a scratch copy); unit checks of `Quote()` against the
Windows argv rules with tricky names (commas, parentheses, `&`, `%`, José, Cyrillic, Chinese, `D:\`, UNC), of the
key checks with those folder names, of the failure and pacman explanations on the players' logs; build.sh with a
`cmake` that exits 127 silently; shellcheck. Not checked: anything that needs Windows.

Still open (decisions): the `.exe` runs elevated (`requireAdministrator`), so drag and drop from Explorer is
blocked by Windows (UIPI) and mapped network drives are not visible to it (kept: neither is supported);
devkitPro is looked for in `C:\devkitPro`, then `X:\devkitPro` on the other fixed drives (installer and make_sd); a partial update is repaired with `pacman -Syu` from MSYS2's mirrors only (see above).

### Manual Windows test checklist (built `.exe`)

Run each with a fresh look at `installer.log`; "stops" means it stops with a message naming the cause and the fix.

1. Clean PC, `.wua` on the Desktop: builds; log shows Python, "devkitPro tools ready", "Parallel compiles: N".
2. Run the `.exe` from Downloads, from the Desktop and straight from inside its zip (Explorer's "open"): same.
3. Disc image named `Legend of Zelda, The - The Wind Waker HD (USA, Asia) (En,Fr,Es).wux`:
   a. no `.key` → the game page and Install both name the exact `...(En,Fr,Es).key` path, before any download;
   b. key saved as `GAME.key` → "Rename GAME.key to ...";
   c. key saved from Notepad as `...key.txt` (extensions hidden) → ".txt" message;
   d. no `common.key` / `common.key` with spaced hex → exact message; e. both right → builds.
4. EUR game (`.wua` or image): builds; log says "The Wind Waker HD (Europe), version 0: OK".
5. Extracted game folder at a drive root (USB stick `E:\` with code/content/meta): builds.
6. User names: "Test User" (space), "José", a Cyrillic and a Chinese name, and a 20-letter name: working folder is
   `C:\SwitchWakerHD-Build`; builds. Game on an OneDrive Desktop (online-only): warning, builds.
7. Game on a second drive (`D:\Games\...`) and on a UNC share: builds.
8. Less than 10 GB free on C: → stops before any download.
9. devkitPro blocked: hosts line `127.0.0.1 pkg.devkitpro.org` (see B above): with tools already installed →
   warning, builds; with `liblz4.a` renamed (C above) → stops in the devkitPro step with the network reason.
10. Clock set a year ahead → stops with the date/time message (or warns, then pacman's reason is the clock).
11. `pacman -R cmake`, then in MSYS2 install an older/mismatched cmake or delete one of its DLLs
    (`ldd /usr/bin/cmake.exe` shows them) → the devkitPro step stops with "run pacman -Syu" and ldd in the log.
12. Cancel during "Compiling for the Switch... 40%", then Retry: builds (if a link error from a cut-off file
    appears, the message says so and the next Retry builds).
13. Re-run after a success: no download, reuses the release; re-run with the network unplugged: uses the
    downloaded release ("using the one already downloaded").
14. Python: only a 32-bit Python installed → "Not usable" in the log, 3.12 installed, builds. No winget → the
    "install Python from python.org" message.
15. Windows Defender "Controlled folder access" on, working folder not allowed → the access-denied hint.
16. Copy to SD: a card with too little space → "does not fit"; a card without `atmosphere/` → warning; a normal
    card → `switch/wwhd/` at the root; the game starts from the Homebrew Menu.
