#!/usr/bin/env python3
"""The pinned devkitPro toolchain for the players' build (docs/pinned-toolchain-plan.md, step 3): devkitPro's own
package files at the versions of tools/toolchain/lock.json, downloaded from devkitPro's server, checked against
their SHA-256 and unpacked into DEST/<id>/devkitpro (with zig, the host compiler for the prepared graphics'
dksh_cache, in DEST/<id>/zig), without pacman and without any devkitPro already installed.
<id> names the lock's files for this platform; the NRO's toolchain line (cmake/SwitchToolchainInfo.cmake) shows it.

usage: python3 -I tools/toolchain/fetch.py [--dest DIR] [--platform P] [--cache DIR]
         prints the DEVKITPRO folder (DIR/<id>/devkitpro; default DIR: build/toolchain)
       python3 -I tools/toolchain/fetch.py --check [--platform P|all]
         downloads and checks every locked file, unpacks nothing (the weekly check: is everything still there?)
Python 3.14 or newer (zstd), standard library only.
"""
import argparse
import hashlib
import json
import os
import platform as host
import shutil
import stat
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dkp  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PREFIX = "opt/devkitpro/"


def this_platform():
    os_name = {"win32": "win", "darwin": "osx"}.get(sys.platform, "linux")
    arch = host.machine().lower()
    arch = {"amd64": "x64", "x86_64": "x64", "arm64": "arm64", "aarch64": "arm64"}.get(arch, arch)
    return os_name + "-" + arch


def files_for(lock, plat):
    if plat not in lock["hosts"]:
        sys.exit("no pinned devkitPro toolchain for %s (there is one for: %s)" % (plat, ", ".join(lock["hosts"])))
    return lock["any"] + lock["hosts"][plat]


def lock_id(files, zig):
    """12 hex digits naming exactly these files (and the zig archive)"""
    text = "\n".join("%s %s" % (f["path"], f["sha256"]) for f in sorted(files, key=lambda f: f["path"]))
    text += "\nzig %s %s" % (zig["url"], zig["sha256"])
    return hashlib.sha256(text.encode()).hexdigest()[:12]


def get(f, server, cache):
    """the checked bytes of a locked file (from the cache when there and right); f: a package (path on devkitPro's
    server) or the zig archive (url)"""
    url = f.get("url") or server + f["path"]
    name = f.get("path") or "zig/" + url.rsplit("/", 1)[1]
    cached = os.path.join(cache, name) if cache else None
    if cached and os.path.isfile(cached):
        with open(cached, "rb") as fh:
            data = fh.read()
        if dkp.sha256(data) == f["sha256"]:
            return data
    data = dkp.download(url)
    got = dkp.sha256(data)
    if got != f["sha256"]:
        sys.exit("%s: SHA-256 %s, the lock says %s (the file changed on its server? nothing was unpacked)"
                 % (name, got, f["sha256"]))
    if cached:
        os.makedirs(os.path.dirname(cached), exist_ok=True)
        with open(cached + ".part", "wb") as fh:
            fh.write(data)
        os.replace(cached + ".part", cached)
    return data


def unpack(data, dest):
    """the package's opt/devkitpro/... into dest. Links become copies on Windows (symbolic links need rights there);
    hard links are copies everywhere (their target is named from the archive's root)."""
    windows = sys.platform == "win32"
    links = []
    with dkp.open_package(data) as t:
        for m in t.getmembers():
            if not m.name.startswith(PREFIX):
                continue  # .PKGINFO, .MTREE, .BUILDINFO
            rel = m.name[len(PREFIX):]
            if not rel or ".." in rel.split("/"):
                continue
            out = os.path.join(dest, *rel.split("/"))
            if m.isdir():
                os.makedirs(out, exist_ok=True)
            elif m.isfile():
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with t.extractfile(m) as src, open(out, "wb") as fh:
                    shutil.copyfileobj(src, fh)
                if not windows and m.mode & 0o111:
                    os.chmod(out, os.stat(out).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
            elif m.islnk() or m.issym():
                links.append((m, out))
    for m, out in links:  # after the files they point at
        os.makedirs(os.path.dirname(out), exist_ok=True)
        if m.islnk():
            target = os.path.join(dest, *m.linkname[len(PREFIX):].split("/"))
        else:
            target = os.path.normpath(os.path.join(os.path.dirname(out), m.linkname))
        if os.path.lexists(out):
            os.remove(out)
        if m.issym() and not windows:
            os.symlink(m.linkname, out)
        elif os.path.isfile(target):
            shutil.copy2(target, out)


def unpack_zig(data, dest):
    """zig's archive (.tar.xz, or .zip on Windows) into dest, without its top folder"""
    import io
    import tarfile
    import zipfile
    os.makedirs(dest, exist_ok=True)
    if data[:2] == b"PK":
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for m in z.infolist():
                rel = m.filename.split("/", 1)[1] if "/" in m.filename else ""
                if not rel or m.is_dir() or ".." in rel.split("/"):
                    continue
                out = os.path.join(dest, *rel.split("/"))
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with z.open(m) as src, open(out, "wb") as fh:
                    shutil.copyfileobj(src, fh)
        return
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:xz") as t:
        for m in t.getmembers():
            rel = m.name.split("/", 1)[1] if "/" in m.name else ""
            if not rel or ".." in rel.split("/"):
                continue
            out = os.path.join(dest, *rel.split("/"))
            if m.isdir():
                os.makedirs(out, exist_ok=True)
            elif m.isfile():
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with t.extractfile(m) as src, open(out, "wb") as fh:
                    shutil.copyfileobj(src, fh)
                if m.mode & 0o111:
                    os.chmod(out, os.stat(out).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def fetch(lock, plat, dest_root, cache):
    files = files_for(lock, plat)
    zig = lock["zig"][plat]
    ident = lock_id(files, zig)
    home = os.path.join(dest_root, ident)
    dkp_dir = os.path.join(home, "devkitpro")
    manifest = os.path.join(dkp_dir, "toolchain.json")
    if os.path.isfile(manifest):
        return dkp_dir  # complete: the manifest is written last
    tmp = home + ".partial"
    shutil.rmtree(tmp, ignore_errors=True)
    for f in files:
        print("devkitPro: %s" % f["path"], file=sys.stderr, flush=True)
        unpack(get(f, lock["server"], cache), os.path.join(tmp, "devkitpro"))
    print("zig: %s" % zig["url"], file=sys.stderr, flush=True)
    unpack_zig(get(zig, lock["server"], cache), os.path.join(tmp, "zig"))
    with open(os.path.join(tmp, "devkitpro", "toolchain.json"), "w", newline="\n") as fh:
        json.dump({"id": ident, "platform": plat, "packages": files, "zig": zig}, fh, indent=1)
    shutil.rmtree(home, ignore_errors=True)
    os.replace(tmp, home)
    # the previous toolchain stays (a rebuild of an older release); older ones go
    others = sorted((d for d in os.listdir(dest_root) if d != ident and not d.endswith(".partial")
                     and os.path.isfile(os.path.join(dest_root, d, "devkitpro", "toolchain.json"))),
                    key=lambda d: os.path.getmtime(os.path.join(dest_root, d)))
    for d in others[:-1]:
        shutil.rmtree(os.path.join(dest_root, d), ignore_errors=True)
    return dkp_dir


def check(lock, plats):
    seen, bad = set(), 0
    for plat in plats:
        for f in files_for(lock, plat) + [lock["zig"][plat]]:
            key = f.get("path") or f["url"]
            if key in seen:
                continue
            seen.add(key)
            try:
                data = dkp.download(f.get("url") or lock["server"] + f["path"])
                ok = dkp.sha256(data) == f["sha256"]
            except Exception as e:  # noqa: BLE001  (a report, not a crash)
                ok, data = False, str(e)
            print("%s %s" % ("ok  " if ok else "FAIL", key) + ("" if ok else "  (%s)" % (
                data if isinstance(data, str) else "different SHA-256")), flush=True)
            bad += not ok
    print("%d files, %d missing or changed" % (len(seen), bad))
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dest", default=os.path.join(ROOT, "build", "toolchain"))
    ap.add_argument("--platform", default=this_platform())
    ap.add_argument("--cache", default=None, help="keep the downloaded package files here")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args(argv)
    with open(os.path.join(HERE, "lock.json")) as fh:
        lock = json.load(fh)
    if a.check:
        return check(lock, list(lock["hosts"]) if a.platform == "all" else [a.platform])
    os.makedirs(a.dest, exist_ok=True)
    print(fetch(lock, a.platform, os.path.abspath(a.dest), a.cache))
    return 0


if __name__ == "__main__":
    sys.exit(main())
