# Spike: GPU optimizations for the deko3d renderer (2026-10-23)

Code read: `origin/main` 113fa63 (`runtime/src/gfx/deko`), then checked against `origin/main` 754ab3b (fetched
2026-10-23). The only renderer change since then is PR #78: zcull on by default, plus the `WWHD_DK_DUAL_ISSUE` test
switch. Data:
- the console log `wwhd_2026-10-23_05-26-32.log`: stock CPU 1020 / GPU 307 / memory 1331 MHz, Outset, built with
  zcull already on (`[dk] zcull: ON (queue)`);
- PR #78's A/B (docs/deko3d-plan.md);
- the round notes in docs/switch-port.md and docs/deko3d-plan.md.

**Constraint (owner):** nothing here may cost CPU. Each item lists its CPU cost. Items that would trade CPU for GPU
time are in "Rejected".

## 1. Where the GPU time goes today

Heaviest Outset views (6,600-7,300 draws, 23-25 fps), `[dk] GPU passes` (4-5 sampled frames each):

| Pass | GPU ms/frame | Draws | Share |
|---|---:|---:|---:|
| Main scene 1280x720, RGB10A2 + D32F (`color 25 depth 2062`) | 14.7-19.4 | 2,700-3,000 | ~40-45% |
| Shadow cascades 1024x1024 D16 (`depth 5`) | 13.8-15.5 | 3,670-4,150 | ~35% |
| FXAA (only when the player turns it on) | 2.1 | - | 5% |
| Full-screen 1280x720 passes (`color 25`, 87 draws) | 1.2-2.2 | 87 | 4% |
| Present | 1.2-2.1 | - | 4% |
| Post-processing at 640x360 / 320x180 / bloom chain | ~2.5-3.5 | ~10 | 7% |
| **GPU busy (without the frame start)** | **38-44** | | |

Other figures from the same windows:
- `render thread busy 993 ms/s`, and `GPU behind at 99-100% of presents`.
- `15 Fragments barriers` (13 of them read-after-write) and **`11 Full barriers` a frame** (5 before and 6 after transfers),
  although the frame has `0 surface copies, 0 feedback copies`.
- `zcull dropped 12, kept 12 binds`.
- 3,246 of ~3,670 shadow draws keep their full pixel shader because of the **alpha test** (only 423 use the empty
  depth-only shader).
- Depth compression is **off** (`WWHD_DK_DEPTH_COMPRESSION`, opt-in). The tiled cache is **off** (`WWHD_DK_TILED_CACHE`,
  opt-in). Color render-target compression is on. Neither opt-in has been tested on hardware.

### Caveat: these timings overstate the GPU's own work

The pass times are timestamp differences, so they include the gaps where the GPU waits for the render thread's next
submit (one every 1024 draws, 8.4 a frame). In the CPU-bound view, the GPU "busy" figure (38.3 ms) follows the CPU
frame time (39.4 ms) almost exactly, and the shadow pass works out to 3.8 µs a draw, close to the render thread's own
4.9 µs a draw. Two pieces of evidence say the true GPU work is lower:
- At CPU 1122-1200 MHz with the GPU at 307 MHz, Outset holds 30 fps (round 42; the owner). So the real GPU work in
  these views is under 33 ms.
- The owner's 0.75 internal-resolution window (log line 1245) is not CPU-saturated (render thread 746 ms/s). There the
  main pass fell from 13.3 to 9.9 ms for the same ~1,490 draws, with 0.56 times the pixels. Solving for it: **about
  7.8 ms of that 13.3 ms main pass scales with pixels**. The other ~5.5 ms is per-draw, vertex or waiting time. So the
  main pass is genuinely partly fill-bound or bandwidth-bound at 307 MHz.

### What GPU savings buy right now

Which thread or unit limits the frame depends on the view:
- **Some views are GPU-bound at 307 MHz.** PR #78 proved it. Zcull alone, a pure GPU change with no CPU cost, took
  Outset's grass from **23.4 to 29.4 fps** (GPU time 41.9 → 34.8 ms, ABBA, 2 boots each). Village and Windfall lost
  0.5-1.5 ms of GPU time. In views like these, every GPU millisecond counts.
- **The densest island views** (pig farm, 6,600-7,300 draws) are limited by the **render thread** (993 ms/s).
- **Sea views** are limited by the **main thread**.

In the render-thread-bound and main-thread-bound views, a GPU-only saving adds about 0 fps by itself. It pays off in
these cases:
1. **When the CPU work lands** (round A2, PGO, write-back elision in docs/memory `cpu-1020-perf-effort`). If the render
   thread gets under 33 ms, the next limit is the GPU at 307 MHz, whose true cost in these views is unknown (under 33
   ms, possibly close to it). GPU headroom decides whether the CPU wins actually show up as 30 fps.
2. **Shared memory bus.** The CPU and GPU share one 64-bit LPDDR4 bus (~21 GB/s at 1331 MHz). Round 41 found that the
   render thread's remaining stage times "look like cache misses". Cutting GPU memory traffic (compression, tiled cache)
   can make the CPU slightly faster for free. This is unproven, but an A/B shows it directly in `us per draw`.
3. **Dynamic resolution** steps down less often, so the picture stays sharper.
4. **Docked mode and FXAA** get room (FXAA costs 2.1 ms a frame).

### Measured since: the GPU profile bench (`perf_test.py gpu-profile`, docs/perf-test.md)

At Outset's tall grass seen from the sea stage (`outset_tallgrass`, ~5,200 draws, CPU 1020 MHz, dynamic resolution
off, 4 boots per profile on console 1; 460 MHz from a second session, against 384 MHz again: 26.69 fps there):

| GPU | fps | Render thread busy, ms | Main thread CPU, ms | GPU behind at present | Cores 1 / 2 busy |
|---|---:|---:|---:|---:|---|
| 307 MHz | 21.96 | 42.5 | 34.7 | 100% | 77 / 76% |
| 384 MHz | 26.70 | 34.0 | 35.2 | 100% | 94 / 92% |
| 460 MHz | 27.23 | 33.2 | 35.4 | 0% | 96 / 95% |

What this shows:
- **At 307 MHz this view is GPU-bound.** 384 MHz gives +4.74 fps (±0.22, +21.6%).
- **The render thread's "busy" time includes waiting for the GPU**, so it is not proof of a CPU limit. Read it
  together with the cores' load.
- **At 460 MHz the GPU is ahead** ("behind" 0%) and the main thread (35 ms CPU a frame) is the limit, so the step
  from 384 adds only +0.54 fps (±0.40, +2.0%).
- **Section 1's "0 fps today" is too pessimistic for views like this one.** Here every GPU millisecond saved is worth
  frames until the GPU frame fits in ~37 ms, which is the main thread's frame at 1020 MHz.

## 2. Candidates, ranked by value per risk

| # | Change | CPU cost | Expected GPU gain at 307 MHz (heavy Outset) | Risk | Effort |
|---|---|---|---|---|---|
| M1 | **Measure the true GPU time first**: a Debug-tab toggle that, on the sampled frames only (every 30th), records the frame's draws without mid-frame submits, then submits once. Pass times then exclude CPU starvation | none in play (off by default; test frames only) | - (it decides the rest) | none | small |
| G1 | **Depth compression on** (`WWHD_DK_DEPTH_COMPRESSION=1`, already written). Main D32F 1280x720 and 3 D16 shadow layers: compressed depth traffic, near-free depth clears | **0** | **0.5-2 ms** (part of the main pass's ~7.8 ms of pixel work plus the shadow writes) | medium: copy-engine paths (uploads and captures of depth) broke this on nouveau (round 26). Check shore foam, DoF and fog composites, depth captures | flag + menu toggle |
| G2 | **Tiled cache on** (`WWHD_DK_TILED_CACHE=1`, already written). Maxwell binning keeps ROP traffic in L2 for the main pass | ~0 (one flush command per barrier, ~17 a frame) | **0.5-2 ms** (bandwidth on the main pass; can also regress, so it needs an A/B) | medium: blocks, or passes that miss earlier ones' results. A `WWHD_DK_TILE_SIZE` sweep (128, 256) | flag + menu toggle |
| G3 | **Alpha-only pixel shaders for shadow casters.** For a depth-only draw whose PS has the alpha test, a variant of the GLSL that writes no color, keeping only the alpha expression and `discard`. uam's dead-code removal drops the RGB lighting and toon math, and texture fetches that feed only RGB | **0 or negative**: the variant reads fewer uniforms and textures, so `prepare_stage` binds and packs less (the ufBlock is 0.88 µs a draw today, on 3,246 draws a frame) | **1-4 ms** if the shadow pass is pixel-bound; less if it's per-draw-bound (M1 tells which) | low-medium: a new variant per caster PS, which goes into the offline DKSH cache and translation records (no console compiles in play) | medium |
| G4 | **Find the 11 Full barriers a frame.** A `Full` makes the host wait for every engine and stops command prefetch. With 0 surface or feedback copies, the 5 transfers a frame are probably rescales or scan or GamePad copies. Trace one frame (`g_traceFrame`), then remove the ones that need no ordering, or batch them as uploads already are | 0 (fewer commands) | 0.1-0.5 ms (11 pipeline drains) | low | small |
| G5 | **Zcull: 12 drops a frame.** Check which drops are avoidable (cascade layer switches, `R.zcullEpoch` bumps from transfers or new images). Each avoidable drop costs early rejection for the draws after it | 0 | 0-1 ms (uncertain) | low | small |
| G7 | **Dual issue back on** (`WWHD_DK_DUAL_ISSUE=1`, PR #78: a start-time test that undoes uam patch 7 and uses its own cache id). Maxwell issues two instructions per clock where the scheduler pairs them; patch 7 turned that off to fix the grass specks, which may really have been zcull (#72 D3) | **0** (one-time recompile into a separate cache; it needs its own offline `dksh_cache` build so the console doesn't compile in play) | 0-10% of ALU-bound pass time (main pass lighting, post-processing); unknown until measured | medium: the specks may come back. Same grass check as PR #78 | flag exists; offline cache |
| G6 | **Present pass at 1.2-2.1 ms** is ~10 times what one full-screen RGB10A2 sample should cost at 307 MHz (~0.2 ms). It probably includes waiting (the barrier before it, or the swapchain image). If M1 shows it's real, compile the identity-grade case without the branches and `pow` | 0 | 0-1.5 ms | low | small |

**Realistic total: about 2-5 ms of GPU time a frame** in the heavy Outset views (roughly 5-13% of the measured 38 ms;
a larger share of the true, unknown figure). G1 and G2 are independent, so their gains roughly add up. G3 is the
largest single item and the only one that might also return CPU time.

In fps:
- **GPU-bound views** (Outset's grass, some village views): up to roughly 1.5-4 fps. PR #78's 7 ms gave 6 fps; 2-5 ms
  more is worth about 2-4 fps where the GPU is still the limit after zcull (34.8 ms there is still above 33.3).
- **Pig farm:** about +0 fps (render-thread bound); perhaps a few tenths through the memory bus (case 2).
- **After the CPU rounds** (render thread under 33 ms): this is the margin that keeps 30 fps from becoming 27-28 fps
  with the GPU as the new limit, and it keeps dynamic resolution at 1.00.
- FXAA becomes affordable in more places.

## 3. Rejected (they cost CPU, or quality)

- **More frequent submits** to feed the GPU sooner: each submit costs ~51 µs of render thread (round 40). The move to
  1024 draws per submit was chosen for that reason.
- **GPU-side culling, occlusion queries, or reordering draws by state**: these need CPU work per draw, on the
  bottleneck thread.
- **Lower internal resolution or fewer shadow cascades by default**: the GPU win is real (see the 0.75 window), but it
  costs picture quality. It stays the player's choice in the menu, as now.
- **SMAA**: several passes (docs/switch-port.md).

## 4. Suggested order

1. **M1**, plus a Debug-tab A/B line for G1 and G2. AGENTS.md says test options go in the menu; both are env-only today.
   This is one console session (lock owner's OK, `tools/switch/perf_test.py` toggle).
2. **G1 and G2** A/B at the pig farm. Watch `GPU busy`, the main and shadow pass times, **and the render thread's
   `us per draw`** (the memory-bus effect). Keep each only with hardware evidence (the team's rule).
3. **G7** (dual issue): A/B with its own offline cache, plus the grass specks check.
4. **G4, G5, G6** from one traced frame. These are cheap.
5. **G3**, if M1 shows the shadow pass is pixel-bound. Otherwise keep it only for its possible CPU saving in
   `prepare_stage`.

Estimates in earlier round notes were often too optimistic (Retrospective, rounds 39-45). Treat the ranges above as
upper bounds until the console measures them.
