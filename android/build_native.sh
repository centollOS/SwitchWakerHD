#!/usr/bin/env bash
# Builds the native libraries of the Android app (arm64-v8a) with the NDK:
#   SDL3 (shared), glslang and lz4 (static) into android/build/prefix, then the game as libmain.so,
# and copies the .so files to android/app/libs/arm64-v8a for Gradle.
#
# usage: android/build_native.sh [deps|game|all]   (default all)
# env:   ANDROID_SDK (default C:/Android/Sdk), NDK_VERSION, DEPS (sources of SDL, glslang, lz4;
#        default C:/DEV/android-deps), JOBS (default 8), GEN_DIR (default build/gen),
#        CPU (default oryon-1: Snapdragon 8 Elite, e.g. Galaxy S25; the APK then runs only on that
#        CPU family. CPU=generic builds for any arm64 phone)
# The game code in build/gen is generated from your own game (tools/recomp/recomp.py) and stays on
# your machine; the APK built from it contains it and must not be distributed.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
sdk="${ANDROID_SDK:-C:/Android/Sdk}"
ndk="$sdk/ndk/${NDK_VERSION:-30.0.16248370}"
deps="${DEPS:-C:/DEV/android-deps}"
jobs="${JOBS:-8}"
api=33  # Android 13: bionic backtrace() (crash logs); Vulkan 1.3 phones ship 13 or newer
cmake="$(ls -d "$sdk"/cmake/*/bin | tail -1)/cmake.exe"
ninja="$(dirname "$cmake")/ninja.exe"
[ -x "$cmake" ] || cmake="cmake"
[ -x "$ninja" ] || ninja="ninja"
out="$here/build"
prefix="$out/prefix"
cpu="${CPU:-oryon-1}"
cpuflags=()
if [ "$cpu" != generic ]; then cpuflags=("-DCMAKE_C_FLAGS=-mcpu=$cpu" "-DCMAKE_CXX_FLAGS=-mcpu=$cpu"); fi
common=(-G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake"
        -DANDROID_ABI=arm64-v8a "-DANDROID_PLATFORM=android-$api" -DANDROID_STL=c++_shared
        -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$prefix" "-DCMAKE_FIND_ROOT_PATH=$prefix"
        "-DCMAKE_PREFIX_PATH=$prefix")

dep() {  # name source-dir cmake-args...
    local name="$1" src="$2"; shift 2
    echo "== $name"
    "$cmake" -S "$src" -B "$out/$name" "${common[@]}" "$@" > "$out/$name.log"
    "$cmake" --build "$out/$name" -j "$jobs" >> "$out/$name.log"
    "$cmake" --install "$out/$name" >> "$out/$name.log"
}

build_deps() {
    mkdir -p "$out"
    dep SDL3 "$deps/SDL" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    dep glslang "$deps/glslang" -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF -DBUILD_SHARED_LIBS=OFF \
        -DGLSLANG_ENABLE_INSTALL=ON -DENABLE_HLSL=OFF
    dep lz4 "$deps/lz4/build/cmake" -DLZ4_BUILD_CLI=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON
}

build_game() {
    echo "== game (libmain.so)"
    "$cmake" -S "$root" -B "$out/game" "${common[@]}" "${cpuflags[@]}" \
        "-DGEN_DIR=${GEN_DIR:-$root/build/gen}" > "$out/game.log"
    "$cmake" --build "$out/game" -j "$jobs" --target wwhd >> "$out/game.log" 2>&1 || { tail -40 "$out/game.log"; exit 1; }
    local libs="$here/app/libs/arm64-v8a"
    mkdir -p "$libs"
    local strip="$ndk/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
    "$strip" --strip-unneeded "$out/game/libmain.so" -o "$libs/libmain.so"  # 400 MB -> 52 MB
    cp "$prefix/lib/libSDL3.so" "$libs/"
    cp "$ndk/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$libs/"
    ls -la "$libs"
}

case "${1:-all}" in
deps) build_deps ;;
game) build_game ;;
all) build_deps; build_game ;;
*) echo "usage: $0 [deps|game|all]"; exit 2 ;;
esac
