# HOME-screen icon (forwarder NSP)

`build/forwarder/wwhd_forwarder.nsp` installs a "Wind Waker HD" icon (the game's own icon, from your
dump) on the HOME screen. Opening it starts `sdmc:/switch/wwhd/wwhd.nro` as an application: all the
memory, and apm accepts the handheld GPU profile (in applet mode, from the album, it is skipped). No
need to open hbmenu by holding R over a game. Ported from centollOS's forwarder.

Built with `tools/switch/forwarder/build_forwarder.sh` (keys from `~/.switch/prod.keys` by default;
never copied into the repository). Title ID: `01FF575748440000`.

The title ID and the pinned nx-hbloader / hacBrewPack commits are in `forwarder.env`, read by both
build scripts (`WWHD_FORWARDER_TITLE_ID` overrides the title ID in both). `patch_hbl.py` makes the
loader's `hbl.json` edit for both.

On Windows without Docker, `tools/switch/forwarder/build_forwarder_windows.py` does the same with a native devkitPro
(keys: `%USERPROFILE%\.switch\prod.keys` by default, or `--keys PATH`; never inside the checkout); `make_sd.py` runs it after a native build.
See [INSTALL.md](../../../INSTALL.md), "Windows: devkitPro (no Docker or WSL)".

## Requirements

- Atmosphère with sigpatches up to date for your firmware (the NSP does not carry an official
  signature). If installing fails over the signature, or the icon shows an error when opened,
  update the sigpatches (including the loader/ACID ones).
- The NRO **must** stay at `sdmc:/switch/wwhd/wwhd.nro`: the forwarder only contains a loader. To
  update the game, replace the NRO; the NSP needs reinstalling only when the icon, name or version
  shown change.

## Install

- **DBI**: copy the `.nsp` to the SD card (e.g. `sdmc:/NSP/`), open DBI → *Browse SD card* → the
  `.nsp` → *Install*. Or DBI's *Run MTP responder* and drag the `.nsp` into "SD Card install".
- **Goldleaf**: *Explore SD card* → the `.nsp` → *Install* → *SD card*.

Go back to HOME: the icon is there. The `.nsp` on the SD card can be deleted afterwards.
