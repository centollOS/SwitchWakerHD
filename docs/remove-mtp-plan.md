# Removing the USB copy with DBI (MTP) from the installers (plan)

Branch `feature/remove-mtp`, on main. Maintainer's call, 2026-10-26.

## Why

- On Windows the copy over DBI's MTP responder kept failing: the shell's `CopyHere` to the portable device can
  return without the file ever landing, and DBI's storage then reports files as "not listed".
- An MTP copy cannot be read back past a cache like the card copy (#48), so it only had a size check: a bad copy
  shows up later as a crash in the game.

## What goes

- Windows (`installer/windows`): `MtpCopy.cs`, the "Over a USB cable, with DBI" card and its panel, the MTP code in
  `MainWindow.xaml.cs`, and the `Microsoft.CSharp` reference (only `MtpCopy.cs` used `dynamic`).
- macOS/Linux (`installer/desktop`): `GioMtp.cs` (Linux, through gio), the same card and panel, `HasMtp`, the MTP
  strings in `LinuxStrings.cs`. macOS never offered it.
- The `mtp_*`, `xfer_mtp*`, `btn_copy_mtp` and `next_1_mtp` strings, and "or copy over USB with DBI" in the FAT32
  message.
- The option in INSTALL.md.

## What stays

- The microSD copy, with its read-back check: a card reader, or hekate's USB mass storage (*Tools > USB Tools >
  SD Card*). It becomes the default choice on the done page, and its description says how to do it without a card
  reader.
- "I'll copy it myself".
- DBI for installing the forwarder `.nsp`, and MTP in the developer notes (AGENTS.md) and in the bug-report docs
  (getting logs off the card with the player's own tools).

## Tests

1. Both installers build (Windows WPF net48 with `EnableWindowsTargeting`, the desktop one on the Mac).
2. `tools/installer/test_*.py` pass.
3. `git grep -i mtp`: only the kept mentions above and old plans.
4. By hand, at the next installer test round: the done page shows two choices, the SD card one selected, the copy
   and its check work (`docs/installer-windows-tests.md` test 7, release checklist A3v).
