#!/usr/bin/env bash
# Builds build/uamtest2/uamtest2.nro (tools/switch/uamtest) in the devkitPro devkitA64 container.
# On the SD card: the console's sdmc:/switch/wwhd/shadercache_gl.bin is the input; copy the Mac's
# shadercache_dksh.bin (tools/switch/dksh_cache) to sdmc:/switch/uamtest/ for the bit-exactness check.
# Results: sdmc:/switch/uamtest/uamtest2.log.
set -euo pipefail
cd "$(dirname "$0")/../../.."
engine=$(command -v podman || command -v docker || true)
if [[ -z "$engine" ]]; then
    echo "podman or docker is required (the devkitpro/devkita64 image provides the Switch toolchain)" >&2
    exit 1
fi
jobs=${WWHD_JOBS:-3}
"$engine" run --rm -v "$PWD":/src -w /src docker.io/devkitpro/devkita64:latest bash -c "
    cmake -S tools/switch/uamtest -B build/uamtest2 -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
          -DCMAKE_BUILD_TYPE=Release >/dev/null &&
    ninja -C build/uamtest2 -j $jobs"
echo "built build/uamtest2/uamtest2.nro"
