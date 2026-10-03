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
| Performance on hardware | 20–30 fps in light scenes, 8–12 fps in gameplay with 3,000–6,000 draws per frame (log of 2026-10-03, before the draw-path optimizations below). The render thread is the limit |
| On-screen FPS counter | Top-left corner; `WWHD_FPS=0` hides it, `WWHD_FPS=2` adds render-thread load, draws per frame and GPU lag |
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
| GL debug output | `wwhd.log` | `WWHD_GL_DEBUG=1`: first 200 non-notification messages |
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

### Round 2: the draw path (current build)

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

### Next steps

- Hardware test of round 2: compare `render thread busy`, `submit` and `GPU lag` in gameplay against the 2026-10-03 log. If the render thread stays saturated, go after the remaining submit cost: keep static vertex buffers in GPU buffers across frames (fewer vertex-buffer rebinds and copies), and sort or skip redundant texture rebinds.
- If GPU lag grows to a frame or more, the GPU is the limit. The game renders the TV picture at its native 1920×1080; a lower internal resolution would be the next step.
- If the render thread is no longer busy but fps stays low, profile the guest threads (`WWHD_SCHED_STATS=1`).
- Find the crash after the intro (if it still happens) using the Atmosphère crash report, symbolized against `build/switch/wwhd.elf` with the `[boot] code at` base address.

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
- Performance: see [Performance](#performance-in-progress). Gameplay ran at 8–12 fps before round 2.
- Mods that need a mouse or keyboard are inactive.
- All of the above is committed on `feature/switch-port`; round 2 is waiting for a hardware test.
