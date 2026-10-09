# Shader cache from the player's dump (branch `feature/shader-cache-from-dump`)

**Goal:** players start with a complete `shadercache_dksh.bin`, so the first start no longer shows black or late
textures while the console compiles the game's shaders (the first-start warning in
`platform/startup_checks_switch.cpp`). `tools/switch/make_sd.py` builds the file on the player's computer from
their own dump, the same way it builds `wwhd.nro`.

## Why the file cannot ship

`shadercache_dksh.bin` holds the game's GPU programs (GX2 microcode) translated to GLSL and compiled to the Switch
GPU's machine code (DKSH): a derived form of the game's code, like the NRO. The README says the caches are for
the player's own use, and upstream ships none. `shadercache_gl.bin`, the list the console writes as it plays,
holds the translated GLSL too, so it cannot ship either.

## What can ship: a manifest of identifiers

The console's `shadercache_gl.bin` is not enough to rebuild the shaders elsewhere: its translation records hold a
variant's key and the decompiler's *outputs*, not its *inputs*. A new file holds only identifiers, no game code:

- per GPU program: the hash and size of its microcode, and the stage;
- per variant: the context registers the decompiler reads (`gx2_shader_regs`, the shader key's inputs) and the
  fetch shader's description (attribute formats, offsets, strides: state the game sets, not code);
- the uam/glsl_to_deko revision it was made for.

The game's programs themselves are in every dump: `content/Common/Shaders/{Prim,particle,render_buffer}/*.sharcfb`
and `content/Cafe/Common/**/primitive_renderer_cafe.gsh` (the same files in the USA and European games: the European
session of round 48 found every shader it drew in the USA-built caches, 0 compiled).

## Steps

1. **Recording (runtime, Switch and desktop).** Each new variant appends its identifiers to
   `shader_manifest.bin` next to the other caches (`WWHD_SHADER_MANIFEST=0` turns it off). No change to how shaders
   are translated or drawn.
2. **Harvest on the owner's console.** Play, or drive the Warp tab's tour through the debug server
   (`tools/switch/wwhd_debug.py`), until the manifest covers what the existing `shadercache_gl.bin` covers (3547
   sources). First version: the owner's console only.
3. **Check against the dump (computer).** Every program hash of the manifest is found byte for byte in the
   `.sharcfb`/`.gsh` files of a dump (USA and EU). The open question of the plan: if the game patches a program when
   it loads it (relocations, constants), the tool has to apply the same change. If this fails, stop and rethink.
4. **Tool (computer).** `tools/switch/dksh_cache` gains `from-dump <manifest> <game dir> <shadercache_dksh.bin>`:
   find the programs in the dump, run the same Latte decompiler as the runtime (runtime/third_party/cemu,
   decompiler_glue) with the recorded registers, then glsl_to_deko + uam as `build` does. **Acceptance: the result is
   byte-identical to the owner's console-made `shadercache_dksh.bin`** for the variants the manifest covers.
5. **make_sd.py.** Ships the manifest in the release (`tools/switch/shader_manifest.bin`, checked by
   `tools/release/guard.py`: identifiers only), runs the tool after the NRO build and copies the result to
   `build/sd/switch/wwhd/`. First version through Docker (macOS, Linux, Windows with WSL); the native Windows path
   (devkitPro, no Docker) needs the tool built for Windows: a later step, until then it skips with a note.
6. **Text and docs.** The first-start warning tells players without the file to rebuild with make_sd.py; INSTALL.md
   and the README say what the file is and that it is made on their computer.

Independent of the plan (done on main, 43d74c8): `tools/release/guard.py` refuses `shadercache_*.bin` and `*.dksh`.

## Status

- [x] First-start warning (hardware-tested; its text still names a file players cannot get)
- [x] 1. Recording: `gfx/deko/shader_manifest.cpp` (`WWHD_SHADER_MANIFEST=1`; builds, not yet run on the console)
- [~] 2. Harvest on the owner's console: a first one at the title screen (956 variants, 367 programs)
- [x] 3. Programs found in the dump: **NO for about two thirds** (see "Step 3 result"). Stopped here.
- [ ] 4. Tool, byte-identical result
- [ ] 5. make_sd.py and release
- [ ] 6. Text and docs

## Step 3 result (2026-10-10): most programs are not in the dump as they run

First harvest at the title screen: 956 variants, 367 distinct programs. With `WWHD_SHADER_MANIFEST_DUMP=1` (a
diagnostic: each program's bytes in `sdmc:/switch/wwhd/shader_programs/`, for the owner's own analysis, deleted
afterwards; never shared) the programs as they run were compared with the dump's shader files:

| Programs | Count | What differs |
|---|---|---|
| identical to bytes of a `.sharcfb` | 40 | nothing |
| identical except the last 8 bytes | 89 | the size the registers give is rounded up: the last 8 bytes are memory after the program (two programs with different hashes come from the same place of the file) |
| first bytes differ, middle found | 119 | the game changes the program when it loads it |
| not found at all | 119 | changed more, or made by the game |

So about two thirds of the programs are made or changed by the game's own code at run time (most likely its
emulation of the GameCube pipeline building TEV programs). Rebuilding them on a computer would mean reproducing
that code, not just reading the dump with a list of identifiers. The plan as written is not viable.

Options to decide on:

- **A. Partial cache from the dump:** the 129 programs found (35% of this sample), with the hash computed without the
  last 8 bytes. Less first-start black, not none.
- **B. Understand the game's changes:** if they are a small, regular patch (constants or branch targets taken from
  registers or tables), the tool could apply it; how much work is unknown until the patched words are studied.
- **C. On the console instead:** make the first start's compiles faster or less visible (draw with a waiting shader
  instead of skipping, a "preparing graphics" pass over the shaders seen so far, more compiler threads).

