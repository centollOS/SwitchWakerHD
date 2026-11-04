# Live input demo

A small guest mod for the Mod SDK's live features ([docs/mod-sdk-v2.md](../../../docs/mod-sdk-v2.md),
"Live options, input filters and touches"):

- **Live option** `swap` ("Swap A and B", on the item menu's Settings page under Demo): the mod reads
  `wwhd_config_changed("swap")` on each controller read and follows a change at once, without a restart.
- **Input filter**: while `swap` is on, A and B are swapped for the game.
- **Touch claim**: the picture's top-right corner (440..640 x 200..360 in layout units) is the mod's; a finger
  there (or a mouse click on the desktop) holds Y, and no on-screen stick appears under it.

Build with `make` in `examples/guest-mods` (needs clang with the PowerPC target and ld.lld).
