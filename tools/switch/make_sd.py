#!/usr/bin/env python3
"""SwitchWakerHD: build the game for the Switch from your own dump and lay out an SD card folder.

No game files, code or keys come with this repository or its releases. This script does every step on
your computer, from your own legally dumped copy (The Wind Waker HD, USA, title 00050000-10143500,
version 0):

  1. extracts the disc image (.wux/.wud) into build/sd-game/ (or takes an already extracted folder);
  2. checks that code/cking.rpx is the version the port is made for;
  3. translates the game's PowerPC code to C (tools/recomp/recomp.py -> build/gen/);
  4. builds the Switch homebrew in the devkitPro container (tools/switch/build.sh -> build/switch-dk/wwhd.nro);
  5. writes OUT/switch/wwhd/ (wwhd.nro and game/): copy OUT's contents to the root of the SD card.

usage:
  make_sd.py --image GAME.wux [--out DIR]     disc key: GAME.key next to the image (16 bytes or 32 hex digits);
                                              common key: WIIU_COMMON_KEY (32 hex digits) or common.key
                                              next to the image or in the current folder
  make_sd.py --wua GAME.wua [--out DIR]       a Cemu Wii U archive (no keys; needs: pip install zstandard)
  make_sd.py --game-dir EXTRACTED [--out DIR] a folder with code/, content/, meta/ (e.g. from Cemu's
                                              mlc01/usr/title/00050000/10143500, without the update)
  options: --shaders shadercache_gl.bin       also compile a shader list into shadercache_dksh.bin
           --jobs N                           parallel compiles (each needs ~1.5 GB of memory)

Needs Python 3 with pycryptodome (only for --image: pip install pycryptodome) and Docker or Podman, or
a native devkitPro (Windows: INSTALL.md, "Windows without Docker").
What it makes contains the game: it is for your own console only; do not share it.
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "installer"))
import setup  # noqa: E402  (the desktop installer's game checks: same title, same version)


def fail(msg):
    sys.exit("\nmake_sd: " + msg)


def step(n, text):
    print("\n[%d/5] %s" % (n, text), flush=True)


def run(cmd, env=None):
    print("  $ " + " ".join(cmd), flush=True)
    if subprocess.call(cmd, cwd=ROOT, env=env) != 0:
        fail("this step failed (see the messages above)")


# Native build (no Docker/Podman): used when neither is installed. On Windows it needs a devkitPro install
# (C:/devkitPro, https://devkitpro.org) with: pacman -S switch-dev deko3d uam switch-lz4 switch-zlib
DKP_WIN = "C:/devkitPro"


def have_container_engine():
    return bool(shutil.which("docker") or shutil.which("podman"))


def native_toolchain_ok():
    if os.name == "nt":
        return os.path.isdir(DKP_WIN + "/devkitA64") and os.path.isfile(DKP_WIN + "/msys2/usr/bin/bash.exe")
    return bool(os.environ.get("DEVKITPRO"))


def native_shell(env):
    """argv prefix that runs build.sh with devkitPro's own bash. On Windows a plain `bash` may be WSL's, so
    devkitPro's MSYS2 bash is used, with DEVKITPRO as MSYS2 sees it."""
    if os.name != "nt":
        return ["bash"]
    env["DEVKITPRO"] = "/opt/devkitpro"
    env["CHERE_INVOKING"] = "1"  # stay in the repository folder
    env["MSYS2_PATH_TYPE"] = "inherit"  # keep the Windows PATH
    return [DKP_WIN + "/msys2/usr/bin/bash.exe", "-l"]


def extract(image):
    try:
        import Crypto.Cipher.AES  # noqa: F401
    except ImportError:
        fail("extracting a disc image needs pycryptodome: python3 -m pip install pycryptodome")
    if not os.path.isfile(os.path.splitext(image)[0] + ".key"):
        fail("the disc key was not found: put it next to the image as %s" % (os.path.splitext(image)[0] + ".key"))
    dst = os.path.join(ROOT, "build", "sd-game")
    tmp = dst + ".partial"
    shutil.rmtree(tmp, ignore_errors=True)
    run([sys.executable, "-I", os.path.join(ROOT, "tools", "wudextract.py"), image, "extract", tmp])
    if not setup.valid_game_folder(tmp):
        fail("the extracted files are incomplete (no code/cking.rpx, content/ or meta/meta.xml)")
    shutil.rmtree(dst, ignore_errors=True)
    os.replace(tmp, dst)
    return dst


def extract_wua(archive):
    try:
        import zstandard  # noqa: F401
    except ImportError:
        fail("extracting a .wua needs zstandard: python -m pip install zstandard")
    dst = os.path.join(ROOT, "build", "sd-game")
    tmp = dst + ".partial"
    shutil.rmtree(tmp, ignore_errors=True)
    run([sys.executable, "-I", os.path.join(ROOT, "tools", "wuaextract.py"), archive, "extract", tmp])
    if not setup.valid_game_folder(tmp):
        fail("the extracted files are incomplete (no code/cking.rpx, content/ or meta/meta.xml)")
    shutil.rmtree(dst, ignore_errors=True)
    os.replace(tmp, dst)
    return dst


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--image", help="the disc image (.wux/.wud)")
    src.add_argument("--wua", help="a Cemu Wii U archive (.wua; no keys needed)")
    src.add_argument("--game-dir", help="an extracted game folder (code/, content/, meta/)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "sd"), help="where the SD card folder goes")
    ap.add_argument("--shaders", help="a shadercache_gl.bin from the console, compiled into shadercache_dksh.bin")
    ap.add_argument("--jobs", type=int, help="parallel compiles")
    args = ap.parse_args()
    if not (args.image or args.wua or args.game_dir):
        ap.error("one of --image, --wua or --game-dir is required")
    native = not have_container_engine()
    if native:
        if not native_toolchain_ok():
            fail("Docker or Podman is needed (the Switch toolchain runs in the devkitpro/devkita64 image), or "
                 "a native devkitPro with the Switch libraries: see INSTALL.md, \"Windows without Docker\"")
        if args.shaders:
            fail("--shaders needs Docker or Podman (tools/switch/dksh_cache runs in a container): "
                 "leave it out on the native build")

    step(1, "the game files")
    if args.image:
        game = extract(os.path.abspath(args.image))
    elif args.wua:
        game = extract_wua(os.path.abspath(args.wua))
    else:
        game = os.path.abspath(args.game_dir)
    if not setup.valid_game_folder(game):
        fail("%s is not an extracted game (code/cking.rpx, content/, meta/meta.xml)" % game)
    print("  " + game)

    step(2, "checking the game version")
    try:
        setup.check_game_version(game)
    except setup.SetupError as e:
        fail(str(e))
    print("  The Wind Waker HD (USA), version 0: OK")

    step(3, "translating the game code to C (about a minute)")
    gen = os.path.join(ROOT, "build", "gen")
    if os.path.islink(gen):
        fail("build/gen is a symbolic link: remove it (or run from a checkout without one)")
    shutil.rmtree(gen, ignore_errors=True)
    run([sys.executable, "-I", os.path.join(ROOT, "tools", "recomp", "recomp.py"), os.path.join(game, "code", "cking.rpx"), gen])

    step(4, "building the Switch homebrew (10-20 minutes the first time)")
    env = dict(os.environ)
    if args.jobs:
        env["WWHD_JOBS"] = str(args.jobs)
    if native:
        env["WWHD_NATIVE"] = "1"
    run((native_shell(env) if native else ["bash"]) + ["tools/switch/build.sh"], env=env)
    nro = os.path.join(ROOT, "build", "switch-dk", "wwhd.nro")
    if not os.path.isfile(nro):
        fail("the build made no %s" % nro)
    dksh = None
    if args.shaders:
        dksh = os.path.join(ROOT, "build", "shadercache_dksh.bin")
        run(["bash", os.path.join("tools", "switch", "dksh_cache", "build.sh"), "build", os.path.abspath(args.shaders), dksh])

    step(5, "the SD card folder")
    out = os.path.abspath(args.out)
    wwhd = os.path.join(out, "switch", "wwhd")
    os.makedirs(wwhd, exist_ok=True)
    shutil.copyfile(nro, os.path.join(wwhd, "wwhd.nro"))
    if dksh:
        shutil.copyfile(dksh, os.path.join(wwhd, "shadercache_dksh.bin"))
    gdst = os.path.join(wwhd, "game")
    if os.path.realpath(gdst) != os.path.realpath(game):
        shutil.rmtree(gdst, ignore_errors=True)
        for part in ("code", "content", "meta"):
            shutil.copytree(os.path.join(game, part), os.path.join(gdst, part))
    print("""
Done: %s

Copy the contents of that folder to the root of the SD card (switch/wwhd/ ends up at
sdmc:/switch/wwhd/). Saves and settings already on the card are kept. Then hold R while starting
any installed game to open the Homebrew Menu in title mode and pick SwitchWakerHD.

These files contain the game: they are for your own console only; do not share them.""" % out)


if __name__ == "__main__":
    main()
