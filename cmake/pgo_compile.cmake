# SwitchWakerHD (this fork): the compiler launcher of a WWHD_PGO=GEN|USE build (cmake/SwitchPgo.cmake,
# docs/pgo-release-plan.md step 1). Run as
#   cmake -DWWHD_PGO_ROOT=<checkout> -DWWHD_PGO_REL=<checkout relative to the build folder> -DWWHD_PGO_BUILD=<build folder>
#         -DWWHD_PGO_SYSINC=<build folder>/pgo-sysinc.cmake -P pgo_compile.cmake -- <compiler> <args...>
# (the "--": without it cmake takes the compiler's -D options for its own)
#
# GCC keeps a function's profile only when the function's lineno_checksum matches, and that checksum hashes the source
# file's name as the compiler was given it. CMake gives absolute names, and every player's checkout is somewhere else, so a
# profile made by the maintainers would be dropped for almost every function (-fprofile-prefix-map and -ffile-prefix-map do
# not change the checksum; docs/pgo.md). For a profiled file (its flags have -fprofile-use or -fprofile-generate) every
# argument naming something in the checkout (the source, -I, -isystem, -include) is given relative to the build folder,
# "../../runtime/src/x.cpp", the same on every computer while the build folder is a folder of the checkout's build/; and
# devkitPro's headers (the compiler's own ones, libnx's, the portlibs') are read from copies in the build folder,
# "pgo-sysinc/devkitA64/...", since the pinned toolchain's folder is named after its host system's files. The working
# folder stays the build folder, so the dependency files keep paths ninja understands. Any other file is compiled exactly
# as CMake wrote it.

# the command: everything after the "--"
set(_args)
set(_start 0)
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE 1 ${_last})
  if(_start GREATER 0 AND _i GREATER_EQUAL _start)
    list(APPEND _args "${CMAKE_ARGV${_i}}")
  elseif(_start EQUAL 0 AND "${CMAKE_ARGV${_i}}" STREQUAL "--")
    math(EXPR _start "${_i} + 1")
  endif()
endforeach()
if(NOT _args)
  message(FATAL_ERROR "pgo_compile.cmake: no compiler command")
endif()

set(_profiled FALSE)
foreach(_a IN LISTS _args)
  if(_a MATCHES "^-fprofile-(use|generate)")
    set(_profiled TRUE)
    break()
  endif()
endforeach()

# a path in the checkout or in devkitPro's copied headers -> the name a profiled compile uses for it (unchanged otherwise)
function(_pgo_map value out)
  # the build folder first (its generated headers: the release's training build and the players' build have different
  # names), then devkitPro (the pinned toolchain is inside the checkout), then the checkout
  foreach(_base "${WWHD_PGO_BUILD}" "${WWHD_PGO_DKP}" "${WWHD_PGO_ROOT}")
    string(LENGTH "${_base}/" _blen)
    string(SUBSTRING "${value}" 0 ${_blen} _head)
    if(_head STREQUAL "${_base}/")
      string(SUBSTRING "${value}" ${_blen} -1 _rest)
      if(_base STREQUAL "${WWHD_PGO_BUILD}")
        set(${out} "${_rest}" PARENT_SCOPE)
        return()
      endif()
      if(_base STREQUAL "${WWHD_PGO_ROOT}")
        set(${out} "${WWHD_PGO_REL}/${_rest}" PARENT_SCOPE)
        return()
      endif()
      foreach(_c ${WWHD_PGO_COPIED})
        string(FIND "${_rest}/" "${_c}/" _at)  # (a plain prefix: "c++" in the names)
        if(_at EQUAL 0)
          set(${out} "pgo-sysinc/${_rest}" PARENT_SCOPE)
          return()
        endif()
      endforeach()
    endif()
  endforeach()
  set(${out} "${value}" PARENT_SCOPE)
endfunction()

if(_profiled)
  # devkitPro's folder, the header folders copied into the build folder and the compiler's own (implicit) ones, relative
  # to devkitPro (written by cmake/SwitchPgo.cmake)
  include("${WWHD_PGO_SYSINC}")
  set(_out)
  set(_i 0)
  foreach(_a IN LISTS _args)
    # the argument itself, or its value after a joined option (-I/x, -isystem/x). Not the compiler (argument 0) nor the
    # -fprofile-* folders (-fprofile-prefix-path must stay absolute: GCC compares it to the object's absolute name; the
    # profile's own folder is not part of any checksum)
    if(_i GREATER 0 AND NOT _a MATCHES "^-fprofile-")
      foreach(_opt "-isystem" "-iquote" "-include" "-I" "")
        string(LENGTH "${_opt}" _optlen)
        string(SUBSTRING "${_a}" 0 ${_optlen} _head)
        if(_head STREQUAL _opt)
          string(SUBSTRING "${_a}" ${_optlen} -1 _value)
          _pgo_map("${_value}" _mapped)
          if(NOT _mapped STREQUAL _value)
            set(_a "${_opt}${_mapped}")
            break()
          endif()
        endif()
      endforeach()
    endif()
    list(APPEND _out "${_a}")
    math(EXPR _i "${_i} + 1")
  endforeach()
  # the compiler's own header folders, from the copies: -nostdinc drops them, and -isystem at the end keeps their order
  # after the command's own -isystem folders (the order #include_next relies on)
  list(GET _args 0 _cc)
  get_filename_component(_ccname "${_cc}" NAME)
  if(_ccname MATCHES "(\\+\\+|g\\+\\+)(\\.exe)?$")
    set(_implicit ${WWHD_PGO_CXX_IMPLICIT})
  else()
    set(_implicit ${WWHD_PGO_C_IMPLICIT})
  endif()
  list(APPEND _out -nostdinc)
  foreach(_r ${_implicit})
    list(APPEND _out -isystem "pgo-sysinc/${_r}")
  endforeach()
  set(_args "${_out}")
endif()

execute_process(COMMAND ${_args} RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  if(COMMAND cmake_language AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    cmake_language(EXIT 1)
  endif()
  message(FATAL_ERROR "")
endif()
