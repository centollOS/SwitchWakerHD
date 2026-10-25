# dksh_cache (host): WGS1 shadercache_gl.bin -> WDK1 shadercache_dksh.bin with glsl_to_deko + uamlib.
# Built by build.sh in a Debian container; also builds anywhere with CMake, a C++17 compiler and zlib.
cmake_minimum_required(VERSION 3.20)
project(dksh_cache C CXX)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(WWHD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_subdirectory(${WWHD_ROOT}/runtime/third_party/uam ${CMAKE_BINARY_DIR}/uam EXCLUDE_FROM_ALL)
find_package(ZLIB REQUIRED)
# translate (translate.cpp): the runtime's Latte decompiler (runtime/third_party/cemu, OpenGL mode as on deko3d)
set(CEMU_DIR ${WWHD_ROOT}/runtime/third_party/cemu)
file(GLOB_RECURSE CEMU_SOURCES CONFIGURE_DEPENDS ${CEMU_DIR}/*.cpp)
list(FILTER CEMU_SOURCES EXCLUDE REGEX "LatteDecompilerEmitMSL[^/]*\\.cpp$")
add_library(cemu_latte_host STATIC ${CEMU_SOURCES})
# runtime/src: the decompiler includes gfx/vulkan/vertex_cpu_policy.h (header only)
target_include_directories(cemu_latte_host PUBLIC ${CEMU_DIR} ${CEMU_DIR}/Cafe ${WWHD_ROOT}/runtime/third_party/fmt/include
                           ${WWHD_ROOT}/runtime/src)
target_compile_definitions(cemu_latte_host PUBLIC ENABLE_OPENGL=1)
target_compile_options(cemu_latte_host PRIVATE "SHELL:-include \"${CEMU_DIR}/cemu_shim.h\"" -w -O2)
set_target_properties(cemu_latte_host PROPERTIES CXX_STANDARD 20)

# fetch_attributes.cpp: the runtime's decoder of semantic fetch attributes (WWFA0001, a shipped manifest's form)
add_executable(dksh_cache dksh_cache.cpp translate.cpp ${WWHD_ROOT}/runtime/src/gfx/deko/glsl_convert.cpp
                          ${WWHD_ROOT}/runtime/src/gfx/deko/shader_files.cpp ${WWHD_ROOT}/runtime/src/gx2/fetch_attributes.cpp)
target_include_directories(dksh_cache PRIVATE ${WWHD_ROOT}/runtime/src/gfx/deko ${WWHD_ROOT}/runtime/src/gx2)
set_target_properties(dksh_cache PROPERTIES CXX_STANDARD 20)
target_compile_options(dksh_cache PRIVATE -Wall -Wextra)
set_source_files_properties(translate.cpp ${WWHD_ROOT}/runtime/src/gx2/fetch_attributes.cpp
                            PROPERTIES COMPILE_OPTIONS "-include;${CEMU_DIR}/cemu_shim.h;-Wno-unused-parameter")
target_link_libraries(dksh_cache PRIVATE uamlib ZLIB::ZLIB cemu_latte_host)
