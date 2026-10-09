#!/usr/bin/env python3
"""build_forwarder_windows.py: build the HOME-screen forwarder (wwhd_forwarder.nsp) on Windows, no Docker.

The forwarder is a small NSP that puts a "Wind Waker HD" icon on the Switch HOME screen and starts
sdmc:/switch/wwhd/wwhd.nro as an application, so the game launches without holding R over another game
(INSTALL.md in this folder). This is the native Windows version of build_forwarder.sh, for a devkitPro
install instead of a container. make_sd.py runs it after a successful native build; it can also be run
on its own, from the repository folder:

  python tools/switch/forwarder/build_forwarder_windows.py [--keys C:\\path\\prod.keys] [--sd build\\sd]

Needs your own console's prod.keys (dump them with Lockpick_RCM): by default %USERPROFILE%\\.switch\\prod.keys,
or --keys PATH. Like build_forwarder.sh it refuses keys inside this folder. Without keys nothing is built and
the script exits with code 3; the game itself (wwhd.nro) does not need them. The keys are only passed by path
to hacBrewPack for the pack step: they are never copied, and the output is checked so that no key value is printed.

Needs (besides the native build of INSTALL.md): git, Pillow (pip install pillow), and gcc in devkitPro's
MSYS2 (open devkitPro > MSYS2, then: pacman -S gcc). Output: build\\forwarder\\wwhd_forwarder.nsp, and with
--sd DIR a copy in DIR\\NSP\\. Install it with DBI or Goldleaf; it needs up-to-date Atmosphere sigpatches.
Not verified offline with hactool, unlike build_forwarder.sh.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
FDIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(FDIR, "..", "..", ".."))
DKP = os.environ.get("DEVKITPRO_WIN", "C:/devkitPro")
EXIT_NO_KEYS = 3

# "01FF" is outside retail ranges, "57574844" is ASCII "WWHD"; the same override as build_forwarder.sh
TITLE_ID = re.sub(r"^0[xX]", "", os.environ.get("WWHD_FORWARDER_TITLE_ID", "01ff575748440000")).lower()
NRO_PATH = "sdmc:/switch/wwhd/wwhd.nro"
NAME, PUBLISHER = "Wind Waker HD", "SwitchWakerHD"
HBLOADER = ("https://github.com/switchbrew/nx-hbloader.git", "82b95122c5ae8dc059bf23893ba7623c72c86773")  # v2.4.5
HACBREWPACK = ("https://github.com/TooTallNate/hacBrewPack.git", "745b16ecfc9ce055743067d200572204cb2aac6c")  # v3.05
LANGS = ["AmericanEnglish", "BritishEnglish", "Japanese", "French", "German", "LatinAmericanSpanish", "Spanish",
         "Italian", "Dutch", "CanadianFrench", "Portuguese", "Russian", "Korean", "TraditionalChinese",
         "SimplifiedChinese", "BrazilianPortuguese"]


def fail(msg):
    sys.exit("\nbuild_forwarder_windows: " + msg)


def run(cmd, cwd=ROOT, env=None):
    print("  $ " + " ".join(cmd), flush=True)
    if subprocess.call(cmd, cwd=cwd, env=env) != 0:
        fail("this step failed (see the messages above)")


def msys(script, env_extra=None, capture=False):
    """Runs a bash script in devkitPro's MSYS2 (a plain `bash` may be WSL's). Variables in env_extra are
    available to the script; paths in them are Windows paths (convert with cygpath -u)."""
    env = dict(os.environ, DEVKITPRO="/opt/devkitpro", MSYS2_PATH_TYPE="inherit", **(env_extra or {}))
    cmd = [DKP + "/msys2/usr/bin/bash.exe", "-l", "-c", "set -e\n" + script]
    if capture:
        p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    return subprocess.call(cmd, env=env), ""


def key_values(path):
    with open(path, "r", errors="replace") as f:
        return {m.group(1).lower() for m in re.finditer(r"=\s*([0-9A-Fa-f]{16,})", f.read())}


def scrub(text, secrets):
    """Drops every line that mentions keys (hacBrewPack prints some), then refuses to return text that still
    contains a key value."""
    kept = chr(10).join(l for l in text.splitlines() if "key" not in l.lower())
    low = kept.lower()
    if any(v in low for v in secrets):
        fail("the tool output contained key material; not printing it")
    return kept


def redacted(text):
    """The full tool output for an error report: every long hex string (a key value) is replaced."""
    return re.sub(r"[0-9A-Fa-f]{16,}", "<hex>", text)


def fetch(dst, url, rev):
    if not os.path.isdir(os.path.join(dst, ".git")):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        run(["git", "-c", "core.autocrlf=false", "clone", "--quiet", url, dst])
    # --force + clean: the patch below must apply to pristine sources
    run(["git", "-C", dst, "-c", "advice.detachedHead=false", "checkout", "--quiet", "--force", rev])
    run(["git", "-C", dst, "clean", "--quiet", "-fdx"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keys", help="your console's prod.keys, outside this folder (default: .switch\\prod.keys in "
                    "your user folder)")
    ap.add_argument("--icon", help="the game's iconTex.tga (default: from the extracted game in build\\)")
    ap.add_argument("--sd", help="also copy the NSP into DIR\\NSP\\ (the SD card folder make_sd.py made)")
    ap.add_argument("--version", help="the version shown on the HOME screen (default: from the folder name)")
    args = ap.parse_args()

    keys = os.path.abspath(args.keys or os.path.join(os.path.expanduser("~"), ".switch", "prod.keys"))
    if not os.path.isfile(keys):
        print("No prod.keys found at %s: the HOME-screen forwarder (.nsp) is skipped.\n"
              "The game itself does not need it. To build the forwarder, put your console's prod.keys there,\n"
              "or pass --keys PATH (outside this folder)." % keys, flush=True)
        sys.exit(EXIT_NO_KEYS)
    if os.path.normcase(os.path.realpath(keys)).startswith(os.path.normcase(os.path.realpath(ROOT)) + os.sep):
        fail("keep prod.keys outside this folder (%s): it is easy to zip or share it with the build by mistake" % ROOT)
    secrets = key_values(keys)

    fdir = FDIR
    for f in ("make_nacp.py", "make_icon.py", "nx-hbloader-forwarder.patch"):
        if not os.path.isfile(os.path.join(fdir, f)):
            fail("missing %s next to this script" % os.path.join(fdir, f))
    missing = [n for n, p in {
        "devkitA64": DKP + "/devkitA64/bin/aarch64-none-elf-gcc.exe", "libnx": DKP + "/libnx/lib/libnx.a",
        "npdmtool (switch-tools)": DKP + "/tools/bin/npdmtool.exe", "devkitPro MSYS2": DKP + "/msys2/usr/bin/bash.exe",
        "gcc in MSYS2 (pacman -S gcc)": DKP + "/msys2/usr/bin/gcc.exe"}.items() if not os.path.exists(p)]
    if missing:
        fail("devkitPro is not set up at %s (missing: %s); see INSTALL.md" % (DKP, ", ".join(missing)))
    if not shutil.which("git"):
        fail("git is needed (https://git-scm.com/download/win): it fetches nx-hbloader and hacBrewPack")
    try:
        import PIL  # noqa: F401
    except ImportError:
        fail("the icon needs Pillow: python -m pip install pillow")

    icon_src = args.icon
    if not icon_src:
        for c in (os.path.join(ROOT, "build", "sd-game", "meta", "iconTex.tga"),
                  os.path.join(ROOT, "build", "sd", "switch", "wwhd", "game", "meta", "iconTex.tga")):
            if os.path.isfile(c):
                icon_src = c
                break
    if not icon_src or not os.path.isfile(icon_src):
        fail("no icon: give --icon path\\to\\meta\\iconTex.tga (from your dump)")
    # the display version (NACP: at most 15 bytes): as build_forwarder.sh, git describe without the "v" and
    # without the hash when that is too long; a release zip has no git, so then the folder name's version
    version = args.version
    if not version:
        p = subprocess.run(["git", "-C", ROOT, "describe", "--tags", "--always", "--dirty"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        version = p.stdout.decode().strip().lstrip("v") if p.returncode == 0 else ""
        if not version:
            m = re.search(r"(\d+(?:\.\d+)+)", os.path.basename(ROOT))
            version = m.group(1) if m else "1.0"
        if len(version) > 15:
            version = re.sub(r"-g[0-9a-f]+.*$", "", version)
    version = version[:15]

    out = os.path.join(ROOT, "build", "forwarder")
    src = os.path.join(out, "src")
    hbl, hbp = os.path.join(src, "nx-hbloader"), os.path.join(src, "hacBrewPack")
    print("[1/4] fetching nx-hbloader %s and hacBrewPack %s" % (HBLOADER[1][:7], HACBREWPACK[1][:7]), flush=True)
    fetch(hbl, *HBLOADER)
    fetch(hbp, *HACBREWPACK)
    run(["git", "-C", hbl, "apply", "--ignore-whitespace", os.path.join(fdir, "nx-hbloader-forwarder.patch")])

    print("\n[2/4] building the loader and the packer", flush=True)
    with open(os.path.join(hbl, "hbl.json")) as f:
        conf = json.load(f)
    tid = "0x" + TITLE_ID
    conf.update(name="wwhd_fwd", title_id=tid, title_id_range_min=tid, title_id_range_max=tid)
    for cap in conf["kernel_capabilities"]:
        if cap["type"] == "application_type":
            cap["value"] = 1  # application (hbl.json: 2, applet): the application's memory
    with open(os.path.join(hbl, "hbl.json"), "w") as f:
        json.dump(conf, f, indent=4)
    rc, text = msys('''export DEVKITA64=$DEVKITPRO/devkitA64
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"
cd "$(cygpath -u "$HBL")" && make -s RELEASE=1 >/dev/null
cd "$(cygpath -u "$HBP")" && cp config.mk.template config.mk && (make -s >/dev/null 2>&1 || make)
''', {"HBL": hbl, "HBP": hbp}, capture=True)
    if rc != 0:
        print(scrub(text, secrets))
        fail("building nx-hbloader or hacBrewPack failed")
    hbp_exe = next((os.path.join(hbp, n) for n in ("hacbrewpack.exe", "hacbrewpack") if os.path.isfile(os.path.join(hbp, n))), None)
    if not hbp_exe or not os.path.isfile(os.path.join(hbl, "hbl.nso")) or not os.path.isfile(os.path.join(hbl, "hbl.npdm")):
        fail("the build made no hbl.nso/hbl.npdm/hacbrewpack")

    print("\n[3/4] laying out the NSP", flush=True)
    pack = os.path.join(out, "pack")
    shutil.rmtree(pack, ignore_errors=True)
    for d in ("exefs", "romfs", "control"):
        os.makedirs(os.path.join(pack, d))
    shutil.copyfile(os.path.join(hbl, "hbl.nso"), os.path.join(pack, "exefs", "main"))
    shutil.copyfile(os.path.join(hbl, "hbl.npdm"), os.path.join(pack, "exefs", "main.npdm"))
    for f in ("nextNroPath", "nextArgv"):
        with open(os.path.join(pack, "romfs", f), "w", newline="") as fh:
            fh.write(NRO_PATH)
    run([sys.executable, "-I", os.path.join(fdir, "make_nacp.py"), os.path.join(pack, "control", "control.nacp"),
         "--title-id", TITLE_ID, "--name", NAME, "--publisher", PUBLISHER, "--version", version])
    icon = os.path.join(out, "icon.jpg")
    run([sys.executable, "-I", os.path.join(fdir, "make_icon.py"), icon_src, icon])
    for lang in LANGS:
        shutil.copyfile(icon, os.path.join(pack, "control", "icon_%s.dat" % lang))

    print("\n[4/4] packing (your prod.keys is read by path only)", flush=True)
    rc, text = msys('''cd "$(cygpath -u "$PACK")"
"$(cygpath -u "$HBPEXE")" -k "$(cygpath -u "$KEYS")" --titleid %s --nologo --exefsdir exefs --romfsdir romfs \\
    --controldir control --nspdir nsp --ncadir nca --tempdir temp --backupdir backup 2>&1
''' % TITLE_ID, {"PACK": pack, "HBPEXE": hbp_exe, "KEYS": keys}, capture=True)
    log = scrub(text, secrets)
    nsp = os.path.join(pack, "nsp", TITLE_ID + ".nsp")
    if rc != 0 or not os.path.isfile(nsp) or os.path.getsize(nsp) == 0:
        print(redacted(text))
        fail("hacBrewPack made no NSP (are the keys from a console with firmware new enough for this game?)")
    with open(nsp, "rb") as f:
        if f.read(4) != b"PFS0":
            fail("the NSP is not a PFS0 container")
    final = os.path.join(out, "wwhd_forwarder.nsp")
    shutil.copyfile(nsp, final)
    if args.sd:
        d = os.path.join(os.path.abspath(args.sd), "NSP")
        os.makedirs(d, exist_ok=True)
        shutil.copyfile(final, os.path.join(d, "wwhd_forwarder.nsp"))
    print("""
Done: %s (%d bytes), title ID 0x%s, "%s" %s
%sInstall it with DBI (Browse SD card > the .nsp > Install) or Goldleaf; it needs current Atmosphere sigpatches.
The NRO must stay at %s. (The NSP has not been checked with hactool.)"""
          % (final, os.path.getsize(final), TITLE_ID, NAME, version,
             "Also copied to %s\\NSP\\.\n" % os.path.abspath(args.sd) if args.sd else "", NRO_PATH))


if __name__ == "__main__":
    main()
