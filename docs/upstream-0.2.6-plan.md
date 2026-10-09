# Upstream v0.2.3–v0.2.6: what the Switch takes, and the plan for the rest

Upstream (ZeldaWWHDRecomp) released v0.2.3–v0.2.6 on 2026-10-06/07 and rewrote its history (devel and
main force-pushed, v0.1.0 and v0.2.2 retagged). There is no common ancestor any more: dev took the
diff from our old base `c3fb7ce` to `upstream/devel` `4da1e34` (commit 7589856). **The next sync
starts from `4da1e34`** (`git diff 4da1e34 upstream/devel | git apply -3`).

2026-10-08: v0.2.7-v0.2.8 (`main` 853d7b1) taken the same way; **the next sync starts from `853d7b1`**
(docs/switch-port.md, "Round 46").

2026-10-09: v0.2.9-v0.2.11 (`main` defb89f) taken the same way; **the next sync starts from `defb89f`**
(docs/switch-port.md, "Round 48").

## Taken as is (common code, now in dev)

| Upstream change | On the Switch |
|---|---|
| Mod manager update (content mods, Cemu graphics packs, profiles) | Mods tab; still no file picker, native mods still refused |
| Render-thread profiler (`render_prof`) | Common hooks in gx2_core run; thread CPU time from `svcGetInfo` ThreadTickCount |
| Performance report header (build, system) | OS line from `hosversionGet` (`Switch HOS x.y.z (Atmosphere)`) |
| GamePad audio follows the game's output mode (#49) | Common code; to check on hardware |
| Gyro aiming (#45) | Fed from the libnx six-axis sensors (commit 0921610), Switch tab > Gyro aiming |
| Tick-based flips (120/240 fps) | Relaxed vsync rebased onto them; 30/60 behaviour unchanged by design |

## Not applicable as code, worth copying as ideas

Vulkan-only: the 15 "validated CPU paths" (deko3d already has its own memo/state caches), lazy
GX2DrawDone and async present (deko3d already only submits on DrawDone and presents without
waiting), "never read mapped upload memory" (deko3d writes its uncached blocks only). Nothing to do
for those.

### Step 1 — narrow shader keys in deko3d (from v0.2.6) — highest value

Upstream: Outset from an empty cache went from ~6,100–6,700 translations to 569, ~100 MB less
memory, a third fewer >50 ms frames, 4–12% less render-thread time.

Now: `shaders_dk.cpp` `translate` keys on `coreHash ^ program_hash ^ fsKey ^ texture_state_hash`,
where `gather_state` is the old wide GL key, which also hashes the render-target formats
(`CB_COLOR*_INFO`), `CB_TARGET_MASK`, `CB_COLOR_CONTROL`, `DB_DEPTH_CONTROL & 0x83`,
`CB_SHADER_CONTROL` and `DB_SHADER_CONTROL`. Each new combination costs a CPU re-translation and a
`Shader` entry; the DKSH itself is already shared by GLSL hash (`code_for`).

Plan:
1. Port upstream's `gather_linkage` / `variant_hash` rules (vulkan/shaders.cpp, docs/vulkan.md
   "shader keys") to `gather_state`, keeping in the key only what the Cemu decompiler reads in GL
   mode (check each dropped register against `LatteDecompiler*` sources: some of the CB/DB registers
   may matter for GL output, e.g. integer render targets).
2. `WWHD_DK_SHADER_KEY_VERIFY=1`: translate with both keys and log any narrow-key collision whose
   GLSL differs (upstream's `WWHD_VK_SHADER_KEY_VERIFY`), run the warp tour on desktop headless.
3. Re-key the offline caches (`tools/switch/dksh_cache`, `merge_shadercache.py`); the GL cache
   compatibility can go (deko3d is the only Switch renderer).
4. Measure on hardware: translations and `Shader` entries after Outset and Windfall, heap, hitches.
   A/B switch `WWHD_DK_NARROW_KEYS=0`.

### Step 2 — profiler hooks and a performance report on the Switch

deko3d calls none of the backend `rprof::` hooks, and the report button is in the desktop Graphics
tab (`#ifndef __SWITCH__`); `hostui::set_clipboard` has no Switch version.

Plan: `rprof::mark` phases in `gfx/deko/draw.cpp`, `add_upload` in `memory.cpp` `stream_upload`
(with `UploadKind`), `add_wait` around `dkFenceWait` in `frame_begin` and `dkQueueAcquireImage`,
`shader_variant` in `translate`; renderer label "deko3d" in the report header; a "Save performance
report" button in the Switch tab writing `sdmc:/switch/wwhd/logs/perf_<time>.txt`. This also gives
step 3 its numbers (unique bytes uploaded per frame).

**Done 2026-10-09 (built, not run on the console yet):** phase marks in `gfx/deko/draw.cpp` `draw_impl`
(shader, indices, targets, textures+uniforms, pass, descriptors, pipeline state, vertex, record),
`add_upload` in `stream_upload` with `UploadKind` at the uniform-block, index and vertex call sites and
`guest_read` in `stream_guest` (unique bytes), `add_wait` around the frame fence (`kWaitGpu`) and
`dkQueueAcquireImage` (`kWaitAcquire`); Switch tab > Debug > "Save performance report" writes the header
(renderer "deko3d") and `rprof::latest_report()` to `sdmc:/switch/wwhd/logs/perf_<date>_<time>.txt`. The
profiler stays off by default (A/B switch "Render profiler (slower)"). Not wired: `shader_variant` (its
groups are the Vulkan translator's).

**Run on the console 2026-10-09** (NRO with patches 1-8, handheld, scale 1.0, 30 fps, profiler on, the
Forsaken Fortress area, `logs/perf_2026-10-09_09-36-23.txt`): render thread CPU 14.9 ms/frame and idle 17.9
ms/frame (it waits for the game); 1492 draws/frame at 7.15 us/draw. Draw phases ms/frame: textures 2.79,
shader 1.75, vertex 1.61, indices 1.35, targets 0.93, pass 0.65, descriptors 0.53, pipeline 0.48, record
0.15. Register writes 4.30 ms/frame (11433). Uploads 2.02 MiB/frame (vertex 1.72, index 0.26, ubo 0.04);
unique guest bytes 1.63 MiB/frame (vertex 1.60 of 1.72: 93% of the vertex data is new each frame). The
game's main thread holds core 1 at 93-98%: that is the frame's limit, not the render thread. The report
button and its file work (without the profiler it writes a note saying to turn it on).

### Step 3 — cross-frame buffer reuse (from v0.2.4's guest buffer cache) — only if step 2 says so

Upstream keeps unchanged vertex/index/uniform data on the GPU, validated by write_watch page stamps
(6–17% less render-thread time on macOS). On the Switch write_watch is off (no page protection or
signals on Horizon), so the cache cannot be validated the same way; hints alone
(DCFlush/GX2Invalidate) are not safe.

Today deko3d copies guest data into the per-frame stream ring for every draw, deduplicated only
within a frame (`stream_guest`) plus `UboMemo`. Options, decided with step 2's upload numbers:
- content check: hash (xxh3) or compare against a heap copy, keep a GPU copy when unchanged; pays
  off only if the hash is clearly cheaper than the copy to uncached memory;
- static ranges: data the game never rewrites (model vertex data loaded once) detected by a few
  frames of equal hashes, then checked only every N frames (with `_VERIFY` mode as upstream).
Behind `WWHD_DK_BUFFER_CACHE`, off until measured.

**Decision 2026-10-09: not now.** Step 2's numbers: 93% of the vertex bytes a frame uploads are unique
within it, and the whole vertex phase is 1.61 ms of the render thread's 14.9; a content check would hash
those 1.6 MiB every frame (no write_watch on Horizon), so the best case saves about 1 ms of a thread that
already idles 17.9 ms/frame while the game's main thread is the limit. The larger render-thread costs are
textures (2.79 ms/frame) and shaders (1.75; step 1). Revisit only if the render thread becomes the limit
(docked, higher scales).

### Step 4 — small fixes found while comparing

- deko3d `wait_idle` ignores the save-state "full GPU wait" request (`gx2_ss_drain`, payload 1): wait
  on the queue there. **Done 2026-10-09:** backend hook `gpu_idle` (optional; deko3d: submit, then
  `dkQueueWaitIdle`), called by `OP_DRAW_DONE` when its payload asks for the full wait. Save and load
  checked on the console 2026-10-09 (slot 1 in the Forsaken Fortress, back at distance 0.0).

## Hardware checks for this sync (NRO from dev 0921610)

Boot to Outset and a scene change; fps vs the previous build (relaxed vsync on the new tick flips);
Mods tab; sound; gyro with `WWHD_PRO_CONTROLLER=0`: enable it in the Switch tab, aim the bow, check
directions and the log line `[gyro] first sample` (if an axis is reversed, fix the conversion in
`input_switch.cpp` `gyro_sample`, not with the invert options).
