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
