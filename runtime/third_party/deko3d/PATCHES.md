# deko3d in SwitchWakerHD

deko3d 0.5.0, https://github.com/devkitPro/deko3d at `5cc144d` (tag v0.5.0, the version of devkitPro's
`libdeko3d.a` this project linked before; LICENSE: zlib). Built by `CMakeLists.txt` (no Makefile) as the
static libraries `deko3d_release` and `deko3d_debug` (upstream's `libdeko3d` / `libdeko3dd`, same
`-DNDEBUG` / `-DDEBUG` and `-O2 -fno-rtti -fno-exceptions -std=gnu++17`). Only `include/`, `source/`,
`LICENSE` and `README.md` are kept.

`generated/` holds what upstream's Makefile writes with devkitPro's dekotools
(https://github.com/devkitPro/dekotools, `c15523c`), which the devkitA64 image does not ship:

    for d in source/maxwell/*.def; do n=$(basename $d .def)
        if [[ $n == *_3d ]]; then dekodef -h generated/$n.h -m generated/$n.mme $d
        else dekodef -h generated/$n.h $d; fi; done
    dekomme -o generated/mme_macros.h generated/engine_3d.mme $(ls source/maxwell/*.mme | sort)

Rerun it after editing a `.def` or `.mme` file (dekotools builds with meson, bison and flex, e.g. in a
`debian:bookworm` container).

Every change to upstream files is marked `SwitchWakerHD patch N`.

1. **Separate alpha destination factor** (`source/maxwell/gpu_3d_state.cpp`, `dkCmdBufBindBlendStates`):
   upstream writes `dstColorBlendFactor` into `FuncAlphaDst`, so a blend state with its own alpha
   factors blended alpha with the color's destination factor. The Picto Box writes each subject's
   number into the picture's alpha with color (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) and alpha (ONE, ZERO)
   (CB_BLEND0_CONTROL 20010504): the alpha stayed 255 and Lenzo never found the subject (issue #7).
   Issue #12 (place names) was the same bug, worked around in the renderer before. Still unfixed
   upstream at `350f2b0` (2026-02).

2. **Control memory chunks lost on clear** (`source/dk_cmdbuf.cpp`, `CmdBuf::clear`): the command buffer's
   control memory comes in 1 KiB chunks kept in a used list (newest first). `clear()` linked only the newest
   chunk to the free list, overwriting its link to the older ones, so every frame that needed more than one
   chunk leaked the rest: ~22,000 chunks (21.6 MiB) in a 14-minute warp tour, found with the malloc census
   (WWHD_HEAP_CENSUS, docs/memory-round2-plan.md); several hundred MiB over a long session. Still unfixed
   upstream at `350f2b0` (2026-02).
