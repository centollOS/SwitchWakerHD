# Shader variant memory (#37)

A player's 4-hour session ended in `std::bad_alloc` (#37, log `wwhd_2026-10-13_10-32-58`): free heap fell from
~790 MiB to 0, in step with the new shader variants (~52,000 in the session, ~9 KB each). On a PC this is noise; on
the Switch it ends long sessions.

## What grows (read-only audit, main 6964b16; byte figures are estimates)

| Severity | Container | Grows with | Per entry |
|---|---|---|---|
| high | `shaders` (shaders_dk.cpp) with each `Shader`'s `dec`, `ufCache`, `vtxLayout` | each new variant key | 5–9 KB |
| high | the variant key: `gather_state` hashes all 32 `SQ_VTX_SEMANTIC`, all 10 `SPI_VS_OUT_ID` and all 8 `CB_COLOR*_INFO` words, including words an earlier shader left behind (GX2SetVertexShader writes only the shader's own) | draw order | multiplies the row above |
| high, across sessions | `storedTrans`/`storedBlob`: the whole `shadercache_dk_translations.bin` in RAM, appended every session, never pruned | every session | 0.5–0.8 KB |
| medium | `shader_manifest.bin` and `shadercache_gl.bin` read whole at boot | every session | — |
| medium | `S.list`/`byAddr` Surface structs (kept after eviction); feedback/volume/mip-chain/`g_wbSized` copies; image heap chunks never released | new surfaces | 1.5–3 KB + images |

What the decompiler reads of those registers (third_party/cemu, LegacyShaderDecompiler):
- `SQ_VTX_SEMANTIC`: per fetch attribute, the first slot holding its semantic id (LatteDecompiler.cpp
  emitAttributeImport, the analyzer); `LatteDecompiler_UnfetchedVertexInputs` (already in the key) for slots
  `SQ_VTX_SEMANTIC_CLEAR` leaves. Upstream's Vulkan renderer keys the resolved location per attribute instead of
  the 32 words (gfx/vulkan/shaders.cpp `gather_linkage`).
- `SPI_VS_OUT_ID`: only for the parameters the program exports (`outputParameterMask`, known after its first
  translation, like `textureUnits`).
- `CB_COLOR*_INFO`: only the analyzer's render-target-as-texture check (Metal); the GLSL path doesn't read it.

## Steps

1. **Counters.** A `[mem]` line every 5 s: malloc in use against its arena (`mallinfo`, to tell a leak from
   fragmentation), heap never used, shader variants, distinct GLSL, programs, translation records and their bytes,
   Surface structs including evicted ones. Measured on the console: idle at the title screen, and a Warp tour.
   If variants grow far faster than GLSL, the extra keys are redundant (same code).
2. **Exact keys.** Hash the resolved attribute locations instead of the 32 semantic words, the exported
   parameters' out ids instead of the 10 words, and drop `CB_COLOR*_INFO` from the pixel key (keeping what the
   GLSL path reads). Keys change: bump the cache version; check what that means for players' existing
   shadercache files and "Prepare graphics".
3. **Smaller variants and a cap.** Replace `dec` after translation with what draws still read (output mask, the
   two uniform remap lists, the texture lists); evict variants not drawn for a long time (LRU, frame-epoch safe).
4. **Cache files.** Load an index of `shadercache_dk_translations.bin` instead of the whole file; prune or
   compact the files on boot.
5. **Long test on the console** (2–3 h Warp tour through many places, `[mem]` slope per hour) before merging;
   kept as a release check.

Surfaces (the medium rows) come in a second round, after the counters show how much they matter.
