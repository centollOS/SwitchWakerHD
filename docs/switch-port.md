# Switch port — current state

Status as of 2026-10-07 (runtime round 30, branch `dev`, merged with upstream devel v0.2.2).

## Summary

| Area | State |
|---|---|
| Target | Horizon OS homebrew (`.nro`, launched from hbmenu in title mode via Atmosphère) |
| Toolchain | devkitPro devkitA64 (GCC 15.2, libnx) in the `devkitpro/devkita64` container; Mesa 20.1 rebuilt with a persistent shader cache (`tools/switch/mesa`) |
| Graphics | OpenGL 4.3 core on Mesa nouveau through EGL + glad |
| Build | Works: `tools/switch/mesa/build_mesa.sh` once, then `tools/switch/build.sh` → `build/switch/wwhd.nro` (~42 MB) |
| Boot on hardware | Works: picture, sound and controller input (as a Wii U Pro Controller, so everything is on one screen) |
| Performance on hardware | Stock CPU 1020 MHz. Handheld with the official GPU profile (GPU 460.8 MHz, memory 1600 MHz, round 29): median 29.5 fps, 72% of reports at 28 fps or more in views over 2,000 draws; dynamic resolution 0.75-1.00 (1.00 in 38% of reports). At the system's 307 MHz the same play was 26.9 fps |
| Settings | In-game menu (hold Minus, round 30): GPU profile, picture adjustments, frame-rate counter, save states, mods, language. Saved in `settings.ini`; `env.txt` values win at start |
| On-screen FPS counter | Top-left corner; menu or `WWHD_FPS=0/1/2` |
| Desktop reproduction | Works: a headless Linux build of the same GL renderer (Mesa llvmpipe) renders the game correctly |

## History of decisions

1. **Graphics API.** The first plan was NVK (Vulkan). After choosing Horizon homebrew over Linux-on-Switch, deko3d was considered. It was rejected because the Cemu shader decompiler emits GLSL at runtime, while deko3d needs shaders compiled offline with uam. The final choice is **OpenGL through switch-mesa (nouveau)**, which compiles the decompiler's GLSL at runtime.
2. **Platform.** Horizon `.nro` was chosen over L4T Linux, as the user preferred.
3. **Game data.** The user provides their own decrypted dump in `Rom/Decrypted/<title folder>/` (US, v0: `code/`, `content/`, `meta/`). `Rom/` is gitignored. Generated code (`build/gen`) comes from the user's dump and must never be committed.

## Building

### Recompile the game (once per dump)

```sh
python3 tools/recomp/recomp.py "Rom/Decrypted/<title folder>/code/cking.rpx" build/gen
```

Result: 39,713 functions in 78 files (~171 MB of C), with 42 unhandled `op=0` sites.

### Switch `.nro`

```sh
tools/switch/build.sh            # WWHD_JOBS=<n> limits parallel -O3 compiles (default nproc/2)
```

- The script runs CMake with `/opt/devkitpro/cmake/Switch.cmake` in `docker.io/devkitpro/devkita64` (podman or docker), then Ninja.
- A full build takes about 3.5 minutes; incremental builds take seconds.
- Output: `build/switch/wwhd.nro`, created with `nx_generate_nacp` and `nx_create_nro`.

### Headless desktop reproduction (debugging aid)

The same OpenGL renderer runs on desktop Linux without a window. It renders offscreen with Mesa (llvmpipe) and dumps PNGs.

- The container image `localhost/wwhd-glhost` is built from `/tmp/wwhd-hostct/Containerfile`. That file is not in the repo; its contents are:

  ```Dockerfile
  FROM registry.fedoraproject.org/fedora:latest
  RUN dnf -y install clang lld cmake ninja-build zlib-devel lz4-devel mesa-libEGL-devel mesa-libGL-devel libepoxy-devel mesa-dri-drivers python3 && dnf clean all
  ```

- Configure once with: `-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DWWHD_RENDERER=OPENGL -DCMAKE_BUILD_TYPE=RelWithDebInfo` into `build/headless`.
- Build:

  ```sh
  podman run --rm -v $PWD:/src:Z -w /src wwhd-glhost bash -c 'ninja -C build/headless wwhd'
  ```

- Run:

  ```sh
  G="$PWD/Rom/Decrypted/<title folder>"
  podman run --rm -v $PWD:/src:Z -v "$G":/game:ro,Z -v /tmp/wwhd-run:/run:Z -w /run \
    -e LIBGL_ALWAYS_SOFTWARE=1 -e WWHD_DUMP_FRAMES=30,100,200,300 -e WWHD_DUMP_TARGETS=300 \
    -e WWHD_EXIT_AT_FRAME=305 wwhd-glhost /src/build/headless/wwhd --game /game --save /run/save
  ```

- Add `--trace` to log HLE calls, including file system opens.
- Symbolize crashes with:

  ```sh
  podman run --rm -v $PWD:/src:Z wwhd-glhost addr2line -f -C -e /src/build/headless/wwhd <addrs>
  ```

- With Docker (on an arm64 Mac, `docker build -t localhost/wwhd-glhost` with the Containerfile above), `build/gen` may be a symlink to another checkout: mount that path at the same place. Docker Desktop sometimes does not show a freshly linked binary through a `:ro` mount of the whole tree; mount `build/headless` itself (`-v $PWD/build/headless:/bin2:ro`). Build with `-j4`–`-j6` (the generated code needs a lot of RAM per file).
- The headless link uses `-ffunction-sections`/`--gc-sections` like the Switch's, so test hooks the generated code does not call need no `f_X_orig`; it also uses the Switch's portable `hostui_switch.cpp` (settings.ini).

#### Harvesting the shader cache

The headless build generates the same GLSL as the Switch (same decompiler, same register keys; `glVendor` is never set, so Cemu's vendor branches are equal). Checked 2026-10-07: a headless run to Outset produced 462 sources and 249 pairs, all present with the same hashes in the console's `shadercache_gl.bin`. Translation records (3) are equal except bytes of `textureUnitList` past `textureUnitListCount`, which are uninitialized and never read.

`WWHD_WARP_TOUR=all|main[,dwell_seconds[,start_index]]` (`mods/cheats.cpp`) warps through `mods/warps.h` once a file is loaded (`all`: `kMainWarps` then `kAllWarps`, 148 destinations), stays `dwell_seconds` of game time in each and quits at the end; a destination not reached in 45 s quits with status 3. With the scripted input below the camera turns all the time (`RX=1`) and A is pressed every 90 frames (text boxes). A driver restarts after a crash or a stuck destination from the next one; each run appends to the same `shadercache_gl.bin`:

```sh
A="300-304:A,400-404:RIGHT,460-464:A"; for f in $(seq 500 40 1100); do A="$A,$f-$((f+4)):A"; done
A="$A,1200-100000000:RX=1"; for f in $(seq 1300 90 60000); do A="$A,$f-$((f+3)):A"; done
docker run --rm -v $PWD/build/headless:/bin2:ro -v "$G":/game:ro -v $RUN:/run -w /run \
  -e LIBGL_ALWAYS_SOFTWARE=1 -e WWHD_PRO_CONTROLLER=1 -e WWHD_SCRIPT_INPUT="$A" -e WWHD_CHEAT_INFINITE=health \
  -e WWHD_WARP_TOUR=all,6,0 localhost/wwhd-glhost /bin2/wwhd --game /game --save /run/save
```

(`$RUN/save` holds a copy of the console's save, `save/user/...`.) `tools/switch/merge_shadercache.py out.bin console.bin harvested.bin` merges the files: each source, translation and pair once, sources first (the loader takes a translation only after its GLSL), and drops pairs and translations whose GLSL is missing.

First harvest (2026-10-07, 6 s per destination, ~85 min with restarts): 138 of 148 destinations reached. Not reached: GanonK (crash, twice), Cave08, ENDumi, I_SubAN, M2ganon, PShip (crashes), E3ROOP (scene change never ends), sea_E (not reached in 45 s), sea_T (hang; the title stage, covered at boot). Harvested 7,620 sources, 5,721 pairs, 58,282 translations; merged with the console's file (1,561 / 956 / 13,449): **7,680 sources, 5,763 pairs, 65,907 translations** (44 MB). The headless build loads it (5,763 programs linked in 23 s on llvmpipe; sources and translations 122 MiB of malloc). On the Switch, linking every pair at boot costs ~13 ms each with Mesa's disk cache (~75 s per boot) and ~180 ms each on the first boot (~17 min), and every linked program stays in memory: check the heap before shipping the whole file.

## Architecture of the port

### Build system (`CMakeLists.txt`)

- `WWHD_RENDERER` gained `OPENGL`. It is the default and the only allowed value on Switch (`WWHD_SWITCH`, set when the devkitPro toolchain defines `NX_ROOT`). On other hosts it builds the headless debugging variant (`WWHD_HEADLESS_GL=1`).
- GCC 15 or newer is allowed on Switch. GCC 15 `__attribute__((musttail))` works on the generated code.
- `cemu_latte` gets `ENABLE_OPENGL=1`. The `EmitMSL` sources are excluded when Metal is off.
- Game code flags are `-O3 -ffp-contract=off -fno-strict-aliasing -fwrapv -w`. `-fwrapv` was added for all platforms.
- The GL sources in `runtime/src/gfx/gl/*.cpp` are globbed. The Switch build uses `input_switch.cpp`; other hosts use `input_headless.cpp`.
- Switch link: `glad EGL glapi drm_nouveau` plus NACP/NRO generation. Headless link: `epoxy EGL dl` and lz4, with include directory `runtime/src/gfx/gl/desktop` (an epoxy-backed `glad/glad.h` shim).
- Tests are skipped on Switch.

### Guest memory (`runtime/include/ppc.h`, `runtime/src/core.cpp`)

- On `__SWITCH__`, `PPC_MEM_BASE` reads the global `ppc_mem_base_var` through non-volatile inline asm (`adrp`/`ldr`), so the compiler can CSE it.
- `init_switch()` reserves a 4 GiB window with `virtmemFindCodeMemory`. Each guest region is backed by `aligned_alloc` and mapped with `svcMapProcessCodeMemory` + `svcSetProcessMemoryPermission(Perm_Rw)`.

| Guest range | Purpose |
|---|---|
| `0x00010000–0x04000000` | code / data |
| `0x10000000–0x50000000` | MEM2 |
| `0x60000000–0x70000000` | runtime |
| `0xC0000000 + 0x2100000` | |
| `0xE0000000 + 0x2800000` | foreground bucket |
| `0xF4000000 + 0x2000000` | MEM1 |

Total backed memory is 1448 MiB, so hbmenu must run in title-takeover mode, not as an applet.

### Process, threads and logging

- **`runtime/src/main.cpp` (Switch only):**
  - Creates `sdmc:/switch/wwhd`, changes into it, and redirects stderr to `wwhd.log` (line-buffered).
  - Reads `env.txt`: `KEY=VALUE` lines become environment variables, and `--opt` lines become extra arguments.
  - `std::set_terminate` logs uncaught exceptions. hbloader intercepts CPU exceptions, so crashes appear only in Atmosphère's `/atmosphere/crash_reports`.
- **`runtime/src/platform/host.h`:** Switch versions of the thread name (no-op), `executable_base` (`svcQueryMemory`), `page_size` (0x1000), `config_dir` (`sdmc:/switch/wwhd`) and `replace_file` (removes the target first). It also adds:
  - `set_thread_core(core)`: `svcSetThreadCoreMask(core % 3, 0x7)`.
  - `start_thread(fn, stack)`: a pthread with an explicit stack on Switch, `std::thread` elsewhere. The libnx default stack is 128 KB, and stack sizes must be 0x1000-aligned.
- **`runtime/src/core.cpp`:** `__syscall_thread_detach` returns 0, because libnx does not implement it. Without this, `pthread_detach` failed with ENOSYS and caused a "Function not implemented" crash.
- **Thread setup:**

  | Thread | File | Stack | Notes |
  |---|---|---|---|
  | Guest threads | `threads.cpp` | 16 MB | pinned to the guest core; no `dladdr` on Switch |
  | Alarm | `threads.cpp` | 4 MB | |
  | AX frame | `hle/ax.cpp` | 4 MB | |
  | GX2 render | `gx2/gx2_core.cpp` | 8 MB | pinned to core 2 |

### Audio (`runtime/src/audio_out.cpp`)

- On Switch, an `audout` thread uses 4 × 1024-frame buffers.
- The AX frame thread paces itself to keep `WWHD_AUDIO_LATENCY_MS` of sound queued (80 ms on the Switch, 40 ms elsewhere). Shorter stalls than that are not heard.
- In the headless build, a null device pulls 1024 frames per real-time interval.

### Input (`runtime/src/platform/input_switch.cpp`)

- The controller acts as a **Wii U Pro Controller** by default: the game reads it through WPAD/KPAD
  (`hle/padscore.cpp`), the GamePad stays connected but idle, and HUD, map and menus all go on the
  TV picture (the only screen shown on the Switch). At the controller question, choose the Pro
  Controller. `WWHD_PRO_CONTROLLER=0` in `env.txt` makes it act as the GamePad instead (VPAD), as
  in earlier builds; the log says which mode is active (`[input] Switch controller acts as …`).
- libnx `pad` buttons map by position, the same in both modes:

  | Switch | Wii U |
  |---|---|
  | A / B / X / Y | A / B / X / Y |
  | L / R, ZL / ZR | L / R, ZL / ZR |
  | + / − | + / − |
  | D-pad | D-pad |
  | L3 / R3 | L3 / R3 |

- Both sticks are mapped. swkbd provides the on-screen keyboard.
- Mouse and host-key hooks used by mods are stubs. `input_headless.cpp` provides null input with the same stubs.

### File system (`runtime/src/hle/fs.cpp`)

- `/vol/content`, `/vol/code`, `/vol/meta` and `/vol/save` map to host directories.
- Opens, stats, reads and writes release the caller's emulated core while the host works, and are counted and timed for the stats (`files` in the `[gl]` and `[hitch]` lines).
- **Case-insensitive lookup on Linux (`fold_case`).** The game requests `content/Common/Audiores/...`, but the disc has `AudioRes`. On case-sensitive file systems the open failed and the game crashed at guest address `0x3C` during audio init. Missing path components are now matched case-insensitively and the results are cached. The Switch (FAT/exFAT) and macOS are case-insensitive already, so this only affects Linux.

### OpenGL renderer (`runtime/src/gfx/gl/`, namespace `gfxgl`, `render::Api::OpenGL`)

| File | Role |
|---|---|
| `gl.h` | `FormatInfo`, `Surface`, `SurfaceDesc`, `Renderer R`, `StreamSlice`, `clip_control()`, declarations |
| `formats.cpp` | GX2 → GL formats (with CPU conversions such as RGB565, RGBA5551, D24S8) |
| `surfaces.cpp` | guest-layout cache, detiled uploads, `glTexStorage` textures, `glTextureView` sampled views (swizzle), render-target lookup, blits, clears, surface copies, invalidation |
| `shaders.cpp` / `shaders.h` | fetch shaders, Cemu decompiler → GLSL (GL mode, no `VULKAN` define), compile/link, uniform locations; compile failures written to `shaderfail_*.glsl` |
| `draw.cpp` | full draw path: index conversion, targets, FBO attach cache, textures/samplers, uniforms, viewport/clip control, rasterizer, depth/stencil, blend, logic op, vertex buffers |
| `backend.cpp` | EGL setup, present, stats, dumps, host loop, scan copy, swap, streaming buffers, renderer table |
| `dump.cpp` | zlib PNG writer, `dump_surface`, `dump_framebuffer`, `surface_mean`, `framebuffer_mean` |
| `desktop/glad/glad.h` | epoxy shim for the headless build |

Key conventions:

- **Bindings.** Pixel-shader textures use unit = slot and vertex-shader textures use 32 + slot. UBOs use i for VS and 32 + i for PS.
- **Loose uniforms set by name:** `uf_remappedVS/PS`, `uf_uniformRegisterVS/PS`, `uf_windowSpaceToClipSpaceTransform`, `uf_alphaTestRef`, `uf_pointSize`, `uf_fragCoordScale`, `uf_texNScale`.
- **Viewport and clip control.** `glClipControl` comes from `eglGetProcAddress`, since glad is generated for 4.3.
  - If `ys < 0`: `GL_UPPER_LEFT`, viewport y = `yo+ys`, height `-2ys`, Latte front face.
  - Otherwise: `GL_LOWER_LEFT`, y = `yo-ys`, height `2ys`, inverted front face.
  - Depth uses `ZERO_TO_ONE` when `DX_CLIP_SPACE_DEF` is set, else `NEGATIVE_ONE_TO_ONE`.
- **Scissor** uses memory rows directly.
- **Streaming.** There are 4 × 32 MB buffers, persistently mapped (`glBufferStorage`, coherent) with a fence per buffer, or orphaned and written through unsynchronized `glMapBufferRange` without `GL_ARB_buffer_storage`.
- **GL state cache** (`draw.cpp`, `gs`). A draw only issues the GL calls whose values changed since the previous draw. Code outside `draw()` that changes GL state (clears, blits, presentation, program links, texture deletion) calls `forget_gl_state()`, and the next draw sets everything again.
- **Scan out.** `copy_to_scan` handles only TV target 1; the GamePad image is not shown. It blits the guest color buffer into `R.tvScan`.
- **Present** (rewritten this session):
  - Draws a full-window triangle that samples `tvScan`, using a linear, clamped sampler that skips sRGB decode, like a blit. Guest row 0 appears at the top, and the image is fitted to 16:9.
  - The pipeline state the game leaves behind is reset first: blend, logic op, depth, stencil, cull, polygon offset, clip control, viewport, depth range.
  - If the shader fails to link, it falls back to the old `glBlitFramebuffer` path.
- **Backend table.** Every entry is wrapped in `guarded()`, so an exception skips the command and is logged, rather than ending the game.

### Diagnostics in the current build

| Feature | Where | Notes |
|---|---|---|
| Extension report at startup | `wwhd.log` | `GL_ARB_clip_control`, `GL_ARB_texture_view`, `GL_ARB_copy_image`, `GL_EXT_texture_sRGB_decode`, `GL_ARB_buffer_storage`, `GL_ARB_viewport_array`: yes/NO |
| FPS counter | screen, top-left | Frame rate over the last half second. `WWHD_FPS=0` hides it; `WWHD_FPS=2` adds `RT <busy>% DR <draws/frame>` and `GPU LAG <ms>` (updated with the 5 s stats) |
| Stats every 5 s | `wwhd.log` | See [Reading the stats line](#reading-the-stats-line) |
| Hang watchdog | `wwhd.log` | `[watchdog]` lines when no frame comes for 3 s: render-thread step, every guest thread's wait and call stack, core owners |
| Hitch report | `wwhd.log` | `[hitch]` line for every frame over 55 ms (first 300): where that frame's time went |
| GL debug output | `wwhd.log` | `WWHD_GL_DEBUG=1`: first 200 non-notification messages; also GL errors and heap use in the stats |
| Frame dumps | `/switch/wwhd/` | `WWHD_DUMP_FRAMES=N[,M]` (`frame_N.png` = TV image, `frame_N_window.png` = presented window, with the FPS counter) |
| Render-target dumps | `/switch/wwhd/` | `WWHD_DUMP_TARGETS=N[,M]`: every GPU-written color target, `target_<frame>_<addr>_<w>x<h>_f<fmt>.png` |
| Headless exit | headless only | `WWHD_EXIT_AT_FRAME=N` |

The heartbeat square and the default frame dumps were black-screen aids; they have been removed.

## Issues fixed during the port

| Symptom | Cause | Fix |
|---|---|---|
| Metal headers in non-Metal builds | `latte_support.cpp` included Metal parts unconditionally | Wrapped them in `ENABLE_METAL`, included `Renderer.h`, added the GL `resourceMapping` |
| Link errors for mouse functions | the mods layer expects mouse hooks | Stubs in `input_switch.cpp` and `input_headless.cpp` |
| `Json` failed to compile with libstdc++ 16 | `std::vector<std::pair<string, Json>>` with an incomplete type | `JsonMember` struct in `input_map.cpp` |
| Missing `<climits>` | `Common/betype.h` | Added the include |
| Duplicate definitions | `fetch_shader_parse.cpp` | Include guards |
| Headless EGL found no config | surfaceless Mesa has no window configs | `EGL_SURFACE_TYPE = EGL_PBUFFER_BIT` in headless builds |
| Switch: crash on boot, no log | — | Log moved to SD, line-buffered; terminate handler |
| Switch: `uncaught exception: Function not implemented` | libnx lacks `__syscall_thread_detach` | Stub returning 0 |
| Headless: crash at guest `0x3C` after `Audiores/c_king.bfsar -> not found` | path case mismatch on Linux | `fold_case` in `fs.cpp` |

## Black screen on Switch: fixed (confirmed on hardware)

The fix below was confirmed by `logs-switch/graphics_works_but_slow`: the log shows `[gl] window: EGL surface 0x0, native window 1280x720`, and the game appears on screen.

## Performance (in progress)

### Measured on hardware before the optimizations

- 10–19 fps in menus, with multi-second stalls when new scenes start (shader compiles).
- The game paces itself by frames, so the picture runs at about half speed while audio stays in real time.
- The game crashed after the intro music; no crash report was available.

### Changes in the current build

| Change | Why |
|---|---|
| No internal clock changes; use sys-clk | The user prefers sys-clk. The built-in overclock was removed |
| Shaders shared by GLSL source; programs keyed by the (vertex, pixel) source hashes | Many GPU states produce identical GLSL. Headless title screen: about 950 states needed only 285 compiles |
| Persistent shader cache `sdmc:/switch/wwhd/shadercache_gl.bin` (zlib GLSL plus linked pairs), compiled at startup with a progress bar | switch-mesa reports 0 program binary formats, so sources are cached and compiled at boot (like Cemu's GL cache). On hardware the render thread spent about 990 ms/s compiling during new scenes. Headless second run: 0 compiles in game |
| Decompiler GLSL text freed after compiling | Tens of KB per variant were kept forever |
| Heap use from `mallinfo` in the stats line | The kernel's used-memory figure counts the whole pre-reserved heap (3185/3189 MiB from the start) |
| Per-frame reuse of guest vertex/uniform uploads (`stream_guest`; `WWHD_GL_NO_DEDUP=1` turns it off) | Gameplay reaches 4,000–5,400 draws per frame, and each draw copied its whole vertex buffer and uniform blocks. An address is now uploaded once per frame, until `GX2Invalidate` (attribute/uniform), `GX2DrawDone` or a stream-buffer wrap. Headless title screen: streamed data fell from 335 to 158 MB/s with the same picture |
| Host service threads at Horizon priority 0x2C (`host::raise_thread_priority`): GX2 render, audout, AX frame, alarms, scheduler tick | libnx creates all pthreads at 59, the only priority Horizon time-slices (10 ms) on cores 0–2, so the render thread waited behind busy guest threads |
| `GX2DrawDone` → `glFlush` instead of `glFinish` | The GL renderer never writes GPU results back to guest memory; waiting for the GPU serialized the game, the render thread and the GPU |
| Persistent, coherent stream buffers (`glBufferStorage`), with a fence per buffer (`WWHD_GL_NO_PERSISTENT=1` turns them off) | One `memcpy` per vertex/index/uniform upload instead of a driver map and unmap |
| GL debug output only with `WWHD_GL_DEBUG=1` | Driver overhead, plus log spam from GLSL warnings |
| No heartbeat square, no default frame dumps; colour means only with `WWHD_GL_STATS=1` | These were black-screen diagnostics; the means need full GPU readbacks |
| 5 s stats line | fps; draws/frame; render-thread ms/s in draws, shader compiles, texture checks/uploads and present; MB/s streamed; surface count; process memory |

Headless check (llvmpipe): rendering is unchanged with persistent buffers, at about 955 draws and 17 MB streamed per frame on the title screen. The Switch's GL driver reports `program binary formats: 1` on desktop Mesa; the Switch value is logged at startup. If it is non-zero there too, compiled shaders can be cached on the SD card.

### Hardware log of 2026-10-03 (`logs-switch/wwhd.log`, before round 2)

Overclocked console (CPU ≈1.7 GHz, GPU ≈900 MHz, RAM 1600 MHz), Pro Controller mode, shader cache loaded at boot (536 shaders, 298 programs in 35 s, no compiles in game).

| Scene | fps | draws/frame | render thread ms/s in draws | streamed MB/s |
|---|---|---|---|---|
| Title / menus | 19–30 | 200–950 | 164–741 | 2–168 |
| Gameplay | 8–12 | 3,200–5,900 | 790–830 | 155–308 |

- The render thread spent 80–83% of every second inside `draw()`, about 16–25 µs per draw. That alone caps gameplay at 11–13 fps, so it is the limit; `present` was only 20–35 ms/s.
- About 27 MB were copied into stream buffers per gameplay frame (308 MB/s at 11.5 fps).
- Shader compiles no longer happen in game (the cache works).

### Round 2: the draw path

Findings from reading the code and the headless renderer:

- **64 KB uniform copies.** Skinned vertex shaders declare their bone palette as `vec4 uf_blockVS1[4096]` (64 KB). When the guest's block was smaller than the declared size, every draw zero-filled and copied the full 64 KB. On the busy headless scene that was 15.9 MB per frame, most of the streamed data.
- **Shader lookup on every draw.** Each draw hashed about 200 registers twice (vertex and pixel shader keys) and did 4–6 hash-map lookups, even when nothing shader-relevant had changed.
- **About 80 GL calls per draw regardless of changes**: 8 colour masks, 8 blend enables, blend functions, stencil, depth, cull, polygon offset, clip control, viewport, scissor, all texture/sampler/UBO bindings, vertex formats and the program. Mesa runs each call through context lookup, vertex flush and dirty-flag work even when the value is the same, and nouveau revalidates what was flagged.
- **Indices** were converted one by one through a lambda with a switch, pushed into a vector, scanned again for the maximum, and always widened to 32 bits.

| Change | Effect / why | Off switch (in `env.txt`) |
|---|---|---|
| Uniform blocks copy and bind only the guest's bytes (rounded to 16), not the declared size; a shared zero buffer replaces per-draw zero blocks | Busy headless scene: uniform data 15.9 → 0.3 MB per frame. Reads past a bound range return zero on NVIDIA hardware and in llvmpipe, as the zero-filled copy did | `WWHD_GL_FULL_UBO=1` |
| Shader lookup memo: the previous draw's fetch shader, shaders and program are reused while `g_shader_state_gen`, the frame, the primitive type and the shader epoch are unchanged (as the Metal and Vulkan renderers do) | 21% of draws on the title screen, 71–75% in busy scenes skip the lookup | `WWHD_GL_NO_MEMO=1` |
| GL state cache: only changed state is sent; blend/mask state only for attached colour targets; blend colour only when a constant blend factor is used | Removes most per-draw GL calls when consecutive draws share state | `WWHD_GL_NO_STATE_CACHE=1` |
| Loose uniforms (`uf_remapped*`, register files, point size, alpha ref, window-to-clip) are uploaded only when the program's last values differ | Each upload makes nouveau re-upload the constant buffer | `WWHD_GL_NO_UNIFORM_SHADOW=1` |
| Index conversion: one tight loop per guest index type, maximum computed in the same pass, 16-bit output for 16-bit lists (restart index 0xFFFF) | Half the index bytes; no second pass | `WWHD_GL_WIDE_INDICES=1` |
| Converted index lists reused within a frame (same address, count, type, primitive, restart) until guest buffers may have changed | Shadow and reflection passes draw the same meshes again | `WWHD_GL_NO_INDEX_CACHE=1` |
| Sampler cache keyed by a 16-byte struct instead of a heap-built string | Less work per texture per draw | — |
| On-screen FPS counter (one shader, 3×5 pixel font) | Requested; also shows load and GPU lag with `WWHD_FPS=2` | `WWHD_FPS=0` |
| New stats: render-thread busy time, GPU lag (timestamp queries), the draw time split into lookup / indices / resources / state / submit, shader-memo hit rate, uniform/index/vertex MB per frame | To tell CPU-bound from GPU-bound, and where draw time goes, from the next hardware log | — |

#### Tests (headless, llvmpipe, title screen to frame 405)

- Rendering compared against a baseline build of the previous commit. The title animation follows real time, so frames are compared visually as well as numerically: frames 100 and 200 are pixel-identical, frames 300 and 400 match (including the King of Red Lions, skinned with the 64 KB palette).
- A bisection during this round found a bug in the new polygon-offset caching (a missing brace kept stale offset units, so the boat failed the depth test). It is fixed, and that check is how it was caught.
- No GL errors and no skipped draws.
- In the busy scene (~6,800 draws/frame) the data copied per frame fell from 26.4 to 6.6 MB (uniforms 15.9 → 0.3 MB). Lookup, index, resource and state work now take about 125 ms/s together. On llvmpipe the rest, about 750 ms/s, is the software rasterizer working inside the draw calls ("submit"), so desktop fps does not predict the Switch's. The GL calls this round removes are driver work on nouveau.

#### Reading the stats line

```
[gl] 10.6 fps, 6856 draws/frame, 0 skipped, GL errors 0 (first 0x0); render thread busy 907 ms/s, GPU lag 0.2 ms;
draws 878 ms/s (lookup 45, indices 6, resources 37, state 38, submit 750; 75% shader memo hits; shaders 26: ...;
texture uploads 6, 11), present 1; streamed 69.9 MB/s (reused 209.7; per frame: uniforms 0.3 MB, indices 0.3 MB,
vertices 25.8 MB); 126 surfaces, heap ...
```

- **render thread busy** (ms per second): near 1000 means the GX2 render thread is the limit. Well below 1000 with low fps means the game's own CPU threads (or the GPU) are the limit.
- **GPU lag**: how long after a frame's commands are submitted the GPU reaches them. Near 0 means the GPU waits for the CPU. A frame time or more (more than 33 ms) means GPU-bound. Not shown if the driver has no GPU clock.
- **draws … (lookup, indices, resources, state, submit)**: where the draw time goes. *resources* covers render targets, texture checks and uploads, and uniform blocks; *state* covers the GL state and vertex buffers; *submit* is the `glDraw*` call (driver validation).
- **per frame**: bytes the draws referenced (before per-frame reuse); **streamed** is what was actually copied.

#### Hardware test of round 2 (`logs-switch/wwhd.log`, 2026-10-03)

| Draws/frame | fps before round 2 | fps after | Render-thread ms per draw (before → after) |
|---|---|---|---|
| ~200 (menus) | 29–30 | 29.5 (the game's 30 fps cap) | — |
| ~3,200 | 11.5 | 18.6–19.4 | ~22 µs → ~11 µs |
| 5,000–7,400 | 8–10 | 14–15 | ~16 µs → ~7 µs |

- Gameplay is still limited by the render thread: `render thread busy` 860–950 ms/s.
- Split at ~7,200 draws/frame (ms/s): lookup 79, indices 32, resources 155, state 220 (includes the vertex-buffer copies), submit (driver) 245, present 24. About 120–210 ms/s of the busy time is outside draws (GX2 command processing, clears, copies) and is not broken down yet.
- `GPU lag -1.0`: the driver returns no GPU clock (`GL_TIMESTAMP` reads 0), so the lag is not measured and the overlay leaves that line out. `present` stays at 20–40 ms/s, so presentation is not stalling: no sign that the GPU is the limit.
- One 5 s window dropped to 5.7 fps with `state` at 712 ms/s, probably the render thread waiting on a stream-buffer fence during a scene change.
- Shader cache at boot: 556 shaders and 309 programs in 22.5 s.

### Round 3: our own per-draw work, and Mesa's GL thread

Target: 30 fps in gameplay with 3,000–7,400 draws per frame, i.e. at most ~4.3 µs of render-thread
time per draw (round 2: ~8 µs including the work outside draws). Measured on the headless build with
`perf` (see [Profiling](#profiling-the-headless-build)) and a temporary probe, then changed:

| Finding | Change | Off switch (`env.txt`) |
|---|---|---|
| Per frame, ~530 vertex/uniform uploads (6.4 MB), of which only ~1.8 MB is unchanged from an earlier frame: most of it is animated geometry rewritten every frame | Not cached across frames (little to gain, risk of stale geometry) | — |
| Every draw resolved each texture again: decode the descriptor, multimap lookup of the surface, view and sampler lookups | Per-unit cache of the last texture lookup, reused while the descriptor words, sampler words and the set of surfaces (`R.surfaceEpoch`) are unchanged and the surface is the only one at its address. 86% hits on the title screen; resource time −22% | `WWHD_GL_NO_TEXTURE_CACHE=1` |
| Render targets were looked up in the surface multimap on every draw | Per-slot cache of the colour/depth target lookup (same rule) | — |
| The shader memo missed on 22% of draws. A probe of the registers that broke it: the fetch-shader address (49k changes), `SX_ALPHA_TEST_CONTROL`, `SX_ALPHA_REF`, the vertex/pixel program addresses | `SX_ALPHA_REF` (a uniform) and the fields the GL shader key ignores no longer count (`WWHD_GL_SHADER_KEY_DIRTY=0` restores the old rule). A second counter, `g_shader_regs_gen`, ignores program registers, so the ~190-register state hash is kept when a draw only switches programs. A 256-entry cache of recent (programs, state) combinations covers another 14–15% of draws. Lookup time −40% | `WWHD_GL_NO_MEMO=1` |
| Program bytes were fully re-hashed once per frame per program | 32 sampled words per frame; a full hash when they differ and every 64 frames (staggered) | — |
| Per-frame upload and index-list caches were `std::unordered_map`s cleared every frame: an allocation per entry | `FrameTable`: open addressing, entries stamped with the frame, nothing freed or allocated per frame | — |
| `set_uniforms` built a zero-filled array every draw, compared it, then copied it | Each 16-byte entry is compared with the program's copy in place; one upload when any differed | `WWHD_GL_NO_UNIFORM_SHADOW=1` |
| `glCheckFramebufferStatus` on every render-target switch | Asked once per attachment set | — |
| Texture invalidations (~85 per frame) walked the surface multimap | A flat surface list | — |
| Every changed register went through range checks to decide if shader state changed (~3 million register writes per second in gameplay) | A per-register class table, built once | — |
| Full texture verification (every 64 frames per texture) used one multiply chain | Four-lane hash | — |
| Texture units, samplers, uniform buffers and vertex buffers were bound one call at a time | `GL_ARB_multi_bind`: one call per dirty range | `WWHD_GL_NO_MULTIBIND=1` |
| Stage timers called `clock_gettime` ~8 times per draw | On the Switch, the tick counter is read directly | — |
| **Mesa and nouveau are about half of the render thread on the Switch** (state validation, command building). switch-mesa contains Mesa's GL thread, but its EGL never starts it | **`WWHD_GL_THREAD=1`** starts it: our GL calls are recorded, and a second thread (high priority, cores 0–2) runs Mesa and the driver. See below | opt-in |

#### Dark shadows and dim interiors: fixed (missing sRGB encoding at presentation)

- **Symptom on hardware:** interiors very dim, shadows of trees and rocks far too dark.
- **Cause:** the game's TV scan buffer has an sRGB format (`GX2SetTVBuffer` format `41A`), so the Wii U's scan-out encodes the linear picture to sRGB. The OpenGL renderer ignored that (its `set_tv_format` was empty) and presented the linear values as they are. Every committed Switch build had this, not an optimization.
  - The Vulkan renderer handles it with an sRGB swapchain (`gfx/vulkan/backend.cpp`, `set_tv_format`).
- **Found by comparing with the Vulkan renderer** on the same frames (see [Reference renders](#reference-renders-vulkan-and-gpu-headless)). In the shadowed cliff, tree and house area of title frame 600, average brightness was 40 (OpenGL) against 86 (Vulkan).
- **Fix:** `set_tv_format` records whether the TV format is sRGB (`R.tvSrgb`). The present shader then applies the sRGB encoding (the Switch's EGL window is not an sRGB surface), and `frame_N.png` dumps are encoded the same way. The same area is now 84 against Vulkan's 86. The log says `[gl] presenting with sRGB encoding (sRGB TV format)`.
- **Picture adjustments** (`env.txt`, applied by the present shader, neutral = 1): `WWHD_EXPOSURE` (scales the linear picture before encoding; below 1 tames bright sand and sky), `WWHD_CONTRAST` (S-curve around mid-grey, black and white fixed), `WWHD_SATURATION`, `WWHD_GAMMA` (above 1 deepens mid-tones). The shipped `build/switch/env.txt` turns on a "punchy" profile (0.85 / 1.3 / 1.1 / 1.0), with the original colours and a milder profile commented out. The values are logged at startup (`[gl] WWHD_CONTRAST=1.30`).
- Round 3's zero-padding of short uniform blocks stays: it is a real correctness fix, but it was not the cause of the darkness.

#### The GL thread (`WWHD_GL_THREAD=1`)

- switch-mesa 20.1 is linked statically. `_mesa_glthread_init` needs `st_manager::set_background_context`, which its EGL leaves empty; the worker thread calls it first.
- The slot's location comes from this build's `st_set_background_context`: `gl_context` + `0x22E88` → `st_context`, whose first field is the `st_manager`, which holds the callback at +24. The code checks the pointers before starting, and logs `[gl] GL thread started` or why not.
- The callback raises the worker to the render thread's priority and lets it use cores 0–2.
- switch-mesa's `eglSwapBuffers` flushes the driver from the calling thread, so `present()` waits for the GL thread first (`_mesa_glthread_finish`). This is the only per-frame wait; queries that would wait (the overlay's uniform locations, the GPU-busy fence) are cached or done right after it.
- libnx gives threads created without attributes a 128 KiB stack, and the worker runs the GLSL compiler for shaders first seen in game. The link wraps `pthread_create` (`-Wl,--wrap=pthread_create`, `core.cpp`) so such threads get 4 MiB.
- Untested: desktop Mesa does not run its GL thread on llvmpipe, so the headless build cannot exercise it. If the game crashes or misrenders with it on, remove the line.

#### Tests (headless, llvmpipe, title screen to frame 605)

- Frames 400, 500 and 600 match the round 2 build, including the skinned boat; no GL errors, no skipped draws.
- On llvmpipe the software rasterizer is ~700 of the ~860 ms/s, so fps does not change here. Our own measured stages (lookup, indices, resources, state) went from 130 to ~107 ms/s.
- `/tmp` was cleared between sessions, so the headless saves and shader cache were rebuilt: the first run compiles every shader and the title animation runs behind, later runs match the earlier timeline.

#### Reference renders (Vulkan and GPU headless)

- **OpenGL on the GPU:** the headless build runs on this PC's AMD GPU (radeonsi) instead of llvmpipe. Add `--device /dev/dri --group-add keep-groups --security-opt label=disable` to `podman run` and leave out `LIBGL_ALWAYS_SOFTWARE=1`. The title screen then runs at the game's 30 fps cap, and the driver is a hardware one.
- **Vulkan reference:** the Vulkan renderer (the macOS app's second renderer) builds for Linux with the SDL3 host and runs offscreen. It is the reference the OpenGL output should match.

  ```sh
  # image: wwhd-glhost + dnf install SDL3-devel vulkan-headers vulkan-loader-devel glslang glslang-devel spirv-tools-devel mesa-vulkan-drivers
  cmake -S . -B build/vulkan -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DWWHD_RENDERER=VULKAN -DCMAKE_BUILD_TYPE=RelWithDebInfo
  ninja -C build/vulkan wwhd
  # run with the GPU flags above and: -e SDL_VIDEO_DRIVER=offscreen -e SDL_AUDIO_DRIVER=dummy -e WWHD_HIDDEN_WINDOWS=1
  #   -e WWHD_DUMP_FRAMES=400,600 -e WWHD_EXIT_AT_FRAME=605
  ```

- Compare `frame_N.png` from both. The title animation partly follows real time, so compare regions (brightness of the same area), not whole-frame pixel diffs.

#### Profiling the headless build

`perf` works in the container once SELinux labels are disabled. A frame-pointer build in `build/headless-prof` gives call graphs:

```sh
podman run --rm -v $PWD:/src:Z -w /src wwhd-glhost bash -c 'cmake -S . -B build/headless-prof -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DWWHD_RENDERER=OPENGL -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  "-DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer" "-DCMAKE_C_FLAGS=-fno-omit-frame-pointer" && ninja -C build/headless-prof wwhd'
# run as in "Headless desktop reproduction", with an image that has perf installed (dnf install perf), adding
#   --security-opt label=disable --security-opt seccomp=unconfined
# and prefixing the binary with: perf record -e task-clock -F 1999 -g --call-graph=fp -o /run/perf.data
perf report -i perf.data --children --comm "GX2 render" --dsos wwhd --sort sym -g none --stdio
```

`apitrace` does not work with the headless build (it aborts inside libepoxy).

#### Reading the stats line (round 3)

```
[gl] 13.7 fps, 7350 draws/frame, 0 skipped, GL errors 0; render thread busy 863 ms/s (GX2 commands 21, flushes 0: 4 per
frame), GPU busy at 0% of swaps; draws 813 ms/s (lookup 14, indices 5, resources 40, state 48, submit 701; stream copies 7,
fence waits 0; 77% shader memo hits (+15% recent combinations), 86% texture cache hits, 140 feedback copies; ...),
clears/copies/invalidates 12, present 16 (waiting for the GL thread 0); streamed ...; 126 surfaces, heap ...;
CPU: render thread 812 ms/s, cores 85/97/60%
```

- **render thread busy**: wall time not spent waiting for commands. **GX2 commands** is the part outside draws, clears, copies, flushes and present (command dispatch and register writes); **flushes** is `GX2Flush`/`GX2DrawDone` (`glFlush`, a GPU submission on the Switch).
- **GPU busy at N% of swaps**: how often the previous frame's fence had not signalled when the next frame was presented. Near 0% means the GPU keeps up; near 100% means GPU-bound. This replaces the GPU lag, which needs a GPU clock that switch-mesa lacks.
- **stream copies** / **fence waits**: memcpy into the stream buffers, and waiting for the GPU to release one.
- **feedback copies**: snapshots of a render target that a draw also samples.
- **waiting for the GL thread** (with `WWHD_GL_THREAD=1`): the render thread waiting at present for Mesa's thread to catch up. High means the GL thread is now the limit.
- **CPU** (Switch only): the render thread's own CPU time (less than *busy* when it was preempted), and the load of cores 0–2, sampled by a small thread that visits each core every 5 s (Horizon reports idle time only to a thread on that core).

#### Hardware test of round 3 (`logs-switch/wwhd.log`, 2026-10-04, with `WWHD_GL_THREAD=1`)

| Draws/frame | fps | render thread busy (ms/s) | per draw |
|---|---|---|---|
| 500–700 (interiors, menus) | 28–29.5 | 640–770 | — |
| 1,200–1,400 | 24–27 | 840–875 | — |
| 2,000–2,800 | 19–22 | 760–870 | — |
| 4,300–5,000 | 15–17 | 795–880 | ~10.6 µs including the driver |

- **The GL thread did not start** (`GL thread: could not be started`); see round 4 for the cause.
- The cores were 30–85% busy (averages 40–60%), so there is spare CPU for a second render-side thread. `GPU busy` was 0–1%: the GPU is not the limit.
- Clears, copies and invalidates took up to 225 ms/s in mid-size scenes (1,200 draws), against 12 ms/s on the headless title screen. Round 4 times them separately.
- Uniform padding and the sRGB fix were in this build; the colours were confirmed fixed on hardware afterwards.

### Round 4: the GL thread for real, and our own per-draw code

**Why the GL thread failed.** Mesa creates its worker with C11 `thrd_create`, which in this build is newlib's. Newlib defines `thrd_success` as **4**, while Mesa's `u_thread_create` (compiled against newlib's header, but with Mesa's own check) treats any non-zero result as failure (`cmp w0, #0`). So the worker was created and running, Mesa believed it was not, and `util_queue_init` freed the queue under it: the GL thread never started, and that path was a use-after-free. The link now wraps `thrd_create` (`-Wl,--wrap=thrd_create`, `core.cpp`) to return 0 on success. Mesa is the only caller, and it only tests for zero. Also checked in the binary: Mesa 20.1's marshal for `glDrawElementsInstancedBaseVertex` queues the draw without waiting when the context is a core profile (`ctx->API == 3`), which ours is. The worker now runs at priority 0x2E, below audio and the render thread (0x2C) and above the game's threads (0x3B). The shipped `env.txt` turns it on.

**Profiling on the GPU.** The headless build now runs on this PC's AMD GPU, where Mesa runs its driver on its own thread (as the Switch should now), so the render thread's profile is our own code. On the title screen's 7,370-draw scene:

| Finding | Change |
|---|---|
| 44% of `draw()`'s own time was the state cache's `memcmp` of structs just written on the stack (store-forwarding stalls) | Field-by-field `operator==` for each cached state (floats compared as bits) |
| `upload_surface` (7%): `sparse_hash`, which read 256 sample words per mip level of every texture, every frame (a cache miss each) | 64 samples per level; textures changed in the last 64 frames are still checked every frame, others every 4th frame (staggered); full verification every 128 frames instead of 64. Invalidated textures are still checked fully at once |
| `shader_state_hash` (4%): one 8-byte multiply chain over ~190 words, twice per lookup | Four-lane hash (`hash_words4`) for the in-memory keys; the cache file keeps `hash_bytes` |
| Register writes in the uniform-constant range went through the per-register class check | Skipped: they never count for shader state |
| Each texture invalidate (200 per frame) walked all surfaces | `GuestRanges`: surfaces' guest byte ranges sorted, with running maximum ends; an invalidate visits only overlapping ones (1 ms/s for 200 per frame) |

Result on the GPU headless build (ms per second of render-thread time, 7,370 draws/frame at 30 fps): total 465 → 423, state 170 → 146, resources 81 → 69, lookup 55 → 45, texture checks 21 → 4. The picture matches the Vulkan reference (shadowed area 84 vs 86).

**New stats.** The old "clears/copies/invalidates" figure is split: `clears N (per frame)`, `surface copies N (per frame, on the CPU)`, `invalidates N (per frame)`, `scan copies N`. The next hardware log will show which one cost 225 ms/s.

**Expected on the Switch.** About half of the render thread there is Mesa and nouveau (state validation, `glDraw*`, flushes). With the GL thread those run on another core, and the render thread keeps our code plus Mesa's recording of the calls. Estimate: from ~10.6 µs to ~5–6 µs per draw on the render thread, which would put scenes up to ~5,000 draws near 30 fps if the GL thread keeps up.

#### Hardware test of round 4 (`logs-switch/wwhd.log`, 2026-10-04)

- `[gl] GL thread started` and `GL thread running (priority 0x2E)`: the GL thread works.
- Render thread busy fell from 800-880 to ~400-550 ms/s in heavy scenes; `waiting for the GL thread` stays at 7-15 ms/s.
- fps: 28-29 in light scenes, 20-26 at 2,000-4,600 draws/frame, 19-23 at 5,000-7,000.
- The render thread now idles half the time while fps stays in the 20s: the game's own CPU work became the limit (see round 5). The cores were 50-75% busy.
- Clears, surface copies and invalidates were all small (invalidates ~190/frame at ~1 ms/s).

### Round 5: the game's own code

**Where the game thread waits.** The engine's end-of-frame function (`f_0274C8C4`, from `02035118`) calls `GX2DrawDone` twice per frame: the CPU waits for the GPU to finish frame N before running the logic of N+1. Here the "GPU" is our render thread (`render_sync`), so logic and rendering alternate. Skipping the wait is not safe: the game rewrites vertex and uniform data and display lists for the next frame while the render thread may still read them. New stat: `game waited for it N ms/s (M times per frame)` in the `[gl]` line.

**Profiling real gameplay.** The headless build takes scripted input (`WWHD_SCRIPT_INPUT`, see below) and the user's save (`save/` at the project root, git-ignored), so the desktop runs reach Outset Island in Pro Controller mode (`WWHD_PRO_CONTROLLER=1`, now honoured by the headless build). With frame-number scripts the presses depend on timing, so the title presses repeat until the menus are past. The main game thread is 86% recompiled game code; our runtime is ~10% of it, and the game enqueues only ~85 GX2 commands per frame (the ~5,000 draws come from display lists), so queue batching was not worth doing.

| Change | Detail | Measured |
|---|---|---|
| **Condition-register liveness** (`tools/recomp/crlive.py`) | Each PowerPC compare wrote four CR bytes (lt, gt, eq, so) into the Cpu struct, ~9 host instructions; usually one bit is read. A backward liveness pass per chunk keeps only the bits read later; at a return only cr2-cr4 (callee-saved) count, a call reads cr1.eq (varargs) and passes the rest through, and every unknown exit (tail jump, other chunk, indirect jump, trap, mfcr, unimplemented instruction, instruction hook) reads all. `cr_set_*_m` / `cr0_rc_m` (ppc.h) store the live bits. | 161,086 writers; 153,894 now store one bit; CR byte stores 27% of before. Switch game code 44.5 → 37.9 MB |
| **Leaf functions keep registers in locals** (`tools/recomp/leaflocal.py`) | 16,398 functions that call nothing use C locals for r/f registers and, at a return, write back only what callers see: r1-r4, r13, r14-r31, f1, f2, f14-f31. Other exits write back everything used. Same C expressions; the scratch registers' dead stores go away. Registers a leaf only reads are never written back (the game's compiler does keep values in scratch registers across calls to leaves it knows leave them alone). | Main thread −3.3% on the desktop (x86 hides most of it) |
| **Native JAudio DSP loops** (`runtime/src/hooks_audio.cpp`, `hooks_perf.txt`) | The audio thread (`JASThread`) spent 85% in two loops: an 8-tap filter over 80 samples (`0281B4EC`) and a mix with a volume ramp (`0281B970`), each converting 16-bit integers to float through guest memory. Native versions compute the same values in the same order with ppc.h's `to_single`/`round25`/`ppc_fctiwz` | Audio thread −67% (21,442 → 7,111 samples) |
| Paired-single loads/stores | Always-inline float fast path; quantized formats out of line | — |
| `PPC_ENTER` on the Switch | No guest-trace check (a debug switch): one load and branch less per guest call | — |
| Stats | Game-thread wait per frame; per-guest-thread CPU on the Switch (`WWHD_SCHED_STATS=2`, Horizon thread ticks) | — |

**Verification.**
- `WWHD_RECOMP_CR_CHECK=1` generates a build where dropped CR bits hold a poison value (0x55) and every CR read aborts on it: 1,500 title frames and 3,700 frames of scripted gameplay (dock, rocks, swimming, the village beach), no read of a dropped bit.
- `WWHD_RECOMP_LEAF_POISON=1` generates a build where every scratch register a leaf assigns gets garbage at its return (0xDEADBEEF, NaN): the same gameplay runs normally (an earlier version that also poisoned registers a leaf only reads crashed at boot, showing the compiler's cross-call register use above).
- `WWHD_HOOK_CHECK=1` runs the original of each native audio loop on a snapshot and compares: 2.8 million calls of each over the gameplay script, 0 mismatches.
- Options to turn the passes off when generating: `WWHD_RECOMP_CRLIVE=0`, `WWHD_RECOMP_LEAF=0`. The verification harness (`tools/verify`) intercepts `psq_load`/`psq_store` by macro and does not see the `_l` forms leaves use.

**Desktop tools added.** Headless scripted input: `WWHD_SCRIPT_INPUT="300-304:A,400-404:RIGHT,1400-2000:LY=1"` (renderer frames; buttons A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT, sticks LX LY RX RY). Into gameplay with the save: A at 300, RIGHT at 400 and A at 460 (Pro Controller), then A every 40 frames to 1100. The headless name-entry keyboard does not answer, so a new game cannot be started headless. Every test container gets `podman run --timeout`.

#### Hardware test of round 5 (`logs-switch/wwhd.log`, 2026-10-04)

- Gameplay on Outset Island: 28.3-28.7 fps at 1,600-3,500 draws/frame, 21.6-22.8 fps at 6,400-7,400. The user reports a much more playable game.
- `game waited for it` ~63-65 ms/s in steady play; the main game thread ran 68-83% of the time (61-78% CPU).
- **Microstutters.** Every few seconds a frame drops the counter towards 18 fps, and the sound crackles at the same moment. In the 5 s stats the slow windows are the ones with bursts of shader translations: 23.1 fps with 820 new shader states (91 ms/s translating, ~0.5 ms each) and 47 ms/s of texture uploads; 25.3 fps with 322; 26.2 fps with 511. Nothing was compiled in those windows (the shader cache had every GLSL source): the time went into translating Latte programs into GLSL that already existed.

### Round 6: microstutters

| Change | Detail | Measured (desktop, scripted gameplay to frame 3,700) |
|---|---|---|
| **Per-stage shader keys** (`gfx/gl/shaders.cpp`) | The shader key held the registers of both stages (copied from the Vulkan renderer), so a pixel shader was translated again for every vertex format it was drawn with (`SQ_VTX_SEMANTIC_n`, `SPI_VS_OUT_ID_n`), a vertex shader for every color-buffer setup, and both for stale `SPI_PS_INPUT_CNTL_n` slots past the active count. Each stage is now keyed on the registers its translation reads (listed above `state_hash`). | Translations 7,169 → 2,564; translation time 269 → 56 (sum of ms/s) |
| **Texture units the program samples** | The key held the dimension and format of all 18 texture units; earlier textures stay bound in units a program never samples. After a program's first translation (the analyzer's `textureUnitMask`, which depends only on the code), its key holds only those units, and only what the GLSL reads of them (dimension, integer format). | (included above) |
| **Asynchronous log** (`core.cpp`) | `LOG` wrote and flushed to the SD card in the calling thread; the audio threads log too. Lines now go to a buffer that a writer thread flushes every 250 ms (1 MiB cap; fatal errors flush first). | — |
| **No GL-thread sync in the stats** | `glGetError` (a full wait for Mesa's GL thread) only with `WWHD_GL_DEBUG`. The heap walk (`mallinfo`, which holds malloc's lock and stalls every allocating thread) only with `WWHD_GL_DEBUG`, every 60 s. | — |
| **Stream buffer reuse without fences** (`backend.cpp`) | Reusing the streaming buffer created a `glFenceSync` at every wrap (another GL-thread sync, several per second). One fence per frame now; a wrap waits only if the GPU has not finished the frame that last used that part. | — |
| **One scheduler report** | With `WWHD_SCHED_STATS=2` the `[sched]` report also ran on the game's main thread at every swap. Only the timed report remains. | — |
| **Spread periodic checks** | The full program-hash check (every 64 frames) and the full texture check (now every 256 frames) were staggered by address bits, so many came due on the same frame. A hash of the address spreads them out. | — |
| **Deeper audio queue** (`audio_out.cpp`, `hle/ax.cpp`) | The AX frame thread kept 40 ms of sound queued, so any frame over ~40 ms emptied the device: an audible gap. 80 ms on the Switch (`WWHD_AUDIO_LATENCY_MS`, 20-300), and after a stall the queue refills up to 15% faster (was 5%). | — |
| **File access releases the core** (`hle/fs.cpp`) | `fopen` and `stat` on the SD card now release the caller's emulated core like reads do. Background music streams 160 KB every ~4 s during play. | — |

**Checking the shader keys.** `WWHD_GL_KEY_CHECK=1` translates again whenever the full register state (both stages, every texture unit and sampler) is new for a program whose shader came from the cache, and compares the GLSL. Over the gameplay script: 23,000 checks, 0 mismatches. Over the title screen: 3,000 checks, 0 mismatches.

**Hitch report.** Every frame over 55 ms logs a `[hitch]` line with what happened since the previous frame: render-thread time, draws, game wait, stream wraps, fence waits, shader translations and compiles, texture uploads, clears and copies, flushes, the game's file accesses, and the audio gap. The 5 s `[gl]` line ends with `files N (ms), audio gaps ms` and the hitch count. On the desktop, after this round's changes, the only hitches during play are the frame dumps; the others are menu and loading frames.

#### Hardware test of round 6 (`logs-switch/wwhd.log`, 2026-10-04)

- The intro's sound is much better (the deeper audio queue).
- **The picture froze** soon after reaching gameplay, while the camera was turned; no crash. The log ends at frame ~1,040 with no error: the last lines are the island loading in (hitches of 60-280 ms with 150-230 new shader states and 2-60 texture uploads each, and `GPU busy at 100% of swaps`).
- The log cannot show where it stopped: lines reach the SD card four times a second, and the writer thread ran at the game threads' priority.

**Watchdog (current build).** When no frame is swapped for 3 s, a watchdog thread logs `[watchdog]` lines (again every 15 s, at most 6 times) and writes the log out at once:
- the render thread: waiting for commands or executing, the command number, and the blocking step it is in, if any (waiting for the GL thread, `eglSwapBuffers`, a GPU fence for the stream buffer or for a frame, translating, compiling or linking a shader);
- every guest thread: core, priority, whether it holds its core, what it waits on (mutex, event, message queue... with the object's address), its `lr`, and its guest call stack from the stack back chain;
- each core's owner and the threads waiting for it.

Tested on the desktop with a forced 5 s stall of the game thread: the report names the main thread inside the frame-end function (`0274C8C4`, from `02035118`). The log writer now runs above the game threads (priority 0x2C). `WWHD_NO_WATCHDOG=1` turns the watchdog off.

#### Hardware test of round 6 with the watchdog (`logs-switch/wwhd.log`, 2026-10-04)

- No freeze (no `[watchdog]` line), no audio crackle (`audio gaps 0 ms` in every window after loading), a steadier frame rate.
- Two different limits, depending on the scene:
  - **The game's main thread** in most of the island: 23-27 fps at 3,500-5,000 draws/frame while `game waited for it` is only 15-55 ms/s, and the main thread runs 84-91% of the time. Its own code is the limit.
  - **The render thread** in the busiest views: 20-22 fps at 6,300-6,900 draws/frame, the game waiting 380-460 ms/s. There the render thread is "busy" 812-842 ms/s but runs on the CPU only 448-464 ms/s, and the three cores are ~48% busy: it is blocked about 400 ms/s below the GL calls.

### Round 7: the game's code on ARM, and where the render thread blocks

**The main thread.** A desktop profile of scripted gameplay (`perf --comm guest`) is flat: recompiled game code is 88% of the main thread, spread over 2,770 functions; the top 100 make 70% of it. The two hottest are leaf functions that compare floats (bounding-box tests). In the Switch build (GCC 15, ARM), each float compare took ~15 instructions: the CR bit stored into the Cpu struct, FPSCR's FPCC field read-modified-written, and explicit NaN tests. So the fixes are in the recompiler and apply everywhere:

| Change | Detail |
|---|---|
| **No FPCC update in float compares** (ppc.h `cr_set_f`) | fcmpu/fcmpo also set FPSCR[FPCC], which only `mffs` and `mcrfs` read; the game has neither (ppc2c now warns if a game does). lt/gt/eq need no NaN test (IEEE comparisons with a NaN are false); `un` uses `__builtin_isunordered`. |
| **Leaf functions keep CR bits in locals** (`leaflocal.py`) | Like the r/f registers: compares write C locals (ppc.h `crl_*` helpers) and a return writes back only cr2-cr4 (callee-saved). Functions that use the CR as a whole (mfcr, mtcrf, stwcx.) keep it in the struct. 4,980 leaves. |
| **Indirect-call cache** (ppc.h `PPC_ICALL`) | 14,947 `bctrl`/`blrl` sites. Each kept one 64-bit entry (guest address, host function as a 32-bit offset): a hit skips the 32 MB dispatch table, a cache miss per lookup (`dispatch::lookup` was 2.7% of the main thread on the desktop). The table never changes once filled. |
| `mfcr` | Packs the 32 CR bytes with four multiplies instead of a 32-step loop (1.5% of the main thread on the desktop, not inlined). |
| Switch code generation | Game code built with `-fomit-frame-pointer`; `g_core_preempt` (read at every function entry) has hidden visibility, so it is read directly instead of through the GOT. |

Result: the hottest function went from ~150 to ~50 ARM instructions; Switch game code 37.9 → 36.7 MB. On the desktop (clang, x86, where these compares were already cheaper) the main thread used 4.7% fewer CPU samples over the same gameplay.

Checks: `WWHD_RECOMP_CR_CHECK=1` (dropped CR bits poisoned; leaves now check their locals through `ppc_crl_read`) and `WWHD_RECOMP_LEAF_POISON=1` builds play the 3,700-frame gameplay script with no poisoned read; the gameplay picture matches.

**The render thread.**
- The per-draw stage timers read the clock ~12 times per draw (a fifth of the render thread on the desktop). They now time one draw in 16 and scale it (`SampledTime`). Desktop, 7,400-draw scene: render thread busy 472 → 398 ms/s, game wait 35 → 10 ms/s. The stage figures in the stats are now estimates from that sample.
- **Why it blocks on the Switch.** The switch-mesa 20.1 library was disassembled: every GL call the draw path makes is queued to Mesa's GL thread without a wait (in a core-profile context even the indexed draw), so no per-draw call forces a sync. But Mesa 20.1 queues at most four 8 KB batches; when its GL thread (Mesa's state handling and nouveau's command building) or the GPU behind it falls behind, the render thread waits for a free batch.
- **Mesa probe** (`gfx/gl/mesa_probe.cpp`). The link wraps Mesa's and libdrm_nouveau's internal functions (`-Wl,--wrap`) and times them. A second `[gl]` line every 5 s, `Mesa per second: ...`, gives: batches handed to the GL thread and how long the render thread waited for them; synchronous calls (by GL function name) and full waits; and on the GL thread, nouveau submissions to the GPU, waits for command-buffer space, and fence and buffer waits (time spent waiting for the GPU).

#### Hardware test of round 7 (`logs-switch/wwhd.log`, 2026-10-04)

- Better overall; still ~20 fps in some views. The frame rate depends on where the camera looks: in the forest at the top of Outset (where Tetra fell), facing the fairy-fountain rock gives 30 fps, facing the middle of the forest from the same spot ~20 fps.
- The Mesa probe answers round 7's question. In the 20 fps views (~7,000 draws/frame):
  - the render thread waits ~350 ms/s for a free GL-thread batch;
  - the GL thread waits ~340 ms/s in `nouveau_pushbuf_space` (~100 calls/s, ~3.4 ms each): the GPU command buffer is full and the GPU has not consumed the previous ones;
  - submissions themselves cost 2 ms/s, and there are no fence or buffer waits; the only regular synchronous calls are the per-frame fence (`FenceSync`, `GetSynciv`).
- So in those views the GPU is behind on the command stream: ~100 command buffers per second, ~2.5 MB of commands per frame, ~360 bytes per draw.

### Round 8: what the GPU is given, and the game code's layout

**Shader constants in uniform buffers (`WWHD_GL_UNIFORM_BLOCKS`, on by default).** 62% of the game's shaders read their constants through Cemu's "remapped" loose uniform array (`uf_remappedVS/PS`, filled with `glUniform4iv`), a few through the full register file. Mesa's nouveau driver does not read loose uniforms from memory: whenever one changes (almost every draw: per-object matrices), it copies the program's whole default uniform block into the GPU command stream. Now each stage's loose uniforms are compiled into one std140 uniform block (`ufBlockVS` at binding 30, `ufBlockPS` at 62; the game's own blocks use 0-15 and 32-47), filled from the stream buffer: binding it is a few command words, and the GPU reads the values from memory.
- The decompiler's text is unchanged (and so are the shader cache's keys): `compile()` rewrites the OpenGL branch of the uniform section into the block, and `link()` reads the members' offsets back from the program. Shaders using more than 12 of the game's blocks would keep loose uniforms (none do: at most 4).
- 367 of the 555 cached shaders take the block form. Desktop renders: two runs with blocks give identical frames; blocks against loose uniforms differ in 11 pixels by more than 8 levels (two runs with loose uniforms differ in 41,000, from the title's real-time animation). Gameplay renders correctly.

**Hot game code together (`tools/recomp/hot_functions.txt`).** A call-graph profile of the main thread has no dominant subsystem (actor logic ~56%, drawing ~26%, spread over thousands of functions), but the code that runs is small: in scripted gameplay 500 functions make 88% of game-code time (1.4 MB), all 2,827 sampled ones 4.4 MB, out of 35 MB. The list (gameplay and title runs, hottest first, 2,977 functions) makes recomp.py emit those functions into `code_hot_*.c` in that order; in the Switch binary they now span 4.5 MB instead of being spread over 35 MB (fewer instruction-cache, TLB and L2 misses on the A57). `WWHD_RECOMP_HOT=0` turns it off. Functions not in the list stay where they were.

**Session logs.** Each session writes `wwhd.log` and the same lines to `logs/wwhd_<date>_<time>.log` (console clock at startup; the user's console reads January 2025), so the newest file in `logs/` is always the latest session; the newest 30 are kept. Both start with `[session] <date> <time> (build ...)`. A `wwhd.log` from an older build is moved to `logs/wwhd_earlier_build_<n>.log` first. (The first version of this round moved the previous session's `wwhd.log` into `logs/` at startup, which made the newest file there the session before the last one: `wwhd_undated_1736954247.log` was the round 7 build's log.)

#### Hardware test of round 8 (two sessions, 2026-10-04: uniform blocks on, then `WWHD_GL_UNIFORM_BLOCKS=0`)

| Forest view, ~6,970 draws/frame | blocks off | blocks on |
|---|---|---|
| batches to Mesa's GL thread per second | ~6,550 | ~1,350 |
| GPU command buffers filled per second | ~102 | ~30 |
| render thread waiting for batches | ~350 ms/s | 20-120 ms/s |
| presentation waiting for the GL thread | ~10 ms/s | 320-430 ms/s |
| fps | 20.5 | 19-20 |

- The uniform blocks cut the GPU command stream to a third, and island views that were ~20 fps reached ~23 and more. The forest did not move: the render thread now finishes each frame early and waits at presentation (`_mesa_glthread_finish`) for Mesa's GL thread instead of waiting for batches during the frame. Command-buffer waits are now small, so the GPU is not the limit there: Mesa's own per-draw work on its single GL thread is (Mesa 20.1's state validation for each of ~7,000 draws a frame on the A57).

### Round 9: fewer driver draw calls, and leaner game code

**Per-draw state changes (new stats line `GL state changed per draw`).** On the desktop, in the 7,400-draw title scene: shader-constant block 92-96% of draws, vertex buffers 21-25%, program 6-7%, textures 2-3%; in gameplay: constants 91-94%, vertex buffers 37-46%, program 18-28%, textures 11-15%. Most consecutive draws differ only in their constants: the same mesh drawn again with other matrices (foliage, rocks).

**Multi-draw batching (`WWHD_GL_BATCH`, on by default, needs the uniform blocks).**
- Such runs of draws go to the driver as one `glMultiDrawElementsIndirect` (or `glMultiDrawArraysIndirect`); the switch-mesa 20.1 library queues these without a wait in a core-profile context (disassembled like the other calls).
- Each stage's constant block is an array of per-draw structs (`WwhdUfVS wwhd_ufVS[n]`, `n` = 64 KB / struct size, at most 256), and `#define`s turn each Cemu uniform into the current draw's member. A draw's index is its `baseInstance`: the vertex shader reads it from attribute 15, fed by a buffer of 0, 1, 2... whose divisor is so large that instancing never advances it, and passes it to the pixel shader in flat varying 14. The game uses attributes 0-6 and varyings 0-9. Every shader gets this plumbing, so any vertex shader links with any pixel shader.
- The draw path records draws instead of issuing them. Every GL state call in the state code goes through `FLUSHED(...)`, which issues the recorded draws first, so recorded draws always share the current state. Texture uploads, feedback copies, clears, surface copies, invalidations, scan copies, presentation, frame dumps and `forget_gl_state()` (shader links, deleted textures) also flush first. Instanced draws go alone (their `baseInstance` must be 0).
- Desktop, title scene: driver draw calls 22% of draws (4.5 times fewer), constant-block rebinds 37% of draws (was 96%). Gameplay: driver draw calls 39-53% of draws. Frame 400 with batching against without: 983 pixels differ, none by more than 8 levels (two unbatched runs from different sessions: 1,481). Gameplay frames render correctly.

**Vertex rebasing (`WWHD_GL_VERTEX_REBASE`, on by default, with batching).** A draw with one vertex buffer gets its data at a multiple of the stride in the stream buffer and binds the buffer at offset 0; the difference goes into the draw's base vertex (part of each indirect command). Consecutive draws of different meshes then keep the same binding and stay in one multi-draw. Desktop: driver draw calls in the title scene 22% → 7% of draws (14 times fewer than draws), gameplay 51-53% → 43-44% (what still breaks batches there: program changes on 34-36% of draws, textures 19-20%, vertex formats 16-18%). Frames render correctly (the title frame differs from earlier runs only by the title animation's timing: edges shifted all over the picture).

**Registers in locals in functions with calls (`tools/recomp/leaflocal.py` `transform_nonleaf`, `WWHD_RECOMP_NONLEAF=0` turns it off).** GCC's ARM code for functions that call others stored every guest-register assignment into the Cpu struct and read registers again after each guest memory store (it does not trust `__restrict` across the byte-wise guest stores). 23,200 such functions now keep registers in C locals, like the leaves:
- A forward analysis over each function's jumps tracks which registers may differ from the struct; a call writes back only those (the callee may read any register), an exit only those its caller can see. Edges entering a loop write back first, so registers set up before a loop are not written back again at every call inside it.
- After a call, the volatile registers the function uses (r0, r3-r12, f0-f13) are read again. The callee-saved ones (r1, r14-r31, f14-f31) are not on the Switch (`PPC_KEEP_R/F` in ppc.h); desktop builds read them again, for save states that replace a thread's registers while it waits inside a call.
- Calls into the runtime (`imp_*`), instruction hooks (`site_*`), traps, and the 559 functions recomp.py finds changing callee-saved registers on purpose read every register again. Those are the compiler's save/restore helpers (stores and loads through r11; the save helper also puts the return address in r31) and the frame helpers that push or pop the caller's stack frame: any function that does not both push and pop a stack frame and sets a callee-saved register, itself or in the code it falls into.
- Checks: `WWHD_RECOMP_NONLEAF_CHECK=1` builds abort if a callee changes a callee-saved register. The first two runs found the save helper (r31 = return address) and the frame helpers; with the rule above, the gameplay script (3,700 frames) and 1,500 title frames run clean. `WWHD_RECOMP_LEAF_POISON=1` now also poisons the volatile registers that functions with calls leave unwritten at their returns: the gameplay script runs normally.
- Hot functions: 5-8% fewer ARM instructions (`f_02759564` 544 → 507, `f_0246C088` 1,670 → 1,535), more inside loops; Switch game code 36.7 → 34.1 MB together with the next change. On the desktop (clang, x86, reading the callee-saved registers again) the main thread's CPU samples fell only 0.6%.

**Preemption check in the Cpu struct.** Every function entry read `g_core_preempt[c->core]` (two dependent loads). The scheduler now sets a `preempt` byte in the Cpu of the thread holding the core (in what was padding: the struct's layout and save states are unchanged); the entry check is one byte load.

**Stats.** The `CPU:` part of the `[gl]` line now includes `Mesa GL thread N ms/s` (its own CPU time, from Horizon's thread ticks); `GL state changed per draw` ends with the driver draw calls as a share of draws and the number of multi-draws.

#### Hardware test of round 9 (`logs-switch/`, 2026-10-04: defaults, `WWHD_GL_BATCH=0`, `WWHD_GL_VERTEX_REBASE=0`)

- With batching (defaults, and with rebasing off) the forest is a solid 30 fps; with `WWHD_GL_BATCH=0` it is as before (19-22 fps at 6,300-7,400 draws, the render thread waiting 300-500 ms/s for the GL thread).
- What remains is the view from the top of the island (the whole island and the pirate ship): 20.1-21.8 fps at 5,500-6,600 draws. There no thread is saturated: render thread 625 ms/s (all on the CPU), Mesa GL thread 375 ms/s, the game waiting for the renderer only 16 ms/s, no GPU waits, presentation 12 ms/s; the main thread holds its core 70% of the time (63% on the CPU).
- So the frame takes ~35 ms of main-thread work, just over two vblanks, and the frame rate is quantized: the game sets swap interval 2, a frame flips at the first vblank after its swap and at least two vblanks after the previous flip, and the game's flip wait (`f_0274C874`: `GX2WaitForVsync` then `GX2GetSwapStatus` until flips == swaps) only sees the flip at a vblank. A 35 ms frame costs three vblanks: 20 fps (the Wii U behaves the same way).

### Round 10: frame pacing and the main thread's core

**Relaxed vsync (`WWHD_RELAXED_VSYNC`, on by default on the Switch, off on the desktop).** A rendered frame flips once a whole swap interval has passed since the previous flip, on a vblank or not; a frame on time flips at that moment as before (so never above 30 fps), a late one as soon as it is rendered. `GX2WaitForVsync` returns at once if a flip happened since its previous return (the late frame often flips before the game asks), and while a flip is pending it checks every millisecond instead of sleeping to the vblank; without a new flip it waits for a real vblank as before. The game's logic runs per frame, so a slow frame now costs a little speed instead of a third of it.
- Desktop test with `WWHD_DEBUG_FRAME_MS=n` (adds n ms of work to every frame on the game thread), scripted gameplay: no extra load 30 fps either way (never above); +24 ms 30 fps either way; +34 ms (frames just over two vblanks) 20.0 fps with the hardware timing, 29.1 fps relaxed. The 300-frame `[gx2]` line counts the frames flipped before their vblank.

**Core layout (`WWHD_CORE_LAYOUT`, on by default on the Switch).** The game's main thread (emulated core 1) gets host core 1 to itself; the render thread, Mesa's GL thread and the guest threads of emulated cores 0 and 2 run on cores 0 and 2 (`host::place_thread`). The higher-priority host threads no longer preempt the main thread on its core.

#### Hardware test of round 10 (`logs-switch/wwhd.log`, 2026-10-04)

- The top of the island now reaches ~28 fps (was 20). The 300-frame `[gx2]` lines show 190-290 frames of 300 flipped before their vblank in the heavy views: frames just over 33 ms.
- The main thread is now the only limit there: it holds its core 93-98% of the time, but runs on the CPU only 82-84% of it, while host core 1 is ~15% idle. Waiting for the render thread (`GX2DrawDone`, the only wait inside the runtime that keeps the core) is ~2% of that gap.

### Round 11: the main thread's core

- **Service threads off core 1.** `host::raise_thread_priority` (audio output, AX frame, scheduler tick every 0.5 ms, watchdog, the threads `host::start_thread` creates) and the log writer now also keep to cores 0 and 2 with `WWHD_CORE_LAYOUT`, and so does the boot thread.
- **One wake per core release.** Every time a thread gave up its emulated core (the main thread does at each wait, many times a frame), all threads waiting for that core were woken (`notify_all`) to find it was not their turn; with the core layout they all sat on the main thread's host core. Each thread now waits on its own condition variable, and a release wakes only the thread that runs next (the 2 s timeout stays as a safety net). Gameplay and title runs: no thread waiting >10 s, no audio gaps.
- **Experiment: small hot leaves inlined (`WWHD_RECOMP_INLINE=1` when recompiling, off by default).** recomp.py emits an always-inline copy (`fi_X`, `inline_leaves.h`) of each hot leaf of at most 24 instructions that ends in a plain return (437), and the hot callers use it (1,886 call sites). It removes the call overhead and lets GCC forward the register traffic between caller and callee, but the hot code grows from 4.5 to 5.2 MB; on the desktop the main thread did not change. Shipped as a second build, `wwhd_inline.nro`, for a comparison on the Switch. The log's `[boot] recompiled code:` line names the recompiler options.

#### Hardware test of round 11 (`logs-switch/wwhd.log`, 2026-10-04, default build, GPU at 921 MHz)

- Top of the island: 25.8-26.5 fps at 6,450-6,530 draws. The main thread holds its core 97-98% of the time but runs on the CPU only 79-81%, and host core 1 (its alone) is ~20% idle: it is blocked inside the runtime ~18% of the time while keeping its emulated core. Waiting for the render thread (`GX2DrawDone`) is ~2% of it. Render thread 820 ms/s, Mesa GL thread 470 ms/s; cores 0 and 2 up to 97%.
- Target from now on: GPU at most 768 MHz (the safe clock on every Switch model), CPU 1,785 MHz.

### Round 12: where the main thread waits, and less code per call

- **Main-thread sampler (`WWHD_MAIN_SAMPLER=1`, on in the shipped env.txt).** The `HLE(...)` macro now wraps every runtime function with `HleMark`, which, on the game's main thread only, records the function's name in `g_main_hle` while it runs. A sampler thread (cores 0/2) reads it 1,000 times a second and logs every 5 s `[main] in runtime calls N% of the time: <function> x%, ...` — waits that keep the emulated core included, so the next log names the call behind the ~18%. Desktop gameplay: `GX2WaitForVsync` ~55% (idle), `OSSendMessage` ~2% (the main thread waiting for room in a full message queue), `GX2DrawDone` ~1%.
- **Return-address stores left out (`PPC_SET_LR`, `WWHD_RECOMP_LR=0` turns it off).** Every `bl` stored its return address into `c->lr` (one store and usually two instructions to build the constant). The recompiled code returns with C returns, so the value only reaches the guest stack through the callee's `mflr`, for stack walkers. Direct calls to ordinary game functions now use `PPC_SET_LR`, nothing on the Switch (desktop builds keep the store: save states compare saved return addresses). Calls into hooked functions (hooks read `c->lr`), the runtime, indirect calls and `bl $+4` keep it. Switch game code 34.1 → 32.5 MB; hottest function 1,533 → 1,470 instructions. On the Switch, the watchdog's guest stack traces can show stale return addresses. The `WWHD_RECOMP_NONLEAF_CHECK=1` build now also leaves the stores out (`PPC_ELIDE_LR`): gameplay script (3,700 frames) and 1,500 title frames run clean and render correctly.
- `wwhd_inline.nro` rebuilt with these changes (the default build was the one tested).

#### Hardware test of round 12 (`logs-switch/wwhd.log`, 2026-10-04, GPU at 768 MHz)

- Outset, past the trees, looking at the sea: 26.7-27.3 fps at 6,600-6,700 draws. GPU waits stay small at 768 MHz (`buffer full` ~3 ms/s): the GPU is not the limit.
- The sampler names the wait: `OSSendMessage` takes 16.6-19.1% of the main thread's time there (`GX2DrawDone` ~2%), matching the gap between holding its core (98%) and running (82-83%).
- Desktop count: the main thread sends ~24,000 messages a second (~800 a frame), mostly to the sound thread (`nw::snd::SoundThread`, queue `104B7EE8`, 32 entries) and to `1602A1E0`; ~1,700 sends per 5 s find the queue full.

### Round 13: the message queues

- **Lock-free object lookup.** Every OSMutex, OSEvent and OSMessageQueue call first looked its host object up in a table guarded by one mutex (`ObjTable`), shared by all guest threads. The audio threads use queues constantly on host cores 0 and 2, where the render and GL threads (higher host priority) preempt them, so the main thread waited for the table's mutex while keeping its core. Lookups now go through a lock-free cache of the table (entries are only added; a re-initialized object keeps its old copy allocated).
- **Targeted wakes.** A send woke every waiter of the queue, receivers and blocked senders alike (`notify_all`, a kernel call each, then all of them taking the queue's mutex). Receivers now wait on `notEmpty`, senders on `notFull`; a call wakes one thread of the other side, only if one is waiting, and a send does not wake a receiver again while a wake is on its way (a receiver that finds the queue empty again clears that mark when it goes back to sleep; one that finds more messages and another receiver asleep passes the wake on).
- **Audio threads above the render thread (Switch, `WWHD_CORE_LAYOUT`).** Guest threads of priority <= 8 (JASThread 2-3, nw::snd 3-4) get host priority 0x2B, above the render (0x2C) and GL (0x2E) threads: they are light and mostly wait for messages, and drain the sound queue at once instead of letting it fill.
- Desktop gameplay: `OSSendMessage` 2.0% → 1.4% of the main thread (the rest: waiting for room in a full queue); gameplay and title runs without stuck threads or audio gaps, gameplay renders correctly.
- The log's `[boot] recompiled code:` line now ends with `runtime: round 13 (...)`, to tell builds apart. The inline-leaves build is no longer shipped.

#### Hardware test of round 13 (`logs-switch/wwhd.log`, 2026-10-04, GPU at 768 MHz)

- No better: 25.6-27.4 fps at the top of the island (6,300-7,800 draws), `OSSendMessage` still 19-23% of the main thread, main thread `run` 97-99% / `cpu` 78-80%. So the lock was not the main cost: almost every send found the sound thread asleep (it handles one message and waits again), and the kernel call that wakes it costs several µs on the Switch, ~800 times a frame.
- Turning the camera after loading a save gives a lag spike once, then not again: the `[hitch]` lines show 200-230 shader translations in those frames (41-55 ms) plus 4-9 texture uploads (17-24 ms). Nothing was compiled (the GLSL was cached); the translations were redone because each session translated again the first time a view needed a variant.

### Round 14: one wake per batch of messages, and translations kept between sessions

- **Deferred wakes (`WWHD_DEFER_WAKES`, on by default).** A send to a queue whose receiver sleeps only marks the queue (`wakeDeferred`, `g_deferred_wakes`); the scheduler tick (every 0.5 ms, on cores 0/2) and any thread about to block (`block_begin`: a thread waiting for a reply to what it sent) deliver one wake for everything queued meanwhile. A queue half full is woken at once, so it never fills because of the delay. The sound thread sees its commands up to 0.5 ms later; the main thread makes almost no wake calls. Desktop: gameplay and title runs without stuck threads or audio gaps.
- **Saved translations (shader cache record 3).** After a translation, what a draw uses of it goes into `shadercache_gl.bin` next to the GLSL: key, program hash and its texture-unit mask, GLSL hash, register count, colour-output mask, texture-unit list, sampler assignments, depth-compare flags, the resource mapping, and the remapped-uniform lists. At startup these become ready shaders (`load_translation`), so a view seen in an earlier session needs no translation. Desktop, scripted gameplay twice with the same cache: first run 2,464 translations; second run 2,464 loaded, 0 during play; frames equal within noise (4 pixels differ by more than 8 levels). The cache file grows by ~0.5 KB per translation (0.85 → 2.1 MB here). An older build reading the file stops at the first record 3 and rewrites it without the rest (nothing is lost but the saved work).
- The log's `[boot]` line ends with `runtime: round 14 (...)`; `[gl] shader cache: ... N translations loaded`.

#### Hardware test of round 14 (`logs-switch/wwhd_2026-10-05_00-27-32.log`, GPU at 768 MHz): worse

- **Stutters everywhere a new view appears.** Each new translation now cost ~4.2 ms instead of ~0.2 ms (frame 554: 87 translations, 389 ms; round 13, frame 2033: 208 in 41 ms). The translation record was written and flushed to the SD card from the render thread for every translation (`cache_write`'s `fflush`), and an SD write costs milliseconds on the Switch. The session still needed ~550 translations (2,737 loaded from the earlier session): new variants of the views the earlier session did not show.
- **Top of the island: 27.2-27.3 fps** (round 13: 25.6-27.4). `OSSendMessage` 11-12.5% of the main thread (was 19-23%), main thread `run` 98% / `cpu` 87% (was 78-80%).
- **Where the messages go (desktop trace, `WWHD_TRACE_MSG=1`):** ~95% of the main thread's sends go to queue `3B7B7D1C` (32 entries) of the game's **`update_ubo`** thread (guest priority 18), not to the sound thread (~340 messages a second). The game's code (`f_027F9AE0`) hands it a uniform-buffer job with a non-blocking send and, **when the queue is full, runs the job itself on the main thread**: ~1,850 of ~22,000 sends a second on the desktop with deferred wakes, ~1,370 without (then ~3,750 wake calls a second instead of ~800). So the deferred wakes traded wake calls for jobs on the main thread.

### Round 15: the regression fixed, and the main thread's message sends

- **Shader cache written in the background.** Cache records go to a buffer; a writer thread (cores 0/2) writes and flushes it about once a second, in order (`cache_writer`). The render thread never waits for the SD card. The log has `[gl] shader cache: saved N KB in M ms (writer thread)` for the first 40 writes (the SD card's cost per write). A session that ends within a second of a translation loses only that record. Desktop, gameplay twice with the same cache: run 1 2,464 translations saved (file 2,102,529 bytes, the same as round 14), run 2 2,464 loaded and 0 during play; frames equal.
- **Message queues without malloc.** A queue's messages sit in a ring of its capacity, allocated at `OSInitMessageQueue` (the `std::deque` allocated or freed a block every 32 messages, and malloc's lock is shared with Mesa's threads). The deferred-wake list is drained in place (swapping it out made the next send allocate again).
- **Queue locks spin before sleeping.** A thread that finds a queue's mutex taken retries 256 times (`yield`) before blocking in the kernel (a contended lock and its hand-over were two kernel calls on the Switch, with the main thread keeping its core).
- **`update_ubo` above the render thread (Switch, `WWHD_BOOST_THREADS`, default `update_ubo`).** It gets host priority 0x2B like the audio threads: the render and GL threads can no longer preempt it while it holds its queue's lock (the main thread waited for it with its core) or while the queue fills. The log says `[thread] "update_ubo" runs above the render thread`. An empty value turns it off; other thread names can be listed.
- **Counters (with `WWHD_MAIN_SAMPLER`):** every 5 s `[main] OSSendMessage per second: N sends; queue lock taken (ms), queue full: waited (ms), not sent (= jobs the main thread ran itself); receiver woken at once (ms), wakes deferred`. The 5 s `[gl]` line counts translations of programs seen before (`new states (N of programs seen before)`): a high share means new variants of known views, not keys lost between sessions.
- Desktop gameplay (3,700 frames, GPU): 30 fps, no audio gaps, no translations with the saved cache, picture equal to round 14; the threads reported waiting > 2 s are idle loaders and workers waiting for their next job (as with `WWHD_DEFER_WAKES=0`).
- The log's `[boot]` line ends with `runtime: round 15 (...)`.

#### Hardware test of round 15 (`logs-switch/wwhd_2026-10-05_01-13-38.log`, GPU at 768 MHz): better

- **Top of the island: 27.9-28.8 fps at 6,500-6,800 draws** (round 14: 27.2). `OSSendMessage` 0.8-1.0% of the main thread (was 11-12.5%); the main thread now runs on the CPU all the time it holds its core (`run` 95-97% / `cpu` 95-97%; the gap was 11%). It is purely CPU-bound there: ~45,000 sends a second, the queue lock taken ~5,500 times (2 ms/s), and 5,400-9,800 `update_ubo` jobs a second run on the main thread because the queue was full.
- **Camera turns where the cache already has the view: no translations** (3,285 loaded). The turn hitches left are first-time texture uploads (7-8 uploads, 16-31 ms). A new area still translates at ~0.2 ms each (238 in 43 ms).
- The SD card's cost, from the writer thread: 2-20 ms per write of 0.5-5 KB (what the render thread paid per translation in round 14).

### Round 16: less work for the render thread (not yet tested on hardware)

The round 15 log shows both threads at their limit at the top of the island: the main thread runs 95-97% of the time, and the render thread is busy 880-905 ms/s at 28.5 fps (~945 would be needed for 30), copying 320 MB/s of vertices (31.7 MB a frame) and spending 3.4-4.7% of the main thread's time in `GX2DrawDone` waits. A desktop profile of the current build (scripted gameplay, `perf`): render thread 22% in `memmove`, 15% decoding GX2 commands, ~14% in the per-draw shader lookup; main thread spread over game code (the paired-single matrix library at `028E8D4C`-`028E9108` the largest group, ~7%).

- **Vertex range trimming (`WWHD_GL_VERTEX_TRIM`, on unless =0).** A draw copied its vertex buffer from vertex 0 up to the highest index it uses. A model's parts share one vertex buffer and each draws its own range, so every part copied all parts below it again, and the copy made for an earlier part was too short to be reused. Draws with vertex rebasing (97-99% of them) now copy from their lowest vertex (`IndexList::minIndex`, found in the same pass as the highest); the base vertex becomes slice / stride - first vertex (negative for GL when the first vertex lies past the slice's start). Desktop gameplay: streamed data 242 → 110 MB/s, vertices 20 → 11.6 MB a frame, stream copies 43 → 23 ms/s, render thread ~5% less busy; geometry correct.
- **The GamePad picture skipped (`WWHD_GL_SKIP_GAMEPAD`, on unless =0).** With the Pro Controller too, the game draws the Wii U GamePad's picture every frame into two 854x480 buffers and hands them to `GX2CopyColorBufferToScanBuffer(target 4)`, which the Switch never shows (`copy_to_scan` ignores it). A color buffer that went to the GamePad within 60 frames, never to the TV, and was never read by any other draw or copy is GamePad-only (also a texture only GamePad draws sample: none in this game); its draws and color clears are skipped. Outset: 26-48 draws a frame, 3-6 ms/s of the desktop render thread. The TV picture is pixel-identical (four runs, on/off pairs at frames 1500 and 2500: 0 pixels differ where the runs reached the same game state). The log has `[gl] GamePad picture: N draws/frame ..., skipped`.
- **Staged GX2 commands (`WWHD_GX2_STAGING`, on unless =0).** ~45,000 commands a frame went one by one through the queue's mutex. The guest threads of emulated core 1 (the main thread and the Prepare Thread, the only ones that issue commands) now collect them in a buffer of their own and hand them over in blocks of up to 1,024 words, at once when the render thread is idle, at present/flush/fence commands, and whenever the thread gives up its emulated core (`core_release`; two threads of one core never run at once, so the order is kept). Desktop: within noise (main thread 43.1 vs 43.5%); on the Switch the locked operations cost more.
- **The render thread's watchdog counter.** It was incremented with a locked instruction for every command, next to the queue's mutex and vectors that the main thread writes for every GX2 call: ~12% of the render thread's samples on the desktop were on that instruction. The counters now have a cache line of their own, and the count is a plain load and store.
- **Batched draws' constants** are written without comparing each 16-byte entry first (a batched draw's copy is always taken).
- The 300-frame `[gx2]` line counts GX2 commands per frame. `[boot]` ends with `runtime: round 16 (...)`.
- Desktop gameplay with everything on (3,700 frames): 30 fps, no audio gaps, no GL errors, renders correctly.

**Tried and dropped:**
- *Vertex and uniform data kept across frames until the game invalidates it.* The game calls `GX2Invalidate(0xF, 0, 0xFFFFFFFF)` (everything) twice a frame and targeted invalidates only while loading, so nothing could stay; checking the data instead costs as much as copying it.
- *Moving the jobs the main thread runs itself (`update_ubo` queue full) to the worker*: the job body is ~0.05% of the main thread on the desktop; at most ~2% on the Switch, with a backlog risk. Not done.

### Round 17: cheaper float math, fewer lookups and kernel calls (not yet tested on hardware)

Both threads stay the limit at the top of the island. A frame-pointer profile with call graphs (desktop, `build/headless-prof`, `perf record -g`) and the Switch's own code (devkitA64 objdump of the generated files) gave these:

- **Single-precision tracking in the recompiler (`WWHD_RECOMP_SINGLE`, on unless =0 when generating).** `fmuls`, `fmadds` and the paired-single multiplies round their multiplier operand to 25 bits (`round25`, the Espresso's behaviour). On the Switch that moved every such operand from a floating-point register to an integer register and back (five instructions, two of them slow register-file crossings). A value that is already single precision (from `lfs`/`psq_l` or a single-precision result) is unchanged by `round25`, so `recomp.py` now follows which register halves hold such values through each function (forward dataflow over its branches, intersected where paths meet; f14-f31 survive calls except to the register save/restore helpers and hooks; instruction hooks and unreachable code start empty) and `ppc2c.py` leaves `round25` out for them: 16,022 of 35,886 multiplier operands. The paired-single matrix library (`028E8D4C`-`028E9108`, the largest group of the main thread's time) gets about half as long on the Switch: the 4x3 matrix product 803 → 411 instructions, others 296 → 120, 134 → 55, 189 → 90. Results are identical by construction (`round25` is the identity on single-precision values, including NaN, infinity and denormals); the checking build (`WWHD_RECOMP_SINGLE_CHECK=1`: every operand left as it is goes through `ppc_single_check`) ran the gameplay script (3,700 frames) and the title flyover without a single report.
- **Static GQRs (`WWHD_RECOMP_GQR`, on unless =0 when generating).** Every paired-single load and store read the GQR register from `Cpu` and branched on its type, re-reading it after each guest store; the branches also broke the code into small blocks. The game writes only GQR2-5 (`mtspr` scan of the whole code at generation time) and threads start with GQR0/1 = 0, so the generated `funcs.h` defines `PPC_GQR_STATIC_FLOAT 0x03` and `psq_load_l`/`psq_store_l` need no check for them: 2,082 of the game's 2,106 paired loads and stores.
- **Shader combinations across frames (`WWHD_GL_COMBO_FRAMES`, on unless =0).** A draw whose (programs, shader-relevant registers) combination was seen recently skips the shader and program maps, but combinations lasted one frame (program memory can change between frames), so each one's first draw of every frame did the full lookup: a quarter of all draws. A combination now keeps references to its programs' hash entries and is used again in a later frame when the vertex and pixel programs still have the same per-frame-checked hash and the fetch shader is the same. Desktop gameplay: draws served without a lookup 75% → 97-98%, lookup time 59 → 41 ms/s, frames identical (the on/off runs reached the same game state: 0 pixels differ). The table also grew from 256 to 4,096 entries.
- **Uniform registers without the comparison.** `apply_regs` compared every register block with the current values before copying it, to decide whether shader keys must be recomputed; uniform registers (blocks of hundreds of words) never count for that. Comparing and copying them was ~15% of the render thread on the desktop; they are now just copied.
- **The main-thread sampler's marker.** Every runtime call of the main thread swapped the marker with an atomic exchange (2.6% of the main thread on the desktop; on the Switch a load-exclusive/store-exclusive loop) and read a thread_local to know it was the main thread (on the Switch, with `-mtp=soft`, a function call per access). The flag is now in `Cpu` (`main_thread`, in former padding), the marker is a plain load and store, and nothing happens without `WWHD_MAIN_SAMPLER`.
- **Wakes only when someone waits.** An `OSUnlockMutex` signalled the mutex's condition variable every time, and `OSSignalEvent`/`OSWakeupThread` too, waiters or not; on the Switch a condition-variable signal is a kernel call (`svcSignalProcessWideKey`) in any case (the round 15 log: `OSUnlockMutex` 0.7-1.2% of the main thread, more than `OSLockMutex`). Mutexes count their waiters, events and sleep queues their non-guest waiters, and they signal only then. The mutex owner comes from `Cpu::thread` instead of two thread_local reads.
- **Fewer waits for Mesa's GL thread when textures are made.** Every new texture called `glGenTextures` and `glGetError` twice, every new texture view `glGenTextures`, every new sampler `glGenSamplers`: each one waits for the GL thread to run everything queued (the round 14 log: `GetError` 181 calls, 59 ms, in the 5 s with a camera turn into a new area). Texture and sampler names now come from pools filled 256 (64) at a time, and allocation errors are checked once at the end of a frame that made textures (`[gl] GL error ... in a frame that made N textures`; at once, as before, with `WWHD_GL_DEBUG`). Targets the hitches of first-time textures in a turn.
- Desktop gameplay with everything (3,700 frames): 30 fps, no audio gaps, no GL errors, no translations; render thread 280 ms/s (round 16: 289). With a cache without saved translations (2,464 translations, all textures new): no GL errors either. The desktop main thread does not show the ARM-specific gains (register-file crossings, kernel calls, thread_local calls).
- `[boot]` ends with `runtime: round 17 (...)`; `recompiled code:` lists `single-precision tracking, static GQRs`.

**Looked at and left:** the guest memory base is loaded again in every basic block that touches guest memory (1,470 times in the hottest file; GCC does not reuse the inline-asm load across blocks): keeping it in a reserved register would need every way into game code from foreign threads and callbacks to set it first. Indirect tail jumps (`bctr`, ~1% of the main thread on the desktop: cache misses in the 20 MB dispatch table) could get the per-site cache `PPC_ICALL` has, but their text is what the register-locals passes recognize as exits. The remaining 19,864 `round25` operands mostly come from function arguments: removing them needs the callers' states (whole-program analysis).

### Round 18: whole-program float analysis, jump-site caches, the base in a register (prepared, not yet tested on hardware)

Built separately from rounds 16/17, so that build can be tested first: `build/switch-r18/wwhd.nro` (round 18) and `build/switch-r18x28/wwhd.nro` (round 18 with the experimental register base), copied next to env.txt as `build/switch/wwhd_r18.nro` and `build/switch/wwhd_r18_x28.nro` (any of the three .nro files uses the same sdmc:/switch/wwhd folder, env.txt and shader cache). `build/switch/wwhd.nro` stays round 17 (copy in `build/wwhd_round17.nro`); `build/gen-r17` is round 17's generated code, `build/gen` is round 18's.

- **Single precision across functions (`tools/recomp/singleflow.py`, `WWHD_RECOMP_SINGLE_IP`, on unless =0 when generating).** Round 17's per-function analysis knew nothing at a function's entry or after a call (except f14-f31). Now each function has an entry state (what holds at all its direct calls, tail branches and the fall-through into it) and a return summary (the halves it always returns as single precision, whatever it was given). Open functions start with nothing known: those whose address is taken (relocations: vtables, function pointers), the program entry, hooked functions, functions with no known caller, and any game address written in the runtime's sources (`guest_call` by address, e.g. `mods/climb.cpp`). After an ordinary call the caller keeps f14-f31 and takes the volatile halves from the callee's summary; after a register save/restore helper it keeps every half the helper never writes; after an indirect call or an import, f14-f31; after a hooked function, nothing. Solved as a greatest fixpoint (118,000 function analyses, 16 s). 16,022 → 16,405 multiplier operands without `round25`; in the hot code 514 → 405 left (the rest are values the callers do not know either). The checking build (`WWHD_RECOMP_SINGLE_CHECK=1`) ran the gameplay script and the title flyover without a report.
- **Jump-site caches (`WWHD_RECOMP_IJUMP`, on unless =0 when generating).** Indirect jumps (`bctr`: mostly virtual tail calls) went through `ppc_dispatch`, a lookup in a table of 8 bytes per instruction of the game (cache misses). They now remember their targets per site like `PPC_ICALL` (`MUSTTAIL return PPC_IJUMP(c)(c)`, ppc.h; `ppc_ijump_resolve`). Both kinds of site cache now keep two entries, the latest target first: a virtual call site often alternates between a few object types. Desktop: `dispatch::lookup` 0.39% → 0.22% of all samples.
- **The guest memory base in x28 (`-DWWHD_SWITCH_BASE_REG=ON`, experimental, Switch only).** The generated code read the base with an asm load that GCC repeats in every basic block touching guest memory (1,468 times in the hottest file). With the option, everything the project compiles for the Switch leaves x28 alone (`-ffixed-x28`), the generated code reads the base from x28 (a GCC global register variable, C only; the runtime keeps the load), and `guest_call` (every way from host code into game code: thread entries, alarms, audio and swkbd callbacks, mods) sets x28 and restores the previous value afterwards (`GuestBaseScope`, also when an exception leaves). x28 is callee-saved, so library code (libnx, newlib, Mesa) gives it back unchanged. Hottest file: 0 base loads, 0 writes to x28, 115,997 → 113,119 instructions (`f_0200B380` 308 → 277). It cannot run on the desktop (fixed base there, and clang has no global register variables): a mistake shows on the console at once, so it is a separate build.
- Desktop gameplay with round 18 (3,700 frames): 30 fps, no audio gaps, no GL errors, renders correctly.
- `[boot]` ends with `runtime: round 18 (...)`; `recompiled code:` lists `whole-program single precision, ... jump-site caches`.

#### Hardware test of round 17 (`logs-switch/wwdh_round17_912.log`, GPU at 912 MHz; the 768 MHz file is a launch without full RAM)

- The CPU side got much faster: at the forest entrance (~7,000 draws) the main thread holds its core only 49% of the time (round 15, top of the island: 96%). The GPU is now the limit there: 27.3 fps at 912 MHz (23-24 at 768, reported), "GPU busy at 100% of swaps", and the render thread waits 430 ms/s in present for Mesa's GL thread (`finishes 27 (428 ms)`). The frame rate follows the GPU clock (27.3/23.5 ≈ 912/768).
- Not new: the round 8-9 logs show the same ~7,000-draw forest views GPU-bound (19-22 fps, present waits 200-420 ms/s); round 9 reached 29.5-30 there (probably at 921 MHz). Whether the GPU now does ~8% more there, or the view differed, the logs cannot say (the GPU clock and time were not logged).
- Desktop check of rounds 16-17: the GPU's work per frame (pipeline statistics: vertex and fragment shader invocations, primitives, every 50 frames) is identical with vertex trimming, the GamePad skip and the GX2 staging on or off (four of five runs reached the same game state). The views are heavy: ~11-12 million fragment shader invocations a frame (~12x the 1280x720 pixels: shadow map, reflections, effects).
- Feedback copies (a draw sampling its own render target) are one depth and one color copy (1280x720) a frame: ~15 MB of copies, under 1 ms of GPU time.

### Round 19: where the GPU's time goes, and the whole GamePad picture skipped

`build/switch/wwhd.nro` (round 18's code and runtime plus these) and `build/switch/wwhd_x28.nro` (the same with the register base).

- **GPU time per frame** (`GL_TIME_ELAPSED` from one present to the next, read 8 frames later right after present's wait for the GL thread): `[gl] GPU time per frame N ms` every 5 s.
- **GPU time per render pass (`WWHD_GL_GPU_PASSES`, on unless =0).** Every 30th frame a `GL_TIMESTAMP` query goes where the render targets change (draw.cpp), at clears, copies and present; the next sampled frame reads them, and every 5 s `[gl] GPU passes:` lists the passes that took the most GPU time (target size and formats, draws, ms a frame). On the desktop the GPU waits for the CPU most of the frame, so its figures mean little there; on the Switch, where the GPU is the limit, they say what to make cheaper. `WWHD_GL_PIPESTATS=1` (desktop drivers) logs the shader invocations of every 50th frame.
- **Clocks** (Switch): `[gl] clocks: CPU, GPU, memory MHz` every 5 s (clkrst, or pcv before 8.0.0).
- **The whole GamePad picture skipped.** It is 117 draws a frame at Outset (not 26-48): one read of the GamePad buffer at the save load (a GamePad-sized capture for the transition) had kept it drawn for the rest of the session, and the first frame's reads, before the first GamePad copy, had kept its textures drawn. A buffer read by another draw now stays drawn for 300 frames after the read (one the TV reads every frame never qualifies), and reads from before a texture became a GamePad source do not count. Desktop, four runs: 117 draws skipped a frame (12 ms/s of the desktop render thread), the TV picture identical where the runs reached the same game state.
- `[boot]` ends with `runtime: round 19 (...)`.

#### Hardware test of round 19 (`logs-switch/wwhd_r19.log`, `wwhd_r19_x28.log`; CPU 1785, GPU 768, memory 1600 MHz)

- **Top of the island: 29.8-29.9 fps** at ~6,500 draws (round 17 at 912 MHz: 29.4-29.6). The main thread holds its core 93% of the time there: the CPU is the next limit.
- **Forest entrance: 27.2 fps at 768 MHz**, the same as round 17 at 912 MHz (same view: ~6,900 draws, the same feedback copies, the same data per frame). Not a regression: round 17 needed 16% more GPU clock for it (23-24 fps at 768 MHz, reported). The x28 run held a heavier view of the forest (main scene pass +20%: 24.6 fps); x28 itself costs the same per frame (main thread 1.8% per fps in both runs).
- **The forest is GPU-bound.** The render thread waits 433 ms/s in present for Mesa's GL thread, but that thread runs only 234 ms/s of CPU and the three cores are ~50% idle: it is held back by the GPU (none of the wrapped Mesa wait functions shows it: libdrm's own internal waits are not wrapped). "GPU busy at 100% of swaps".
- **The GPU timestamps run slow.** Every `GPU time per frame` is 0.60× the frame time in all three logs (33.3 ms → 20.0, 36.8 → 22.0, 40.7 → 24.5): the Tegra X1's GPU timer runs at 19.2 MHz and is read as if it ran at 31.25 MHz. Real time = logged × 1.63. Corrected, the forest's passes fill the whole frame: main scene (1280x720, ~2,660 draws) 18.4 ms, shadow cascades (1024x1024, ~4,020 draws) 5.4 ms, the rest (post-processing, present) ~13 ms. 30 fps there needs 10% (this view) to 17% (the x28 view) less GPU time.
- **The pass split was approximate.** nouveau's timestamp is written when the commands before it pass the geometry stages, not when their pixels are done, so a full-screen pass's cost landed in the next pass's time (a 1-draw 480x270 pass showed 2 ms). The totals are right.
- **The frame (desktop trace, `WWHD_GL_TRACE_FRAMES=2500`, Outset):**
  - Three shadow cascades into the 1024x1024 array (534 draws each).
  - A 1280x720 scene buffer (561 draws) with the scene depth.
  - Effects: a 640x360 depth reduction, the 960x540 screen-space shadow buffers (they sample the scene depth and the shadow map) and their blurs (960x540, 240x135), and a 640x360 half-resolution effects pass (32 draws).
  - The main scene into the TV buffer with an R8 mask (143 + 596 draws).
  - DOF and fog (960x540, 480x270; a feedback copy of the depth), the bloom chain (480x270 → 60x33, R11G11B10F), the bloom composite (a feedback copy of the TV buffer).
  - The HUD (66 draws, no depth, only the game's own textures) into the same TV buffer.
  - The TV scan copy.

### Round 20: dynamic resolution, the HUD at full resolution, the TV picture presented directly, exact pass times

`build/switch/wwhd.nro` and `build/switch/wwhd_x28.nro` (the same with the register base), round 18's generated code.

- **Internal resolution in the GL renderer (`WWHD_RES_SCALE`, surfaces.cpp).** Ported from the Metal/Vulkan renderers (which scale up), for factors below 1:
  - Screen-shaped render targets (16:9 ±3%: 1280x720 and its reductions down to 60x33; not the GamePad's 854x480 chain, the shadow maps, mip chains or arrays) are allocated at the factor times their guest size (`Surface::pw/ph`).
  - Everything that talks to the game keeps the guest size; draws scale the viewport, scissor and point size.
  - Shaders sample with normalized coordinates: none of the 555 Outset shaders uses `texelFetch`, `gl_FragCoord`, `textureSize` or window-space positions. For any that does, `uf_fragCoordScale` and `uf_texNScale` are now set per draw, as Cemu's decompiler expects.
  - A render target takes a new factor when a draw, clear or copy next writes to it: its contents are resampled, and a whole clear skips that. Guest data uploads go through the guest size.
  - Copies between targets of different factors are scaled blits; feedback copies and the TV picture take their source's factor.
- **Dynamic resolution (`WWHD_DYNAMIC_RES`, on unless =0; =0.x sets the lowest factor, default 0.75), backend.cpp `DynamicRes`.**
  - **Steps down:** when a half second runs below 29 fps with the GPU still on the previous frame at more than half the presents. It takes what the frame rate says is missing (75-85% of the GPU's time grows with the pixels), in steps of 0.05, at most two at a time.
  - **Steps up:** by 0.05 when three sampled frames in a row show enough idle GPU time at their start for the larger picture (all of the busy time grown by the pixel ratio, plus 3 ms).
  - **Backing off:** a step up that does not hold within 4 s is undone, and the next try waits twice as long (4 s up to 64 s).
  - **Logging:** each change is logged (`[gl] dynamic resolution 1.00 -> 0.90 (GPU-bound: 27.2 fps, ...)`), and the 5 s report gives the factor in use.
  - **Desktop test:** with an artificial GPU load proportional to the pixels (`WWHD_GL_TEST_GPU_LOAD=60000`), 1.0 → 0.9 → 0.8, back up to 0.85, then stable for the rest of the 100 s gameplay script. Without load it stays at 1.0.
  - It is separate from the game's own 1080p/720p switch (see "Rendering resolution"), which stays at 720p.
- **The HUD at full resolution.** The game draws the HUD into the same buffer as the scene, last. At a factor below 1, the first draw into the TV picture's buffer that comes after something read that buffer this frame (the post-processing), has no depth buffer, has one color target and samples only the game's own textures, switches the buffer to a full-size texture with the scene scaled up into it (one blit). The next frame's clear switches it back (both textures are kept: no allocation). A 3D draw into it afterwards also switches it back. Desktop at 0.75: one switch a frame, the HUD's text and icons as sharp as at full resolution.
- **The TV picture presented directly.** `GX2CopyColorBufferToScanBuffer` only notes its buffer; present reads that buffer unless something writes to it first (then the copy is made: `scan_flush`). This saves a 1280x720 copy a frame. Desktop: copied in 0-1% of frames. Present also skips the window clear when the picture covers the window.
- **GPU times in real time, and exact pass times.**
  - The GPU timer's factor is measured over the session: GPU timestamps of the sampled frames' starts against the CPU clock. It is logged once after 30 s (`[gl] GPU timer: ... real ns per GPU ns`; 1.000-1.007 on the desktop, 1.63 expected on the Switch), and every GPU time in the log is converted.
  - Each pass timestamp of a sampled frame is preceded by `glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT)`: a wait for idle on nouveau, so a pass's pixel work is counted in that pass.
  - Dynamic resolution keeps the sampling on even with `WWHD_GL_GPU_PASSES=0`.
- **Frame trace (`WWHD_GL_TRACE_FRAMES=n,...`, testing).** Logs every pass of those frames (`[trace]`): its render targets, draws and the surfaces it samples, plus the clears, copies, feedback copies and scan copies.
- `[boot]` ends with `runtime: round 20 (...)`.

#### Hardware test of round 20 (`logs-switch/wwhd_2026-10-05_18-19-38.log`; GPU clock varied from 768 down to 307 MHz)

- **"That did wonders."** GPU timer measured at 1.6271 real ns per GPU ns (expected 31.25/19.2 = 1.6276).
- **768 and 614 MHz (memory 1600):** the island at 30 fps at full resolution (up to ~7,300 draws).
- **The forest:** 30 fps at 614 MHz with the resolution at 0.75 (also with memory at 1331 MHz). At 537 MHz 26-29 fps, at 384-460 MHz 24-30 fps, all at the 0.75 floor.
- **Stock handheld clocks (CPU 1020, GPU 307, memory 1331):** 23-30 fps on the island. There the main thread holds its core 91-97% of the time: the CPU is the limit at stock clocks.
- **Dynamic resolution** followed the scene as intended: 1.0 → 0.9 → 0.8 on entering heavy views, back up to 1.0 step by step (0.75 → 0.8 → 0.85 → 0.9 → 0.95 → 1.0) on leaving them, with one step up undone and backed off.
- **Every change cost a hitch:** 55-106 ms on the frame of the change, often a second slow frame (73-99 ms) after a step down. About 25 render targets got new textures at once.
- **The GamePad picture came back for ~10 s after area changes** (854x480 passes with 92-121 draws, 2-3 ms of GPU time at 384-460 MHz, exactly in pairs of 5 s reports). The desktop trace (frames 650-720 of the gameplay script) shows why. At an area change the game captures the GamePad picture into a buffer of its own (`21568800`, 854x480) for the GamePad's fade, and that capture draw is not one of the GamePad picture's draws. Its read of the GamePad buffer counted as a read by the TV picture, which keeps a buffer drawn for 300 frames.
- Pass costs in the forest at 614 MHz, 0.75: main scene 14.9 ms, shadow cascades 7.5 ms (~4,020 draws; the shadow maps are not scaled), the HUD pass with the switch to full resolution 2.0 ms.

### Round 21: the GamePad picture skipped after area changes, render-target textures reused (not yet tested on hardware)

`build/switch/wwhd.nro` and `build/switch/wwhd_x28.nro`.

- **GamePad captures.**
  - A draw that is not the GamePad picture's and samples a GamePad-only surface now marks what it renders as derived from that surface (`Surface::derivedFrom`) instead of counting a read. A GPU copy from one does the same.
  - A read of the derived surface counts as a read of the original (`note_read`), and so does a TV scan copy of it. A whole clear ends the derivation.
  - Desktop gameplay script: no 854x480 pass at all (before: 13 reports with one, after the save load). The TV picture with the skip on and off differs only in animation timing (wind streaks, clouds).
- **Render-target textures reused between resolutions.**
  - A rescale's old texture (with its views) goes to a pool, up to 128 MB, least recently released first out. A later rescale to the same format and size takes it from there.
  - New textures are limited to 4 a frame: a pass whose render targets all still share their old scale keeps it until a later frame. Mixed scales in one draw are always fixed at once.
  - Feedback copies follow their source through the same path.
  - Desktop, 1.0 → 0.8 → 0.9 → 0.8 → 0.9: the second visits to 0.8 and 0.9 took all 27 textures from the pool. The 5 s report gives `N render targets resampled, M from kept textures`.
- **Dynamic resolution steps down up to three steps at once** (was two), so entering a heavy view takes one change instead of two.
- `[boot]` ends with `runtime: round 21 (...)`.

### Round 22: graphics glitches (lines in shadows, black shapes on characters, missing shore) (not yet tested on hardware)

`build/switch/wwhd.nro` and `build/switch/wwhd_x28.nro`. The user's report and screenshots are in `logs-switch/screenshots/glitches/` (a save at Tetra's beach in `go_to_tetra_and_shore_glitches/save`, Quest Log 1). All three glitches were there since the first build.

| Glitch | Seen on | Cause | Fix | Off switch (`env.txt`) |
|---|---|---|---|---|
| Line pattern (and grainy bands) in shadows on grass and sand | Switch and the desktop GL build; not the Vulkan renderer | The game's ambient-occlusion pass (VS `44BDF900`, PS `44BDFD00`) point-samples its centre depth from a 640x360 depth copy while drawing 960x540, so every third row and column is half a texel off. The Wii U draws the same lines; the Metal and Vulkan renderers carry a fix, the GL renderer did not | As in those renderers: the centre fetch is bilinear (mode 1), and the 4x4 noise is tiled per 960x540 pixel (the vertex shader's first remapped constant `.w` x1.5) so the game's blur averages it out (mode 2, default) | `WWHD_AO_MODE=0` (or `1`); `WWHD_NO_AO_QUIRK=1` = 0 |
| Black triangles flickering on Tetra and Sturgeon; shore water losing its foam below a straight line at some camera angles | Switch only (not reproduced on the desktop GL or Vulkan builds) | Most of the scene is drawn in several passes over the same mesh with a less-or-equal depth test and no depth writes (desktop trace at Tetra: 1,855 + 118 such draws a frame), so a later pass must compute the same depth bit for bit. The GL renderer did not declare `gl_Position` invariant (the Vulkan renderer does; Cemu's Metal emitter uses `[[invariant]]`). Maxwell's FFMA rounds once, and nouveau fuses multiply-adds depending on the rest of each shader, so two programs can round a vertex differently; where the later pass loses, the earlier pass shows through on whole triangles, which is how the screenshots look (pure black regions bounded by triangle edges). AMD's `v_mad_f32` rounds like separate operations, which is why the desktop never shows it | `invariant gl_Position;` in every vertex shader, added at compile time (so it also applies to sources from the shader cache). Mesa propagates it to the contributing operations as `precise`. If the driver rejected the declaration, the shader is compiled without it and the log says so once | `WWHD_GL_INVARIANT=0` |

- **How the shadow lines were found.** The headless GL build on the AMD GPU shows the lines on the sand in shadow (gameplay script, frame 1900). The frame trace showed the 960x540 shadow mask built from two draws: the occlusion draw (red and green) and the shadow-map draw. The red channel ramps and resets every 12 pixels (the 4-pixel noise tile against the 3-row texel cycle), and the Metal renderer's comments describe exactly this. After the fix the occlusion term is flat (255) on open ground and the sand matches the Vulkan reference.
- **Ruled out for the Switch-only glitches:** internal resolution (the shore is unchanged at a fixed 0.8), a race with the game's `update_ubo` thread (the game calls no GX2 timestamps and waits for the render thread twice a frame), and vertex trimming (an edge-of-screen green triangle seen once turned out to be a grass patch at the camera's position of that frame).
- **Regression check (desktop, Tetra route):** new defaults against `WWHD_GL_INVARIANT=0 WWHD_AO_MODE=0` differ only in shadowed areas and wave animation (mean 0.6-0.8 levels); 0 GL errors, 0 skipped draws, no shader failures.
- **Test tooling.**
  - `WWHD_GL_TRACE_DRAWS=1` (with `WWHD_GL_TRACE_FRAMES`) logs every draw of a traced frame: targets, program addresses and GLSL hashes, depth function and writes, stencil, polygon offset, blend, and each texture with its sampler words. The GLSL of a hash is in `shadercache_gl.bin` (record 1: `{1, vertex, hash, packed size, size, zlib GLSL}`).
  - The trace now lists a draw's sampled surfaces under its own pass (they were listed under the previous pass, since textures are resolved before the render targets are bound).
  - The Vulkan reference follows the same scripted route: the SDL host reads `WWHD_PRESS` (hex buttons: A `8000`, RIGHT `0400`), `WWHD_STICK` and `WWHD_RSTICK` (`from-to:x:y`) instead of `WWHD_SCRIPT_INPUT`; add `WWHD_NO_HOST_INPUT=1`.
  - Route to Tetra with the user's save (Pro Controller): the menu presses of the gameplay script, then `1200-1217:RX=1,1300-1700:LY=1,1701-1745:LX=-0.45+LY=1,1770-1774:A` (on the beach next to her at 1735-1765, her dialogue from 1780).
- The log says `[gl] vertex positions invariant: on (WWHD_GL_INVARIANT); ambient occlusion mode 2 (WWHD_AO_MODE)` after the shader cache line, and `[boot]` ends with `runtime: round 22 (...)`.

#### Hardware test of round 22 (`logs-switch/wwhd_with_crash.log`, 2026-10-05; CPU 1785, GPU 614, memory 1331 MHz)

- **Fixed on hardware:** the lines in shadows, and the black shapes on Tetra. 30 fps on the island; the log says `vertex positions invariant: on`.
- **Still there:** the shore water losing its foam at some camera angles (backlog, see Next steps).
- **New: the game froze, then closed, on entering the pirate ship's hold** (frame 21884):
  - The watchdog: `Render thread: executing (linking a program)`, the game waiting on its sync. No frame for 3 s; the log ends a few seconds later.
  - No new translations near the freeze: the pair being linked was two shaders compiled at startup (from the cache) that had never been linked together. Mesa 20.1's nouveau compiles a pair's GPU code inside `glLinkProgram` (`nvc0_sp_state_create` calls `nvc0_program_translate`), so a driver compile that never finishes stops the render thread there.
  - Earlier, at frame 15485, two pairs failed to link: `fragment shader input 'passParameterSem254' with explicit location has no matching output`. Their draws were skipped from then on (~31 a frame, the `skipped` count).

### Round 23: unmatched pixel-shader inputs, and the shader pair named while linking (not yet tested on hardware)

`build/switch/wwhd.nro` and `build/switch/wwhd_x28.nro`.

- **Pixel-shader inputs the vertex shader does not write (`varyings.h`).** The game paired a pixel shader that reads semantic 254 with a vertex shader that does not export it; the hardware gives such an input its default value. Vulkan allows the mismatch and Cemu's GL renderer links its stages separately, but a GL program with explicit varying locations fails to link. When a link fails with "no matching output", the vertex shader's source (`glGetShaderSource`) gets an output for each missing input (same location and qualifiers, written as zero, `DEFAULT_VAL` 0) and the pair is linked again. The log says `[gl] program ...: its pixel shader reads passParameterSem254, which the vertex shader does not write; linked with them set to zero`.
  - Tested with a standalone GL program on desktop Mesa (radeonsi): a real decompiled vertex shader with one output and a real pixel shader with two inputs give the same link error as the Switch; with the added output they link; a matching pair is left unchanged.
- **The pair named while linking.** The render thread's stage during an in-game link is now `linking vertex shader <hash> with pixel shader <hash>`, so a freeze inside the driver names the pair in the watchdog line at no cost per link (a log line per link would be an SD card write each). Failed links and links over 100 ms are logged.
  - Desktop check with the test aid `WWHD_GL_TEST_LINK_STALL=5000` (holds the first in-game link): `[watchdog] no frame for 3.0 s (frame 676). Render thread: executing (linking vertex shader 7e0d4961b0852bcb with pixel shader ac9d51224a1f69b4)`.
- **`tools/switch/extract_shadercache.py`** writes every GLSL source of a `shadercache_gl.bin` as `<hash>_vs.glsl` / `<hash>_ps.glsl`, the hashes the log uses. With the Switch's cache file, the pair a freeze names can be compiled offline.
- Regression (desktop, Tetra route): 0 GL errors, 0 skipped draws.
- `[boot]` ends with `runtime: round 23 (...)`.

#### Hardware test of round 23 (`logs-switch/wwhd_r23.log`, `logs-switch/screenshots/glitches/sudden_black_sky.jpg`; CPU 1785, GPU 614, memory 1331 MHz)

- **Black sky** on the way up to the top of the island (the path between the cliffs from Link's house): the rest of the picture correct, the sky black (the scene buffer's clear colour where the sky should be).
- **Crash entering the forest**, ~13 s after the area change (frame ~2550): the log just stops. No watchdog line (not a freeze), no link failure, 0 skipped draws. The session's dynamic resolution had gone 1.00 → 0.90 → 0.95 → 1.00 on the way up (26 textures taken back from round 21's pool).
- The forest worked in round 20 (18:19 session, at 0.75): rounds 21-23 are the suspects.
- **Why the crash left nothing:** on the Switch the log is a memory buffer that a thread writes out four times a second (round 14), so a crash loses its last quarter second; and CPU exceptions went to Atmosphère's crash report only.

### Round 24: crashes reported in the log, a capture button, the round 21 GamePad and pool changes made safe (not yet tested on hardware)

`build/switch/wwhd.nro` and `build/switch/wwhd_x28.nro`; their ELFs are kept as `build/switch/wwhd_r24.elf` and `wwhd_x28_r24.elf` for the crash addresses.

| Change | Why |
|---|---|
| **CPU exception handler** (`main.cpp`, `__libnx_exception_handler`). hbloader passes a CPU exception to the NRO's entry point (nx-hbloader `trampoline.s`), whose crt0 calls libnx's handler on its own 64 KB stack. It writes the waiting log lines and then `[crash]` lines straight to the files' descriptors (no lock): the kind (bad memory access, bad jump, trap...), thread, frame, pc/lr as `code+0x...`, the fault address, the registers, the frame-pointer chain, the code addresses on the stack, the render thread's stage, the guest thread's lr/r1/r3/r4, and the heap left. Then libnx raises `svcBreak` and Atmosphère writes its report as before. The old comment that exceptions never reach the NRO was wrong | Both crashes so far left nothing in the log |
| **`abort()` wrapped** (`-Wl,--wrap=abort`, 42 call sites including Mesa's): the same report, caller as pc, then the real `abort` | `abort` raises `svcBreak`, which the exception handler does not see |
| **Heap left in every stats line** (`heap never used N MiB`: libnx's heap end minus malloc's break, no lock); below 256 MiB it is logged and the texture pool is emptied | Running out of memory in Mesa/nouveau crashes instead of failing; GPU memory comes out of the same heap |
| **Texture pool (round 21) bounded:** 64 MB instead of 128, and a texture unused for 600 frames (20 s) is deleted | Round 21 kept up to 128 MB of old render-target textures after resolution steps (in the crash session, two sets: 0.90 and 0.95) |
| **GamePad skip (round 21) narrowed.** A draw that is not the GamePad picture's and samples a GamePad-only surface counted as "derived" (not a read) for every such surface, so a texture the TV also needs could stay GamePad-only for good: its draws skipped, the TV showing what it held. Now that applies only to the GamePad picture's own buffer (its area-change capture, round 21's case). A GamePad-picture texture that anything else reads after the GamePad started sampling it is shared for good (`Surface::tvShared`). The first skipped draw into each surface is logged (`draws into ... skipped from frame N`) | One way to a sky that stops being drawn; on the desktop only the GamePad's 854x480 buffers are skipped, and all 117-121 GamePad draws a frame still are |
| **AO fix (round 22) by contents, not only address:** the occlusion programs must also have their size and hash (pixel 1,584 bytes `26870ca3f2e34dfa`, vertex 384 bytes `36e37317f62658e9`); the log says once whether the fix applies | Another area could load another program at those addresses; the fix would then change that program's constant |
| **Capture with both sticks:** clicking both sticks at once traces the next frame into the log and writes its TV picture and render targets as PNGs next to `wwhd.log` (`[input] both sticks clicked`, `[gl] capture of frame N`) | For pictures that go wrong where the desktop cannot reproduce them (the black sky) |
| Desktop testing: `-DWWHD_SANITIZE=address` (CMake) builds the runtime, GX2 layer, renderer and Cemu's decompiler with AddressSanitizer (the generated game code is left out); the headless script knows `LS` and `RS` (stick clicks) | |

- **Tests (desktop):**
  - AddressSanitizer build, no errors: the Tetra route with 8 forced resolution changes and her dialogue (to frame 2400), and the gameplay route with 15 changes of up to three steps (0.75-1.0, to frame 3300).
  - No black sky after the crash session's sequence (1.00 → 0.90 → 0.95 → 1.00, 26 textures from the pool) nor on the path between the cliffs (route: from the Tetra save, `1300-1307:LX=-0.3+LY=1,1308-1490:L+LY=1,1491-1515:LX=-0.7+LY=0.7,1516-1640:LY=1`).
  - The forest itself was not reached on the desktop (the path past Link's house leads down to the village; the forest path starts near Aryll's lookout and crosses the wooden bridge at the top).
  - Capture: a scripted click of both sticks at frame 1850 traced frame 1852 (63 lines) and wrote 33 targets and the TV picture.
- `[boot]` ends with `runtime: round 24 (...)`.

#### Hardware test of round 24 (`logs-switch/wwhd_r24.log`; CPU 1785, GPU 614, memory 1331 MHz)

- The sky stayed right on the way up; the forest was entered, then the game crashed while the camera turned.
- **The crash report worked** (`[crash]` lines at the end of the log), decoded with `wwhd_r24.elf`: a data abort at address 0 in the GX2 render thread, `translating a vertex shader`: `StringBuf::add` in Cemu's `LatteDecompiler_emitGLSLShader`, which had just asked `malloc` for **12 MB** for the generated source and got null. `heap never used: 5 MiB`.
- **The heap was nearly full from the start:** 89 MiB never used after startup, then 87 → 58 → 26 → 15 → 5 MiB through the session (texture pool 0-28 MiB, emptied when low).
- **Where it went (measured on the desktop with the game's GLSL, malloc in-use bytes):** Mesa keeps a compiled shader object's whole intermediate code until the object is deleted, ~460 KB each (789 objects: +354 MB; deleting them after linking: -349 MB). Linked programs keep ~59 KB each without their shaders. The renderer compiled every cached source at startup and kept its object for the session (to link new pairs): with the Switch's cache at 1,902 sources, ~870 MB. The cache grows with every session (round 20: 1,429 sources; round 23: 1,902), which is why each session had less memory.

### Round 25: shader objects freed after linking, no 12 MB block per translation (not yet tested on hardware)

`build/switch/wwhd.nro` and `build/switch/wwhd_x28.nro`; ELFs `wwhd_r25.elf`, `wwhd_x28_r25.elf`.

- **Shader sources instead of shader objects (`shaders.cpp`).** Each GLSL source is kept compressed in memory (as stored in the cache file, ~1.5 KB each). Its GL shader object is compiled when a link needs it (`object_for`), and at most 32 recently used objects are kept (256 during startup). A link detaches its shaders, so deleting them frees their memory. At startup the cache's sources are kept without compiling, then the cached pairs are linked in order: each source is compiled when a pair first needs it and deleted after its last pair. A source no cached pair uses is not compiled at all. `Shader::compiled` replaces the object in `Shader`.
- **1 MB instead of 12 MB for a translation's source** (Cemu's `LatteDecompiler_emitGLSLShader`; the game's largest shader is ~52 KB of GLSL). `StringBuf` no longer crashes when `malloc` or `realloc` fails: the shader comes out empty and fails to compile (its draws are skipped and logged) instead.
- **Memory in the log:** `[mem] ...: N MiB of heap never used` after the guest memory, the GL setup and the shader cache; every 5 s `[gl] memory: heap never used ...; textures ... MiB in ... surfaces (render targets ..., pool ...); shaders: ... sources (... KB), ... compiled objects, ... programs`.
- **Tests (desktop):**
  - Cache with 789 sources and 455 pairs: startup links all 455 (789 compiles, 2.6 s); 0 objects kept afterwards; malloc in use 134 MiB for the whole process (the objects alone were ~350 MB). Sources and 2,975 translations: 6 MiB. Tetra route: 0 compiles in game, 0 skipped draws, same picture.
  - AddressSanitizer, smaller cache (555 sources), Tetra route: 282 compiles and 144 links in game (about a dozen were recompiles of evicted objects), 0 errors, 0 skipped draws.

#### Hardware test of round 25 (`logs-switch/wwhd_r25.log`, `logs-switch/screenshots/glitches/r25/`, save `Saves/Forsaken Fortress`)

- **No crash:** forest, the pirate ship and on to the Forsaken Fortress (~16 min). Heap never used: 1,691 MiB after the guest memory, 1,562 after the GL setup, **986 after the shader cache** (round 24: 89), lowest 203 MiB late in the session (textures 198 MiB in 826 surfaces, 2,191 sources, 1,346 programs).
- The startup's 576 MiB are mostly the 1,346 linked programs (~340 KB each on nouveau) plus the startup's peak of kept objects: the next memory target if needed.
- **Long black screens at doors:** a door's first visit compiles the new place's shaders. One frame took **18.7 s** (273 compiles, 137 links; ~250 programs never seen before); others 0.3-3.6 s. Some were round 25's recompiles: "6 compiled, 2 linked, 0 new states" is a new pair of two known sources, both recompiled (887 ms).
- **Spotlights at the Forsaken Fortress** flicker and show as hard-edged bands across the walls. Not on the desktop (GL and Vulkan match there, same save and camera, 60 frames compared): Switch only.

### Round 26: depth copies by the 3D engine, env.txt before static initialisation, quick doors and fast scenes (not yet tested on hardware)

`build/switch/wwhd.nro` and `wwhd_x28.nro`; ELFs `wwhd_r26.elf`, `wwhd_x28_r26.elf`.

| Change | Why | Off switch |
|---|---|---|
| **Depth copies with `glBlitFramebuffer`** (the feedback copies of an attached depth buffer, and GX2 surface copies of depth) instead of `glCopyImageSubData` | Mesa 20.1's nouveau gives 32-bit float depth a compressed memory kind (`nvc0_mt_choose_storage_type`: 0x86 when `drm->version >= 1.1.1`), and `glCopyImageSubData` of equal formats is a raw memory-to-memory copy (`nvc0_resource_copy_region` → m2mf): the copy gets compressed tiles without their compression state, and a shader reading it sees wrong depth in blocks. Effects that read a depth copy: the shore foam (round 22: cut off along straight lines, Switch only) and a full-screen composite after the spotlights. A blit goes through the 3D engine, which reads compressed depth correctly (resolution changes already copied depth that way). Colour targets are compressed only with multisampling | `WWHD_GL_DEPTH_COPY=0` |
| **env.txt read before the static initialisers** (`main.cpp`, a constructor of priority 101: libnx mounts the SD card before `__libc_init_array`, and `switch.ld` sorts `.init_array` by priority; checked: first entry) | Switches read into globals at static initialisation never took effect from env.txt on the Switch: draw.cpp's off switches, `WWHD_AO_MODE`, `WWHD_GL_INVARIANT`, the gameplay mods' switches... | — |
| **Quick doors and fast scene changes** (the original project's mods, `mods/turbo.cpp`) on in the shipped env.txt; `[mods] quick doors on` at startup | Asked for as a test. Doors and fades run extra logic steps per frame; nothing is skipped. They do not shorten a first visit's shader compiling. Desktop, save load: black 20 frames sooner, the room 10 frames sooner | comment out `WWHD_MOD_QUICK_DOORS` / `WWHD_MOD_FAST_SCENES` |
| **96 shader objects kept** (was 32), and after startup the 96 sources in the most cached pairs keep theirs | A new pair of known sources compiled both again (round 25) | — |

- Desktop: depth copies by blit give pixel-identical frames (FF route, frames 1300 and 1420: mean difference 0.00). AddressSanitizer, Tetra route with both mods: 0 errors, 0 skipped draws.
- `[boot]` ends with `runtime: round 26 (...)`; the log says `[gl] depth copies: 3D-engine blits (WWHD_GL_DEPTH_COPY)`.

#### Hardware test of round 26 (`logs-switch/wwhd_r26.log`, `logs-switch/screenshots/glitches/r26/`)

- **env.txt now reaches the static switches:** the log shows `[mods] quick doors on`, `[mods] fast scene changes on` and `[gl] depth copies: 3D-engine blits`.
- **The searchlights still go wrong** when a beam comes near Link: the pool of light is missing or drawn as slabs across the walls, and the haze of the beam is gone. Three captures (frames 1788, 1856 and 2751; each capture frame takes ~8.8 s to write its PNGs, which is expected). One caught a correct frame. So the depth copies were not the cause.
- **Quick doors crashed the game:** a door played sped up, then the game stopped on its own check:
  ```
  Source File: J3DPacket.cpp
  Line Number: 157
  Description: mEntryPtr == (0)
  ```
  followed by `OSPanic` and the `[crash]` lines (abort in the guest thread, frame 13,300). Quick doors runs every process's logic step (`fpcEx_Handler`) up to 3 more times per frame without a draw in between; a model packet entered during one step is entered again before the draw list is reset, which this check forbids. It is the mod, not the port.
- Memory: 824 MiB of heap never used after the shader cache, lowest 237 MiB late in the session.

### Round 27: near-plane clip distance for the searchlights, quick doors off (not yet tested on hardware)

`build/switch/wwhd.nro` and `wwhd_x28.nro`; ELFs `wwhd_r27.elf`, `wwhd_x28_r27.elf`.

| Change | Why | Off switch |
|---|---|---|
| **Every game vertex shader writes `gl_ClipDistance[0] = z + w`** (`shaders.cpp` `with_near_clip`: the declaration goes into Cemu's `gl_PerVertex` block and the write into its `SET_POSITION` macro), and `GL_CLIP_DISTANCE0` is enabled for each draw unless the game turned near clipping off (`PA_CL_CLIP_CNTL.ZCLIP_NEAR_DISABLE`; the shadow maps do) | The searchlights are light volumes: back faces drawn with depth GREATER into a 640x360 light buffer (`vs 44461600`, `ps 44461F00`, cull front). When a beam comes near Link the camera is inside the cone, and its triangles reach behind the camera. Mesa 20.1's nouveau programs the hardware to clip geometry at w = 0 only (`VIEW_VOLUME_CLIP_CTRL` with the guard band) and leaves the near plane to a per-pixel depth test, so those triangles are drawn from vertices just in front of the camera at huge screen coordinates. A user clip distance is always clipped as geometry, at the plane. With the game's clip space (-1..1), z + w = 0 is exactly the near plane, so other drivers draw the same picture | `WWHD_GL_NEAR_CLIP=0` |
| A program gets the plane only if its vertex shader really writes it (`link()` reads the linked vertex shader's source back) | An enabled clip distance that the shader does not write is undefined | — |
| A vertex shader that does not compile with the clip write is compiled without it (logged once) | A driver that rejects it then draws as before instead of skipping draws | — |
| **Quick doors off** in the shipped env.txt; fast scene changes stay on | The J3DPacket crash above. Fast scene changes only reruns the transition machinery (scene loading, the fade's timers, scene requests), not the actors' logic | `WWHD_MOD_QUICK_DOORS=1` turns it back on |
| `[mods] door event done: N extra logic steps` and `[mods] scene change done: N extra transition steps` | The log shows which mod ran just before a problem | — |
| The per-draw trace (`WWHD_GL_TRACE_DRAWS=1`, and the capture's frame) gives each draw's culling, front face, viewport direction, depth range, clip convention, near/far clipping and whether the near clip plane is on | For the next capture | — |

- nouveau (Mesa 20.1 sources): a vertex shader that writes a clip distance is marked as not needing generated user clip planes (`genUserClip = -1`), so enabling the plane costs no shader rebuilds. The hardware gets `CLIP_DISTANCE_ENABLE = 1` for those draws only.
- The internal draws (the overlay, the present and the `WWHD_GL_TEST_GPU_LOAD` test pass) turn `GL_CLIP_DISTANCE0` off.
- The shader cache file is unchanged: it stores Cemu's GLSL, and the clip write is added when a source is compiled.
- **Tests (desktop):**
  - Forsaken Fortress route: 455 programs linked at startup, 0 without the clip write. The 12 searchlight draws into the light buffer have the plane on. The 444 draws without it are shadow-map passes where the game turned near clipping off. The beam and its haze look the same as before, 0 GL errors, 0 skipped draws. Gameplay frames of two runs cannot be compared pixel by pixel (they drift by a few frames), so the check is by eye.
  - A first version put the two edits at the wrong offsets (Cemu's macro comes before the block): every vertex shader failed to compile. The order is fixed, and the compile fallback above now guards against this kind of failure.
  - AddressSanitizer, Forsaken Fortress route with the per-draw trace: 0 errors, to the end of the route.
  - Fast scene changes alone, Forsaken Fortress and Tetra routes: three scene changes each (171, 66 and 174-195 extra transition steps), no halt.
- `[boot]` ends with `runtime: round 27 (...)`; the log says `near-plane clip distance: on (WWHD_GL_NEAR_CLIP)`, and the shader cache line ends with `0 programs without the near-plane clip distance`.

#### Hardware test of round 27 (`logs-switch/wwhd_r27.log`, `logs-switch/wwhd_r26_2.log`, `logs-switch/screenshots/glitches/r27/`)

- **The searchlights still go wrong.** The console's own screenshots (`r27_real_capture*.jpg`) show the real symptom: the beam leaving the lamp is a few thin, hard-edged rays instead of a soft cone. The capture (frame 9053) caught one ray. Its 640x360 light buffer is nearly black, where the desktop has an orange glow at a similar view.
- **Two runs:**
  - `wwhd_r27.log` ran at stock clocks: CPU 1020 MHz and GPU 307 MHz, against 1785 and 614 in round 26. Startup linking took 261 s instead of 171 s, about what the clock ratio predicts. The overclock was not applied in that run; the port only reads the clocks.
  - `wwhd_r26_2.log` is the same round 27 build at 1785/614 MHz. Startup took 171.7 s, the same as round 26.
- **Microstutters: a round 27 regression.** Both round 27 runs stall at texture uploads. Each upload first waits for Mesa's GL thread to finish its queued work (`_mesa_glthread_finish_before`):

  | Wait | Rounds 24-26 | Round 27 |
  |---|---|---|
  | `CompressedTexSubImage2D` | 0.10 ms each | 4.9-6.6 ms each |
  | `CheckFramebufferStatus` | 0.1-0.3 ms each | 48-70 ms each |
  | Same scene load, 78-79 uploads | 354 ms | 4,459 ms |

  The waits at the end of each frame (`GetQueryObjectui64v`, `GetSynciv`) stayed at 0 ms, and the GPU's own time per draw did not grow (main pass 14 µs/draw in round 26, 8 µs in round 27). So the driver's CPU side waits at loads, not the GPU. Round 27's only change on that path is the near-plane clip distance; nouveau's sources show no shader rebuild for it, so the exact mechanism is not known. It is off by default in round 28.

### Round 28: near-plane clip off, the searchlight beam found, probe frames for the Switch (not yet tested on hardware)

`build/switch/wwhd.nro` and `wwhd_x28.nro`; ELFs `wwhd_r28.elf`, `wwhd_x28_r28.elf`.

- **The beam's draws.** A desktop test aid (below) read the main picture back after every draw. The beam is two draws per searchlight into the 1280x720 scene buffer:
  - vertex shader `BB5B9D69776570A0`, pixel shader `BCB22BAD319DC0BD`;
  - a 234-index cone, no culling, alpha blending (`05040504`), depth test LEQUAL without writes, polygon offset (-0.5, -2).

  Its pixel shader computes the alpha from three factors:
  - the vertex alpha;
  - a facing term, the absolute cosine between the interpolated view-space normal and the view vector, which softens the cone's edges;
  - a depth fade, the scene's linear depth from the 640x360 R32F buffer (`F40E8000`) minus the fragment's depth, which fades the beam where it meets geometry.

  The light buffer pass (`F415B800`) is the glow on surfaces, not the beam.
- **Ruled out on the desktop:**
  - Dynamic resolution: `WWHD_RES_SCALE=0.75` and `0.85` draw the beam correctly.
  - The desktop shows no invalid values in the broad-beam view. In one edge-on view, a few hundred pixels have a NaN facing term and alpha, and AMD's blending leaves them invisible.
- **Probe frames (draw.cpp, shaders.cpp).** After a capture (both sticks), each of the next 16 frames draws the beam with one diagnostic output in place of its color. The draw's target is written to `probe_<first frame>_m<mode>_<n>.png` right after each beam draw; mode 0 is also written before it. Opaque outputs show the value itself where the cone is drawn. Modes:
  - 0 original;
  - 1 coverage;
  - 2 normal (|N|; magenta NaN, cyan infinite);
  - 3 facing (recomputed);
  - 4 depth (R scene depth, G fragment depth, B 0.5 + 4 x difference; magenta where the sky is behind);
  - 5 screen position;
  - 6 final alpha;
  - 7 final color;
  - 8 gradient texture;
  - 9 vertex alpha;
  - 10 the game's own facing term;
  - 11 the game's own depth fade;
  - 12 the beam without the depth fade;
  - 13 NaN map (R facing NaN, G alpha NaN);
  - 14 NaN source (R zero or infinite normal length, G NaN normal, B zero or bad view vector);
  - 15 depth source (R infinite scene depth, G NaN scene depth).

  Flag modes draw dark gray where the cone is drawn, and a channel goes to 1 where its flag is set. The beam is found by its pixel shader's GLSL hash, or by its draw state if the hash differs on the Switch (logged). `WWHD_GL_PROBE_PS=hash,...` probes other pixel shaders. `WWHD_GL_PROBE_FRAME=n` starts the probes at a frame on the desktop.
- **A capture also traces every draw of its frame** (as `WWHD_GL_TRACE_DRAWS=1`). The log buffer is now 4 MB instead of 1 MB: a capture frame's per-draw trace is about 1.5 MB, and none of it is dropped.
- **Near-plane clip distance off by default.** `WWHD_GL_NEAR_CLIP=1` turns it on. The shader cache line mentions it only when it is on.
- **Desktop test aids:**
  - `WWHD_GL_TEST_SKIP=frame:first-last,...` leaves out those draws of a frame (counted in trace order).
  - `WWHD_GL_TRACE_PIXELS=1` (desktop only, with the per-draw trace) logs the pixels each draw changed in its first color target.
- **Tests (desktop):**
  - The capture flow scripted (both sticks at frame 1395): the capture frame traced 2,311 draws. The beam's draw-state signature matched exactly its 4 draws. All 16 probe modes linked. A 16-mode run writes 68 PNGs: 4 beam draws per frame, plus the "before" pictures of mode 0.
  - AddressSanitizer, the same flow: 0 errors, and the game ran on after the probes.
- `[boot]` ends with `runtime: round 28 (...)`. The log says `near-plane clip distance: off (WWHD_GL_NEAR_CLIP)` and `capture of frame N requested: ... probe frames from N+1`, then one `probe frame` line per mode.

### Next steps

- Hardware test of round 28:
  - Are the stutters gone? The `synchronous calls` in the `[gl] Mesa per second` lines should be back near 0.1 ms per `CompressedTexSubImage2D`. If they are not, another part of round 27 is the cause.
  - At a broken searchlight, one capture. Keep the beam on screen and wait about a minute. Send `probe_*.png`, `frame_*.png`, `target_*.png` and the log.
- First visits still compile every new shader while the screen is black. Spreading the compiles over frames (drawing the place without the not-yet-compiled objects for a moment) is the option; a second GL context compiling on another core is risky with nouveau in Mesa 20.1.
- Programs: ~340 KB each on the Switch; linking the cached pairs at first use instead of at startup would trade memory for short stalls.
- Hardware test of round 25: the forest. Expected: `[mem] after the shader cache` several hundred MiB higher than round 24's 89 MiB. Startup may take somewhat longer or shorter (each cached source compiled once, as before; the unused ones not at all).
- Hardware test of round 24: the forest and the top of the island. A crash now ends the log with `[crash]` lines: `addr2line -f -C -e build/switch/wwhd_r24.elf 0x<offset>` names each `code+0x<offset>`. If the sky goes black, click both sticks while it is on screen and send the PNGs with the log. If the forest crashes again, a run with `WWHD_GL_INVARIANT=0` and one with `WWHD_DYNAMIC_RES=0` separate rounds 22 and 20-21.
- Hardware test of round 23: enter the pirate ship's hold. If it freezes again, the watchdog line names the shader pair. Then the same with `WWHD_GL_INVARIANT=0` (if that enters fine, the invariant positions trigger the driver problem for that pair). Needed from the SD card: the log, `sdmc:/switch/wwhd/shadercache_gl.bin`, and any report in `sdmc:/atmosphere/crash_reports/`.
- The skipped draws should be gone (`0 skipped` after the place where the two pairs failed).
- Backlog: the shore water losing its foam below a straight line at some camera angles (Switch only; round 22 did not fix it). Not reproduced on the desktop.
- Hardware test of round 21: hitches at resolution changes (the `[hitch]` lines next to `[gl] dynamic resolution`), the forest and area changes.
- Stock clocks: the main thread is the limit at 1020 MHz (91-97%): the CPU side comes next. The shadow cascades (~4,000 draws, unscaled) are the largest fixed GPU cost at low GPU clocks.
- First-time texture uploads (16-31 ms in a turn) remain: decoding on another thread is the option.

### Upstream merge (2026-10-07): devel v0.2.2

Upstream `devel` (c3fb7ce, 100 commits: mod manager, native mod SDK, crash logs, Linux build) was merged
into the port (b63b189). The Switch keeps its own paths: write tracking off, `crash_addr.cpp` left out
(the Switch has its own handler in `main.cpp`), native mod libraries refused, the true60 POSIX aids skipped,
and `platform/hostui_switch.cpp` keeps settings in `sdmc:/switch/wwhd/settings.ini`. Plain `char` is signed
on the Switch (`-fsigned-char`), as upstream builds on Linux and Android arm64 (0776035).

### Round 29: official GPU profile in handheld (hardware-tested)

`apmSetPerformanceConfiguration` for handheld (`ApmPerformanceMode_Normal`) with Nintendo's own
configurations for games; the CPU stays at 1020 MHz and docked is left to the system:

| Configuration | CPU | GPU | Memory |
|---|---|---|---|
| 0x00020003 (system default) | 1020 | 307.2 | 1331.2 |
| 0x00020004 | 1020 | 384.0 | 1331.2 |
| 0x92220008 | 1020 | 460.8 | 1331.2 |
| 0x92220007 | 1020 | 460.8 | 1600.0 |

`0x92220007` is accepted in handheld (`rc 0x0`, `[gl] clocks: CPU 1020 MHz, GPU 460 MHz, memory 1600 MHz`).
Same play, ~4 min each (`logs-switch/wwhd_2026-10-07_11-25-47.log` at 307 MHz, `..._11-47-37.log` with 0x92220007):

| | GPU 307 / memory 1331 | GPU 460 / memory 1600 |
|---|---|---|
| median fps | 26.9 | 29.5 |
| median fps, views over 2,000 draws | 26.5 | 29.3 |
| reports at 28 fps or more | 33% | 71% |
| worst report | 6.6 fps | 17.1 fps |
| internal resolution 1.00 | 2 of 48 reports | 18 of 48 |

The main thread's wait moved from `GX2DrawDone` (GPU) to `GX2WaitForVsync`. The configuration found at start
is restored at exit; apm needs title mode (in applet mode the profile is skipped and logged).

### Round 30: settings menu, Mesa shader cache (not yet tested on hardware)

- **Settings menu.** Upstream's Dear ImGui overlay (`overlay/overlay.cpp`), drawn by a new OpenGL renderer
  (`gfx/gl/overlay_gl.cpp`: ImGui 1.92 texture protocol, its own vertex array and buffers) at the end of
  `present()`. Hold Minus half a second to open; B or Minus closes; L / R change tabs; the game keeps running
  without buttons. Tabs on the Switch: Saves, Switch, Mods, Language / About (Graphics, Display and Controls
  are desktop options). The Switch tab (`platform/settings_switch.cpp`) sets the GPU profile live (default
  `0x92220007`, falling back to `0x92220008` and `0x00020004`), the picture adjustments (sliders; Original
  and Vivid) and the frame-rate counter, saved as `switch*` keys in `settings.ini`. Changes made by the menu
  run on the host loop's thread (`hostui::run_posted`). Font: the system's shared font.
- **Mesa's persistent shader cache.** devkitPro's Mesa 20.1 has no disk cache, so every program in
  `shadercache_gl.bin` was compiled and linked again at every start (171-261 s with a full cache).
  `tools/switch/mesa/build_mesa.sh` rebuilds the same package with centollOS's patches (single-file disk
  cache on Horizon, nvc0 machine code through it, compile counters); there a cached program loaded in
  11-13 ms instead of 144-209 ms. Cache: `sdmc:/switch/wwhd/cache/`; `WWHD_MESA_CACHE=0|reset`; log lines
  `[mesa] shader cache:` and `[mesa] shader compile:`.
- The GPU timer correction (×1.63, round 20) also applies to the `WWHD_GL_PIPESTATS` line now.

## Rendering resolution

Measured on the headless build (title screen, busy scene) with `WWHD_DUMP_TARGETS=500`, which now also logs every surface that frame's draws rendered to (`[gl] frame N target ...`, depth included, with its draw count). `GX2SetTVBuffer` now logs the TV render mode.

**The 3D scene is drawn at 1280×720, not 1920×1080.** Render targets drawn in frame 500 (~7,400 draws):

| Size | GX2 format | Draws | What it is |
|---|---|---|---|
| 1024×1024 ×3 | 005 (D16, depth) | 4,221 | shadow map (cascades) |
| 1280×720 | 80E (D32F, depth) | 3,118 | main scene depth |
| 1280×720 | 019 (RGB10A2) | 1,668 and 1,472 | main scene colour (two buffers) |
| 1280×720 | 001 (R8) | 1,616 | full-res mask written with the scene |
| 1280×720 | 001 (R8) / 019 (RGB10A2) | 1 each | full-screen passes |
| 960×540 | 019 (RGB10A2) / 01A (RGBA8) | 1–3 each | post-processing (half of 1080p) |
| 854×480 | 019 (RGB10A2) | 1 | GamePad-size buffer |
| 640×360 | 80E (D32F) / 001 / 019 / 806 (R16F) | 41–42, 1 | half-res pass (effects drawn at half resolution) |
| 480×270 → 60×33 | 816 (R11G11B10F) / 01A / 019 / 806 | 1–3 each | bloom/blur down-sample chain |

The picture copied to the TV scan buffer is 1280×720. The Switch window is 1280×720, so presentation does not scale.

### Why 720p

- **Not the TV mode.** The game calls `GX2CalcTVSize`/`GX2SetTVBuffer` with TV render mode 5 (1080p): it chooses 5 itself (`02751208`: mode 5, or 3 when a word of its display object is 0). The runtime reports no TV mode (the game imports no scan-mode query), and `GX2CalcTVSize` (`gx2_core.cpp`) sizes the scan buffer from the mode it is given.
- **The game has a dynamic resolution.**
  - It first builds its whole render-target set at 1920×1080 (with 960×540 half-res buffers). Before the first frame it rebuilds the same set at 1280×720 (640×360 half-res), from the same call sites (`0279F9B4`, `027D6774`, under `0272A674`).
  - The size is chosen at `0272A2C4`–`0272A5C8`. The default size comes from a table at `1049FB6C` (1920×1080) and the low one from the layer object (`+0x167C`, 1280×720).
  - The game switches to the low size when a GPU time it measures is above a threshold (`+0x1674`), and back after it stays under `+0x1678` for 4 frames. The measurement is a 20-entry ring of samples in the object at `*(101F8D74)`.
- **Our GPU timestamps trigger it.**
  - The samples come from `GX2SampleTopGPUCycle`/`GX2SampleBottomGPUCycle` (`0274C42C`, `0274C468`): GPU commands that write the GPU clock when the GPU reaches them. The game reads them back a frame later (`0274C054`–`0274C24C`, through `GX2GPUTimeToCPUTime`).
  - The runtime (`gx2_core.cpp`, `HLE(gx2, GX2SampleTopGPUCycle)`) writes the CPU clock at the moment of the call instead. The differences the game computes then come out negative, and the stored "GPU time" is about 4×10¹⁵ (float bits `59690453`), far above any threshold.
  - So the game always picks 1280×720. It is stable (same value every frame), but it is an accident of the stub, not a choice.

### Options

The CPU cost (draw count, the render thread) does not depend on the resolution: only the GPU's per-pixel work does.

- **Forcing 1080p** (make the samples report a fast GPU, or patch the size choice).
  - The screen-sized passes (scene colour and depth, the 1280×720 mask and full-screen passes, the half-res chain) get 2.25× the pixels. Shadow maps and the 960×540 post chain do not change.
  - The scan copy and presentation would scale 1920×1080 down to the 1280×720 window. Handheld that is wasted work; docked it only helps with a 1920×1080 window (not set up: the window is 1280×720).
  - Memory: about 5 MB more per 4-byte target (RGB10A2, RGBA8, D32F), roughly 30 MB in all.
  - The Switch GPU (Tegra X1 at 921 MHz: ~470 GFLOPS, 16 ROPs, 25.6 GB/s LPDDR4) has more raw power than the Wii U's (~176 GFLOPS, 8 ROPs), which could show this game at 1080p. But the Wii U kept its render targets in 32 MB of eDRAM, while here every pass goes through shared memory: bandwidth would be the limit.
  - Not worth it until `GPU busy` (round 3 stats) shows GPU headroom in gameplay.
- **Lowering the resolution** (patch the low size at `+0x167C`, e.g. 960×540 or 1152×648).
  - Saves GPU fill and bandwidth in proportion (960×540 is 0.56× the pixels of 720p), with a softer picture.
  - Gains fps only while the GPU is the limit. Today the render thread (CPU) is, so it would gain nothing yet. It becomes useful if `GPU busy` is high, or at stock GPU clocks (307–460 MHz handheld, a third to a half of 921 MHz).
  - Round 20 does this in the renderer instead, only when the GPU is the limit (dynamic resolution, `WWHD_DYNAMIC_RES`), and keeps the HUD at full resolution.
- **Pinning 720p explicitly.** Rather than relying on the invalid timestamps, the samples could report a constant GPU time, so a future change to the stub cannot silently switch the game to 1080p. Not done yet.

## Black screen on Switch: diagnosis

### Hardware test of the shader-present build (`logs-switch/afterfix`)

- The driver is `nouveau / NV120 / 4.3 (Core Profile) Mesa 20.1.0-rc3`, and every extension we check for is present.
- **Rendering works on the Switch.**
  - `TV mean` is non-zero, for example 0.51 / 0.60 / 0.33.
  - `frame_600.png` shows the correct game picture (the controller selection screen).
  - There are 0 GL errors and 0 skipped draws.
- **The window stays black.**
  - `window mean` is 0.000 throughout.
  - Every pixel of `frame_600_window.png` is exactly `000000ff`, including the heartbeat corner.
  - So the full-window clear (which ignores size) lands, but everything sized from the window does not: the present draw, the old blit and the scissored heartbeat.

### Cause

- switch-mesa's `switch_create_window_surface` (`src/egl/drivers/switch/egl_switch.c`) never sets the EGL surface's `Width`/`Height`, so `eglQuerySurface(EGL_WIDTH/HEIGHT)` returns **0×0**.
- `present()` sized the destination rectangle from those values. The present viewport (and earlier the blit rectangle) therefore had zero size, and the heartbeat scissor was at y = −24.

### Fix (built, not yet tested on hardware)

- `present()` takes the window size from `nwindowGetDimensions(nwindowGetDefault())`, which is 1280×720 (set by libnx `__nx_win_init`).
- Startup logs `[gl] window: EGL surface WxH, native window WxH` to confirm the 0×0 surface size on hardware.

### Hypotheses ruled out by the hardware log

- Missing extensions.
- Dark rendering on nouveau.
- Render-thread swaps: a full clear done on the render thread reaches the window buffer.

## Installing on the Switch

1. Copy `build/switch/wwhd.nro` to `sdmc:/switch/wwhd/wwhd.nro`.
2. Copy the decrypted game to `sdmc:/switch/wwhd/game/`, containing `code/`, `content/` and `meta/`. Saves go to `sdmc:/switch/wwhd/save/`.
3. Launch from hbmenu in **title-takeover mode**: hold R while starting a game, not from the album applet, because 1.4 GiB of memory is needed.
4. Optional `sdmc:/switch/wwhd/env.txt`, one entry per line:

   ```
   WWHD_DUMP_FRAMES=600,1800
   WWHD_DUMP_TARGETS=1800
   --trace
   ```

5. Logs go to `sdmc:/switch/wwhd/wwhd.log`; native crashes go to `sdmc:/atmosphere/crash_reports/`.

## Changed and added files

Modified:
`.gitignore` (adds `/Rom/` and `shaderfail_*.glsl`), `CMakeLists.txt`, `README.md` (Switch section), `runtime/include/ppc.h`, `runtime/src/audio_out.cpp`, `runtime/src/core.cpp`, `runtime/src/gfx/renderer.cpp`, `runtime/src/gfx/renderer.h`, `runtime/src/gx2/decompiler_glue.cpp`, `runtime/src/gx2/gx2_core.cpp`, `runtime/src/hle/ax.cpp`, `runtime/src/hle/fs.cpp`, `runtime/src/input_map.cpp`, `runtime/src/main.cpp`, `runtime/src/platform/host.h`, `runtime/src/threads.cpp`, `runtime/third_party/cemu/Common/betype.h`, `runtime/third_party/cemu/fetch_shader_parse.cpp`, `runtime/third_party/cemu/latte_support.cpp`.

Added:
`.github/agents/switch-port.agent.md`, `runtime/src/gfx/gl/` (all files), `runtime/src/platform/input_switch.cpp`, `runtime/src/platform/input_switch.h`, `runtime/src/platform/input_headless.cpp`, `runtime/third_party/cemu/Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h`, `tools/switch/build.sh`, `docs/switch-port.md`.

## Known limitations and follow-ups

- The GamePad (second-screen) picture is not shown; only the TV image is presented. The controller acts as a Pro Controller by default, so the game puts its HUD, map and menus on the TV picture.
- Renderer features that are no-ops on GL: the in-game AO toggle and the full-size occlusion depth (`WWHD_AO_HIRES` in the Vulkan/Metal renderers; the AO line fix itself is on, `WWHD_AO_MODE`), anisotropic filtering, FXAA, aspect-ratio adjustment, renderer restart, capture. Resolution scale works below 1 (round 20).
- Performance: see [Performance](#performance-in-progress). Gameplay ran at 8–12 fps before round 2 and 14–19 fps after it.
- Mods that need a mouse or keyboard are inactive.
- Rounds 1–15 are committed on `feature/switch-port`; rounds 16-26 are not committed yet.
