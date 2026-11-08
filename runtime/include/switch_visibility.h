/* (this fork) Force-included into the runtime's own sources on the Switch (cmake/SwitchPort.cmake,
   WWHD_SWITCH_HIDDEN): see switch_code_align.h for why. Declarations from the headers that follow, the project's and
   the toolchain's, are hidden; libstdc++'s own `namespace std` keeps default visibility by its attributes. */
#pragma GCC visibility push(hidden)
