#!/usr/bin/env python3
"""Pack an already-packaged Linux release folder as one AppImage (issue #55).

usage: appimage.py PKG_DIR --out OUT_DIR [--appimagetool PATH] [--runtime-file PATH]
       appimage.py --extract-runtime APPIMAGE OUT_FILE

PKG_DIR is the WindWakerHD-VERSION-linux-ARCH folder tools/release/package.py produced (with
wind-waker-hd, tools/, sdk/, portable.txt, ...). An AppImage mount is read-only, so the result
drops portable.txt: setup then keeps the game, its code, saves and settings in the per-user
folders (~/.local/share/wwhd and ~/.config/wwhd, setup.py default_data_dir) instead of next to
the program, and nothing is written beside the .AppImage. Portable use is the AppImage runtime's
own: a "<AppImage>.home" folder next to the file becomes $HOME (see APPRUN).

Adds what appimagetool wants at the AppDir root (AppRun, a .desktop, the project's own icon) and
squashes it with the AppImage project's appimagetool. Output: OUT_DIR/<basename of PKG_DIR>.AppImage.
"""
import argparse
import hashlib
import json
import os
import platform
import struct
import shutil
import subprocess
import sys
import tempfile
import urllib.request

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
# the project's own icon (not game data: guard.py sees it like any other PNG)
ICON = os.path.join(ROOT, "android", "app", "src", "main", "res", "mipmap-xxxhdpi", "ic_launcher.png")

# appimagetool (the AppImage project's packager), pinned. "continuous" moves; these are release
# assets. Change a pin only together with a new release.
APPIMAGETOOL_VERSION = "1.9.1"
APPIMAGETOOL = {
    "x86_64": ("appimagetool-x86_64.AppImage",
               "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"),
    "aarch64": ("appimagetool-aarch64.AppImage",
                "f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158"),
}
APPIMAGETOOL_URL = "https://github.com/AppImage/appimagetool/releases/download/%s/" % APPIMAGETOOL_VERSION

# The AppImage runtime (the ELF in front of the squashfs: mounts the image, sets $APPIMAGE, the
# portable .home/.config folders, --appimage-extract-and-run), pinned to exactly the one v0.2.13
# shipped: type2-runtime commit 8f39b89 (its "continuous" build of 2026-09-28; MIT, links libfuse3,
# squashfuse, zstd, zlib, mimalloc statically). Without --runtime-file appimagetool would fetch
# whatever "continuous" is today. The SHA-256 decides, not the URL: any download that does not match
# fails the build.
#
# Updating the pin, deliberately and together with a release:
#   1. take the new runtime-<arch> files (a type2-runtime build: release asset or your own build);
#   2. check them (`strings runtime-x86_64 | grep commit/` names the commit; upstream signs them, the
#      .sig files next to the assets, key: signing-pubkey.asc in the type2-runtime repository);
#   3. put their SHA-256 below, RUNTIME_COMMIT to the commit, and host them where RUNTIME_URLS looks;
#   4. update the license texts in RUNTIME_LICENSES (the components and versions the new build links,
#      from its scripts/ and the binary's version strings) and their AppImage-runtime-README.txt;
#   5. build, then check `<AppImage> --appimage-version` on the result and run the release workflow's
#      AppImage step (portable home, extract-and-run).
# To get back the exact runtime of a released AppImage: appimage.py --extract-runtime FILE OUT
# (the ELF before the squashfs, with the per-image MD5 digest that appimagetool writes zeroed).
RUNTIME_COMMIT = "8f39b89"
RUNTIME = {
    "x86_64": ("runtime-x86_64", "156f4bdbde9c52d01814600013e0a273f0118dc2de98975f3c8c63427ec79074"),
    "aarch64": ("runtime-aarch64", "b4ff0030242d0c3bb12ce40541828303cf167493f4793456f0436edd6255c39d"),
}
# tried in order; "continuous" serves the pinned bytes only until upstream publishes a newer build
RUNTIME_URLS = [
    # our own copy (release asset "appimage-runtime-8f39b89": the runtimes plus type2-runtime's LICENSE);
    # upstream's "continuous" release is replaced over time and would stop matching the pin
    "https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp/releases/download/appimage-runtime-8f39b89/",
]

# The runtime's own license and those of what it links statically (libfuse, squashfuse, zstd, zlib,
# mimalloc, musl; AppImage-runtime-README.txt lists versions and the corresponding source). They go
# into the AppImage's third-party-licenses/ next to the package's own; guard.py checks they are there.
# The zip releases contain no AppImage runtime and do not get them.
RUNTIME_LICENSES = os.path.join(ROOT, "tools", "release", "appimage-runtime-licenses")


def runtime_license_files():
    return sorted(f for f in os.listdir(RUNTIME_LICENSES) if f.startswith("AppImage-runtime-") and f.endswith(".txt"))


def add_runtime_licenses(appdir):
    licdir = os.path.join(appdir, "third-party-licenses")
    os.makedirs(licdir, exist_ok=True)
    names = runtime_license_files()
    if "AppImage-runtime-README.txt" not in names or len(names) < 2:
        sys.exit("the AppImage runtime's license texts are missing in " + RUNTIME_LICENSES)
    for name in names:
        shutil.copyfile(os.path.join(RUNTIME_LICENSES, name), os.path.join(licdir, name))
    with open(os.path.join(licdir, "README.txt"), "a") as f:
        f.write("\nThe AppImage runtime (the first part of the .AppImage file, type2-runtime %s) and what it\n"
                "links statically: AppImage-runtime-README.txt (components, versions, source), and\n" % RUNTIME_COMMIT)
        for name in names:
            if name != "AppImage-runtime-README.txt":
                f.write("  %s\n" % name)


APPRUN = """#!/bin/sh
# Wind Waker HD (AppImage): start the release's launcher (next to tools/ and sdk/).
# This image is read-only: the setup keeps the game, its code, saves and settings in the
# per-user folders (issue #55), never beside this file. Portable use is the AppImage runtime's
# standard one: a folder "<this file>.home" next to the AppImage (--appimage-portable-home
# creates it) becomes $HOME before this script runs, and setup and game derive every folder
# from $HOME and the XDG variables. Nothing to do here, except a hint: v0.2.13 had a portable
# folder "<this file>.data" of its own, which is no longer read (no paths come from it).
if [ -n "$APPIMAGE" ] && [ -d "$APPIMAGE.data" ]; then
  n=$(basename "$APPIMAGE")
  echo "Found $n.data: this folder is no longer used. Move its contents to $n.home (see the changelog) to keep your saves and settings." >&2
fi
exec "$(dirname "$0")/wind-waker-hd" "$@"
"""

DESKTOP = """[Desktop Entry]
Type=Application
Name=Wind Waker HD
Comment=The Wind Waker HD (native PC port); the first start prepares the game from your own disc dump
Exec=wind-waker-hd
Icon=wwhd
Categories=Game;
Terminal=false
Actions=setup;

[Desktop Action setup]
Name=Setup (repair, update, change the game)
Exec=wind-waker-hd --setup
"""


def host_arch():
    m = platform.machine().lower()
    return {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "aarch64", "aarch64": "aarch64"}.get(m)


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ensure_appimagetool(given, arch):
    """The pinned appimagetool for `arch`, or the one given/installed. Returns its path."""
    if given:
        if not os.path.isfile(given):
            sys.exit("appimagetool not found: " + given)
        return given
    which = shutil.which("appimagetool")
    if which:
        return which
    if arch not in APPIMAGETOOL:
        sys.exit("no pinned appimagetool for %s (got --appimagetool to a build of it)" % arch)
    name, want = APPIMAGETOOL[arch]
    url = APPIMAGETOOL_URL + name
    # never in the output folder: that must hold only the artifacts
    dst = os.path.join(tempfile.gettempdir(), "wwhd-appimagetool-%s-%s" % (arch, APPIMAGETOOL_VERSION))
    if not (os.path.isfile(dst) and file_sha256(dst) == want):
        print("downloading appimagetool %s (%s)" % (APPIMAGETOOL_VERSION, arch))
        tmp = dst + ".part"
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "wwhd-package"})
            with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
                shutil.copyfileobj(r, f)
        except OSError as e:
            sys.exit("download of appimagetool failed: %s\nFetch %s yourself and pass --appimagetool" % (e, url))
        if file_sha256(tmp) != want:
            os.remove(tmp)
            sys.exit("the download of %s is corrupt or was changed (SHA-256 mismatch)" % url)
        os.replace(tmp, dst)
        os.chmod(dst, 0o755)
    return dst


def ensure_runtime(given, arch, urls=None):
    """The pinned AppImage runtime for `arch` (RUNTIME): the file given, else a download. A file
    whose SHA-256 is not the pinned one is refused (the build fails). Returns its path."""
    if arch not in RUNTIME:
        sys.exit("no pinned AppImage runtime for %s" % arch)
    name, want = RUNTIME[arch]
    if given:
        if not os.path.isfile(given):
            sys.exit("AppImage runtime not found: " + given)
        got = file_sha256(given)
        if got != want:
            sys.exit("AppImage runtime %s has SHA-256 %s, the pinned %s (type2-runtime %s) has %s; "
                     "see RUNTIME in tools/release/appimage.py to update the pin deliberately"
                     % (given, got, name, RUNTIME_COMMIT, want))
        return given
    dst = os.path.join(tempfile.gettempdir(), "wwhd-appimage-%s-%s" % (name, RUNTIME_COMMIT))
    if os.path.isfile(dst) and file_sha256(dst) == want:
        return dst
    tmp = dst + ".part"
    tried = []
    for base in (RUNTIME_URLS if urls is None else urls):
        url = base + name
        print("downloading the AppImage runtime (%s, type2-runtime %s) from %s" % (arch, RUNTIME_COMMIT, url))
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "wwhd-package"})
            with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
                shutil.copyfileobj(r, f)
        except OSError as e:
            tried.append("%s: %s" % (url, e))
            continue
        got = file_sha256(tmp)
        if got == want:
            os.replace(tmp, dst)
            return dst
        os.remove(tmp)
        tried.append("%s: SHA-256 %s, not the pinned %s" % (url, got, want))
    sys.exit("no source has the pinned AppImage runtime %s (type2-runtime %s):\n  %s\n"
             "Pass --runtime-file, or see RUNTIME in tools/release/appimage.py"
             % (name, RUNTIME_COMMIT, "\n  ".join(tried)))


def extract_runtime(appimage, out):
    """The runtime part of a built AppImage (the ELF before the squashfs) as appimagetool got it:
    the .digest_md5 section, which appimagetool fills for each image, zeroed again."""
    with open(appimage, "rb") as f:
        head = f.read(1 << 24)
    if head[:4] != b"\x7fELF" or head[4] != 2:
        sys.exit("%s does not start with a 64-bit ELF runtime" % appimage)
    little = head[5] == 1
    e = "<" if little else ">"
    shoff, = struct.unpack_from(e + "Q", head, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from(e + "HHH", head, 0x3A)
    end = shoff + shentsize * shnum  # the runtime's own rule for where the squashfs starts
    if head[end:end + 4] not in (b"hsqs", b"sqsh"):
        sys.exit("%s: no squashfs after the runtime (offset %d)" % (appimage, end))
    runtime = bytearray(head[:end])
    headers = [struct.unpack_from(e + "IIQQQQIIQQ", head, shoff + i * shentsize) for i in range(shnum)]
    names = headers[shstrndx][4]
    for h in headers:
        start = names + h[0]
        if head[start:head.index(b"\0", start)] == b".digest_md5":
            runtime[h[4]:h[4] + h[5]] = bytes(h[5])
    with open(out, "wb") as f:
        f.write(runtime)
    print("wrote %s (%d bytes, SHA-256 %s)" % (out, len(runtime), hashlib.sha256(runtime).hexdigest()))


def package_version(pkg):
    try:
        with open(os.path.join(pkg, "sdk", "manifest.json")) as f:
            return json.load(f).get("version") or ""
    except (OSError, ValueError):
        return ""


def make_appdir(pkg, appdir):
    """PKG_DIR as an AppDir: no portable.txt (read-only mount), plus AppRun, .desktop and the icon."""
    # data/ is an installation, portable.txt would send every write into the read-only mount
    # (setup.py PORTABLE, host::portable)
    # the zip's own "Wind Waker HD.desktop" (an `sh -c 'cd ...'` launcher for the unpacked folder) is
    # not a valid entry inside an AppImage, and appimagetool validates every .desktop at the root;
    # the AppImage gets its own wwhd.desktop below
    shutil.copytree(pkg, appdir, symlinks=True,
                    ignore=shutil.ignore_patterns("data", "portable.txt", ".appimagetool-*", "*.desktop"))
    with open(os.path.join(appdir, "AppRun"), "w") as f:
        f.write(APPRUN)
    os.chmod(os.path.join(appdir, "AppRun"), 0o755)
    add_runtime_licenses(appdir)
    with open(os.path.join(appdir, "wwhd.desktop"), "w") as f:
        f.write(DESKTOP)
    if not os.path.isfile(ICON):
        sys.exit("project icon not found: " + ICON)
    shutil.copyfile(ICON, os.path.join(appdir, "wwhd.png"))
    shutil.copyfile(ICON, os.path.join(appdir, ".DirIcon"))


def main():
    if sys.argv[1:2] == ["--extract-runtime"]:
        if len(sys.argv) != 4:
            sys.exit("usage: appimage.py --extract-runtime APPIMAGE OUT_FILE")
        return extract_runtime(sys.argv[2], sys.argv[3])
    ap = argparse.ArgumentParser()
    ap.add_argument("pkg", help="the WindWakerHD-VERSION-linux-ARCH folder package.py produced")
    ap.add_argument("--out", required=True, help="where to write the .AppImage")
    ap.add_argument("--appimagetool", help="appimagetool to use (default: one on PATH, else the pinned download)")
    ap.add_argument("--runtime-file", help="the pinned AppImage runtime (RUNTIME; default: download it); "
                    "a file with another SHA-256 is refused")
    a = ap.parse_args()

    pkg = os.path.normpath(os.path.abspath(a.pkg))
    if not os.path.isfile(os.path.join(pkg, "tools", "installer", "setup.py")):
        sys.exit("%s is not a packaged release (no tools/installer/setup.py)" % pkg)
    if not os.path.isfile(os.path.join(pkg, "wind-waker-hd")):
        sys.exit("%s has no wind-waker-hd (package it with --setup-gui)" % pkg)
    if not os.path.isfile(os.path.join(pkg, "portable.txt")):
        sys.exit("%s has no portable.txt: package.py always writes one (the zip is the portable "
                 "release; the AppImage drops it)" % pkg)
    arch = host_arch()
    if not arch:
        sys.exit("unsupported machine " + platform.machine())
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    name = os.path.basename(pkg)
    target = os.path.join(out, name + ".AppImage")
    tool = ensure_appimagetool(a.appimagetool, arch)
    runtime = ensure_runtime(a.runtime_file, arch)

    work = tempfile.mkdtemp(prefix="wwhd-appimage-")
    try:
        appdir = os.path.join(work, name + ".AppDir")
        make_appdir(pkg, appdir)
        env = dict(os.environ)
        env.update({"ARCH": arch, "APPIMAGE_EXTRACT_AND_RUN": "1"})  # no FUSE needed to run appimagetool
        version = package_version(pkg)
        if version:
            env["VERSION"] = version
        subprocess.run([tool, "--runtime-file", runtime, appdir, target], env=env, check=True)
        if not os.path.isfile(target):
            sys.exit("appimagetool did not write " + target)
        os.chmod(target, 0o755)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("wrote", target, os.path.getsize(target) // (1 << 20), "MiB")


if __name__ == "__main__":
    main()
