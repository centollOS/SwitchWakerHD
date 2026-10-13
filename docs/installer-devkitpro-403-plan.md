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
   URL and user agent, so here it looks IP-based. A failed `-Sy` installs nothing at all.
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
