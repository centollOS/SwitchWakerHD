# Release checklist

What to test on the console before tagging a SwitchWakerHD release. Every row comes from a bug a release once
shipped or nearly shipped (the issue is in brackets). Copy the record at the end into the release's notes or
milestone, with the results.

Rules:
- Test the exact commit that will be tagged, built from a clean tree (`tools/switch/build.sh`), at stock clocks.
- **Section C (Picto Box) is never skipped**, even for a hotfix: it broke silently twice (#7, #37) and nothing else
  in the game exercises the CPU read-back path.
- A hotfix may skip sections F and G if it touches nothing in the renderer, memory or threads; note why.
- A failure blocks the release unless it is a known open issue that the release does not claim to fix.

## A. Build and package

| # | Check | Expected |
|---|---|---|
| A1 | USA and EUR NROs build from the release zip (`package_release.sh`, then `make_sd.py`) | Both build; `tools/release/guard.py` passes |
| A2 | `info` and the boot log of a build from the zip | The version is the tag (`vX.Y.Z+N (hash)`), not "dev (unknown)" (#20) |
| A3 | Windows installer, `docs/installer-windows-tests.md` section A | All pass (#1, #28, #30) |
| A4 | The tag is new | Never re-publish or move a tag: bump the version (the installer caches zips by tag) |

## B. Boot, screens and system

| # | Check | Expected |
|---|---|---|
| B1 | USA and EUR games boot to the title and into Quest Log 2 | No `cking.rpx does not match` (#4) |
| B2 | Docked (1080p) ↔ handheld, twice, in game | The picture follows the mode; no black frames left behind |
| B3 | Sleep the console in game for 1 min, wake it | The game resumes (#33) |
| B4 | Overlay: open, change a setting, close; settings survive a restart | No crash; settings kept |
| B5 | HOME menu and back, 3 times | Game resumes, audio back |

## C. Picto Box (always)

The overlay's cheat "every inventory item" gives the Deluxe Picto Box (colour); a save before it has the regular one.

| # | Check | Expected |
|---|---|---|
| C1 | Regular Picto Box: take a picture of a person in Windfall | The picture shows the scene, not black (#37) |
| C2 | Deluxe Picto Box: the same in colour | Not black; colours right |
| C3 | Look at both in the pictograph menu | Each fills its frame (no black right/bottom bands) |
| C4 | Selfie (turn the box around) | Link's picture, not black |
| C5 | The game's judge on a person ("Good", "Excellent"...) | It finds the subject (#7: "too close/too far" always) |
| C6 | Save, restart, open the pictographs | The pictures are kept |

Each picture logs `[dk] 3E9D0000 800x450... written back to guest memory for the CPU` (the 800x450 RGBA8 linear
target, `docs/picto-judge-plan.md`): a black picture with those lines is a rendering problem, without them a
write-back one.

## D. Places that broke before

Use the save states in `sdmc:/switch/wwhd/states/` (`docs/portable-save-states.md`) or the Warp tab.

| # | Check | Expected |
|---|---|---|
| D1 | Sail to a new island | Its name card is drawn whole (#12) |
| D2 | Bloom on (default): Outset by day, then the Helmaroc King tower flooding cutscene | Bloom visible (#15); no green band on the right edge (#32) |
| D3 | A storm at sea with enemies, a void-out | Music and sound effects keep playing (#13) |
| D4 | Dragon Roost Cavern: lava room, the boss door to Gohma | No freeze (#37); frame rate as in the last release |
| D5 | Bow: shoot arrows | Arrows load and fly (#6) |
| D6 | Place the last pearl / Tower of the Gods rises (#42), Ghost Ship floor (#41) | Once fixed: no crash; floor drawn |

## E. Saves

| # | Check | Expected |
|---|---|---|
| E1 | Save in game, return to the title, load | Same place and items |
| E2 | Portable save state: write slot 1, load it, load one made by the last release | Loads; the old one is accepted |

## F. Memory (every release)

The tour (memory round 2, `docs/memory-round2-plan.md`): 40 warps (every third of `warps`; all 136 hit the game's
own assert at M_DaiB, #18), 25 s each, **two laps (~35 min)**, the `[mem]` lines kept. One lap visits every place
once, so the second lap only revisits: what still grows then is a leak. (The deko3d leak of round 2 showed as
+21 MiB in a 14-minute tour; the image heap reaches its 4 chunks in ~3 minutes.)

| # | Check | Expected |
|---|---|---|
| F1 | No crash in the two laps | |
| F2 | `malloc ... in use` at the end of lap 1 and of lap 2 | Lap 2 adds a few MiB at most |
| F3 | Image heap | 4 chunks (256 MiB), no new one in lap 2 |
| F4 | Shader variants | Bounded (1,000-2,000 alive), screenshots fine |

A **2-hour run** only when lap 2 grows, or when the release changes how the renderer or the runtime allocate or
free memory (texture/shader eviction, the image heap, deko3d patches): then the slope over 2 h, in MiB an hour.

## G. Performance (renderer, CPU-side or recompiler changes)

The perf test (kept out of the repo: `perf/benchmark` branch, `~/Documents/SwitchWakerHD-perf-test`):
`perf_test.py ab` of the release against the last release's NRO, at least 4 interleaved boots
(`--order ABCCBA --repeat 2`).

| # | Check | Expected |
|---|---|---|
| G1 | lava (`M_NewD2 2`), windfall, dragonroost | No scene slower by more than 0.3 fps |
| G2 | Frame pacing in those scenes | No new hitches; frametime even (#23) |

## H. Publish

1. Tag, package (`package_release.sh vX.Y.Z`), publish with the `.zip` and `.sha256`.
2. Close every issue of the milestone with a link to the release, remove `fixed on main`, close the milestone and
   create the next one.

## Record

```
Release vX.Y.Z  commit ...  tested YYYY-MM-DD  console: Erista/Mariko, firmware, Atmosphère
A1 ok  A2 ok  A3 ok/skipped (why)  A4 ok
B1 ok  B2 ok  B3 ok  B4 ok  B5 ok
C1 ok  C2 ok  C3 ok  C4 ok  C5 ok  C6 ok
D1 ..  E1 ..  F: malloc lap 1 N MiB, lap 2 +N MiB, chunks N  G: lava ±N, windfall ±N, dragonroost ±N fps
Known open, not blocking: #..
```
