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
# the host compiler for tools/switch/dksh_cache (the prepared graphics): zig's clang, one archive per platform,
# the version upstream's toolchains.json pins for Linux
ZIG = {
    "win-x64": ("https://ziglang.org/download/0.16.0/zig-x86_64-windows-0.16.0.zip",
                "68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e"),
    "osx-arm64": ("https://ziglang.org/download/0.16.0/zig-aarch64-macos-0.16.0.tar.xz",
                  "b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489"),
    "osx-x64": ("https://ziglang.org/download/0.16.0/zig-x86_64-macos-0.16.0.tar.xz",
                "0387557ed1877bc6a2e1802c8391953baddba76081876301c522f52977b52ba7"),
    "linux-x64": ("https://ziglang.org/download/0.16.0/zig-x86_64-linux-0.16.0.tar.xz",
                  "70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00"),
    "linux-arm64": ("https://ziglang.org/download/0.16.0/zig-aarch64-linux-0.16.0.tar.xz",
                    "ea4b09bfb22ec6f6c6ceac57ab63efb6b46e17ab08d21f69f3a48b38e1534f17"),
}


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
    lock = {"server": dkp.SERVER, "any": [], "hosts": {},
            "zig": {p: {"url": u, "sha256": h} for p, (u, h) in ZIG.items()}}
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
