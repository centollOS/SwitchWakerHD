#!/usr/bin/env python3
"""The players' Switch build (docs/pinned-toolchain-plan.md, step 4): build/switch-dk/wwhd.nro with the pinned
devkitPro toolchain (tools/toolchain/fetch.py) and the cmake and ninja next to this Python (the installers' pinned
packages), in an environment built from scratch: no bash, MSYS2, Docker, or anything the computer has installed
(its devkitPro, PATH, DEVKITPRO, CC, ...). tools/switch/build.sh stays the developers' build (Docker, native).

usage: python3 -I tools/switch/build_switch.py [--devkitpro DIR] [--icon PATH] [--name NAME]
  --devkitpro   a pinned toolchain folder (fetch.py prints it); default: fetch.py's in WWHD_TOOLCHAIN_DIR (the
                installers' work folder) or build/toolchain, fetched if missing
  --icon        the NRO's icon: an image or a game folder (default: the title-screen logo from WWHD_GAME_DIR's game,
                else build/sd's or Rom's; make_icon.py); without one, devkitPro's default icon
  --name        the name on the HOME screen (default: Wind Waker HD)
Needs build/gen (tools/recomp/recomp.py). WWHD_JOBS: parallel compiles (default: half the processors).
"""
import argparse
import glob
import json
import os
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
WINDOWS = os.name == "nt"
EXE = ".exe" if WINDOWS else ""


def fail(msg):
    sys.exit("build_switch.py: " + msg)


def python_tool(name):
    """cmake / ninja installed next to this Python (pip's Scripts on Windows, bin elsewhere)"""
    for d in (os.path.join(sys.prefix, "Scripts"), os.path.join(sys.prefix, "bin"), os.path.dirname(sys.executable)):
        p = os.path.join(d, name + EXE)
        if os.path.isfile(p):
            return p
    fail("%s is missing next to this Python (%s): python -m pip install cmake ninja" % (name, sys.executable))


def clean_env(dkp):
    """the build's whole environment: the pinned toolchain's folders first, then the system's own minimum"""
    keep = ("SystemRoot", "SYSTEMROOT", "windir", "TEMP", "TMP", "USERPROFILE", "HOMEDRIVE", "HOMEPATH",
            "PATHEXT", "COMSPEC", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE", "HOME", "USER", "LOGNAME",
            "TMPDIR")
    env = {k: os.environ[k] for k in keep if k in os.environ}
    tools = [os.path.join(dkp, "tools", "bin"), os.path.join(dkp, "devkitA64", "bin"),
             os.path.dirname(python_tool("cmake")), os.path.dirname(sys.executable)]
    if WINDOWS:
        root = os.environ.get("SystemRoot", r"C:\Windows")
        system = [os.path.join(root, "System32"), root]
    else:
        system = ["/usr/bin", "/bin", "/usr/sbin", "/sbin"]
    env.update({
        "PATH": os.pathsep.join(tools + system),
        "DEVKITPRO": dkp.replace("\\", "/"),
        "DEVKITA64": os.path.join(dkp, "devkitA64").replace("\\", "/"),
        "LC_ALL": "C",
        "PYTHONUTF8": "1",
        "WWHD_BUILD_ENV": "pinned",
    })
    return env


def pinned_toolchain(arg):
    if arg:
        if not os.path.isfile(os.path.join(arg, "toolchain.json")):
            fail("%s is not a pinned toolchain folder (no toolchain.json; tools/toolchain/fetch.py makes one)" % arg)
        return os.path.abspath(arg)
    # WWHD_TOOLCHAIN_DIR: where the installers keep it (their work folder, shared by releases), else build/toolchain
    dest = os.environ.get("WWHD_TOOLCHAIN_DIR") or os.path.join(ROOT, "build", "toolchain")
    out = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "toolchain", "fetch.py"),
                          "--dest", dest, "--cache", dest + "-cache"],
                         stdout=subprocess.PIPE, text=True)
    if out.returncode != 0:
        fail("the pinned devkitPro toolchain could not be fetched (tools/toolchain/fetch.py)")
    return out.stdout.strip().splitlines()[-1]


def nro_icon(icon_src):
    """build/nro_icon.jpg from --icon or the game's files (make_icon.py), or "" for devkitPro's default icon"""
    icon = os.path.join(ROOT, "build", "nro_icon.jpg")
    if not icon_src:
        for c in [os.environ.get("WWHD_GAME_DIR", ""), os.path.join(ROOT, "build", "sd", "switch", "wwhd", "game")] + \
                 sorted(glob.glob(os.path.join(ROOT, "Rom", "Decrypted", "*"))) + [os.path.join(ROOT, "Rom")]:
            if c and os.path.isdir(os.path.join(c, "meta")):
                icon_src = c
                break
    if os.path.exists(icon):
        os.remove(icon)
    if not icon_src:
        print("build_switch.py: no game files for the icon (--icon PATH, or WWHD_GAME_DIR); the NRO gets the default"
              " icon", file=sys.stderr)
        return ""
    os.makedirs(os.path.dirname(icon), exist_ok=True)
    rc = subprocess.call([sys.executable, os.path.join(ROOT, "tools", "switch", "forwarder", "make_icon.py"),
                          icon_src, icon])
    if rc != 0 or not os.path.isfile(icon):
        print("build_switch.py: no NRO icon (making it from %s needs Pillow); the NRO gets the default icon" % icon_src,
              file=sys.stderr)
        return ""
    return icon


def configured_for(build_dir):
    """(generator, toolchain file) an existing build folder was configured with"""
    cache = os.path.join(build_dir, "CMakeCache.txt")
    gen = tc = None
    if os.path.isfile(cache):
        with open(cache, errors="replace") as f:
            for line in f:
                if line.startswith("CMAKE_GENERATOR:INTERNAL="):
                    gen = line.split("=", 1)[1].strip()
                elif line.startswith("CMAKE_TOOLCHAIN_FILE:"):
                    tc = line.split("=", 1)[1].strip()
    return gen, tc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--devkitpro")
    ap.add_argument("--icon", default=os.environ.get("WWHD_ICON", ""))
    ap.add_argument("--name", default=os.environ.get("WWHD_NACP_NAME", "") or "Wind Waker HD")
    a = ap.parse_args(argv)
    os.chdir(ROOT)
    if not glob.glob(os.path.join("build", "gen", "code_*.c")):
        fail("build/gen is missing: run python3 tools/recomp/recomp.py game/code/cking.rpx build/gen first")
    if a.icon and not os.path.exists(a.icon):
        fail("icon file not found: " + a.icon)
    dkp = pinned_toolchain(a.devkitpro)
    with open(os.path.join(dkp, "toolchain.json")) as f:
        print("build_switch.py: devkitPro toolchain %s (%s)" % (json.load(f)["id"], dkp), flush=True)
    env = clean_env(dkp)
    build_dir = os.path.join("build", "switch-dk")
    toolchain_file = os.path.join(dkp, "cmake", "Switch.cmake").replace("\\", "/")
    # a folder configured by another generator (build.sh's Makefiles) or another toolchain starts over
    gen, tc = configured_for(build_dir)
    if (gen and gen != "Ninja") or (tc and os.path.normcase(tc) != os.path.normcase(toolchain_file)):
        for p in ("CMakeCache.txt", "CMakeFiles"):
            path = os.path.join(build_dir, p)
            if os.path.isdir(path):
                import shutil
                shutil.rmtree(path)
            elif os.path.exists(path):
                os.remove(path)
    cpus = os.cpu_count() or 2
    jobs = os.environ.get("WWHD_JOBS") or str(max(1, cpus // 2))
    icon = nro_icon(a.icon)
    configure = [python_tool("cmake"), "-S", ".", "-B", build_dir, "-G", "Ninja",
                 "-DCMAKE_MAKE_PROGRAM=" + python_tool("ninja").replace("\\", "/"),
                 "-DCMAKE_TOOLCHAIN_FILE=" + toolchain_file,
                 "-DCMAKE_BUILD_TYPE=Release",
                 # the players' build: what build.sh passes by default, every time (a cached value must not outlive it)
                 "-DWWHD_RENDERER=DEKO3D", "-DWWHD_DEKO3D_DEBUG_LIB=OFF", "-DWWHD_SWITCH_BASE_REG=ON",
                 "-DWWHD_HEAP_CENSUS=OFF", "-DWWHD_PGO=OFF",
                 "-DWWHD_NACP_NAME=" + a.name,
                 "-DWWHD_NRO_ICON=" + icon.replace("\\", "/"),
                 "-DPython3_EXECUTABLE=" + sys.executable.replace("\\", "/")]
    if WINDOWS:
        # devkitPro's pkg-config wrapper is a shell script, which CMake's find_program does not take on Windows;
        # Switch.cmake only insists that it exists, and the build uses no pkg-config
        configure.append("-DPKG_CONFIG_EXECUTABLE=" +
                         os.path.join(dkp, "portlibs", "switch", "bin", "aarch64-none-elf-pkg-config").replace("\\", "/"))
    for cmd in (configure, [python_tool("cmake"), "--build", build_dir, "-j", jobs]):
        print("> " + " ".join(cmd), flush=True)
        rc = subprocess.call(cmd, env=env)
        if rc != 0:
            fail("%s failed (exit code %d)" % (os.path.basename(cmd[0]), rc))
    print("built %s" % os.path.join(build_dir, "wwhd.nro"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
