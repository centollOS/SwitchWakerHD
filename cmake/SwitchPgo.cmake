# SwitchWakerHD (this fork): profile-guided optimisation of the Switch build (docs/pgo.md).
#
# The recompiled game is machine-translated PowerPC: its branches carry no layout information, so GCC lays
# blocks out, allocates registers and decides what is cold from static guesses. A profile of the real game
# on the console tells it which paths run.
#
#   WWHD_PGO=OFF  (default) nothing changes: a normal build.
#   WWHD_PGO=GEN  a training build: the hot game files (code_hot_*.c) and the runtime's frame-thread code are
#                 instrumented (-fprofile-generate, counters not atomic). It is slower and larger and is never
#                 released. The debug server gets "pgo dump|reset|status" (runtime/src/platform/pgo_switch.cpp),
#                 which writes the counters to sdmc:/switch/wwhd/pgo/*.gcda (tools/switch/pgo_train.py).
#   WWHD_PGO=USE  the same files compiled with that profile (WWHD_PGO_DIR). Code the training never reached
#                 is compiled as usual (-fprofile-partial-training), not for size.
#
# The profile only fits the exact sources it was made from: GCC compares a checksum of every function's flow
# graph and ignores (with -Wno-coverage-mismatch) the profile of a function that changed. Make it again after a
# recompile with a different recompiler, or a change to a profiled runtime file.
set(WWHD_PGO "OFF" CACHE STRING "Switch: profile-guided optimisation: OFF, GEN (instrumented training build) or USE (docs/pgo.md)")
set_property(CACHE WWHD_PGO PROPERTY STRINGS OFF GEN USE)
set(WWHD_PGO_DIR "${CMAKE_SOURCE_DIR}/build/pgo" CACHE PATH "WWHD_PGO=USE: the folder with the .gcda files fetched from the console")
set(WWHD_PGO_SCOPE "game;runtime" CACHE STRING "what the profile covers: game (code_hot_*.c) and/or runtime (runtime/src, the frame threads' code)")
# extra flags for the USE build only, to try variants without editing this file (e.g. -fno-tracer -fno-peel-loops)
set(WWHD_PGO_USE_FLAGS "" CACHE STRING "WWHD_PGO=USE: extra compiler flags for the profiled files")

macro(switch_pgo_configure)
  if(WWHD_SWITCH AND NOT WWHD_PGO STREQUAL "OFF")
    if(NOT WWHD_PGO STREQUAL "GEN" AND NOT WWHD_PGO STREQUAL "USE")
      message(FATAL_ERROR "WWHD_PGO must be OFF, GEN or USE")
    endif()
    set(_pgo_files)
    if("game" IN_LIST WWHD_PGO_SCOPE)
      file(GLOB _pgo_hot CONFIGURE_DEPENDS ${GEN_DIR}/code_hot_*.c)
      list(APPEND _pgo_files ${_pgo_hot})
    endif()
    if("runtime" IN_LIST WWHD_PGO_SCOPE)
      get_target_property(_pgo_all wwhd SOURCES)
      foreach(_s ${_pgo_all})
        # the runtime's own C++ (not the generated build info, the vendored libraries or the diagnostics)
        if(_s MATCHES "runtime/src/.*\\.cpp$" AND NOT _s MATCHES "third_party")
          list(APPEND _pgo_files ${_s})
        endif()
      endforeach()
    endif()
    # the object paths (and so the .gcda names) are relative to the build folder: a profile made in one build
    # folder is found by another
    set(_pgo_common -fprofile-prefix-path=${CMAKE_BINARY_DIR})
    if(WWHD_PGO STREQUAL "GEN")
      # not atomic: three cores increment the same counters, a few lost increments do not change a layout
      # -fno-profile-values: no value profiling (indirect-call targets, divisors). Its counters are thread-local, and the
      # runtime's first code (libnx's init hooks, the heap) runs before the console has set up thread-local storage;
      # the generated code's indirect calls go through our own call-site caches anyway
      set(_pgo_flags ${_pgo_common} -fprofile-generate=/pgo -fprofile-update=single -fno-profile-values)
      target_compile_definitions(wwhd PRIVATE WWHD_PGO_GEN=1)
      target_sources(wwhd PRIVATE runtime/src/platform/pgo_switch.cpp)
      target_link_options(wwhd PRIVATE -fprofile-generate)
    else()
      if(NOT IS_DIRECTORY "${WWHD_PGO_DIR}")
        message(FATAL_ERROR "WWHD_PGO=USE: ${WWHD_PGO_DIR} is not a folder (fetch the profile: tools/switch/pgo_train.py)")
      endif()
      # -fno-reorder-functions / -fno-profile-reorder-functions: the profile would rename the functions' sections
      # (.text.hot.*, .text.sorted.*) and tools/switch/hot_sections.ld, which lists .text.<function>, would no
      # longer put them together. Cold basic blocks are still moved out of the functions (.text.unlikely.*).
      set(_pgo_flags ${_pgo_common} -fprofile-use=${WWHD_PGO_DIR} -fprofile-correction -fprofile-partial-training
                     -fno-reorder-functions -fno-profile-reorder-functions -Wno-missing-profile -Wno-coverage-mismatch
                     # -fprofile-use turns on loop unrolling, peeling and tail duplication: on one hot file (code_hot_012)
                     # the object grew 29% (40,180 -> 51,900 bytes), and the hot code is already bigger than the A57's
                     # 2 MiB L2. Without them it is +3%; the block partitioning moves cold blocks out (.text.unlikely).
                     -fno-tracer -fno-unroll-loops -fno-peel-loops -freorder-blocks-and-partition
                     ${WWHD_PGO_USE_FLAGS})
    endif()
    string(REPLACE ";" " " _pgo_flags_text "${_pgo_flags}")
    foreach(_f ${_pgo_files})
      set_property(SOURCE ${_f} APPEND_STRING PROPERTY COMPILE_FLAGS " ${_pgo_flags_text}")
      # the instrumented allocator wrappers leave a relocation against posix_memalign in .debug_info, which --wrap does
      # not redirect (undefined reference at the link): no debug information for that one file in a training build
      if(WWHD_PGO STREQUAL "GEN" AND _f MATCHES "thread_heap_switch\\.cpp$")
        set_property(SOURCE ${_f} APPEND_STRING PROPERTY COMPILE_FLAGS " -g0")
      endif()
    endforeach()
    list(LENGTH _pgo_files _pgo_n)
    message(STATUS "WWHD_PGO=${WWHD_PGO}: ${_pgo_n} files (${WWHD_PGO_SCOPE})")
  endif()
endmacro()
