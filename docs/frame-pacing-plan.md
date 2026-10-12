# Frame pacing: every frame on screen for two vblanks at 30 fps (plan)

Branch `fix/frame-pacing`. Issue #23 ("Uneven/inconsistent frametime": slight judder turning the camera at 30 fps,
v0.4.4, OLED handheld). Status: measured and decided handheld (2026-10-12); final build checked handheld, docked and with 60 fps
interpolation (2026-10-13).

## Cause

Two clocks that nothing ties together:

- The game paces its frames on gx2_core's virtual vsync, a steady clock at 59.94 Hz. With relaxed vsync (the Switch
  default) a frame flips 33.4 ms after the previous one.
- The deko3d swapchain presented with swap interval 1: a frame reaches the screen at the first real vblank after the
  GPU finishes it. The panel runs at **60.000 Hz** (measured).

The phase between the two drifts through a whole refresh every ~15 s. While the GPU finishes frames near a vblank
edge, a few ms of variation in frame time move them across it, and frames are shown for 1 and 3 refreshes instead of
2. That 1/3 alternation is the judder.

## Measurement (kept, `[dk] pacing` in the 5 s stats line)

`gfx/deko/frame_pacing.cpp`: a thread waits on the display's vsync event (`viGetDisplayVsyncEvent`) and reads, at
each vblank, the number of the last frame whose present pass the GPU has finished (the GPU writes it,
`dkCmdBufReportValue` at the end of the present pass). Frames reach the screen in order (FIFO), each at the first
vblank after the GPU finished it and `swap interval` vblanks after the previous one; the line reports:

- the display's measured rate and vblank gaps;
- how many frames were on screen for 1 / 2 / 3 / 4+ vblanks, how many the swap interval held back, and "hold
  changes" (a frame held for a different number of vblanks than the one before it: the judder);
- how long before its vblank the GPU finished each frame (min/avg/max; the GPU's timer and the CPU's counter share
  their origin, so this is real time);
- a histogram of the intervals between presents (<12, 12-22, 22-30, 30-37, 37-45, 45-58, 58+ ms), mean and sd.

Cost: one wake-up of a few microseconds per vblank, one GPU report per frame, one log line every 5 s.

The `"Default"` display is already held by libnx's default window (opening it again fails, rc 0x1272); the thread
falls back to `"Internal"` (the handheld panel), then `"External"`.

## Results (console, handheld, stock clocks, 720p, Outset Island, turning the camera)

Per 5 s window (~147 frames); ranges over 6-7 windows per mode, 2 interleaved rounds per run, two runs (the B rows
from the second run only, after B's late-frame fix). The widest mode 0 window (85 / 43) was a heavier stretch where
the game ran at ~28 fps:

| Mode | Frames on screen for 1 / 2 / 3 vblanks | Hold changes | Held back by the interval |
|---|---|---|---|
| 0: swap interval 1 (before) | **0-9** / 85-148 / 2-43 | 4-88 (usually ~30) | 0 |
| 1: swap interval 2 (A) | **0** / 128-150 / 0-15 | 0-28 (usually ~15) | 0-75 |
| B: virtual vsync on the vblanks | 4-52 / 47-133 / 10-52 | 30-151 | 0 |
| A + B | **0** / 138-145 / 4-8 | 7-17 | 6-93 |

Present intervals: mean 33.4-34.4 ms in every mode. The 37-45 ms intervals (10-20 a window) are frames the game took
longer than 33 ms for; they are the remaining 3-vblank frames and are the same in every mode.

## Decision

- **A, by default**: the swapchain presents with the game's swap interval, 2 at 30 fps and 1 with frame interpolation
  or true 60 (`interp::effective_swap_interval`, `interp::vsync_rate`). No frame is shown for a single vblank any
  more; the cost is up to one vblank of latency for a frame that finished early. `WWHD_DK_PACING=0` or the debug
  command `pacing 0` gives back swap interval 1 for comparisons.
- The game's own interval is not read from gx2_core (an upstream file, unchanged on this branch): Wind Waker HD sets
  2 in every log so far (530 of 530 `[gx2] ... swap interval` lines, title and gameplay), so mode 1 uses 2.
- **B dropped** (reverted): locking the virtual vsync to the vblanks without controlling the phase did not help and can
  lock into the worst phase (52/47/52 in one window). Its first version also made a late frame wait a whole vblank.
  It needed hooks in gx2_core.cpp; none are left.

## Final build on the console (2026-10-13, `wwhd_fp_final.nro`, 698974c)

Same spot (Outset, the pier by Link's house), modes 1 / 0 interleaved twice, 30 s of camera turning each:

| | Mode | 1 / 2 / 3 vblanks per 5 s | Hold changes | 37-45 ms presents |
|---|---|---|---|---|
| Handheld (720p) | 0 | 0-9 / 116-133 / 12-20 | 23-48 | 19-30 |
| Handheld (720p) | 1 | **0** / 133-146 / 3-12 | 6-24 | 6-27 |
| Docked (1080p) | 0 | 0-9 / 113-134 / 10-23 | 23-53 | 14-38 |
| Docked (1080p) | 1 | **0** / 118-130 / 14-22 | 27-45 | 27-58 |

- Boot: `pacing mode 1`, "Default" refused (rc 0x1272), "Internal" vsync event, `[dk] frame 1: swap interval 2`.
- Docked: the operation mode change recreates the swapchain at 1920x1080 and the next present sets the interval
  again. The "Internal" vsync event keeps ticking docked (60.00 Hz, no missed wakes) and the statistics stay
  consistent, but whether it follows the TV's own vblank is not proven.
- 60 fps interpolation (Switch tab > Debug > Frame rate (test), not saved): `[dk] frame N: swap interval 1` at once,
  and `swap interval 2` again back at 30 fps.

## Pending

- Whether the "Internal" vsync event is the TV's vblank when docked (only the statistics depend on it).
