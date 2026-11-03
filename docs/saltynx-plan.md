# The frame rate for the console's overlays (SaltyNX) (plan)

Branch `feature/saltynx`. Status: step 1 built (2026-11-03), console check with the user next.

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
  one, and reservations are never given back for a homebrew program); else command 6 reserves 310 bytes (SaltyNX
  2.1.3's block). Then command 0 ends the session (the port takes one session at a time: the overlays need it).
- **At every present** (`backend.cpp present()`): frameNumber, the ring of ticks between the last 10 frames, the
  average rate, the frames of the last second, `pluginActive` (the overlay clears it and waits 100 ms to see it set
  again); the resolutions once an overlay asks for them (0xFFFF in `renderCalls[0].calls`): the game's picture
  (1280x720 x the internal resolution scale) and the window.
- `WWHD_SALTYNX=0` (settings.ini `[dev]`) turns it off. API field 0 (deko3d is none of NVN, EGL, Vulkan).

## Steps

1. The module and the hooks (this branch).
2. Console: SaltyNX 2.1.3 + Status Monitor 1.4.1+r4 (Ultrahand): the overlay shows the frame rate and the
   resolutions; a restart of the game reuses its block; the log's `[saltynx]` lines. Without SaltyNX: one line,
   nothing else.
3. Later, if wanted: ReverseNX-RT's block (the handheld/docked mode from its overlay).
