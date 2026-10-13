# Upstream sync to devel (after v0.2.11): plan

Status (2026-10-13): **phase 1 written** (steps 1.1-1.7, one commit each, on main 9f75519): the Switch NRO builds
and the changed runtime files compile for macOS; the console gate after 1.7 and the phase 0 baselines are still to do.
In upstream files the fork's lines went from main.cpp +327, core.cpp +249, host.h +83, audio_out.cpp +50,
CMakeLists.txt +141, overlay.cpp +525 to +35, +81, +23, +21, +22, +87 (the rest is phase 2's: generic fork
changes, small Switch branches inside upstream functions).

The last sync took upstream `main` defb89f (v0.2.11, "Round 48", docs/upstream-0.2.6-plan.md). Upstream `devel`
has 418 commits since (481aafe on 2026-10-13). There is still no common ancestor, so the sync is again
`git diff defb89f upstream/devel | git apply -3` on this branch, then the fork-side fixes below.

The read-only audit this plan is built on (every fork change classified as redundant, upstreamable,
Switch-specific or conflicting, with upstream commit hashes) is kept outside the repo:
`~/Documents/windwaker/upstream-drafts/fork-vs-devel-audit-2026-10-13.md`. Item numbers (R1, U1, S1, C1...)
below refer to it.

## Goals

1. Take upstream devel (game fixes, mod manager, pacing, exception reports, overlay extensions, our own
   merged PRs #171 #172 #175 #183 #184 and, if merged by then, #187).
2. Delete what upstream now does better or the same, instead of carrying it (about 1,300 lines).
3. Leave the Switch code in fork-owned modules with small marked hooks in upstream files, so the next sync
   conflicts less (the fork rule: fork code in its own modules, small marked hooks in upstream files).
4. No regression on the console: same frame rate (≥4 interleaved boots), same fixes (#7 #12 #15 #23 #32 #33).

## Phase 0 — before touching anything

- Baselines on the current main NRO (b3d1e23 or newer): `tools/switch/perf_test.py run` on the bench
  scenes (lava, windfall, dragonroost; local perf/benchmark branch), the 5 s `[dk]` stats, boot time with a
  warm shader cache, memory (`heap never used`).
- Note which upstream PRs are merged at that point (#187 changes phase 2's renderer step).
- Fetch: `git fetch upstream devel` (tags off: `remote.upstream.tagOpt=--no-tags`).

## Phase 1 — shrink the fork's footprint on current main (each step its own commit, tested on the console)

These steps don't need upstream code. Each one makes the merge smaller or safer.

| Step | What | Audit | Check |
|---|---|---|---|
| 1.1 | Move `OP_SET_REG_PAIRS` to the end of the `gx2_cmd.h` enum (it renumbers `OP_PEEK_Z` / `OP_LAYOUT_CONTENT` today, against the "append only" comment) | U9, surprise 3 | Build; picto box (#7) and place names (#12) still right |
| 1.2 | Switch code of `main.cpp` (~300 lines: exception handler, crash report, `__wrap_abort`, session logs, `early_settings`, the Switch block in `main()`) into `platform/main_switch.cpp` behind ~4 marked hooks | S1, C4 | Boot, crash report on `crash` debug command, logs in `logs/` |
| 1.3 | `core.cpp`: buffered SD log + writer thread, heap stats, `init_switch()`, thread wraps into `platform/core_switch.cpp`; `GuestBaseScope` stays inline | S2, C5 | Boot, log flush on crash |
| 1.4 | `host.h` Switch parts into `platform/host_switch.h` (one `#elif`) | S4, C10 | Build |
| 1.5 | audout backend out of `audio_out.cpp` into `platform/audio_out_switch.cpp`, asking for stereo | S5, C8 | Audio, DSP guard (#13) |
| 1.6 | Switch CMake (~140 lines) into `cmake/Switch.cmake` with 3-4 `include` / `if(WWHD_SWITCH)` hooks | S7, C7 | Clean configure + build |
| 1.7 | Overlay: gather the Switch/Warp tab code and the other overlay hunks into `platform/overlay_switch.cpp` functions, still called from `overlay.cpp` for now (phase 2 swaps the calls for `overlay::ext` registrations) | R11, S6, C1 | Menu tabs unchanged |

Gate: full console check after 1.7 (boot, menu, a save state, the bench scenes within noise of phase 0).

## Phase 2 — the merge (on feature/upstream-sync)

`git diff defb89f upstream/devel | git apply -3`, then resolve in this order:

| Step | What | Audit | Notes |
|---|---|---|---|
| 2.1 | Recompiler: take upstream's `recomp.py`, `crlive.py`, `leaflocal.py`, `ppc.h` blocks; strip the duplicated CRLIVE/LEAF/NONLEAF/GQR wiring from `fork_passes.py`; re-add the `fork_*` hook calls; port the fork's leaflocal extras (`PPC_ICALL` regexes, volatile-only reload + `PPC_KEEP_R/F`, `clobbers`) onto upstream's file or into fork_passes | R4–R8, C3, C9 | **Remove the fork's `ppc_cr_poisoned`** (duplicate symbol). Upstream leaves functions with instruction hooks "plain"; the fork ran the passes there: re-test, take upstream's rule unless the bench says otherwise |
| 2.2 | Take upstream's `StringBuf.h`, the 1 MB reservation line, `-fwrapv` (identical), quoted `-include "cemu_shim.h"` (#183) | R1–R3 | Trivial |
| 2.3 | `gx2_core.cpp`: drop `WWHD_RELAXED_VSYNC` (~75 lines) for upstream's late-flip pacing (#125, `vsync_pacing.h`); restore upstream's `RegisterBlocks` (~30 lines) and add the fork's category bumps as a 2-line hook (or bump only when `restore()` reports a change); keep command staging, merge it with upstream's `exception_report::boundary` around the render thread | R9, R10, C2 | Pacing on the console: `[dk] pacing` line, `WWHD_LOG_FLIP_PACING=1`; #23 must stay fixed |
| 2.4 | Overlay: take upstream's `overlay.cpp` as is; register the Switch tab, Warp tab, hidden tabs, font loader, ZL+ZR+Minus shortcut, system keyboard, features (`clipboard`, `file_picker`, `screenshots`, `full_states` off) and the prepare-graphics layer from `platform/overlay_switch.cpp` through `overlay::ext` | R11, C1 | User decision 2026-10-12: **enable** upstream's Audio tab, don't hide it. Left over (small): saved `proController` pref, save-state note wording, About "Version" line |
| 2.5 | Renderer selection: **if #187 is merged**, register deko3d through `render::ext` (`Api::Platform`, `select_decompiler` = OpenGL mode, `shader_key_mask`, `gpu_idle` doing the whole wait including the submit) and delete the `Api::Deko3D` patches (~110 lines). If not merged, keep the patches and rebase them | R12, C12 | `can_restart()`: add `__SWITCH__` (C12) |
| 2.6 | `core.cpp` `fatal()`: upstream's version + `log_flush()`; `PPC_ICALL` / `PPC_IJUMP` must not cache targets that go through upstream's new `ppc_host_call` boundary (imports, HLE) | C5, U5 | Measure the per-hooked-call cost of `ppc_host_call` on the console (unmeasured) |
| 2.7 | Link and build fixes for new upstream code: stub `crash_addr::set_game_functions` on the Switch (crash_addr.cpp includes dlfcn/execinfo/signal); guard the new https/curl CMake branch (`elseif(UNIX AND NOT ANDROID)`, devkitPro may report UNIX: check); `exception_report.h`, `game_clock`, `display_modes`, thread_sampler/touch_pad compile as no-ops | C4, C7 | Clean Switch build |
| 2.8 | `threads.cpp`: line-level merge (`park_wait` + simulation deadline, `event_wait`, alarm thread, `GuestExit` moved to exception_report); check notify-only-with-waiters still wakes upstream's virtual-time waits | C6, S3 | Deferred wakes and fixed tick stay Switch-only |
| 2.9 | Mods: re-apply the `#if` guards around dlopen loading in `guest_mods.cpp` / `packages.cpp` | C11 | Mods tab, content mods |
| 2.10 | Small adjacent conflicts (interp, ax, aspect, peekz, savestate, render_prof, report_header, true60_test, gx2_resources, gx2.h, runtime.h); drop the fork's 5-line `gfx/vulkan/shaders.cpp` hunk; re-apply guard.py extras; keep the fork README, refresh `docs/upstream-README.md`; workflows stay `workflow_dispatch` | C13–C18 | |

## Phase 3 — verification

- Switch build USA and EU (`tools/switch/build.sh`, title IDs in the NROs, `touch build/gen/*` after swapping gen dirs).
- Console, USA: boot with an empty and a warm shader cache, title → Outset, menu (all tabs, Audio tab), save/load a portable state, warp list, picto box (#7), place names (#12), bloom and distance blur at 0.75–1.0 dynamic resolution (#15, #32), frame pacing (#23), sleep and wake (#33), debug server after sleep, Pro Controller and GamePad modes.
- EU build: boot and language.
- Performance: `perf_test.py ab --order ABCCBA --repeat 2` against the phase 0 main NRO (≥4 interleaved boots; resolves ~0.3 fps). The lava scene is the main-thread-bound one to watch (also the pre-v0.4.7 check the user asked for).
- `[dk]` stats: memory, shader translations, render-thread ms.

Merge to main only after the user's review (feature branch first, main by the user's decision); then the next sync starts from `upstream/devel`'s commit used here (write it at the top of this file and in docs/switch-port.md).

## Phase 4 — after the sync: PRs to upstream, one at a time

From the audit's upstreamable list, smallest and safest first: U1 (`apply_regs` uniform fast path + `set_regs` stack buffer, ~15 lines), U3 (`HostFsCall`, ~35), U2 (hang watchdog, ~110), U11 (render_prof inline toggles), U13 (`WWHD_SANITIZE`), then U4 (sync-object wakes, ~120, medium), U5 (call-site caches through `ppc_host_call`), U8 (single-precision `round25` elision, check build), U9 (command staging + register-write merging, after 1.1). U7 (per-CPU preempt) changes the guest-mod ABI and would let the Switch load native guest mods: ask upstream first in an issue.

## Open decisions for the user

1. When to run it: before v0.4.7 (bigger risk for the release) or right after (recommended).
2. Wait for #187 to merge before phase 2, or do step 2.5 with the old patches and switch later.
3. TV/GamePad toggle: keep ours, or move the deko3d presenter onto upstream's display modes (`display_modes.h`, `next_view()`): more work (R13).
4. Native guest mods (U7): worth proposing the ABI change upstream?
