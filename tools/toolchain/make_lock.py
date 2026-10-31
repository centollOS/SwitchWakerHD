#!/usr/bin/env python3
"""Writes tools/toolchain/lock.json: devkitPro's package files for the players' build, per platform, with their
path on devkitPro's server and SHA-256 (docs/pinned-toolchain-plan.md, step 3). A new pin is a reviewed change of
PACKAGES below: build with it and check it on the console before a release.

usage: python3 -I tools/toolchain/make_lock.py      (Python 3.14; downloads every file once, ~600 MB, into build/toolchain-cache)
"""
import json
import os
import sys
import urllib.error

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dkp  # noqa: E402

# the versions of our Docker image (devkitA64 r29.2): what we build and test with
HOST_PACKAGES = {  # one file per platform
    "devkita64-gcc": "15.2.0-7",
    "devkita64-binutils": "2.45.1-2",
    "switch-tools": "1.13.1-1",
    "general-tools": "1.4.4-1",
    "uam": "1.1.0-1",
}
ANY_PACKAGES = {  # one file for every platform
    "devkita64-newlib": "4.6.0.20260123-4",
    "devkita64-rules": "1.1.1-1",
    "devkita64-cmake": "1.1.3-1",
    "dkp-cmake-common-utils": "1.5.3-1",
    "switch-cmake": "1.5.2-1",
    "switch-pkg-config": "0.28-4",  # a script Switch.cmake insists on finding
    "libnx": "4.12.0-1",
    "switch-zlib": "1.3.1-1",
    "switch-lz4": "1.9.3-1",
}


CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "toolchain-cache")


def find(folder, name, version, arch):
    """(path on the server, bytes) of the package file: .zst, else .xz (kept in build/toolchain-cache)"""
    for ext in ("zst", "xz"):
        path = "%s%s-%s-%s.pkg.tar.%s" % (folder + "/" if folder else "", name, version, arch, ext)
        cached = os.path.join(CACHE, path)
        if os.path.isfile(cached):
            with open(cached, "rb") as f:
                return path, f.read()
        try:
            data = dkp.download(dkp.SERVER + path)
            os.makedirs(os.path.dirname(cached), exist_ok=True)
            with open(cached, "wb") as f:
                f.write(data)
            return path, data
        except urllib.error.HTTPError as e:
            if e.code != 404:
                raise
    sys.exit("not on devkitPro's server: %s %s (%s)" % (name, version, folder or "any"))


def entry(name, path, data):
    info = dkp.pkginfo(data)
    deps = info.get("depend", [])
    print("  %-24s %-50s %s  depends: %s" % (name, path, dkp.sha256(data)[:12], " ".join(deps) or "-"))
    return {"name": name, "path": path, "sha256": dkp.sha256(data)}


def main():
    lock = {"server": dkp.SERVER, "any": [], "hosts": {}}
    print("any:")
    for name, version in ANY_PACKAGES.items():
        lock["any"].append(entry(name, *find("", name, version, "any")))
    for platform, (folder, arch) in dkp.HOSTS.items():
        print(platform + ":")
        lock["hosts"][platform] = [entry(name, *find(folder, name, version, arch))
                                   for name, version in HOST_PACKAGES.items()]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lock.json")
    with open(path, "w", newline="\n") as f:
        json.dump(lock, f, indent=1)
        f.write("\n")
    print("wrote", path)


if __name__ == "__main__":
    main()
