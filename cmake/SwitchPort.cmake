# SwitchWakerHD (this fork): the Switch homebrew build (deko3d renderer, libnx host, devkitA64's GCC), kept
# out of CMakeLists.txt. CMakeLists.txt includes this file near the top and calls the three macros below at
# marked hooks. devkitPro's toolchain file (its own Switch.cmake) sets NX_ROOT.
if(DEFINED NX_ROOT)
  set(WWHD_SWITCH ON)
else()
  set(WWHD_SWITCH OFF)
endif()

if(WWHD_SWITCH)
  # DEKO3D: deko3d (the Switch's own GPU API, docs/deko3d-plan.md) with the libnx host, the only renderer
  # there (the OpenGL renderer it replaced was removed after dev 207349b). Upstream's renderer choice is skipped.
  set(WWHD_RENDERER DEKO3D CACHE STRING "Graphics backends: DEKO3D (the Switch)")
  string(TOUPPER "${WWHD_RENDERER}" WWHD_RENDERER)
  if(NOT WWHD_RENDERER STREQUAL "DEKO3D")
    message(FATAL_ERROR "The Switch build supports only WWHD_RENDERER=DEKO3D")
  endif()
  set(WWHD_HAS_METAL OFF)
  set(WWHD_HAS_VULKAN OFF)
  set(WWHD_HAS_DEKO3D ON)
  set(WWHD_SDL_HOST OFF)
  message(STATUS "WWHD renderers: ${WWHD_RENDERER} (deko3d, libnx host)")
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    # devkitA64 is GCC; __attribute__((musttail)) needs GCC 15
    if(CMAKE_C_COMPILER_VERSION VERSION_LESS 15)
      message(FATAL_ERROR "The recompiled game needs GCC 15 or newer (musttail); update devkitA64")
    endif()
  elseif(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The recompiled game requires Clang (musttail support); select clang/clang++")
  endif()
  # devkitA64 (aarch64-none-elf) follows the arm64 ABI: unsigned plain char; signed as everywhere else
  # (CMakeLists.txt: Linux and Android on arm64)
  add_compile_options(-fsigned-char)
else()
  set(WWHD_HAS_DEKO3D OFF)
endif()

# after gamecode, cemu_latte and imgui: their Switch options; uam (all platforms: uam_dksh_test) and deko3d
macro(switch_port_libraries)
  if(WWHD_SWITCH)
    # no frame-pointer setup in each of the ~40,000 recompiled functions (code size: the Switch's
    # caches are small and the game code is ~38 MB)
    target_compile_options(gamecode PRIVATE -fomit-frame-pointer)
    # each game file's code on a 64-byte boundary: the game code's alignment no longer moves with the runtime's size
    target_compile_options(gamecode PRIVATE "SHELL:-include \"${CMAKE_SOURCE_DIR}/runtime/include/switch_code_align.h\"")
  endif()
  if(WWHD_HAS_DEKO3D)
    # deko3d uses the decompiler's OpenGL mode (the same GLSL and shader cache keys; docs/deko3d-plan.md 1)
    target_compile_definitions(cemu_latte PUBLIC ENABLE_OPENGL=1)
  endif()
  if(WWHD_SWITCH)
    # no processes to open links with on the console (ImGui's default uses execvp/waitpid)
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
  endif()

  # ---- uam (devkitPro's deko3d shader compiler, MIT, runtime/third_party/uam, 1.1.0 + PATCHES.md): the
  # 'uamlib' target (GLSL -> DKSH, uam_api.h) for the coming deko3d renderer; built only when something
  # links it. -DWWHD_UAM_TEST=ON adds uam_dksh_test (desktop).
  add_subdirectory(runtime/third_party/uam EXCLUDE_FROM_ALL)
  # ---- deko3d (devkitPro's Switch GPU API, zlib, runtime/third_party/deko3d, 0.5.0 + PATCHES.md): built from
  # source for its fixes, in place of devkitPro's libdeko3d / libdeko3dd
  if(WWHD_RENDERER STREQUAL "DEKO3D")
    add_subdirectory(runtime/third_party/deko3d EXCLUDE_FROM_ALL)
  endif()
endmacro()

# the runtime's sources: the deko3d renderer and the libnx host in, desktop crash logs out
macro(switch_port_sources)
  if(WWHD_SWITCH)
    # desktop crash logs (module names through dladdr); the Switch has its own handler in main.cpp
    list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "runtime/src/crash_addr\\.cpp$")
  endif()
  if(WWHD_HAS_DEKO3D)
    # deko3d renderer and the libnx host
    file(GLOB DEKO3D_SOURCES CONFIGURE_DEPENDS runtime/src/gfx/deko/*.cpp)
    list(APPEND GFX_SOURCES ${DEKO3D_SOURCES} runtime/src/platform/input_switch.cpp runtime/src/platform/hostui_switch.cpp
                            runtime/src/platform/settings_switch.cpp runtime/src/platform/settings_ini.cpp
                            runtime/src/platform/debug_server.cpp
                            runtime/src/platform/debug_switch.cpp runtime/src/platform/posix_stubs_switch.cpp
                            runtime/src/platform/startup_checks_switch.cpp runtime/src/platform/prepare_graphics_switch.cpp runtime/src/platform/audio_guard_switch.cpp
                            runtime/src/platform/prepare_graphics_ui_switch.cpp runtime/src/platform/ui_text_switch.cpp runtime/src/platform/shader_scan_switch.cpp runtime/src/platform/shader_list_switch.cpp
                            runtime/src/platform/background_shaders_switch.cpp runtime/src/platform/main_switch.cpp runtime/src/platform/core_switch.cpp runtime/src/platform/audio_out_switch.cpp runtime/src/platform/overlay_switch.cpp
                            runtime/src/platform/upstream_stubs_switch.cpp)
  endif()
endmacro()

# after the wwhd executable's renderer setup: the guest base register, deko3d, its shaders and the NRO
macro(switch_port_executable)
  # Switch: the guest memory base stays in the reserved register x28 (runtime/include/ppc.h, core.cpp
  # guest_call). On by default since v0.4.4 (main thread ~2% faster in the performance test's busy scenes).
  option(WWHD_SWITCH_BASE_REG "Switch: keep the guest memory base in x28" ON)
  if(WWHD_SWITCH AND WWHD_SWITCH_BASE_REG)
    target_compile_options(gamecode PRIVATE -ffixed-x28)
    target_compile_definitions(gamecode PRIVATE PPC_BASE_REG=1)
    target_compile_options(wwhd PRIVATE -ffixed-x28)
    target_compile_definitions(wwhd PRIVATE PPC_BASE_REG=1)
  endif()
  if(WWHD_HAS_DEKO3D)
    target_compile_definitions(wwhd PRIVATE WWHD_HAS_DEKO3D=1)
    find_path(LZ4_INCLUDE_DIR lz4.h REQUIRED)
    find_library(LZ4_LIBRARY NAMES lz4 REQUIRED)
    target_include_directories(wwhd PRIVATE ${LZ4_INCLUDE_DIR})
    # the renderer's own shaders (FPS counter, settings overlay, depth-only): GLSL compiled at build time
    # by devkitPro's uam into DKSH and embedded (bin2s); gfx/deko/shaders/*.glsl, named <name>_<vsh|fsh>.glsl
    file(GLOB DEKO3D_SHADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/src/gfx/deko/shaders/*.glsl)
    set(_dksh)
    foreach(_glsl ${DEKO3D_SHADERS})
      get_filename_component(_name ${_glsl} NAME_WE)
      if(_name MATCHES "_vsh$")
        set(_stage vert)
      elseif(_name MATCHES "_fsh$")
        set(_stage frag)
      else()
        message(FATAL_ERROR "${_glsl}: deko3d shaders are named <name>_vsh.glsl or <name>_fsh.glsl")
      endif()
      nx_add_shader_program(dksh_${_name} ${_glsl} ${_stage} OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/dksh/${_name}.dksh)
      list(APPEND _dksh dksh_${_name})
    endforeach()
    dkp_add_embedded_binary_library(deko_shaders ${_dksh})
    # deko3d_debug (libdeko3dd): deko3d's checks of every call and its messages (gfx/deko/backend.cpp debug_message: a misuse
    # or failure is logged before the process ends). Release libdeko3d aborts with only a 2359-xxxx result.
    # On while the backend is new (also in Release builds); off for performance measurements.
    option(WWHD_DEKO3D_DEBUG_LIB "deko3d: link the debug library libdeko3dd (also in Release builds)" OFF)
    if(WWHD_DEKO3D_DEBUG_LIB OR CMAKE_BUILD_TYPE STREQUAL "Debug")
      target_compile_definitions(wwhd PRIVATE WWHD_DEKO3D_DEBUG_LIB=1)
      target_link_libraries(wwhd PRIVATE deko_shaders ${LZ4_LIBRARY} deko3d_debug)
    else()
      target_link_libraries(wwhd PRIVATE deko_shaders ${LZ4_LIBRARY} deko3d_release)
    endif()
    # uam on the console: the game shaders that no DKSH cache has (gfx/deko/shaders_dk.cpp, its worker thread)
    target_link_libraries(wwhd PRIVATE uamlib)
    # threads created without attributes get a large stack, newlib's C11 thrd_create results are fixed
    # (core.cpp), and abort() logs its caller first (main.cpp)
    target_link_options(wwhd PRIVATE -Wl,--wrap=pthread_create -Wl,--wrap=thrd_create -Wl,--wrap=abort)
    # the default build is wwhd.nro (what the HOME-menu forwarder starts); the debug-library build says so in
    # hbmenu and is named wwhd_dk_debug.nro, so it can sit in its own folder next to it
    if(WWHD_DEKO3D_DEBUG_LIB OR CMAKE_BUILD_TYPE STREQUAL "Debug")
      nx_generate_nacp(OUTPUT wwhd_dk.nacp NAME "SwitchWakerHD (deko3d debug)" AUTHOR "centollOS" VERSION "0.1.0")
      nx_create_nro(wwhd NACP wwhd_dk.nacp OUTPUT wwhd_dk_debug.nro)
    else()
      nx_generate_nacp(OUTPUT wwhd_dk.nacp NAME "SwitchWakerHD" AUTHOR "centollOS" VERSION "0.1.0")
      nx_create_nro(wwhd NACP wwhd_dk.nacp OUTPUT wwhd.nro)
    endif()
  endif()
endmacro()
