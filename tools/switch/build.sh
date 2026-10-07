#!/usr/bin/env bash
# Build the Switch homebrew (build/switch/wwhd.nro) in the devkitPro devkitA64 container.
# Needs podman or docker, and build/gen from tools/recomp/recomp.py (generated from your own game dump).
# Mesa: build/switch-mesa/prefix (tools/switch/mesa/build_mesa.sh: Mesa with the persistent shader
# cache) when it exists, else devkitPro's switch-mesa package; WWHD_STOCK_MESA=1 forces the package.
set -euo pipefail
cd "$(dirname "$0")/../.."

if ! ls build/gen/code_*.c >/dev/null 2>&1; then
    echo "build/gen is missing: run python3 tools/recomp/recomp.py game/code/cking.rpx build/gen first" >&2
    exit 1
fi
engine=$(command -v podman || command -v docker || true)
if [[ -z "$engine" ]]; then
    echo "podman or docker is required (the devkitpro/devkita64 image provides the Switch toolchain)" >&2
    exit 1
fi
image=docker.io/devkitpro/devkita64:latest
# each generated file needs a lot of compiler memory at -O3: WWHD_JOBS limits parallel compiles
jobs=${WWHD_JOBS:-$(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 ))}
mesa=""
if [[ ${WWHD_STOCK_MESA:-0} != 1 && -f build/switch-mesa/prefix/lib/libEGL.a ]]; then
    mesa=/src/build/switch-mesa/prefix
    echo "Mesa: build/switch-mesa/prefix ($(cat build/switch-mesa/prefix/cache_id.txt 2>/dev/null))"
else
    echo "Mesa: devkitPro's switch-mesa (no shader cache; tools/switch/mesa/build_mesa.sh builds one with it)"
fi
"$engine" run --rm -v "$PWD":/src:Z -w /src "$image" bash -c "
    cmake -S . -B build/switch -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
          -DCMAKE_BUILD_TYPE=Release -DWWHD_SWITCH_MESA_DIR=$mesa &&
    ninja -C build/switch -j $jobs"
echo "built build/switch/wwhd.nro"
