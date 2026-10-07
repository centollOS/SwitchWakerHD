#!/usr/bin/env bash
# Build the Switch homebrew in the devkitPro devkitA64 container.
#   tools/switch/build.sh                        deko3d renderer: build/switch-dk/wwhd.nro
#                                                (WWHD_DEKO3D_DEBUG_LIB=ON: deko3d's debug library,
#                                                build/switch-dk/wwhd_dk_debug.nro)
# The console also needs sdmc:/switch/wwhd/shadercache_dksh.bin (tools/switch/dksh_cache, docs/deko3d-plan.md).
# Needs podman or docker, and build/gen from tools/recomp/recomp.py (generated from your own game dump).
# build/gen may be a symbolic link (git worktrees share the main checkout's): its target is mounted into the
# container at the same path.
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
cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc)  # (macOS has no nproc)
jobs=${WWHD_JOBS:-$(( cpus / 2 > 0 ? cpus / 2 : 1 ))}
mounts=(-v "$PWD":/src:Z)
for link in build/gen; do
    if [[ -L $link ]]; then
        target=$(cd "$link" && pwd -P)
        mounts+=(-v "$target":"$target":ro)
    fi
done
dir=build/switch-dk
# deko3d's release library unless WWHD_DEKO3D_DEBUG_LIB=ON (checks every call, readable errors)
debuglib=${WWHD_DEKO3D_DEBUG_LIB:-OFF}
out=wwhd.nro
[[ $debuglib == ON ]] && out=wwhd_dk_debug.nro
options="-DWWHD_RENDERER=DEKO3D -DWWHD_DEKO3D_DEBUG_LIB=$debuglib"
"$engine" run --rm "${mounts[@]}" -w /src "$image" bash -c "
    cmake -S . -B $dir -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
          -DCMAKE_BUILD_TYPE=${WWHD_BUILD_TYPE:-Release} $options &&
    ninja -C $dir -j $jobs"
echo "built $dir/$out"
