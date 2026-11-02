"""devkitPro's package server, as fetch.py and make_lock.py read it (standard library only; Python 3.14 for zstd).

devkitPro's Cloudflare refuses curl and Python's default client but accepts pacman's user agent, which the
installer's pacman-fetch.py already sends for the same files."""
import hashlib
import io
import os
import tarfile
import time
import urllib.error
import urllib.request

SERVER = "https://pkg.devkitpro.org/packages/"
AGENT = "pacman/6.1.0 (SwitchWakerHD pinned toolchain) libalpm/15.0.0"
# platform -> the folder and architecture of its host packages on the server
HOSTS = {
    "win-x64": ("windows/x86_64", "x86_64"),
    "osx-arm64": ("osx/arm64", "arm64"),
    "osx-x64": ("osx/x86_64", "x86_64"),
    "linux-x64": ("linux/x86_64", "x86_64"),
    "linux-arm64": ("linux/aarch64", "aarch64"),
}


def download(url, tries=4):
    """the file's bytes; a 403 or a network error is retried a few times (devkitPro's Cloudflare is moody)"""
    last = None
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": AGENT}), timeout=120) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                raise
            last = e
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            last = e
        time.sleep(5 * (i + 1))
    raise last


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def open_package(data):
    """the package as a tar file (.pkg.tar.zst or .pkg.tar.xz)"""
    mode = "r:zst" if data[:4] == b"\x28\xb5\x2f\xfd" else "r:xz"
    return tarfile.open(fileobj=io.BytesIO(data), mode=mode)


def pkginfo(data):
    """.PKGINFO's fields: {key: [values]}"""
    with open_package(data) as t:
        text = t.extractfile(".PKGINFO").read().decode()
    out = {}
    for line in text.splitlines():
        if " = " in line and not line.startswith("#"):
            k, v = line.split(" = ", 1)
            out.setdefault(k, []).append(v)
    return out
