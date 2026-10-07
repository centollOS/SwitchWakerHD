#!/usr/bin/env bash
# Builds tools/switch/dksh_cache (build/dksh_cache/dksh_cache) in a Debian container (image wwhd-dksh-tools,
# made from this directory's Dockerfile on the first run), then runs it there with the given arguments, if any.
# Paths given to it are relative to the repository root (mounted at /src); absolute paths under $HOME work
# too (mounted at the same place).
#     tools/switch/dksh_cache/build.sh build build/shader-harvest/shadercache_gl.bin build/shader-harvest/shadercache_dksh.bin
#     tools/switch/dksh_cache/build.sh coverage <console shadercache_gl.bin> <harvest shadercache_gl.bin>
set -euo pipefail
cd "$(dirname "$0")/../../.."
engine=$(command -v podman || command -v docker || true)
if [[ -z "$engine" ]]; then
    echo "podman or docker is required" >&2
    exit 1
fi
image=wwhd-dksh-tools
if ! "$engine" image inspect "$image" >/dev/null 2>&1; then
    "$engine" build -t "$image" tools/switch/dksh_cache
fi
jobs=${WWHD_JOBS:-4}
# build/gen and Rom may be symlinks out of the tree: mount $HOME too so that absolute paths resolve
"$engine" run --rm -v "$PWD":/src -v "$HOME":"$HOME" -w /src "$image" bash -c "
    cmake -S tools/switch/dksh_cache -B build/dksh_cache -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null &&
    ninja -C build/dksh_cache -j $jobs dksh_cache &&
    if [[ \$# -gt 0 ]]; then build/dksh_cache/dksh_cache \"\$@\"; fi" bash "$@"
