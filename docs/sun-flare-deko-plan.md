# Sun corona and lens flare on the Switch (issue #94)

## Why they are missing

The game decides whether to draw the sun's corona and lens flare from depth peeks: every step
`dKyr_sun_move` asks for the depth at five points around the sun (`dDlst_peekZ_c::newData`), and
`peekData`, called once a frame after the 3D scene, answers them. A point counts as visible when its
depth is 0xFFFFFF (nothing drawn there). `runtime/src/peekz.cpp` hooks `peekData` and sends the points to
the render thread (`OP_PEEK_Z`); the backend copies those pixels of the main depth buffer and writes the
answers into guest memory when the GPU is done (`gfx::depth_peek::publish`). gfx/vulkan and Metal do; the
deko3d backend had no `peek_z`, so `render::can_peek_z()` was false, the game's own function ran, found no
depth in guest memory and the sun counted as hidden.

## What changes

`runtime/src/gfx/deko/depth_peek_dk.cpp` (fork file) and the backend table entry (`backend.cpp`, `dk.h`):

- At `OP_PEEK_Z` (after the 3D scene, in command order): the newest 1280x720 depth surface (`S.list`,
  highest `writeSeq`); each point mapped from 640x480 to the image's own pixels (`img.pw`/`img.ph`: the
  internal and dynamic resolution scale the whole buffer); one `dkCmdBufCopyImageToBuffer` per point (copy
  engine) into a slot of a small CPU- and GPU-uncached memory block (3 slots x 64 points x 256 bytes).
- Before the copies, the transfer barrier (`sync_transfer_begin`); after them one Full barrier with the
  image and L2 invalidates (the copies reach memory before the CPU reads them, and finish before later draws
  write the depth buffer).
- The slot is answered at a later peek, once `frame_done` says the GPU finished its frame (never waiting);
  no free slot (two in flight): that frame is skipped, the game asks again.
- Depth formats: ZF32_X24S8 (the guest's D24S8), ZF32, Z16, Z24S8; the float -> 24-bit value as the other
  backends (`depth_peek::from_float`).

The method (copy engine, slots in flight, the barrier before the CPU reads) is SwitchWaker #7's
(`switch/deko/aurora/dk_encode.cpp`). SwitchWaker copies an area with a margin because `GXPeekZ` answers at
once on the game thread; here the game accepts the answer a frame later, so only the peeked pixels are copied.

`WWHD_PEEKZ=0` keeps the game's own function (A/B), `WWHD_PEEKZ_LOG=1` logs requests and answers.

## Checks

1. Build (tools/switch/build.sh); the log's "depth peek: first copy" line names the buffer and its size.
2. Console, at sea: corona and flare at day and in the evening; hidden when an island or the sail covers
   the sun; docked (1.5x) and handheld; with dynamic resolution lowering the scale.
3. `WWHD_PEEKZ_LOG=1`: answers 0xFFFFFF for open sky, lower values behind geometry.
4. Performance: the perf test (G1) against main, no frame-time cost beyond noise (two Full barriers a frame).

Results (2026-10-28, handheld): 1 OK (`first copy, 5 points of the depth buffer F500E000 (1280x720 pixels,
format 43)`, ZF32_X24S8); 2 OK at sea, afternoon (time held at 200) and evening, corona and glare shown at 30 fps,
gone behind an island or the sail; docked OK. 4 not run (maintainers' call: a few 1-pixel copies and two barriers
a frame, only while the sun is on screen).
