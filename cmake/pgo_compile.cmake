# SwitchWakerHD (this fork): the compiler launcher of a WWHD_PGO=GEN|USE build (cmake/SwitchPgo.cmake,
# docs/pgo-release-plan.md step 1). Run as
#   cmake -DWWHD_PGO_ROOT=<checkout> -DWWHD_PGO_REL=<checkout relative to the build folder> -P pgo_compile.cmake -- <compiler> <args...>
# (the "--": without it cmake takes the compiler's -D options for its own)
#
# GCC keeps a function's profile only when the function's lineno_checksum matches, and that checksum hashes the source
# file's name as the compiler was given it. CMake gives absolute names, and every player's checkout is somewhere else, so a
# profile made by the maintainers would be dropped for almost every function (-fprofile-prefix-map and -ffile-prefix-map do
# not change the checksum; docs/pgo.md). For a profiled file (its flags have -fprofile-use or -fprofile-generate) every
# argument naming something in the checkout (the source, -I, -isystem, -include) is given relative to the build folder,
# "../../runtime/src/x.cpp", the same on every computer while the build folder is a folder of the checkout's build/. The
# working folder stays the build folder, so the dependency files keep paths ninja understands. Any other file is compiled
# exactly as CMake wrote it.

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

if(_profiled)
  string(LENGTH "${WWHD_PGO_ROOT}/" _rootlen)
  set(_out)
  foreach(_a IN LISTS _args)
    # the argument itself, or its value after a joined option (-I/x, -isystem/x): the checkout's prefix becomes relative.
    # Not the -fprofile-* folders (-fprofile-prefix-path must stay absolute: GCC compares it to the object's absolute
    # name; the profile's own folder is not part of any checksum)
    if(NOT _a MATCHES "^-fprofile-")
      foreach(_opt "" "-I" "-isystem" "-iquote" "-include")
        string(LENGTH "${_opt}" _optlen)
        string(SUBSTRING "${_a}" 0 ${_optlen} _head)
        if(_head STREQUAL _opt)
          string(SUBSTRING "${_a}" ${_optlen} -1 _value)
          string(LENGTH "${_value}" _valuelen)
          if(_valuelen GREATER_EQUAL _rootlen)
            string(SUBSTRING "${_value}" 0 ${_rootlen} _prefix)
            if(_prefix STREQUAL "${WWHD_PGO_ROOT}/")
              string(SUBSTRING "${_value}" ${_rootlen} -1 _rest)
              set(_a "${_opt}${WWHD_PGO_REL}/${_rest}")
              break()
            endif()
          endif()
        endif()
      endforeach()
    endif()
    list(APPEND _out "${_a}")
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
