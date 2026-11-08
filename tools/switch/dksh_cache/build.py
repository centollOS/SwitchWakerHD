#!/usr/bin/env python3
"""Builds tools/switch/dksh_cache with the pinned toolchain's zig (tools/toolchain/fetch.py), cmake and ninja next to
this Python and zlib from its pinned source, in an environment made from scratch, then runs it with the given
arguments, if any: build.sh's interface without bash, Docker or the computer's compilers
(docs/pinned-toolchain-plan.md, step 4). The program: build/dksh_cache-pinned/dksh_cache.

usage: python3 -I tools/switch/dksh_cache/build.py [--devkitpro DIR] [ARGS for dksh_cache...]
       e.g. build build/shader-harvest/shadercache_gl.bin build/shader-harvest/shadercache_dksh.bin
"""
import os
import platform
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "switch"))
import build_switch  # noqa: E402  (its pinned toolchain, python tools and clean environment)

WINDOWS = os.name == "nt"


def macos_target():
    """macOS: an explicit target (zig's lowest macOS) instead of the native one. With the native target zig builds
    its libc++ for this Mac's macOS version and SDK, and on macOS 27 that fails (#129: INFINITY undeclared in
    __random/clamp_to_integral.h). With an explicit one it builds, and the program runs on any macOS from 13 on."""
    arch = {"arm64": "aarch64", "aarch64": "aarch64", "x86_64": "x86_64"}.get(platform.machine().lower())
    return " -target %s-macos" % arch if arch else ""


def wrappers(zig, out):
    """zig's cc, c++, ar and ranlib as programs CMake can be given (it takes no "zig cc" with a space)"""
    os.makedirs(out, exist_ok=True)
    paths = {}
    target = macos_target() if sys.platform == "darwin" else ""
    for name, sub in (("cc", "cc" + target), ("c++", "c++" + target), ("ar", "ar"), ("ranlib", "ranlib")):
        if WINDOWS:
            p = os.path.join(out, "zig-%s.cmd" % name)
            text = '@"%s" %s %%*\r\n' % (zig, sub)
        else:
            p = os.path.join(out, "zig-%s" % name)
            text = '#!/bin/sh\nexec "%s" %s "$@"\n' % (zig, sub)
        with open(p, "w", newline="") as f:
            f.write(text)
        if not WINDOWS:
            os.chmod(p, 0o755)
        paths[name] = os.path.abspath(p).replace("\\", "/")
    return paths


def main(argv):
    dkp_arg = None
    if argv[:1] == ["--devkitpro"]:
        dkp_arg, argv = argv[1], argv[2:]
    dkp = build_switch.pinned_toolchain(dkp_arg)
    zig = os.path.join(os.path.dirname(dkp), "zig", "zig" + (".exe" if WINDOWS else ""))
    if not os.path.isfile(zig):
        build_switch.fail("no zig next to the pinned toolchain (%s): fetch it again (tools/toolchain/fetch.py)" % zig)
    os.chdir(ROOT)
    build_dir = os.path.join("build", "dksh_cache-pinned")
    w = wrappers(zig, os.path.join(build_dir, "zig"))
    env = build_switch.clean_env(dkp)
    # zig keeps its compiled C/C++ runtime here (inside the build folder, not the user's cache)
    env["ZIG_GLOBAL_CACHE_DIR"] = os.path.abspath(os.path.join(build_dir, "zig-cache"))
    env["ZIG_LOCAL_CACHE_DIR"] = env["ZIG_GLOBAL_CACHE_DIR"]
    cmake = build_switch.python_tool("cmake")
    jobs = os.environ.get("WWHD_JOBS") or "4"
    configure = [cmake, "-S", os.path.join("tools", "switch", "dksh_cache"), "-B", build_dir, "-G", "Ninja",
                 "-DCMAKE_MAKE_PROGRAM=" + build_switch.python_tool("ninja").replace("\\", "/"),
                 "-DCMAKE_BUILD_TYPE=Release", "-DDKSH_BUNDLED_ZLIB=ON",
                 "-DCMAKE_C_COMPILER=" + w["cc"], "-DCMAKE_CXX_COMPILER=" + w["c++"],
                 "-DCMAKE_AR=" + w["ar"], "-DCMAKE_RANLIB=" + w["ranlib"]]
    for cmd in (configure, [cmake, "--build", build_dir, "-j", jobs, "--target", "dksh_cache"]):
        rc = subprocess.call(cmd, env=env, stdout=sys.stderr)
        if rc != 0:
            build_switch.fail("dksh_cache: %s failed (exit code %d)" % (os.path.basename(cmd[0]), rc))
    exe = os.path.join(build_dir, "dksh_cache" + (".exe" if WINDOWS else ""))
    if argv:
        return subprocess.call([exe] + argv)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
