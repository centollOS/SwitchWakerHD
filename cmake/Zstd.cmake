# zstd (BSD-3-Clause, https://github.com/facebook/zstd), pinned source (URL + SHA-256) like the
# other dependencies in cmake/WindowsDependencies.cmake. The extractor (wwhd-extract) needs it to
# read Cemu's Wii U archives (.wua); it is always built from this source as a static library with
# the project's compiler (also in non-release builds), so wwhd-extract stays one self-contained
# program and never picks up a system libzstd. Offline builds: -DFETCHCONTENT_SOURCE_DIR_ZSTD=DIR
# with the unpacked zstd-1.5.7 release.
#
# Only lib/common, lib/decompress and lib/compress are compiled (no CLI, no multithreading, no
# assembly); compression is used by the extractor's tests only (a synthetic archive), the linker
# leaves it out of wwhd-extract.
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(zstd
  URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
  URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
  SOURCE_SUBDIR lib)  # no CMakeLists.txt there: only downloaded and unpacked, the library is defined below
FetchContent_MakeAvailable(zstd)
file(GLOB WWHD_ZSTD_SOURCES ${zstd_SOURCE_DIR}/lib/common/*.c ${zstd_SOURCE_DIR}/lib/decompress/*.c
                            ${zstd_SOURCE_DIR}/lib/compress/*.c)
add_library(wwhd_zstd STATIC ${WWHD_ZSTD_SOURCES})
target_include_directories(wwhd_zstd PUBLIC ${zstd_SOURCE_DIR}/lib)
target_compile_definitions(wwhd_zstd PRIVATE ZSTD_DISABLE_ASM XXH_NAMESPACE=ZSTD_)
target_compile_options(wwhd_zstd PRIVATE -w -O2)
set(WWHD_ZSTD_LICENSE ${zstd_SOURCE_DIR}/LICENSE)
