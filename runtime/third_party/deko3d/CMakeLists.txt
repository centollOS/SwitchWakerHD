# deko3d 0.5.0 (devkitPro, zlib licence) built from source instead of devkitPro's libdeko3d.a, for the
# fixes in PATCHES.md. Two static libraries, as upstream's Makefile builds them: deko3d_release
# (-DNDEBUG) and deko3d_debug (-DDEBUG: checks every call, devkitPro's libdeko3dd). generated/ holds
# what dekodef/dekomme (devkitPro's dekotools) write from source/maxwell/*.def and *.mme (PATCHES.md).
file(GLOB DEKO3D_SOURCES CONFIGURE_DEPENDS source/*.cpp source/maxwell/*.cpp)
foreach(variant release debug)
  add_library(deko3d_${variant} STATIC ${DEKO3D_SOURCES})
  target_include_directories(deko3d_${variant} PUBLIC include)
  target_include_directories(deko3d_${variant} PRIVATE generated source source/maxwell)
  target_compile_definitions(deko3d_${variant} PRIVATE __DK_INTERNAL__)
  target_compile_options(deko3d_${variant} PRIVATE -O2 -fno-rtti -fno-exceptions -Wall
                         $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
endforeach()
target_compile_definitions(deko3d_release PRIVATE NDEBUG=1)
target_compile_definitions(deko3d_debug PRIVATE DEBUG=1)
