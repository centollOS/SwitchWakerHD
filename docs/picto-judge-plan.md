# Picto Box judge: Lenzo never accepts a picture (issue #7)

Branch `feature/picto-judge`. Status: fixed, tested on the console (2026-10-12), awaiting review.

## Report

Lenzo rejects every picture of Garrickson ("too close / too far"), at any distance. deko3d only.

## Investigation

- The upstream recompilation has no such issue or fix; its Picto Box write-back (issue #53) is already in
  the deko3d renderer (`guest_writeback`).
- The judge is `025BE664` (HD's `dSnap_packet` judge, as `PcJudgePixels` in the GameCube decomp): it reads
  the picture through agl's CPU accessor (`027BF244` → `027BF3FC`: draw, `GX2DrawDone`, `DCInvalidateRange`)
  and counts, per pixel, `alpha >> 2` = the subject's number (63 = none); each subject's pixel count over
  its projected area is its ratio.
- Console diagnostics (first GPU write per surface, alpha histogram of the write-backs): the CPU-read target
  is `3E9D0000` 800x450 RGBA8 linear (tile 1), written back correctly, but its alpha was 255 in all
  360,000 pixels. The shutter's draw uses CB_BLEND0_CONTROL `20010504`: color (SRC_ALPHA,
  ONE_MINUS_SRC_ALPHA), alpha (ONE, ZERO).
- Cause: deko3d 0.5.0's `dkCmdBufBindBlendStates` writes `dstColorBlendFactor` into `FuncAlphaDst`, so the
  alpha became `srcA + dstA·(1 − srcA)` = 255 over the cleared 255. Still unfixed upstream.

## Fix

deko3d 0.5.0 built from source (`runtime/third_party/deko3d`, `PATCHES.md` patch 1: `FuncAlphaDst` from
`dstAlphaBlendFactor`), linked as `deko3d_release` / `deko3d_debug` instead of devkitPro's libraries. The
renderer's issue #12 workaround (masking one part's channels) is removed: the library now does it right.

## Verified

- Console, Windfall, after the fix: the write-back's alpha holds subject numbers (0, 1, 3, 4; 63 elsewhere)
  and the game judged a picture of a person "Good" (never before).
- Pending: place names (#12) with the workaround removed; Lenzo's own quest with a save that has it.

# Picto Box pictures cropped to 80% (black right and bottom bands)

Report (user, 2026-10-12, console screenshot): every picture fills only the top-left 80% of its frame.

- Console diagnostics (picture draws: targets, viewports, sampled textures, constants, vertices): the first
  picture pass draws the scene (`F5807800`, 1280x720) into `20974000` 1600x900 with the full viewport and UVs
  inside the texture (0.0625..0.9375), but with the scene's depth buffer `F500E000` still bound: 1280x720.
- deko3d's `dkCmdBufBindRenderTargets` sets the screen scissor to the smallest bound target, so only
  1280x720 of the 1600x900 picture was drawn (1280/1600 = 0.8). On the Wii U that depth buffer is 1920x1080
  (the game's high size; it always takes its low size here, `docs/switch-port.md` "Why 720p").
- Fix (`gfx/deko/draw.cpp`, target selection): a depth buffer smaller than a color target is left out when
  the draw cannot use it (no stencil; depth test off, or ALWAYS without writes). Otherwise it stays bound
  and `[dk] depth buffer ... smaller than a color target and used` is logged once.
- Console: the picture fills its frame (user confirmed).
