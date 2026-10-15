# Memory after the first start's shader preparation

## Report (2026-10-15, console, main 295f1e8 + the kModVariables fix)

With empty shader caches (settings.ini `[dev] WWHD_DK_SHADER_CACHE_DIR` = an empty folder, as a new player), the
first-start card ("Preparing the graphics", `background_shaders_switch.cpp`) translated 25,505 variants in 1,218 s.
Afterwards:

| | cached session (long test) | after the preparation |
|---|---|---|
| malloc in use / taken | ~2,465 / 2,506 MiB | ~2,760 / 3,187 MiB (peak 3,093 in use) |
| heap never used | ~730 MiB | **47 MiB** |
| GLSL sources (`codes`) | ~11,000 | 15,257 |

A Picto Box photo then needed image heap chunk 4 (64 MiB): `add_chunk` → `make_block` → `dkMemBlockCreate` →
`Device::allocMem` failed → `RaiseError` → `diagAbortWithResult` (Atmosphère crash report, User Break; the game's
log has no `[crash]` line). Every new player's first session can end this way, whether they wait for the card or
press B.

Two problems: (1) the preparation leaves ~300 MiB more in use than a cached session, and malloc keeps ~430 MiB
taken but free (3,187 − 2,760) that a 64 MiB block cannot use; (2) a chunk that does not fit aborts the game.

## Steps

1. **Census.** The malloc census build (`WWHD_HEAP_CENSUS=ON`, memory round 2) with empty caches: `heap mark` at the
   title, `heap top` / `heap diff` once the card is done. It names the call sites holding the extra ~300 MiB.
2. **Free what the preparation keeps.** By the census: whatever is only needed while translating (program lists,
   sources already written to the cache files, the decompiler's leftovers) is released when the card is done.
3. **No abort on a chunk that does not fit.** `add_chunk` checks the allocation first (`aligned_alloc` of the chunk's
   memory, then `dkMemBlockCreate` on it), and when it fails evicts every texture not drawn in this frame and tries
   again; a draw whose texture still cannot be placed is skipped (logged) instead of ending the game.
4. **Console check:** the same first start, then the 40-warp tour for one lap: no crash, heap never used well above
   one chunk.

## Results

**Step 1, census (2026-10-15, main + kModVariables, census build, empty caches).** Once the card was done the extra
memory was the preparation's own: the recorded variants (`background_shaders_switch.cpp`, ~228 MiB of registers in
156,715 vectors, + 18 MiB of records) kept through the translation, and uam's per-compile leftovers over ~7,800
compiles (the linked shaders' Mesa IR ~98 MiB, `ralloc_asprintf_rewrite_tail` 13 MiB, parameter lists ~11 MiB,
nv50's binary 9 MiB).

**Step 2 (2026-10-15, first starts with empty caches, normal builds unless noted).** Two fixes were tried: uam
patch 9 (a compile's linked shaders, parameter lists and binary freed; DKSH bytes unchanged) and freeing the
recorded variants once the list is made.

| build | translation | compile (mean) | heap never used after |
|---|---|---|---|
| before (main + kModVariables) | 1,218 s | ~100 ms | 47 MiB |
| patch 9 + recorded freed | 1,614 s | 117 ms | 651 MiB |
| patch 9 + recorded freed, census build | 1,936 s (census before: 1,537) | 128 ms | 581 MiB |
| **patch 9 alone (kept)** | **1,325 s** | 85 ms | **476 MiB** |
| patch 9 + recorded in 4 MiB blocks, then freed | 1,621 s | 98 ms | 546 MiB |

Freeing the recorded variants cost ~300 s of translation every time, also with their data in a few 4 MiB blocks
(so not the small holes between other threads' blocks that was the first guess); the cause was not found and
the change was reverted: patch 9 alone leaves ~430 MiB more than before, several image heap chunks. (One run
before these ended at boot frame 150 with the game's ResMgrThread reading NULL+0x14, `f_0275D21C`: the crash
seen once on 2026-10-13, before any shader work, unrelated.)

**Picto Box after a first start (2026-10-15, checklist C).** No crash (the original report's abort). The first
pictures, taken right after loading a save in Windfall with 100-200 shaders queued (Windfall is not in the first
start's list), came out black: their draws were skipped for pending shaders, the known CPU-read surface risk, on
main too. With the same caches and the queue empty, main and this branch both take good pictures. uam patch 9 on
the console's 10,663 converted sources (`dksh_cache build`, with and without Guard Malloc): the same
shadercache_dksh.bin as the unpatched uam, byte for byte.

Next: step 3 (a chunk that does not fit must not abort), then step 4's tour; the black pictures with pending
shaders are their own fix (draws into CPU-read surfaces must not be skipped).
