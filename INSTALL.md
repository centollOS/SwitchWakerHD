# Installing SwitchWakerHD on your Switch

SwitchWakerHD runs The Wind Waker HD natively on a Switch with custom firmware. **Nothing of the game
comes with it**: you build it on your computer from your own copy of the game, with one script, and copy
the result to the SD card. What you build contains the game, so it is for your own console only: do not
share it.

## Windows: Easy Installer (recommended)

On **Windows 10/11**, an automatic installer handles everything for you: Python, devkitPro, the release
download, and the build. You only need your own personal dump of the game.

**[⬇ Download SwitchWakerHD-Installer.exe](https://github.com/centollOS/SwitchWakerHD/releases/latest/download/SwitchWakerHD-Installer.exe)**

1. Download the `.exe` above and double-click it.
2. Accept the UAC prompt (the installer needs Administrator rights to install devkitPro).
3. Pick your game dump: a Cemu archive (`.wua`), a disc image (`.wux`/`.wud`, with its keys), or an
   extracted folder.
   Optional, for a **HOME-screen icon**: choose your console's `prod.keys` (dumped with Lockpick_RCM; they are
   read where they are, never copied). The installer then also sets up Git, Pillow and MSYS2's gcc and builds
   the forwarder `.nsp` ([details](tools/switch/forwarder/INSTALL.md)); it travels in `NSP\` with the rest: install it with
   DBI (*Browse SD card > NSP > wwhd_forwarder.nsp > Install*; it needs current Atmosphère sigpatches).
   Optional, the **GameCube sea minimap**: choose your own GameCube Wind Waker (`.iso`, `.gcm`, `.rvz` or its
   folder); the installer downloads the mod and builds it into the game
   ([details](#optional-code-mods-the-gamecube-sea-minimap)).
4. Press **Install**. If devkitPro is not installed yet, the installer asks you to open its setup: accept the
   license and keep the default path (`C:\devkitPro`).
5. Wait for the build (10–30 minutes), then choose how the files reach the Switch:
   - **Over a USB cable, with DBI**: on the Switch, open DBI and choose *Run MTP responder*, then connect the cable.
   - **With the microSD card in the PC**: a card reader, or hekate's *USB Tools > SD Card* (UMS). After the copy
     the installer reads every file back from the card and copies again any that differs.
   - **By hand**: the installer opens the folder; copy its contents to the root of the SD card.

If something fails, *Show details* shows what happened; the full log is in
`%USERPROFILE%\SwitchWakerHD-Build\installer.log` (in `C:\SwitchWakerHD-Build\installer.log` when your Windows
user name has a space or an accented letter: the installer then works there).

> **Windows SmartScreen** may show *"Windows protected your PC"* on first run because the installer is
> not code-signed. Click **More info → Run anyway**. This is expected for open-source unsigned executables.
> The [source code](installer/windows) and [build script](installer/build.ps1) are in this repository.

The installer is multilingual: it starts in **English** or **Spanish** based on your Windows display
language, and the button in its corner switches between them.

## macOS: Easy Installer (recommended)

On a **Mac with macOS 12 or newer** (Apple Silicon or Intel), the same installer as on Windows, in a window: no
Terminal, no Homebrew, no Docker. It sets up its own Python 3.14, Apple's command line tools and devkitPro, downloads
the release, builds the game natively and copies it to the SD card.

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
   **GameCube sea minimap** (your own GameCube Wind Waker: `.iso`, `.gcm`, `.rvz` or its folder) and the
   **HOME-screen icon** (your console's `prod.keys`), as on Windows above. You can also drag the game onto the
   window.
4. Press **Install**. On the way:
   - if Apple's **command line tools** are missing, macOS shows its own window: press *Install*, wait, then
     *Continue* in the installer;
   - macOS asks for **your password once**, to install devkitPro in `/opt/devkitpro`;
   - if the game is on the Desktop, in Documents, Downloads or on an external disk, macOS may ask to let the
     installer read it: allow it.
5. Wait for the build (10–30 minutes; keep the Mac plugged in), then:
   - **With the microSD card in the Mac**: a card reader, or on the Switch hekate's *Tools > USB Tools > SD Card*
     and the USB cable; the installer copies the files, reads every one back from the card (copying again any
     that differs) and ejects the card. (A Mac has no MTP, so DBI's USB transfer is not offered.)
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
2. Pick your game dump and, if you like, the GameCube sea minimap and the HOME-screen icon, as on Windows above.
3. Press **Install**. It sets up its own Python 3.14 in its working folder, then the Switch toolchain, the first
   that fits:
   - devkitPro already installed (`/opt/devkitpro` with the Switch packages): it builds natively;
   - on Debian, Ubuntu and their derivatives: devkitPro's own apt route (it asks for your password once);
   - else a Docker or Podman your user may use: it builds in the `devkitpro/devkita64` image (the first time it
     downloads it, ~1.5 GB); Fedora, Arch and openSUSE get Podman (your password once). The HOME-screen icon
     needs devkitPro installed, so it is skipped in a container.

   The password window comes from your desktop (polkit). Without one, the installer shows a command to paste in a
   terminal.
4. Wait for the build (10–30 minutes), then:
   - **Over a USB cable, with DBI**: on the Switch, open DBI and choose *Run MTP responder*, then connect the
     cable (through `gio`, which most desktops have);
   - **With the microSD card in the computer**: a card reader, or hekate's *Tools > USB Tools > SD Card* (the
     installer reads every file back from the card and copies again any that differs);
   - **By hand**: *Open folder*, then copy its contents to the root of the SD card.

The full log is in `~/SwitchWakerHD-Build/installer.log`. Source: [installer/desktop](installer/desktop) and
[installer/build-linux.sh](installer/build-linux.sh).

> If `pkg.devkitpro.org` or `apt.devkitpro.org` answers *403*, devkitPro's servers blocked your connection while
> fending off bots: try later or from another network (for example your phone's hotspot).

---

*For a manual setup on Windows, macOS or Linux, continue below.*

---

## 1. What you need

| | |
|---|---|
| **The game** | Your own dump of **The Wind Waker HD, USA or Europe** (title `00050000-10143500` or `00050000-10143600`), **version 0** (the disc, or the eShop release without the update): a disc image (`.wux`/`.wud`) with its disc key and your console's Wii U common key, or the extracted game folder (`code/`, `content/`, `meta/`) |
| **A Switch** | Running Atmosphère with the Homebrew Menu, and about 3 GB free on the SD card |
| **A computer** | Windows 10/11, macOS or Linux; 8 GB of RAM or more, 10 GB of free disk space |
| **On the computer** | Python 3, this release unzipped, and the Switch toolchain: on **Windows**, [devkitPro](#windows-devkitpro-no-docker-or-wsl) (recommended: no Docker, no WSL); on **macOS**, the [Easy Installer](#macos-easy-installer-recommended) sets up devkitPro itself (or by hand: [Docker Desktop](https://www.docker.com/products/docker-desktop/)); on **Linux**, the [Easy Installer](#linux-easy-installer) (or by hand: Docker or Podman) |

How to dump the game and the keys from your own Wii U is outside this guide: the
[Cemu dumping guide](https://cemu.cfw.guide/dumping-games.html) (Dumpling) covers it. A dump made with
Dumpling gives you the extracted folder directly (the base game, `00050000/10143500` or `10143600`, not the update).

### Windows: devkitPro (no Docker or WSL)

The simplest way on Windows 10/11: a native [devkitPro](https://devkitpro.org) install, with no Docker and no
WSL. `make_sd.py` uses it when neither Docker nor Podman is installed. Do this once, and then run step 2 below
in a normal PowerShell or Command Prompt (write `python` where step 2 says `python3`):

1. Install [Python 3](https://www.python.org/downloads/) (tick *Add python.exe to PATH*), then
   `python -m pip install zstandard pycryptodome` (`zstandard` is for a `.wua`, `pycryptodome` for a `.wux`/`.wud`).
2. Install devkitPro from <https://github.com/devkitPro/installer/releases> into the default `C:\devkitPro`.
3. Open *devkitPro > MSYS2* from the Start menu and run
   `pacman -Syu`, then `pacman -S --needed switch-dev deko3d uam switch-lz4 switch-zlib switch-cmake cmake make`.

A Cemu `.wua` archive works directly, with no keys: `python tools/switch/make_sd.py --wua "C:\path\game.wua"`.
The first build takes 10 to 30 minutes.

**HOME-screen icon (optional).** After the game has built, `make_sd.py` also builds the forwarder `.nsp`
([tools/switch/forwarder/INSTALL.md](tools/switch/forwarder/INSTALL.md)) when it finds your console's own
`prod.keys`: put the file in `%USERPROFILE%\.switch\` (not in the release folder), or pass `--keys "D:\path\prod.keys"`. Without
keys that step is skipped and the game builds as usual (`--no-forwarder` skips it always). It needs three more
things: [Git](https://git-scm.com/download/win), `python -m pip install pillow`, and in the same MSYS2 window
`pacman -S gcc`. The `.nsp` ends up in `build\forwarder\` and in `build\sd\NSP\`.

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

# or from a Cemu archive (no keys; python3 -m pip install zstandard)
python3 tools/switch/make_sd.py --wua /path/to/game.wua

# or from an extracted game folder
python3 tools/switch/make_sd.py --game-dir /path/to/10143500
```

It checks the game version first, and every file of the game's `content/` against that version's list (a
missing, cut-off or damaged file is a warning naming it: dump or extract the game again), then translates the game's code (about a minute) and builds the
homebrew in Docker, or with devkitPro on Windows (10 to 30 minutes the first time; Docker downloads the
toolchain image, about 1 GB).
If the computer runs out of memory, add `--jobs 2`.

At the end the folder `build/sd/` holds:

```
build/sd/switch/wwhd/
├── wwhd.nro     the game
└── game/        your game's files
```

### Optional: code mods (the GameCube sea minimap)

The GameCube sea minimap draws a GameCube-style chart of the Great Sea in a corner of the screen. It needs **two
things besides your Wind Waker HD**:

- **Your own GameCube Wind Waker** (USA, Europe or Japan): a disc image (`.iso`, `.gcm`, `.rvz`) or its extracted
  folder. The mod draws its maps from it, on your computer.
On Windows, the [Easy Installer](#windows-easy-installer-recommended) has a box for it: choose your GameCube disc
there. With `make_sd.py`, add the mod and the disc to the command:

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
  **Built-in**, at the bottom, *Code mods in this build*; then close the game and start it again. Or set them when you
  build: `--mod-option gc-minimap.size=260` (`x`, `y`, `size`, `opacity`, `style` = `vector` or `original`).
- The maps it prepares are kept in your user folder (macOS: `~/Library/Application Support/SwitchWakerHD/guest-mods`,
  Windows: `%LOCALAPPDATA%\SwitchWakerHD\guest-mods`, Linux: `~/.local/share/SwitchWakerHD/guest-mods`), and
  copied to `build/sd/switch/wwhd/mods/`.
- To add or remove a mod, run `make_sd.py` again (without `--guest-mod`: no mods) and copy `wwhd.nro` and `mods/`
  again.
- Mods that replace game functions are not supported on the Switch; `make_sd.py` says so.

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
| **ZL + ZR + Minus** | In GamePad mode: switch between the TV picture and the GamePad screen (items, map). On the GamePad screen the touch screen works as the GamePad's. Or show the GamePad screen small in a corner of the TV picture: Minus held > Switch tab > "GamePad screen in a corner of the TV picture" (the touch screen then works inside that corner) |

By default the controller acts as a **Wii U Pro Controller** and everything is on one screen. To play
with the GamePad features (its screen, **gyro aiming**), choose *Wii U GamePad* in the menu's Switch tab
**and** in the game's own options, then turn on *Gyro aiming* there.

### The Switch tab of the menu

Handheld GPU profile (Nintendo's official profiles; stock by default), picture profile per mode
(internal resolution, dynamic resolution), picture adjustments, frame-rate counter, controller (Pro
Controller or GamePad), gyro aiming. The game sets no CPU or GPU overclock; for one, use sys-clk. Other tabs: Saves (save states), Warp (go to any place), Mods, About.

## The first start: preparing the graphics

The console compiles each of the game's graphics effects the first time it is drawn; until then a texture can look
black or an object appear a moment late. So the **first start** asks how to prepare them, once:

- **A: Prepare graphics** (the most complete). Choose your Quest Log, or start a new game you do not save; the game
  then visits every place by itself behind a loading screen (30-40 minutes, best docked; hold B to stop) and restarts
  at the title screen with about 93% of the game's graphics ready. Your saves are not touched. Until a Quest Log is
  loaded it waits, also after a restart; the settings menu (Minus, **Switch** tab) can cancel it.
- **B: Play now.** A "Preparing the graphics" screen instead: behind it the game boots and its title screen runs, then
  the console reads the graphics programs of your own copy of the game and compiles about three quarters of the
  game's graphics, about 20 minutes. **B** on that screen plays at once; what is left goes on in the background while
  you play, gently (the frame rate stays), and in the next starts if needed.

What is compiled is kept. The rest of the game's graphics compile the first time they show, in a few seconds, once.
**Prepare graphics** stays in the settings menu (Minus, **Switch** tab) for later.

## Updating

Build the new release the same way and copy `wwhd.nro` over the old one (`game/` only if you never copied
it). Saves (`save/`), settings (`settings.ini`) and the compiled graphics stay.

## Problems

| | |
|---|---|
| `make_sd: ... not the expected file` | The dump is not version 0 of the USA or European game, or the update was merged into it: use the base game only |
| `the disc key was not found` / `common key not found` | Put `GAME.key` next to `GAME.wux`, and `common.key` next to it (or `WIIU_COMMON_KEY`) |
| Docker errors | Docker Desktop must be running (on Windows, with its WSL integration on) |
| `Docker or Podman is needed ... or a native devkitPro` | On Windows, do the "Windows: devkitPro" setup above; elsewhere, install Docker |
| The game closes at once or says it is out of memory | Start it in title mode (hold R), not from the album |
| No button works | The game's controller setting differs from the menu's Switch tab: set both to the same |
| Something else | `sdmc:/switch/wwhd/wwhd.log` and `logs/` say what happened; include them in a report (they contain no game data) |
