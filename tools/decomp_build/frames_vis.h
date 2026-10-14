/* Pre-included into every decompiled game unit (CMakeLists.txt, WWHD_DECOMP): gabi.h's weak null
 * gmem_frame_info would otherwise be hidden and, after `ld -r`, local to each unit; with default
 * visibility it stays a global weak symbol that the runtime's definition (runtime/src/decomp/frames.cpp)
 * overrides. */
#pragma once
#include <cstdint>
#include "native_frames.h"
extern "C" __attribute__((visibility("default"))) const GabiFrameInfo* gmem_frame_info(uint32_t addr);
