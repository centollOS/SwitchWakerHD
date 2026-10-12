# Texture eviction: the image heap stops growing with every new place (plan)

Branch `feature/texture-eviction`. Issue #11. Status: console-tested (2026-10-11), no measurable performance cost.

## Why

The deko3d renderer never frees a surface. `find_or_create_surface` (gfx/deko/surfaces.cpp) adds one for every new
texture description and nothing removes it, so every place visited stays in the image heap for the rest of the
session. When a new 64 MiB image heap chunk no longer fits in the system heap, `dkMemBlockCreate` fails and deko3d
ends the process (HOME with "an error occurred", no crash report). Issue #11's logs:

- long play session: surfaces 17 → 2,611 (538 MiB), image heap 10 chunks, crash after 2 h 45 min;
- Prepare graphics on `main`: crash after ~9 minutes (7th place), because background shaders already take ~180 MiB of
  the same system heap;
- image descriptors grow the same way (3,556 of 8,192 at the end of the long session).

## What can be freed

A surface the GPU never wrote (`!gpuWritten`) and that was never a render target holds exactly what
`upload_surface` copied from guest memory. Its image can be dropped and made again from guest memory the next time
a draw samples it (the data is still there, or the lookup finds new data anyway). Render targets and anything the
GPU wrote (draws, clears, copies, scan copies, feedback/3D copies) hold contents that exist only in the image: never
evicted.

The `Surface` record itself stays (in `S.byAddr`, `S.list`): pointers to it live in the draw caches, the capture
notes, the GPU-ordering table, `derivedFrom`, GuestRanges. Only its image, views and descriptors go. That keeps the
change local: no pointer anywhere can dangle.

## How

1. **Last use.** `Surface::useFrame`: the recording frame (`R.frame + 1`) of the last draw that sampled it
   (prepare_stage, cache hits included), of its lookup and of its creation.
2. **Evict** (`evict_surface`, surfaces.cpp): free the image's descriptors (`image_descriptor_free_later`) and its
   memory, mark the surface `evicted`, bump `R.surfaceEpoch` and `R.textureEpoch` so every cached lookup (the draw
   path's texture caches and table) is redone. Candidates: image valid, not GPU-written, never a render target,
   scale 1, no twin, no writeback pending, not the TV source, and `frame_retired(useFrame)` (the GPU is done with
   every frame that used it). Because of that last condition the memory goes back to the heap at once
   (`image_free_now`, memory.cpp), not after a frame fence.
3. **Restore.** `find_or_create_surface` returns an evicted surface only after `revive`: the image made again at the
   same description, `dirty` so `upload_surface` reads guest memory in full (contentHash/sparseHash reset), and
   uploaded right there (a texture looked up as a render target or a copy's destination keeps its old contents too).
4. **Under memory pressure.** `image_alloc` (memory.cpp) asks `image_heap_pressure(bytes)` (surfaces.cpp) before it
   adds a chunk: the least recently used candidates idle for at least `WWHD_DK_TEX_PRESSURE_FRAMES` (default 150,
   ~5 s) are evicted, oldest first, until about twice the request was freed; then the allocation is tried again. A
   chunk is added only when nothing (more) can be evicted: the heap grows only with the working set.
5. **When idle.** Every 300 frames, textures not used for `WWHD_DK_TEX_IDLE_FRAMES` (default 9000, ~5 min at 30 fps;
   0 = off) are evicted, so the heap and the descriptor table stay tidy before any pressure.
6. **Stats.** The periodic `[dk] surfaces:` line adds evictions (idle / under pressure, MiB), restores and the count
   of surfaces currently evicted.

Not in this change (follow-ups if the console says so): render targets of places left long ago (their contents
would have to be kept or proven redrawn), freeing empty chunks, the rescale pool under pressure (already capped at
64 MiB and 600 frames).

## Tests

- Build: `tools/switch` NRO (USA).
- Stress the restore path: `WWHD_DK_TEX_IDLE_FRAMES=60` (evict anything unused for 2 s): warp around (Outset,
  Windfall, Dragon Roost, back), menus, Picto Box, sea; textures must look the same, no new errors in the log.
- The issue's reproduction: Prepare graphics from Outset, docked, dynamic resolution on. Before: HOME after ~9 min.
  After: the sweep runs past Forest Haven for 20+ minutes; the `image heap` chunk count levels off and `heap never
  used` stops falling with every place.
- Perf: `bench` (perf/benchmark) lava / windfall against main, the evictions must not cost a frame in a settled
  scene.

## Results (2026-10-11, console, European game, 8cafb51)

- Restore path (`evict idle 60`, debug command): Windfall, Outset, Dragon Roost Cavern, Windfall again: 312 textures
  out at once, 189 restored for the second Windfall; screenshots as before, no errors in the log, image heap 3 chunks.
- Prepare graphics, docked, internal resolution 1.50 with dynamic resolution, the player's Quest Log loaded:
  - places 27 → 63 in 6 min: image heap 4 chunks (224-247 MiB), heap never used 719 → 702 MiB; ended by the game's
    own assert on M2tower → M_DaiB (c_xyz.cpp:285, prepare-graphics-plan.md), not the renderer
  - places 75 → 127 ("complete: 53 places in 7 min, 0 skipped"): 5 chunks from place ~80 to the end (one chunk added
    in the whole run after start-up), 26 evictions under memory pressure instead of new chunks, up to 855 textures
    out at once, heap never used 687 → 608 MiB; no errors
  - before (issue #11): HOME after ~7 places, heap never used falling with every place.
- Prepare graphics restarted from the title (no Link loaded) visits one place per restart: a Prepare graphics problem,
  not this change.


## Performance (2026-10-11, console, USA game, docked, internal resolution 1.50)

`perf_test.py ab` main (514be0d) vs this branch, both with the bench commits, ABBA (2 boots each), 600 frames per
scene, Quest Log 2:

| Scene | main | eviction | difference (95% CI) |
|---|---|---|---|
| lava (Dragon Roost Cavern) | 27.43 fps | 27.24 fps | -0.19 (±0.86) |
| windfall | 27.12 fps | 26.97 fps | -0.14 (±0.64) |

Within the boot-to-boot spread (the second pair of boots was ~0.5 fps slower for both builds): no measurable cost.
