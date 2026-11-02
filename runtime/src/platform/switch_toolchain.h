// SwitchWakerHD (this fork): the toolchain that built this NRO (cmake/SwitchToolchainInfo.cmake writes the
// definition into the build directory). In the log's first lines and in crash reports: a crash address maps to
// a function only with an ELF built by the same toolchain (docs/pinned-toolchain-plan.md).
#pragma once

namespace switch_toolchain {
const char* info();  // "gcc 15.2.0, libnx 4.12.0-1 a1b2c3d4, zlib ..., lz4 ..., uam ... on Linux (docker <image>, host Darwin)"
}  // namespace switch_toolchain
