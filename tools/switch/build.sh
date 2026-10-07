#!/usr/bin/env bash
# Build the Switch homebrew in the devkitPro devkitA64 container.
#   tools/switch/build.sh                        OpenGL renderer: build/switch/wwhd.nro (the default)
#   WWHD_RENDERER=DEKO3D tools/switch/build.sh   deko3d renderer (docs/deko3d-plan.md, work in progress):
#                                                build/switch-dk/wwhd_dk.nro
# Needs podman or docker, and build/gen from tools/recomp/recomp.py (generated from your own game dump).
# Mesa (OpenGL only): build/switch-mesa/prefix (tools/switch/mesa/build_mesa.sh: Mesa with the persistent
# shader cache) when it exists, else devkitPro's switch-mesa package; WWHD_STOCK_MESA=1 forces the package.
# build/gen and build/switch-mesa may be symbolic links (git worktrees share the main checkout's): their
# targets are mounted into the container at the same path.
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
renderer=$(echo "${WWHD_RENDERER:-OPENGL}" | tr '[:lower:]' '[:upper:]')
mounts=(-v "$PWD":/src:Z)
for link in build/gen build/switch-mesa; do
    if [[ -L $link ]]; then
        target=$(cd "$link" && pwd -P)
        mounts+=(-v "$target":"$target":ro)
    fi
done
case "$renderer" in
OPENGL)
    dir=build/switch
    out=wwhd.nro
    mesa=""
    if [[ ${WWHD_STOCK_MESA:-0} != 1 && -f build/switch-mesa/prefix/lib/libEGL.a ]]; then
        mesa=/src/build/switch-mesa/prefix
        echo "Mesa: build/switch-mesa/prefix ($(cat build/switch-mesa/prefix/cache_id.txt 2>/dev/null))"
    else
        echo "Mesa: devkitPro's switch-mesa (no shader cache; tools/switch/mesa/build_mesa.sh builds one with it)"
    fi
    options="-DWWHD_RENDERER=OPENGL -DWWHD_SWITCH_MESA_DIR=$mesa"
    ;;
DEKO3D)
    dir=build/switch-dk
    out=wwhd_dk.nro
    # deko3d's debug library (checks and readable errors) unless WWHD_DEKO3D_DEBUG_LIB=OFF
    options="-DWWHD_RENDERER=DEKO3D -DWWHD_DEKO3D_DEBUG_LIB=${WWHD_DEKO3D_DEBUG_LIB:-ON}"
    ;;
*)
    echo "WWHD_RENDERER must be OPENGL or DEKO3D" >&2
    exit 1
    ;;
esac
"$engine" run --rm "${mounts[@]}" -w /src "$image" bash -c "
    cmake -S . -B $dir -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
          -DCMAKE_BUILD_TYPE=${WWHD_BUILD_TYPE:-Release} $options &&
    ninja -C $dir -j $jobs"
echo "built $dir/$out"
