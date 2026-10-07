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
add_executable(dksh_cache dksh_cache.cpp ${WWHD_ROOT}/runtime/src/gfx/deko/glsl_convert.cpp
                          ${WWHD_ROOT}/runtime/src/gfx/deko/shader_files.cpp)
target_include_directories(dksh_cache PRIVATE ${WWHD_ROOT}/runtime/src/gfx/deko)
set_target_properties(dksh_cache PROPERTIES CXX_STANDARD 17)
target_compile_options(dksh_cache PRIVATE -Wall -Wextra)
target_link_libraries(dksh_cache PRIVATE uamlib ZLIB::ZLIB)
