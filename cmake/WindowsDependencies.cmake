# Native LLVM targets the MSVC ABI. Build missing dependencies with that same
# compiler instead of requiring MSYS2 or a separately configured package manager.
# Also used by release builds (WWHD_BUNDLED_DEPS, any platform): there glslang, zlib and LZ4 are always
# built from source, so no system copy compiled by a different toolchain ends up in the release.
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

if(WWHD_HAS_VULKAN)
  find_package(Vulkan 1.3 REQUIRED)
  if(NOT WWHD_BUNDLED_DEPS)
    find_package(glslang CONFIG QUIET)
  endif()
  if(NOT TARGET glslang::glslang)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glslang
      URL https://github.com/KhronosGroup/glslang/archive/refs/tags/16.0.0.tar.gz
      URL_HASH SHA256=172385478520335147d3b03a1587424af0935398184095f24beab128a254ecc7)
    FetchContent_MakeAvailable(glslang)
    # The runtime includes <glslang/SPIRV/GlslangToSpv.h> (the installed layout); in glslang's source
    # tree the header is SPIRV/GlslangToSpv.h, so give the build tree the installed spelling too.
    set(GLSLANG_SHIM "${CMAKE_BINARY_DIR}/glslang-include")
    file(COPY "${glslang_SOURCE_DIR}/SPIRV/" DESTINATION "${GLSLANG_SHIM}/glslang/SPIRV"
         FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
    target_include_directories(SPIRV INTERFACE "$<BUILD_INTERFACE:${GLSLANG_SHIM}>")
  endif()
endif()

if(WWHD_SDL_HOST)
  find_package(SDL3 CONFIG QUIET HINTS "$ENV{VULKAN_SDK}")
  if(NOT TARGET SDL3::SDL3)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    # the graphical installer links SDL3 statically (one self-contained setup program)
    if(WWHD_SETUP_GUI)
      set(SDL_STATIC ON CACHE BOOL "" FORCE)
    else()
      set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(SDL3
      URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
      URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)
    FetchContent_MakeAvailable(SDL3)
  endif()
endif()

set(ZLIB_USE_STATIC_LIBS ON)
if(NOT WWHD_BUNDLED_DEPS)
  find_package(ZLIB QUIET)
endif()
if(NOT TARGET ZLIB::ZLIB)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(zlib
    URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz
    URL_HASH SHA256=17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c)
  FetchContent_MakeAvailable(zlib)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
  target_include_directories(zlibstatic PUBLIC "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
endif()

if(WWHD_SDL_HOST)
  if(NOT WWHD_BUNDLED_DEPS)
    find_path(LZ4_INCLUDE_DIR lz4.h)
    find_library(LZ4_LIBRARY NAMES lz4)
  endif()
  if(NOT LZ4_INCLUDE_DIR OR NOT LZ4_LIBRARY)
    set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
    set(LZ4_BUNDLED_MODE ON)
    FetchContent_Declare(lz4
      URL https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz
      URL_HASH SHA256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
      SOURCE_SUBDIR build/cmake)
    FetchContent_MakeAvailable(lz4)
  endif()
endif()
