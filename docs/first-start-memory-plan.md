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
