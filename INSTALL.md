# Installing SwitchWakerHD on your Switch

SwitchWakerHD runs The Wind Waker HD natively on a Switch with custom firmware. **Nothing of the game
comes with it**: you build it on your computer from your own copy of the game, with one script, and copy
the result to the SD card. What you build contains the game, so it is for your own console only: do not
share it.

## Windows: Easy Installer (recommended)

On **Windows 10/11**, an automatic installer handles everything for you: its own Python, the Switch tools, the
release download, and the build. You only need your own personal dump of the game. The Switch tools are
[pinned](#the-switch-tools-pinned): the same ones the release was built and tested with, whatever is installed on
the PC.

**[⬇ Download SwitchWakerHD-Installer.exe](https://github.com/centollOS/SwitchWakerHD/releases/latest/download/SwitchWakerHD-Installer.exe)**

1. Download the `.exe` above and double-click it.
2. Accept the UAC prompt.
3. Pick your game dump: a Cemu archive (`.wua`), a disc image (`.wux`/`.wud`, with its keys), or an
   extracted folder.
   Optional, for a **HOME-screen icon**: choose your console's `prod.keys` (dumped with Lockpick_RCM; they are
   read where they are, never copied). The installer then also sets up devkitPro (the icon is still built with it),
   Git and MSYS2's gcc and builds
   the forwarder `.nsp` ([details](tools/switch/forwarder/INSTALL.md)); it travels in `NSP\` with the rest: install it with
   DBI (*Browse SD card > NSP > wwhd_forwarder.nsp > Install*; it needs current Atmosphère sigpatches).
   The **HD minimap** (new) is always built into the game: tick *Turn it on now* to have it from the first start,
   or turn it on later in the game's menu ([details](#optional-code-mods-the-minimaps)).
   Optional, **Brazilian Portuguese**: download Triforce-Heroes' fan translation yourself from
   [their page](https://triforce-heroes.com.br/traducoes/the-legend-of-zelda-the-wind-waker-hd) (the *Emulador*
   version) and choose the downloaded `.rar` (or the folder you extracted it to)
   ([details](#optional-brazilian-portuguese-fan-translation)). The installer itself is in Spanish, English and
   Portuguese (the button at the top of the first page).
   **Prepare the graphics now** (ticked by default): the installer downloads the release's graphics template and
   compiles the game's graphics on your computer, from your own copy of the game, a few minutes more. The console
   then starts with them ready: no first-start screen and no black places
   ([details](#the-first-start-preparing-the-graphics)).
4. Press **Install**. (Only with the HOME-screen icon, if devkitPro is not installed yet, the installer asks you to
   open its setup: accept the license and keep the default path, `C:\devkitPro`.)
5. Wait for the build (10–30 minutes), then choose how the files reach the Switch:
   - **With the microSD card in the PC**: a card reader, or hekate's *Tools > USB Tools > SD Card* (UMS).
     After the copy the installer reads every file back from the card and copies again any that differs.
   - **By hand**: the installer opens the folder; copy its contents to the root of the SD card.

If something fails, *Show details* shows what happened; the full log is in
`%USERPROFILE%\SwitchWakerHD-Build\installer.log` (in `C:\SwitchWakerHD-Build\installer.log` when your Windows
user name has a space or an accented letter: the installer then works there).

> **Windows SmartScreen** may show *"Windows protected your PC"* on first run because the installer is
> not code-signed. Click **More info → Run anyway**. This is expected for open-source unsigned executables.
> The [source code](installer/windows) and [build script](installer/build.ps1) are in this repository.
>
> **Microsoft Defender** (or another antivirus) may also flag it, for example as `Trojan:Win32/Wacatac.B!ml`.
> The `!ml` means a guess by its machine-learning heuristic, not a known malware signature: an unsigned program
> that downloads files and starts other programs looks suspicious to it. The `.exe` is built by
> [GitHub Actions](.github/workflows/release-installer.yml) from the source above; you can compare your download
> with `SwitchWakerHD-Installer.exe.sha256` on the release page (PowerShell:
> `Get-FileHash SwitchWakerHD-Installer.exe`). If you would rather not run it, follow the manual steps from
> [1. What you need](#1-what-you-need) on: they use the release zip and need no installer.
> How the installer is signed: [Code signing policy](#code-signing-policy).

The installer is multilingual: it starts in **English** or **Spanish** based on your Windows display
language, and the button in its corner switches between them.

## macOS: Easy Installer (recommended)

On a **Mac with macOS 12 or newer** (Apple Silicon or Intel), the same installer as on Windows, in a window: no
Terminal, no Homebrew, no Docker. It sets up its own Python 3.14 and the [pinned Switch
tools](#the-switch-tools-pinned), downloads the release, builds the game natively and copies it to the SD card.

**[⬇ Apple Silicon (M1 and newer)](https://github.com/centollOS/SwitchWakerHD/releases/latest/download/SwitchWakerHD-Installer-macOS-AppleSilicon.zip)**
· **[⬇ Intel](https://github.com/centollOS/SwitchWakerHD/releases/latest/download/SwitchWakerHD-Installer-macOS-Intel.zip)**
(Apple menu > *About This Mac* says which: *Chip Apple M…* or *Processor Intel*)

1. Download the zip and double-click it: *SwitchWakerHD Installer* appears next to it (you can move it to
   *Applications*).
2. **The first time**, macOS stops it, because the app is not signed with an Apple developer account
   (*"Apple could not verify…"*): press **Done**, open **System Settings > Privacy & Security**, scroll down to
   *"SwitchWakerHD Installer was blocked…"* and press **Open Anyway**, then **Open** and your password. Later
   starts open directly.
3. Pick your game dump (a `.wua`, a `.wux`/`.wud` with its keys, or an extracted folder), and if you like the
   **GameCube sea minimap** (your own GameCube Wind Waker: `.iso`, `.gcm`, `.rvz` or its folder), **Brazilian
   Portuguese** (the translation you downloaded) and the **HOME-screen icon** (your console's `prod.keys`), as on
   Windows above; **Prepare the graphics now** is ticked by
   default there too. You can also drag the game onto the window.
4. Press **Install**. On the way:
   - only with the HOME-screen icon (it is still built with devkitPro and Apple's tools): if Apple's **command
     line tools** are missing, macOS shows its own window (press *Install*, wait, then *Continue* in the
     installer), and macOS asks for **your password once**, to install devkitPro in `/opt/devkitpro`;
   - if the game is on the Desktop, in Documents, Downloads or on an external disk, macOS may ask to let the
     installer read it: allow it.
5. Wait for the build (10–30 minutes; keep the Mac plugged in), then:
   - **With the microSD card in the Mac**: a card reader, or on the Switch hekate's *Tools > USB Tools > SD Card*
     and the USB cable; the installer copies the files, reads every one back from the card (copying again any
     that differs) and ejects the card.
   - **By hand**: *Show in Finder* opens the folder; copy its contents to the root of the SD card.

The full log is in `~/SwitchWakerHD-Build/installer.log`. The installer starts in Spanish or English from the
Mac's language; the button in its corner switches. Source: [installer/desktop](installer/desktop) and
[installer/build-mac.sh](installer/build-mac.sh).

## Linux: Easy Installer

The same installer for **Linux** (x86-64 or arm64; Ubuntu, Debian, Mint, Fedora, Arch, openSUSE and others with a
desktop), one file with everything inside:

**[⬇ x86-64](https://github.com/centollOS/SwitchWakerHD/releases/latest/download/SwitchWakerHD-Installer-linux-x64.tar.gz)**
· **[⬇ arm64](https://github.com/centollOS/SwitchWakerHD/releases/latest/download/SwitchWakerHD-Installer-linux-arm64.tar.gz)**

1. Extract the archive (double-click it, or right-click > *Extract here*) and double-click
   **SwitchWakerHD-Installer** (if your file manager asks, choose *Run*).
2. Pick your game dump and, if you like, the GameCube sea minimap, Brazilian Portuguese and the HOME-screen icon,
   as on Windows above (a `.rar` needs `bsdtar`, from the `libarchive-tools` package, or extract it yourself and
   choose the folder)
   (**Prepare the graphics now** is ticked by default).
3. Press **Install**. It sets up its own Python 3.14 and the [pinned Switch tools](#the-switch-tools-pinned) in
   its working folder: no password, no devkitPro, Docker or Podman. Only the HOME-screen icon still needs devkitPro
   installed (the installer sets it up the first way that fits: devkitPro's apt route on Debian and Ubuntu, asking
   for your password once through your desktop's polkit window, or a command to paste in a terminal).
4. Wait for the build (10–30 minutes), then:
   - **With the microSD card in the computer**: a card reader, or hekate's *Tools > USB Tools > SD Card* (the
     installer reads every file back from the card and copies again any that differs);
   - **By hand**: *Open folder*, then copy its contents to the root of the SD card.

The full log is in `~/SwitchWakerHD-Build/installer.log`. Source: [installer/desktop](installer/desktop) and
[installer/build-linux.sh](installer/build-linux.sh).

> If `pkg.devkitpro.org` answers *403* while the Switch tools download, devkitPro's servers blocked your connection
> while fending off bots: press *Retry* later or try from another network (for example your phone's hotspot); what
> was downloaded is kept.

### The Switch tools (pinned)

The game is built with **devkitPro's own package files at the exact versions the release was built and tested with**
(GCC 15.2, libnx 4.12, ...: [tools/toolchain/lock.json](tools/toolchain/lock.json)), downloaded once from
devkitPro's server (about 130 MB), checked against their SHA-256 and unpacked into the working folder
(`toolchain\`), with zig as the compiler for the prepared graphics' tool. A devkitPro, MSYS2, Docker, compiler or
Python already on the computer is neither used nor changed. So your `wwhd.nro` is the one we would build from the same
game, and a crash report from it points at the right place in the code. The game's log names the tools that built
it (`[boot] toolchain: ...`). Details: [docs/pinned-toolchain-plan.md](docs/pinned-toolchain-plan.md).

---

*For a manual setup on Windows, macOS or Linux, continue below.*

---

## 1. What you need

| | |
|---|---|
| **The game** | Your own dump of **The Wind Waker HD, USA or Europe** (title `00050000-10143500` or `00050000-10143600`), **version 0** (the disc, or the eShop release without the update): a disc image (`.wux`/`.wud`) with its disc key and your console's Wii U common key, or the extracted game folder (`code/`, `content/`, `meta/`) |
| **A Switch** | Running Atmosphère with the Homebrew Menu, and about 3 GB free on the SD card |
| **A computer** | Windows 10/11, macOS or Linux; 8 GB of RAM or more, 10 GB of free disk space |
| **On the computer** | Python 3.14 with `cmake` and `ninja` (`python3 -m pip install cmake ninja`; `pycryptodome` too for a `.wux`/`.wud`), this release unzipped, and the internet once for the [pinned Switch tools](#the-switch-tools-pinned) (`make_sd.py` downloads them): no devkitPro, Docker or compiler needed |

How to dump the game and the keys from your own Wii U is outside this guide: the
[Cemu dumping guide](https://cemu.cfw.guide/dumping-games.html) (Dumpling) covers it. A dump made with
Dumpling gives you the extracted folder directly (the base game, `00050000/10143500` or `10143600`, not the update).

### Windows: devkitPro (no Docker or WSL)

Not needed for the game any more (the [pinned Switch tools](#the-switch-tools-pinned) replace it); only for the
HOME-screen icon below, or for developers who build with their own devkitPro (`make_sd.py --toolchain devkitpro`).
Do this once, and then run step 2 below in a normal PowerShell or Command Prompt (write `python` where step 2 says
`python3`):

1. Install [Python 3](https://www.python.org/downloads/) (tick *Add python.exe to PATH*), then
   `python -m pip install zstandard pycryptodome` (`zstandard` is for a `.wua` with a Python older than 3.14, which
   has no `compression.zstd`; `pycryptodome` for a `.wux`/`.wud`).
2. Install devkitPro from <https://github.com/devkitPro/installer/releases> into the default `C:\devkitPro`.
3. Open *devkitPro > MSYS2* from the Start menu and run
   `pacman -Syu`, then `pacman -S --needed devkitA64 switch-dev deko3d uam switch-lz4 switch-zlib switch-cmake cmake make`,
   and for the prepared graphics `pacman -S --needed gcc ninja zlib-devel`.

A Cemu `.wua` archive works directly, with no keys: `python tools/switch/make_sd.py --wua "C:\path\game.wua"`.
The first build takes 10 to 30 minutes.

**HOME-screen icon (optional).** After the game has built, `make_sd.py` also builds the forwarder `.nsp`
([tools/switch/forwarder/INSTALL.md](tools/switch/forwarder/INSTALL.md)) when it finds your console's own
`prod.keys`: put the file in `%USERPROFILE%\.switch\` (not in the release folder), or pass `--keys "D:\path\prod.keys"`. Without
keys that step is skipped and the game builds as usual (`--no-forwarder` skips it always). With the
[pinned Switch tools](#the-switch-tools-pinned) (the default) it needs only `python -m pip install pillow` more; with
`--toolchain devkitpro` also [Git](https://git-scm.com/download/win) and, in the same MSYS2 window, `pacman -S gcc`.
The `.nsp` ends up in `build\forwarder\` and in `build\sd\NSP\`.

### Windows with Docker and WSL (alternative)

With Docker Desktop installed, run everything below inside WSL (Ubuntu) instead: install WSL with
`wsl --install` in an administrator PowerShell, turn on Docker Desktop's *Use the WSL 2 based engine* and its
integration with your Ubuntu, then open Ubuntu and work there (your Windows drives are under `/mnt/c/...`).

## 2. Build it

Open a terminal in the unzipped release folder.

```sh
python3 -m pip install pycryptodome      # only needed for a .wux/.wud image

# from a disc image: GAME.key (disc key) next to the image, and your common key in common.key
# next to it too (or WIIU_COMMON_KEY=<32 hex digits> in the environment)
python3 tools/switch/make_sd.py --image /path/to/game.wux

# or from a Cemu archive (no keys; Python 3.14+, or python3 -m pip install zstandard)
python3 tools/switch/make_sd.py --wua /path/to/game.wua

# or from an extracted game folder
python3 tools/switch/make_sd.py --game-dir /path/to/10143500
```

It checks the game version first, and every file of the game's `content/` against that version's list (a
missing, cut-off or damaged file is a warning naming it: dump or extract the game again), then translates the game's code (about a minute) and builds the
homebrew with the [pinned Switch tools](#the-switch-tools-pinned) (10 to 30 minutes the first time, which also
downloads the tools, about 130 MB; `--toolchain devkitpro` or `--toolchain docker` builds with your own devkitPro or the
`devkitpro/devkita64` image instead).
If the computer runs out of memory, add `--jobs 2`.

**Prepared graphics:** `make_sd.py` also downloads the release's graphics template (`shader-template.sealed`: the
game's graphics states without any of the game's code; it only opens with your own copy of the game) and compiles the
graphics from it on your computer, a few minutes, so the console starts with them ready (its tool is compiled with the
pinned tools' zig: nothing more to install). Offline, or with `--no-shaders`, the console compiles them itself
(below); a template downloaded by hand goes in with `--shaders FILE`.

At the end the folder `build/sd/` holds:

```
build/sd/switch/wwhd/
├── wwhd.nro              the game
├── shadercache_dksh.bin  the prepared graphics (with --shaders)
└── game/                 your game's files
```

### Optional: code mods (the minimaps)

Two minimaps from the mod catalogue can be built into the game. The installers build in the HD minimap; the GameCube
sea minimap is built only with `make_sd.py` by hand. Use one of them: they use the same corner of the screen.

**The HD minimap** (`hd-minimap`, based on **Minimap HD by Chironeyes**, the Cemu mod on
[GameBanana](https://gamebanana.com/mods/727687) and [GitHub](https://github.com/Chironeyes/The.Wind.Waker.HD-Minimap.HD)) draws a minimap in the bottom-left corner with the HD game's own GamePad maps: the island of the sea square
you are in, the floor plan of a dungeon once you have its map, and while sailing (D-pad down) a treasure chart you own.
It reads the maps from your own game while it runs, so it needs nothing besides your Wind Waker HD. The
[Easy Installer](#windows-easy-installer-recommended) always builds it into the game: tick *Turn it on now* to have it
from the first start; otherwise it is there switched off. With `make_sd.py`:

```sh
python3 tools/switch/make_sd.py --game-dir /path/to/10143500 --guest-mod hd-minimap
```

(add `--guest-mod-off hd-minimap` to build it in switched off.)

- **On or off** in the game: hold **Minus** > **Mods** > **Built-in**, its switch at the top (*HD minimap*); it
  applies when you start the game again. Switched off it is not loaded at all.

- On foot, D-pad left shows or hides it and D-pad right makes it bigger or smaller; in a dungeon, D-pad down zooms.
  On the boat, D-pad left and right stay the cannon and the grappling hook, and D-pad down switches between the
  compasses, the minimap and the minimap with a chart. It hides in menus, cutscenes and first person.
- Its options (position, both sizes, opacity, fading, the buttons) are in the game: hold **Minus** > **Mods** >
  **Built-in**, at the top, *HD minimap*; then close the game and start it again. Or `--mod-option hd-minimap.size=220` when
  you build.
- Cost on the console (EU, handheld, stock 1020 MHz CPU, 2026-11-08): about 0.15-0.4 ms of the main thread's
  33 ms per frame, and one longer frame (under 0.1 s) the first time an island's map is shown in a session.

**The GameCube sea minimap** draws a GameCube-style chart of the Great Sea in a corner of the screen. It needs **two
things besides your Wind Waker HD**:

- **Your own GameCube Wind Waker** (USA, Europe or Japan): a disc image (`.iso`, `.gcm`, `.rvz`) or its extracted
  folder. The mod draws its maps from it, on your computer.
The installers no longer offer it (since the HD minimap, issue #121); with `make_sd.py`, add the mod and the disc to
the command:

```sh
python3 tools/switch/make_sd.py --game-dir /path/to/10143500 \
    --guest-mod gc-minimap --gc-game /path/to/GameCube-WindWaker.iso
```

`--guest-mod gc-minimap` downloads the mod from the mod catalogue
([ZeldaWWHDMods](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDMods)) and checks it; a mod's `.zip` or folder works too.
The Switch cannot build code mods (Mod SDK v2, "guest" mods) itself, so they go into the game when you build it.

- **A `.rvz` from Dolphin** needs **Python 3.14 or newer** (Dolphin's usual RVZ compression, Zstandard, is read with
  Python 3.14's own library). With an older Python, `make_sd.py` looks for a Python 3.14 on the computer and uses it
  for that step; if there is none it stops at once and says so. Then install Python 3.14 from
  [python.org](https://www.python.org/downloads/), or make an `.iso` in Dolphin (right-click the game, *Convert
  File...*, format *ISO*) and give that. An `.iso`, a `.gcm` or a folder works with any Python 3.
- **Its options** (position, size, opacity, map style) are changed on the console: hold **Minus** > **Mods** >
  **Built-in**, at the top, *GameCube minimap*; then close the game and start it again. Or set them when you
  build: `--mod-option gc-minimap.size=260` (`x`, `y`, `size`, `opacity`, `style` = `vector` or `original`).
- The maps it prepares are kept in your user folder (macOS: `~/Library/Application Support/SwitchWakerHD/guest-mods`,
  Windows: `%LOCALAPPDATA%\SwitchWakerHD\guest-mods`, Linux: `~/.local/share/SwitchWakerHD/guest-mods`), and
  copied to `build/sd/switch/wwhd/mods/`.
- To add or remove a mod, run `make_sd.py` again (without `--guest-mod`: no mods) and copy `wwhd.nro` and `mods/`
  again.
- Mods that replace game functions are not supported on the Switch; `make_sd.py` says so.

### Optional: Brazilian Portuguese (fan translation)

The game was never released in Portuguese. Triforce-Heroes made a Brazilian Portuguese fan translation; it is not
part of SwitchWakerHD: download it yourself from
[their page](https://triforce-heroes.com.br/traducoes/the-legend-of-zelda-the-wind-waker-hd) (the **Emulador**
version, a `.rar`), then:

```
python3 tools/switch/make_sd.py --wua GAME.wua --ptbr "TriforceHeroes_WWHD VERSÃO FINAL (3.0) - USA - EUROPA.rar"
```

- `--ptbr` takes the `.rar` as downloaded (opened with `bsdtar`/`tar`: macOS and Windows 10 or later have it; on
  Linux install `libarchive-tools`), the folder you extracted it to, or its `.pack`.
- Do **not** copy the translation's files over the game: its pack is laid out differently from the game's (and one
  of its layouts is cut short). `make_sd.py` takes only the translated texts and fonts and puts them in your own
  game's English pack, laid out as the game's, in `switch/wwhd/translation/pt-BR/`; your game files stay as they are.
- In the game it is **Português (Brasil)** in the settings (hold Minus) > Language: chosen by default after
  installing it, unless you chose another language there before (then choose it there); pick English (or another
  language) there to go back, and restart the game. The menus of SwitchWakerHD
  itself follow it.
- Without `--ptbr` a translation installed earlier stays on the card; `--no-ptbr` removes it from the SD folder
  (on the card, delete `switch/wwhd/translation/`).

## 3. Copy it to the SD card

Copy the **contents** of `build/sd/` to the root of the SD card, so the game ends up at
`sdmc:/switch/wwhd/wwhd.nro`. You can take the card out, or connect the Switch by USB with hekate
(*Tools → USB Tools → SD Card*).

## 4. Play

- Start the Homebrew Menu **in title mode**: hold **R** while starting any installed game, then pick
  **SwitchWakerHD**. From the album (applet mode) the game does not get enough memory.
- Optional: a HOME-screen icon that always starts it the right way:
  [tools/switch/forwarder/INSTALL.md](tools/switch/forwarder/INSTALL.md).
- The first time you visit a place, new graphics are compiled in the background: an object may appear a
  moment late. They are kept, so it happens only once per console.

### Controls

| | |
|---|---|
| **Minus, held half a second** | Settings menu (L / R change tabs, B or Minus closes it). A short Minus goes to the game |
| **ZL + ZR + Minus** | In GamePad mode: switch between the TV picture and the GamePad screen (items, map). On the GamePad screen the touch screen works as the GamePad's. Or show the GamePad screen small in a corner of the TV picture: Minus held > Dev tab > *Show developer options* > "GamePad screen in a corner of the TV picture" (the touch screen then works inside that corner) |

By default the controller acts as a **Wii U Pro Controller** and everything is on one screen. To play
with the GamePad features (its screen, **gyro aiming**), choose *Wii U GamePad* in the menu's Dev tab
(tick *Show developer options* first) **and** in the game's own options, then turn on *Gyro aiming* in the
menu's Controls tab.

### The Picture and Controls tabs of the menu

Picture: internal resolution per mode (dynamic resolution), picture adjustments, frame-rate counter.
Controls: rumble, gyro aiming. The game runs at the console's stock CPU clock and, in handheld, Nintendo's 384 MHz GPU
profile (not a menu choice; one saved by an older version is reset to 384); for more, use sys-clk. Other tabs: Saves (save states), Warp (go to any place), Mods, About.

## The first start: preparing the graphics

With the installer's **Prepare the graphics now** (or `make_sd.py --shaders shader-template.sealed`) the graphics
were compiled on your computer: the console loads them at start-up and none of this applies (no first-start screen).
Without them:

The console compiles each of the game's graphics effects the first time it is drawn; until then a texture can look
black or an object appear a moment late. So the **first start** asks how to prepare them, once:

- **A: Prepare graphics** (the most complete). Choose your Quest Log, or start a new game you do not save; the game
  then visits every place by itself behind a loading screen (30-40 minutes, best docked; hold B to stop) and restarts
  at the title screen with about 93% of the game's graphics ready. Your saves are not touched. Until a Quest Log is
  loaded it waits, also after a restart; the settings menu (Minus, **Picture** tab) can cancel it.
- **B: Play now.** A "Preparing the graphics" screen instead: behind it the game boots and its title screen runs, then
  the console reads the graphics programs of your own copy of the game and compiles about three quarters of the
  game's graphics, about 20 minutes. **B** on that screen plays at once; what is left goes on in the background while
  you play, gently (the frame rate stays), and in the next starts if needed.

What is compiled is kept. The rest of the game's graphics compile the first time they show, in a few seconds, once.
**Prepare graphics** stays in the settings menu (Minus, **Picture** tab) for later.

## Updating

Build the new release the same way and copy `wwhd.nro` over the old one (`game/` only if you never copied
it). Saves (`save/`), settings (`settings.ini`) and the compiled graphics stay.

## Problems

| | |
|---|---|
| `make_sd: ... not the expected file` | The dump is not version 0 of the USA or European game, or the update was merged into it: use the base game only |
| `the disc key was not found` / `common key not found` | Put `GAME.key` next to `GAME.wux`, and `common.key` next to it (or `WIIU_COMMON_KEY`) |
| Docker errors | Docker Desktop must be running (on Windows, with its WSL integration on) |
| `Docker or Podman is needed ... or a native devkitPro` | Only with `--toolchain devkitpro` or `docker`: leave the option out to build with the pinned Switch tools |
| `the pinned toolchain needs Python 3.14 or newer` / `cmake is missing next to this Python` | Install Python 3.14 from python.org, then `python -m pip install cmake ninja` (with that Python) |
| The game closes at once or says it is out of memory | Start it in title mode (hold R), not from the album |
| No button works | The game's controller setting differs from the menu's Dev tab (*Show developer options*): set both to the same |
| Something else | `sdmc:/switch/wwhd/wwhd.log` and `logs/` say what happened; include them in a report (they contain no game data) |

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by
[SignPath Foundation](https://signpath.org). (SignPath Foundation accepted the project in October 2026; until the first
release signed with its production certificate, the Windows installer is still unsigned.)

- Committers and reviewers: [the members of centollOS](https://github.com/orgs/centollOS/people); every outside
  pull request is reviewed by a member before it is merged.
- Approvers: [Pulpparty](https://github.com/Pulpparty), who approves every signing by hand.

Only the Windows installer, `SwitchWakerHD-Installer.exe`, is signed. It is built from this repository by
[GitHub Actions](.github/workflows/release-installer.yml).

Privacy policy: this program will not transfer any information to other networked systems unless specifically
requested by the user or the person installing or operating it. To install, it downloads the release, the
devkitPro updater and the tools it installs (Python, devkitPro's packages; Git, Pillow and MSYS2's gcc for the
HOME-screen icon; MSYS2's gcc, ninja and zlib for the prepared graphics; the minimap mod if chosen) from GitHub and
from those tools' own servers, and, unless *Prepare the graphics now* is unticked, the release's graphics template
(`shader-template.sealed`: data, never run; encrypted with the game's own files, see
[docs/shader-manifest-release-plan.md](docs/shader-manifest-release-plan.md)) from GitHub.
