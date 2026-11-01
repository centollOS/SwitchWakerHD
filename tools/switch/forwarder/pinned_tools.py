#!/usr/bin/env python3
"""The HOME-screen forwarder's tools from the pinned toolchain (docs/pinned-toolchain-plan.md): nothing from the
computer (no git, make, bash, MSYS2 or C compiler of its own). Used by build_forwarder_windows.py --toolchain.

- sources: nx-hbloader and hacBrewPack at the commits of forwarder.env, downloaded as GitHub's source archives
  over HTTPS and checked against the SHA-256 of their file tree (forwarder.env *_TREE_SHA256: the files and their
  contents, not the archive's bytes, which GitHub may recompress);
- nx-hbloader-forwarder.patch applied by a small unified-diff reader (exact context);
- nx-hbloader: the pinned devkitA64 with the commands of its Makefile (devkitPro's switch_rules / base_rules),
  then elf2nso and npdmtool from the pinned switch-tools;
- hacBrewPack and its bundled mbedtls: the pinned zig (zig cc), with the flags of their Makefiles.
Standard library only.
"""
import concurrent.futures
import glob
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

WINDOWS = os.name == "nt"
EXE = ".exe" if WINDOWS else ""


class ToolError(Exception):
    pass


def _tree_sha256(files):
    """files: (relative path with /, bytes) pairs. The same digest as forwarder.env's *_TREE_SHA256."""
    h = hashlib.sha256()
    for name, data in sorted(files):
        h.update(name.encode() + b"\0" + str(len(data)).encode() + b"\0" + data)
    return h.hexdigest()


def fetch_source(dst, git_url, rev, tree_sha256):
    """dst: the source tree of git_url at rev, from GitHub's archive (https://codeload.github.com/OWNER/REPO/tar.gz/
    REV), checked against tree_sha256. A dst already holding that tree (a stamp file) is kept."""
    stamp = os.path.join(dst, ".wwhd-tree-sha256")
    if os.path.isfile(stamp) and open(stamp).read().strip() == tree_sha256:
        return
    m = re.match(r"https://github\.com/([^/]+)/([^/]+?)(\.git)?$", git_url)
    if not m:
        raise ToolError("not a GitHub repository: %s" % git_url)
    url = "https://codeload.github.com/%s/%s/tar.gz/%s" % (m.group(1), m.group(2), rev)
    print("  downloading %s" % url, flush=True)
    data = None
    for attempt in range(3):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "SwitchWakerHD-forwarder"})
            data = urllib.request.urlopen(req, timeout=120).read()
            break
        except OSError as e:
            if attempt == 2:
                raise ToolError("could not download %s: %s" % (url, e))
    files = []
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
        for member in t.getmembers():
            if member.isfile() and "/" in member.name:
                rel = member.name.split("/", 1)[1]
                if rel.startswith("/") or ".." in rel.split("/"):
                    raise ToolError("unexpected path in %s: %s" % (url, member.name))
                files.append((rel, t.extractfile(member).read()))
    got = _tree_sha256(files)
    if got != tree_sha256:
        raise ToolError("%s at %s is not the pinned source (tree SHA-256 %s, expected %s)" % (git_url, rev, got,
                                                                                            tree_sha256))
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    for rel, b in files:
        path = os.path.join(dst, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(b)
    with open(stamp, "w") as f:
        f.write(tree_sha256 + "\n")


def apply_patch(root, patch_path):
    """A unified diff (git diff format) applied to the files under root. Every hunk's context and removed lines must
    match exactly (line endings aside), at its line number or anywhere else in the file."""
    with open(patch_path, newline="") as f:
        lines = f.read().replace("\r\n", "\n").split("\n")
    i = 0
    while i < len(lines):
        if not lines[i].startswith("+++ "):
            i += 1
            continue
        target = lines[i][4:].strip()
        target = target[2:] if target.startswith("b/") else target
        path = os.path.join(root, *target.split("/"))
        with open(path, newline="") as f:
            text = f.read()
        crlf = "\r\n" in text
        body = text.replace("\r\n", "\n").split("\n")
        i += 1
        offset = 0
        while i < len(lines) and lines[i].startswith("@@"):
            start = int(re.match(r"@@ -(\d+)", lines[i]).group(1)) - 1
            i += 1
            old, new = [], []
            while i < len(lines) and lines[i][:1] in (" ", "-", "+", "\\") and not lines[i].startswith("+++ "):
                tag, content = lines[i][:1], lines[i][1:]
                if tag in (" ", "-"):
                    old.append(content)
                if tag in (" ", "+"):
                    new.append(content)
                i += 1
            at = start + offset
            if body[at:at + len(old)] != old:
                at = next((k for k in range(len(body) - len(old) + 1) if body[k:k + len(old)] == old), -1)
                if at < 0:
                    raise ToolError("%s does not apply to %s (hunk at line %d)" % (os.path.basename(patch_path),
                                                                                   target, start + 1))
            body[at:at + len(old)] = new
            offset = at - start + len(new) - len(old)
        with open(path, "w", newline="") as f:
            f.write(("\r\n" if crlf else "\n").join(body))


def _run(cmd, cwd, env):
    p = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode != 0:
        raise ToolError("%s failed:\n%s" % (" ".join(cmd), p.stdout.decode("utf-8", "replace")))


def build_hbloader(hbl, dkp, env, version="2.4.5"):
    """hbl.nso and hbl.npdm in hbl/, as `make RELEASE=1` with devkitPro's switch_rules would make them."""
    a64 = os.path.join(dkp, "devkitA64", "bin")
    cc = os.path.join(a64, "aarch64-none-elf-gcc" + EXE)
    libnx, portlibs = os.path.join(dkp, "libnx"), os.path.join(dkp, "portlibs", "switch")
    build = os.path.join(hbl, "build")
    os.makedirs(build, exist_ok=True)
    arch = ["-march=armv8-a", "-mtune=cortex-a57", "-mtp=soft", "-fPIE"]
    # INCLUDES (include/), then LIBDIRS = PORTLIBS LIBNX, then the build folder: the Makefile's order
    include = ["-I" + os.path.join(hbl, "include"), "-I" + os.path.join(portlibs, "include"),
               "-I" + os.path.join(libnx, "include"), "-I" + build]
    cflags = ["-g", "-Wall", "-O2", "-ffunction-sections"] + arch + include + \
             ["-D__SWITCH__", '-DVERSION="v%s"' % version]
    objs = []
    src = os.path.join(hbl, "source")
    # OFILES: the .cpp, then .c, then .s files of source/ (none .cpp here)
    for c in sorted(glob.glob(os.path.join(src, "*.c"))):
        o = os.path.join(build, os.path.basename(c)[:-2] + ".o")
        _run([cc] + cflags + ["-c", c, "-o", o], build, env)
        objs.append(o)
    for s in sorted(glob.glob(os.path.join(src, "*.s"))):
        o = os.path.join(build, os.path.basename(s)[:-2] + ".o")
        _run([cc, "-x", "assembler-with-cpp", "-g"] + arch + ["-c", s, "-o", o], build, env)
        objs.append(o)
    elf = os.path.join(hbl, "hbl.elf")
    _run([cc, "-specs=" + os.path.join(libnx, "switch.specs"), "-g"] + arch +
         ["-Wl,-wrap,exit", "-Wl,-Map,hbl.map"] + objs +
         ["-L" + os.path.join(portlibs, "lib"), "-L" + os.path.join(libnx, "lib"), "-lnx", "-o", elf], build, env)
    tools = os.path.join(dkp, "tools", "bin")
    _run([os.path.join(tools, "elf2nso" + EXE), elf, os.path.join(hbl, "hbl.nso")], hbl, env)
    _run([os.path.join(tools, "npdmtool" + EXE), os.path.join(hbl, "hbl.json"), os.path.join(hbl, "hbl.npdm")], hbl,
         env)
    return os.path.join(hbl, "hbl.nso"), os.path.join(hbl, "hbl.npdm")


def build_hacbrewpack(hbp, zig, env):
    """hacbrewpack(.exe) in hbp/, with zig cc: mbedtls's library as its Makefile builds it (with hacBrewPack's
    config.mk.template CFLAGS), then hacBrewPack itself."""
    cflags = ["-O2", "-Wall", "-Wextra", "-pedantic", "-std=gnu11"] + ([] if WINDOWS else ["-fPIC"])
    env = dict(env)
    cache = os.path.abspath(os.path.join(hbp, "zig-cache"))
    env["ZIG_GLOBAL_CACHE_DIR"] = env["ZIG_LOCAL_CACHE_DIR"] = cache
    lib_dir = os.path.join(hbp, "mbedtls", "library")
    jobs = []
    for c in sorted(glob.glob(os.path.join(lib_dir, "*.c"))):
        jobs.append(([zig, "cc"] + cflags + ["-Wall", "-W", "-Wdeclaration-after-statement",
                                            "-I" + os.path.join(hbp, "mbedtls", "include"), "-D_FILE_OFFSET_BITS=64",
                                            "-c", c, "-o", c[:-2] + ".o"], lib_dir))
    hbp_cflags = cflags + ["-D_BSD_SOURCE", "-D_POSIX_SOURCE", "-D_POSIX_C_SOURCE=200112L", "-D_DEFAULT_SOURCE",
                           "-D__USE_MINGW_ANSI_STDIO=1", "-D_FILE_OFFSET_BITS=64"]
    names = ["sha", "aes", "extkeys", "pki", "utils", "main", "filepath", "ConvertUTF", "nca", "romfs", "pfs0",
             "ivfc", "nacp", "npdm", "cnmt", "rsa"]  # the Makefile's order
    for n in names:
        jobs.append(([zig, "cc", "-I", os.path.join(".", "mbedtls", "include"), "-c"] + hbp_cflags +
                     ["-o", n + ".o", n + ".c"], hbp))
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        for f in [pool.submit(_run, cmd, cwd, env) for cmd, cwd in jobs]:
            f.result()
    lib = os.path.join(lib_dir, "libmbedcrypto.a")
    if os.path.exists(lib):
        os.remove(lib)
    _run([zig, "ar", "rcs", lib] + sorted(glob.glob(os.path.join(lib_dir, "*.o"))), lib_dir, env)
    exe = os.path.join(hbp, "hacbrewpack" + EXE)
    _run([zig, "cc", "-o", exe] + [n + ".o" for n in names] + [lib], hbp, env)
    return exe


def pack(hbp_exe, pack_dir, keys, title_id, env):
    """hacBrewPack's NSP from pack_dir/{exefs,romfs,control}: (exit code, its whole output)"""
    p = subprocess.run([hbp_exe, "-k", keys, "--titleid", title_id, "--nologo", "--exefsdir", "exefs",
                        "--romfsdir", "romfs", "--controldir", "control", "--nspdir", "nsp", "--ncadir", "nca",
                        "--tempdir", "temp", "--backupdir", "backup"],
                       cwd=pack_dir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")
