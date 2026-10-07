# Installing SwitchWakerHD on your Switch

SwitchWakerHD runs The Wind Waker HD natively on a Switch with custom firmware. **Nothing of the game
comes with it**: you build it on your computer from your own copy of the game, with one script, and copy
the result to the SD card. What you build contains the game, so it is for your own console only: do not
share it.

## 1. What you need

| | |
|---|---|
| **The game** | Your own dump of **The Wind Waker HD, USA**, title `00050000-10143500`, **version 0** (the disc, or the eShop release without the update): a disc image (`.wux`/`.wud`) with its disc key and your console's Wii U common key, or the extracted game folder (`code/`, `content/`, `meta/`) |
| **A Switch** | Running Atmosphère with the Homebrew Menu, and about 3 GB free on the SD card |
| **A computer** | Windows 10/11 (with WSL), macOS or Linux; 8 GB of RAM or more, 10 GB of free disk space |
| **On the computer** | [Docker Desktop](https://www.docker.com/products/docker-desktop/) (or Docker / Podman on Linux), Python 3, and this release unzipped |

How to dump the game and the keys from your own Wii U is outside this guide: the
[Cemu dumping guide](https://cemu.cfw.guide/dumping-games.html) (Dumpling) covers it. A dump made with
Dumpling gives you the extracted folder directly (the base game, `00050000/10143500`, not the update).

**Windows**: run everything below inside WSL (Ubuntu): install WSL with `wsl --install` in an
administrator PowerShell, turn on Docker Desktop's *Use the WSL 2 based engine* and its integration with
your Ubuntu, then open Ubuntu and work there (your Windows drives are under `/mnt/c/...`).

## 2. Build it

Open a terminal in the unzipped release folder.

```sh
python3 -m pip install pycryptodome      # only needed for a .wux/.wud image

# from a disc image: GAME.key (disc key) next to the image, and your common key in common.key
# next to it too (or WIIU_COMMON_KEY=<32 hex digits> in the environment)
python3 tools/switch/make_sd.py --image /path/to/game.wux

# or from an extracted game folder
python3 tools/switch/make_sd.py --game-dir /path/to/10143500
```

It checks the game version first, then translates the game's code (about a minute) and builds the
homebrew in Docker (10 to 20 minutes the first time; Docker downloads the toolchain image, about 1 GB).
If the computer runs out of memory, add `--jobs 2`.

At the end the folder `build/sd/` holds:

```
build/sd/switch/wwhd/
├── wwhd.nro     the game
└── game/        your game's files
```

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
| **ZL + ZR + Minus** | In GamePad mode: switch between the TV picture and the GamePad screen (items, map). On the GamePad screen the touch screen works as the GamePad's |

By default the controller acts as a **Wii U Pro Controller** and everything is on one screen. To play
with the GamePad features (its screen, **gyro aiming**), choose *Wii U GamePad* in the menu's Switch tab
**and** in the game's own options, then turn on *Gyro aiming* there.

### The Switch tab of the menu

CPU clock (1224 MHz by default; up to 1785 MHz), handheld GPU profile (Nintendo's official profiles, plus
a 614 MHz overclock), picture profile per mode (internal resolution, dynamic resolution), picture
adjustments, frame-rate counter, controller (Pro Controller or GamePad), gyro aiming. Higher clocks drain
the battery faster and warm the console. If sys-clk has its own profile for this title, the two fight
over the clocks. Other tabs: Saves (save states), Warp (go to any place), Mods, About.

## Updating

Build the new release the same way and copy `wwhd.nro` over the old one (`game/` only if you never copied
it). Saves (`save/`), settings (`settings.ini`) and the compiled graphics stay.

## Problems

| | |
|---|---|
| `make_sd: ... not the expected file` | The dump is not version 0 of the USA game, or the update was merged into it: use the base game only |
| `the disc key was not found` / `common key not found` | Put `GAME.key` next to `GAME.wux`, and `common.key` next to it (or `WIIU_COMMON_KEY`) |
| Docker errors | Docker Desktop must be running (on Windows, with its WSL integration on) |
| The game closes at once or says it is out of memory | Start it in title mode (hold R), not from the album |
| No button works | The game's controller setting differs from the menu's Switch tab: set both to the same |
| Something else | `sdmc:/switch/wwhd/wwhd.log` and `logs/` say what happened; include them in a report (they contain no game data) |
