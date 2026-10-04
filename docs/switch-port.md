# Switch port — current state

Status as of 2026-10-03. All changes below are on the `feature/switch-port` branch.

## Summary

| Area | State |
|---|---|
| Target | Horizon OS homebrew (`.nro`, launched from hbmenu via Atmosphère) |
| Toolchain | devkitPro devkitA64 (GCC 15.2, libnx, switch-mesa 20.1) in the `devkitpro/devkita64` container |
| Graphics | OpenGL 4.3 core on Mesa nouveau (switch-mesa) through EGL + glad |
| Build | Works: `tools/switch/build.sh` → `build/switch/wwhd.nro` (~52 MB) |
| Boot on hardware | Works: picture, sound and controller input (as a Wii U Pro Controller, so everything is on one screen) |
| Performance on hardware | Round 2: 29.5 fps in menus (the game's cap), 18–19 fps at ~3,200 draws per frame, 14–15 fps at 5,000–7,400 (overclocked: CPU 1785 MHz, GPU 921 MHz, RAM 1600 MHz). The render thread is the limit. Round 3 is built, not yet tested |
| On-screen FPS counter | Top-left corner; `WWHD_FPS=0` hides it, `WWHD_FPS=2` adds render-thread load, draws per frame and how often the GPU was busy |
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

### Next steps

- The main thread is the limit at the top of the island (CPU-bound, ~29 fps): let a non-blocking send to a full `update_ubo` queue grow it instead of failing (moves 5,000-10,000 jobs a second to cores 0/2), and keep shrinking the recompiled code's cost.
- First-time texture uploads (16-31 ms in a turn, 216 ms when the save loads) remain: decoding on another thread is the option.

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
- Renderer features that are no-ops on GL: resolution scale, AO, anisotropic filtering, FXAA, aspect-ratio adjustment, renderer restart, capture.
- Performance: see [Performance](#performance-in-progress). Gameplay ran at 8–12 fps before round 2 and 14–19 fps after it.
- Mods that need a mouse or keyboard are inactive.
- Rounds 1–15 are committed on `feature/switch-port`.
