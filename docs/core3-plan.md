# Core 3 for background work (plan)

Branch `feature/core3`. Status: steps 1-2 done (2026-11-03), step 3 (the A/B) next.

## Why

The game gets cores 0-2 (`docs/cpu-threading-plan.md`): the main thread on core 1, the render thread on core 2, and
the shader worker on 0 and 2. Background shader compiles (the first start's list, `docs/background-shaders-plan.md`)
run on core 0 at a 50% duty cycle so the game threads there keep their time, and a compile a draw waits for can take
the render thread's core. Step 0.5 of the threading plan ("does core 3 give an application any time?") was never
run. renut-nx (`four-core-mode.md`) gets core 3 through its forwarder's NPDM and puts audio, uam compiles and
pipeline prebuilds there.

## Steps

1. **The forwarder allows cores 0-3** (done): `tools/switch/forwarder/patch_hbl.py` sets hbl.json's
   `highest_cpu_id` to 3 (hbloader's own: 0-2). The boot log names the process core mask (`[boot] process core
   mask 0xf`; 0x7 with an older forwarder or hbmenu). The debug server's `coreprobe [core] [s] [prio]` spins a
   thread on a core and reports the share it got.

   Console (2026-11-03, USA, handheld, stock clocks, 10 s each):

   | Where | Priority 0x2C | Priority 0x3B |
   |---|---|---|
   | Title screen | 89.5% (longest gap 1.6 ms) | 77.2% (2.8 ms) |
   | In play, sea (warp 0) | 86.5% (1.5 ms) | 75.7% (3.9 ms) |
   | In play, sea (warp 2) | 81.0% (1.8 ms) | 77.8% (3.0 ms) |

   With the old forwarder: mask 0x7, a thread on core 3 is refused (0x57857). Core 0 at 0x3B on the title: 100%.
   So the system leaves an application thread about four fifths of core 3, much more than renut-nx's "about half".

2. **Background work on core 3** (done, `platform/cores_switch.h`): only when the process has core 3 and
   `WWHD_CORE3` is not 0 (or the debug server's `core3 0|1`, read at each decision):
   - the shader worker's background jobs on core 3 alone, without the 50% duty cycle (no game thread there);
   - jobs a draw waits for, and the helper compile threads, on cores 0, 2 and 3 (0 and 2 before);
   - the background list's thread (reading, speculation, translation) on core 3 (core 0 before).

3. **A/B on the console** (next): with an empty shader cache directory (`WWHD_DK_SHADER_CACHE_DIR` in
   settings.ini's `[dev]`, so the console's own caches are untouched), the first start's list compiling gently in
   play; `core3 1` / `core3 0` alternating every 60 s at Outset and Windfall: the game's fps and the compiles per
   minute. Then a warp tour on a cold cache: draws skipped for shaders and the time they waited.

4. Then decide: the audio threads (`hle/ax.cpp` frame thread, audout), the log writer, the update check and the
   debug server could follow; the forwarder change ships with the next release (players reinstall the NSP once:
   the release notes say so; the old NSP keeps working, on cores 0-2).
