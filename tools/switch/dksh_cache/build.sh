#!/usr/bin/env bash
# Builds tools/switch/dksh_cache, then runs it with the given arguments, if any:
# - natively (build/dksh_cache-native/dksh_cache) when CMake, Ninja and a C++ compiler are installed: macOS
#   (Xcode's command line tools + Homebrew's cmake and ninja), Linux, or Windows in devkitPro's MSYS2 with
#   pacman -S --needed gcc cmake ninja zlib-devel (on macOS the same output as the container's, byte for byte);
# - else in a Debian container (image wwhd-dksh-tools, made from this directory's Dockerfile on the first run).
#   Paths given to it are relative to the repository root (mounted at /src); absolute paths under $HOME work
#   too (mounted at the same place).
# WWHD_DKSH_CONTAINER=1 always uses the container.
#     tools/switch/dksh_cache/build.sh build build/shader-harvest/shadercache_gl.bin build/shader-harvest/shadercache_dksh.bin
#     tools/switch/dksh_cache/build.sh coverage <console shadercache_gl.bin> <harvest shadercache_gl.bin>
set -euo pipefail
cd "$(dirname "$0")/../../.."
jobs=${WWHD_JOBS:-4}
if [[ "${WWHD_DKSH_CONTAINER:-0}" != 1 ]] && command -v cmake >/dev/null && command -v ninja >/dev/null &&
    { command -v c++ >/dev/null || command -v g++ >/dev/null || command -v clang++ >/dev/null; }; then
    if cmake -S tools/switch/dksh_cache -B build/dksh_cache-native -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null &&
        ninja -C build/dksh_cache-native -j "$jobs" dksh_cache; then
        if [[ $# -gt 0 ]]; then exec build/dksh_cache-native/dksh_cache "$@"; fi
        exit 0
    fi
    echo "dksh_cache: the native build failed; trying the container" >&2
fi
engine=$(command -v podman || command -v docker || true)
if [[ -z "$engine" ]]; then
    echo "dksh_cache needs CMake, Ninja and a C++ compiler (see the top of tools/switch/dksh_cache/build.sh), or" \
         "Docker or Podman" >&2
    exit 1
fi
image=wwhd-dksh-tools
if ! "$engine" image inspect "$image" >/dev/null 2>&1; then
    "$engine" build -t "$image" tools/switch/dksh_cache
fi
# build/gen and Rom may be symlinks out of the tree: mount $HOME too so that absolute paths resolve
"$engine" run --rm -v "$PWD":/src -v "$HOME":"$HOME" -w /src "$image" bash -c "
    cmake -S tools/switch/dksh_cache -B build/dksh_cache -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null &&
    ninja -C build/dksh_cache -j $jobs dksh_cache &&
    if [[ \$# -gt 0 ]]; then build/dksh_cache/dksh_cache \"\$@\"; fi" bash "$@"
