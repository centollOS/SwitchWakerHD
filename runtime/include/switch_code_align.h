/* SwitchWakerHD (this fork): included into every recompiled game file on the Switch (CMakeLists.txt).
   The game code is built with one section per function (devkitPro's -ffunction-sections), and the linker
   lays those sections out file by file after the runtime. An empty, retained section aligned to 64 bytes
   (the Cortex-A57's cache line) at the start of each file makes every file's code start on a 64-byte
   boundary, so where the game code lands no longer depends on the size of the runtime linked before it.
   A runtime change that moved all of it by 16 bytes (mod 64) cost ~1% in the main-thread-bound lava scene
   (2026-10-13, 10 boots per build). */
__asm__(".pushsection .text.0_align, \"axR\", @progbits\n\t.balign 64\n\t.popsection");

/* (this fork) Everything declared from here on has hidden visibility. The Switch build is -fPIC (devkitPro's toolchain
   file), and GCC loads the address of an extern symbol with default visibility through the GOT, because in a shared
   object it might be replaced: `adrp` + `ldr` where `adrp` + `add` would do. Here it was one load more at each of the
   15,000 indirect-call sites (the address of ppc_dispatch, ppc.h PPC_SITE_FN: 26,203 of the game code's 26,508 GOT
   loads) and more in the runtime. Nothing in the NRO is ever preempted or exported, so hidden costs nothing.
   (-fvisibility=hidden does not reach declarations, only this pragma does.) */
#pragma GCC visibility push(hidden)
