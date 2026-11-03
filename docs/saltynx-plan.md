# The frame rate for the console's overlays (SaltyNX) (plan)

Branch `feature/saltynx`. Status: steps 1-2 done (2026-11-03): Status Monitor shows the game's 30 fps on the console.

## Why

Players use Status Monitor (and its forks, as Ultrahand's) to read a game's frame rate. The overlay does not
measure it: it reads a block in SaltyNX's shared memory, written by SaltyNX's NX-FPS plugin from inside a retail
game. SaltyNX never loads into a homebrew program (its debug attach fails, 0xfa01), so the overlay shows nothing for
this port. NFSMW-NX (`docs/console-overlays.md`) writes the block from the program itself; its template is GPL-3.0,
so this one is written from SaltyNX's and Status Monitor's behaviour (their public sources), not from that code.

## Design (`platform/saltynx_switch.cpp`, two marked hooks)

- **At start-up** (`main_switch.cpp`, after the update check): a thread connects to the named port `SaltySD`
  (once a second for a minute; nothing at all when the port does not exist), asks for the shared page's handle
  (command 7, the process ID sent) and maps it (4 KiB).
- **The block**: the first one with the magic 0x465053 already in the page is reused (an overlay reads the first
  one, and reservations are never given back for a homebrew program); else command 6 reserves 314 bytes (SaltyNX
  2.1.3's block, 310, and 4 more: the page's first word is SaltyNX's own, the display's refresh rate in its second
  byte, so a block given offset 0 starts at 4; console: a block at 0 lost its magic and the next start reserved
  another). The process had no IPC session left for the port (0x10801, the forwarder's application pool): the
  limit is raised by 4 (svcSetResourceLimitLimitValue, allowed by hbloader's NPDM), else sm's session is lent. Then command 0 ends the session (the port takes one session at a time: the overlays need it).
- **At every present** (`backend.cpp present()`): frameNumber, the ring of ticks between the last 10 frames, the
  average rate, the frames of the last second, `pluginActive` (the overlay clears it and waits 100 ms to see it set
  again); the resolutions once an overlay asks for them (0xFFFF in `renderCalls[0].calls`): the game's picture
  (1280x720 x the internal resolution scale) and the window.
- `WWHD_SALTYNX=0` (settings.ini `[dev]`) turns it off. API field 1 (NVN: deko3d works at NVN's level on the
  same GPU); Status Monitor shows the resolutions only for an API of 1 and above, and does not print the value.

## Steps

1. The module and the hooks (this branch).
2. Console: SaltyNX 2.1.3 + Status Monitor 1.4.1+r4 (Ultrahand): the overlay shows the frame rate and the
   resolutions; a restart of the game reuses its block; the log's `[saltynx]` lines. Without SaltyNX: one line,
   nothing else.
   Console (2026-11-03, 9e7c4f3b, USA, handheld, SaltyNX 2.1.3, Status Monitor 1.4.1+r4 through Ultrahand): the
   Full mode shows 30 FPS, and the resolutions once the API field is 1 (64637b4d). Log: `[saltynx] sessions 1 of 1 in use: limit raised to 5`, the block reserved, and on
   the next starts `the block of an earlier start at offset 312` (reused).
3. Later, if wanted: ReverseNX-RT's block (the handheld/docked mode from its overlay).
