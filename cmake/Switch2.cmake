# CMake toolchain file for the Switch 2
cmake_minimum_required(VERSION 3.13)

if(NOT CMAKE_SYSTEM_NAME)
  set(CMAKE_SYSTEM_NAME NintendoSwitch)
endif()

if(NOT DEVKITPRO)
  if(NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "Switch 2: set DEVKITPRO")
  endif()
  file(TO_CMAKE_PATH "$ENV{DEVKITPRO}" DEVKITPRO)
endif()
include(${DEVKITPRO}/cmake/devkitA64.cmake)

set(NX_ROOT ${DEVKITPRO}/libounce)
set(WWHD_SWITCH2_TOOLCHAIN ON)
set(DKP_INSTALL_PREFIX_INIT ${DEVKITPRO}/portlibs/switch2)

__dkp_platform_prefix(
  ${DEVKITPRO}/portlibs/switch2
  ${DEVKITPRO}/portlibs/switch
  ${NX_ROOT}
)

find_program(PKG_CONFIG_EXECUTABLE NAMES aarch64-none-elf-pkg-config HINTS "${DEVKITPRO}/portlibs/switch/bin")
if(NOT PKG_CONFIG_EXECUTABLE)
  message(FATAL_ERROR "Could not find aarch64-none-elf-pkg-config")
endif()

find_program(NX_ELF2NRO_EXE NAMES elf2nro HINTS "${DEVKITPRO}/tools/bin")
find_program(NX_NACPTOOL_EXE NAMES nacptool HINTS "${DEVKITPRO}/tools/bin")

set(NX_UAM_EXE "${DEVKITPRO}/tools/bin/uam2" CACHE FILEPATH "uam2, drako3d's shader compiler" FORCE)
if(NOT EXISTS "${NX_UAM_EXE}")
  message(FATAL_ERROR "Switch 2: no ${NX_UAM_EXE} (install uam2 in devkitPro)")
endif()

find_file(NX_DEFAULT_ICON NAMES default_icon.jpg HINTS "${NX_ROOT}" NO_CMAKE_FIND_ROOT_PATH)
