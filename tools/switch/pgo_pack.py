#!/usr/bin/env python3
"""SwitchWakerHD: a PGO profile as a release asset (docs/pgo-release-plan.md, issue #90).

  pgo_pack.py pack --region USA|EU [--gcda build/pgo] [--build-dir build/switch-pgo-gen] [--out build]
        writes OUT/pgo-profile-<region>.zip and its .sha256 (upload both to the release): the .gcda files that
        pgo_train.py fetched, and manifest.json (the region, the commit, the compiler, the scope and the generated
        hot code's fingerprint, from the training build's pgo-build.json)
  pgo_pack.py unpack ZIP --out DIR [--region USA|EU]
        checks the zip (only flat .gcda files and the manifest, each file's SHA-256, the region) and extracts it
        for a WWHD_PGO=USE build (WWHD_PGO_DIR=DIR): cmake/SwitchPgo.cmake then checks the compiler

The .gcda files hold counters: function ids, checksums and how often each branch ran. No game code.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FORMAT = 1
REGIONS = ("USA", "EU")
NAME = re.compile(r"^[A-Za-z0-9#._+-]+\.gcda$")  # the training's flat names (the object path with '#' for '/')
MAX_FILE = 16 << 20
MAX_FILES = 4096


class Fail(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def zip_name(region):
    return "pgo-profile-%s.zip" % region


def git_commit():
    try:
        out = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout
        dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"],
                               capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"
    return out.strip() + ("-dirty" if dirty else "")


def pack(gcda_dir, build_dir, region, out_dir):
    if region not in REGIONS:
        raise Fail("region must be one of %s" % ", ".join(REGIONS))
    info_path = os.path.join(build_dir, "pgo-build.json")
    try:
        with open(info_path) as f:
            info = json.load(f)
    except (OSError, ValueError) as e:
        raise Fail("%s: %s (the WWHD_PGO=GEN build writes it)" % (info_path, e))
    names = sorted(n for n in os.listdir(gcda_dir) if n.endswith(".gcda")) if os.path.isdir(gcda_dir) else []
    if not names:
        raise Fail("no .gcda files in %s (pgo_train.py fetch)" % gcda_dir)
    bad = [n for n in names if not NAME.match(n)]
    if bad:
        raise Fail("unexpected file names: %s" % ", ".join(bad[:5]))
    files = {}
    blobs = {}
    for n in names:
        with open(os.path.join(gcda_dir, n), "rb") as f:
            blobs[n] = f.read()
        files[n] = sha256(blobs[n])
    commit = git_commit()
    if commit.endswith("-dirty") or commit == "unknown":
        print("pgo_pack: warning: the checkout is %s: the profile should come from the release's commit" % commit)
    manifest = {
        "format": FORMAT,
        "region": region,
        "commit": commit,
        "compiler": info["compiler"],
        "scope": info["scope"],
        "hot_sha256": info["hot_sha256"],
        "files": files,
    }
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, zip_name(region))
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        # fixed dates: the same profile gives the same zip
        def add(name, data):
            zi = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, data)
        add("manifest.json", json.dumps(manifest, indent=1, sort_keys=True) + "\n")
        for n in names:
            add(n, blobs[n])
    with open(path, "rb") as f:
        digest = sha256(f.read())
    with open(path + ".sha256", "w") as f:
        f.write("%s  %s\n" % (digest, zip_name(region)))
    print("pgo_pack: %s (%d files, %s, %s, commit %s)" % (path, len(names), region, info["compiler"], commit[:12]))
    return path


def unpack(zip_path, out_dir, region=None):
    """extracts a release profile into out_dir (emptied first); returns its manifest. Fail with the reason."""
    try:
        z = zipfile.ZipFile(zip_path)
    except (OSError, zipfile.BadZipFile) as e:
        raise Fail("not a profile zip: %s" % e)
    with z:
        infos = z.infolist()
        if len(infos) > MAX_FILES + 1:
            raise Fail("too many files")
        names = [i.filename for i in infos]
        if "manifest.json" not in names:
            raise Fail("no manifest.json")
        for i in infos:
            if i.filename != "manifest.json" and not NAME.match(i.filename):
                raise Fail("unexpected file in the zip: %r" % i.filename)
            if i.file_size > MAX_FILE:
                raise Fail("%s is too large" % i.filename)
        try:
            manifest = json.loads(z.read("manifest.json"))
        except ValueError as e:
            raise Fail("manifest.json: %s" % e)
        if manifest.get("format") != FORMAT:
            raise Fail("profile format %r (this source reads %d)" % (manifest.get("format"), FORMAT))
        if region and manifest.get("region") != region:
            raise Fail("the profile is for the %s game, this is the %s one" % (manifest.get("region"), region))
        files = manifest.get("files") or {}
        gcda = sorted(n for n in names if n != "manifest.json")
        if sorted(files) != gcda:
            raise Fail("the manifest's file list does not match the zip")
        blobs = {}
        for n in gcda:
            data = z.read(n)
            if sha256(data) != files[n]:
                raise Fail("%s does not match its SHA-256" % n)
            blobs[n] = data
    os.makedirs(out_dir, exist_ok=True)
    for n in os.listdir(out_dir):
        if n.endswith(".gcda") or n == "manifest.json":
            os.remove(os.path.join(out_dir, n))
    for n, data in blobs.items():
        with open(os.path.join(out_dir, n), "wb") as f:
            f.write(data)
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    return manifest


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("--region", required=True, choices=REGIONS)
    p.add_argument("--gcda", default=os.path.join(ROOT, "build", "pgo"))
    p.add_argument("--build-dir", default=os.path.join(ROOT, "build", "switch-pgo-gen"))
    p.add_argument("--out", default=os.path.join(ROOT, "build"))
    u = sub.add_parser("unpack")
    u.add_argument("zip")
    u.add_argument("--out", required=True)
    u.add_argument("--region", choices=REGIONS)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "pack":
            pack(a.gcda, a.build_dir, a.region, a.out)
        else:
            m = unpack(a.zip, a.out, a.region)
            print("pgo_pack: %s profile (%d files, %s, commit %s) in %s"
                  % (m["region"], len(m["files"]), m["compiler"], m["commit"][:12], a.out))
    except Fail as e:
        print("pgo_pack: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
