# Picto Box: pictures stay black once one was taken with a shader pending

## Report

- User, 2026-10-15 (main + first-start memory build, empty shader caches, Windfall right after the first start):
  the first pictures came out black, and the later ones too, even with the shader queue empty and no draw skipped;
  going back to the title and loading the save again fixed it.
- #37 (LFranco1213, v0.4.7, docked, all shaders prepared): pictographs black, selfies fine. In the attached log
  only the first picture's 5 s window has new shaders (8 compiled, 11 draws skipped); the second and third have
  none, and they are black too.

So the first picture meets a pending shader, and something made then stays black for the session: not the
moment of the picture alone (each later picture draws again).

## Steps

1. **Trace the picture's draws.** `WWHD_DK_TRACE_TARGET=<hex address>,...` ([dev]): every draw into a color target
   at those addresses gets the frame trace's draw lines (shaders, state, sampled textures, `/cpu` for CPU data),
   and a draw skipped there for a pending or failed shader is logged. Targets: `3E9D0000` (800x450, read by the
   CPU), `20974000` (1600x900, the picture pass), `221CC9CC` (800x450, tile 16).
2. **Console repro:** empty caches, the first-start card closed with B, a save in Windfall, a picture at once
   (black), another after the queue is empty (black?), back to the title and the save again, a picture (good).
   The trace of a black and a good picture says which texture or target differs.
3. **Fix** by what step 2 shows, and the same repro to check it.

## Results

**Step 2 (2026-10-15, console, first starts with empty caches and B on the card, Windfall).** The picture's chain, one
frame: `20974000` (1600x900) from the scene; then the color table `3B783000` (8x8x8, GX2 format 0x816, one draw into
its 8 slices as 8 color targets, vs `44F22500`); `F4000800` (1600x900) from `20974000` through that table (vs
`44F4F800`, t1 = the table); two passes `F4592000` ↔ `F4000800`; `2069E000` (800x450) and its alpha; `3E9D0000`
(800x450, read by the CPU). With every draw of the chain drawn and every shader ready, a later picture of the same
session is black: the captures show `20974000` right and `F4000800` black, and the table is CPU data (never drawn by
the GPU; zeros in guest memory). In a session where the pictures are good the table is drawn in the first picture's
frame and not again (traced: no draw into it at the save's load nor later). So the game draws the table once, at the
first picture; when that draw is skipped for a pending shader, the table stays zero for the session and every
picture multiplied by it is black. A new start draws it again (the shaders then come from the cache).

**Step 3 (2026-10-16, the same repro).** A draw into a 3D color target waits for its pending shaders (`shader_wait`)
instead of being skipped; which targets a draw writes comes from the pixel shader's outputs as the target lookup
finds them (`GetActiveColorBufferMask`): the game leaves the table in color slots 1-7 with their write masks on
after the table's draw, so the registers alone made ~75 later draws wait (17 s). `wait_for` also takes a
background job of that shader while the background is paused. Console: the first picture (other shaders of the
chain still pending) is black and waits ~120 ms once (the table's two shaders); the next picture, the queue empty,
is good (before: black until the game was closed).
