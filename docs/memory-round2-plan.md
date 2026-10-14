# Memory, round 2: the image heap and the shader variants

Round 1 (docs/shader-variant-memory-plan.md, v0.4.7) fixed what ended #37's 4-hour session: exact variant keys
took a 40-warp tour from 21,922 variants to 5,158, and the translation records from 64 MiB to 3.7 MiB. Its steps
3-5 (smaller variants, cache files, long test) were left for this round. The tour log of the exact-key build
(2026-10-14, 17 min, 40 warps) says what is left, and it reorders them.

## What still grows (exact-key tour, main 51ba285 + [mem])

| Growth | Amount | Cause |
|---|---|---|
| image heap chunks | 1 → 5 chunks of 64 MiB (+256 MiB) in 17 min | new places' textures; the last chunk came from **fragmentation** |
| shader variants | +37 MiB for +3,450 variants (~11 KB each) | `Shader` 1,152 B + `dec` 920 B + the uniform cache (`data`, up to a few KB) + lists |
| programs, surfaces structs, translation records | a few MiB | small |

The last chunk (log line 4643): the heap had 4 chunks (256 MiB) holding ~190 MiB of surfaces, evicted every
texture unused for 150 frames (160 of them), and still found no 3.5 MiB range: a new chunk. Placement is first fit
over the chunks in order, with small and big images mixed, so freed holes are seldom big enough for a render target.
Nothing bounds the number of chunks: a long session over many places can keep adding them until malloc fails.

What round 1's plan listed that turns out small now:
- `storedTrans`/`storedBlob`: 3.7 MiB after the version-2 restart (step 4's index is not worth it now).
- The cache files read whole at boot: a peak at start-up, freed before the game runs (not a long-session cost).

## Steps

1. **Image heap counters and trace.** The [mem] line gains the image heap: chunks, bytes in use, free bytes, the
   largest free range. `WWHD_DK_IMAGE_TRACE=1` ([dev]) appends every image alloc/free, eviction pass and new chunk
   to `image_trace.bin` (written in batches with the 5 s stats, the file closed between them so the debug server
   can fetch it).
2. **Placement policy, chosen offline.** `tools/switch/image_heap_sim.py` replays a trace with other policies
   (first fit as now, best fit, big images from the top of a chunk and small ones from the bottom) and reports the
   chunks each needs. The best one goes into memory.cpp; one console tour checks the chunk count.
3. **A bound on chunks.** Before a new chunk past N (sized from the trace), evict harder (textures unused for fewer
   frames; they come back from guest memory when drawn) instead of growing.
4. **Shader variants: eviction.** Variants not drawn for a long time (LRU by frame, at a frame start only, the
   memos invalidated through `R.shaderEpoch`) are freed; a later draw rebuilds them from their translation record
   (no decompiler). Slimmer `dec` only if the eviction alone does not bound them.
5. **Long test on the console** (2–3 h tour, `[mem]` slope per hour), kept as a release check
   (`tools/switch/mem_tour.py`).

Surface structs and side copies (feedback/volume/mip chains, `g_wbSized`) stay for later: a few MiB in the tour.

## Results

**Step 1-2 (2026-10-14, trace tour: main 68d7226 + step 1, 40 warps, 17 min).** The image heap reached 4 chunks
at frame 4626 (~2.5 min) and stayed there for the remaining ~15 min (233 MiB placed at the end, peak 255 of 256).
`image_heap_sim.py` on the trace: first fit, best fit, split and split-best all need the same 4 chunks (split
even needs the 4th earlier). The chunks follow the working set (surfaces ~95-200 MiB, the rescale pool 24 MiB,
side copies), not a placement problem: **step 2 dropped**, first fit stays. Step 3 (a chunk bound) waits for the
long test: no growth past the working set in 17 min.

What still grows after the 4th chunk: malloc +33 MiB in ~14 min with +2,780 variants (~12 KB each): step 4.

**Step 4 (2026-10-14, same tour, variants idle 3600 frames freed).** Variants stay at 1,000-1,800 (5,447 freed;
5,152 kept without it), no crash, the 14 screenshots fine. But malloc still grows +24 MiB after the 4th chunk
(+33 MiB without it): the variants were ~9 MiB of it, not ~12 KB each as estimated from the slope. The rest grows
with something else (programs 32k, surface structs 1.4k, the records +2.4 MiB account for some): more counters next.
