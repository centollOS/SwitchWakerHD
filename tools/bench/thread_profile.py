#!/usr/bin/env python3
"""Report for runtime/src/platform/thread_sampler.cpp (WWHD_SAMPLE_THREADS=<prefix>): where the frame
threads' time goes, by module, by function, and which of the game executable's call sites lead into
the Vulkan driver and the kernel.

  tools/bench/thread_profile.py <prefix> [--exe build/prof/wwhd.exe] [--top 30] [--thread render]

Functions of the executable are named with llvm-symbolizer (build with debug information, e.g.
RelWithDebInfo); functions of DLLs with their nearest export (llvm-readobj --coff-exports), which
names system and driver entry points but not their internal functions.
"""
import argparse
import bisect
import collections
import os
import re
import shutil
import struct
import subprocess
import sys


def read_samples(path):
    with open(path, "rb") as f:
        data = f.read()
    head_end = data.index(b"\n")
    magic, n = data[:head_end].split()
    if magic != b"WWHDSAMP1":
        sys.exit("not a thread_sampler file: " + path)
    pos = head_end + 1
    tags = []
    for _ in range(int(n)):
        end = data.index(b"\n", pos)
        tags.append(data[pos:end].decode())
        pos = end + 1
    samples = []
    while pos + 8 <= len(data):
        thread, count = struct.unpack_from("<II", data, pos)
        pos += 8
        if pos + 8 * count > len(data):
            break
        # return addresses may carry a pointer-authentication code in their upper bits (ARMv8.3 PAC):
        # user addresses fit in 48 bits (the sampler strips it too; older captures did not)
        frames = tuple(a & 0x0000FFFFFFFFFFFF for a in struct.unpack_from("<%dQ" % count, data, pos))
        pos += 8 * count
        samples.append((thread, frames))
    return tags, samples


def read_modules(path):
    mods = []
    with open(path) as f:
        for line in f:
            base, size, name = line.rstrip("\n").split(" ", 2)
            mods.append((int(base, 16), int(size, 16), name))
    mods.sort()
    return mods


class Modules:
    def __init__(self, mods):
        self.mods = mods
        self.bases = [m[0] for m in mods]

    def find(self, addr):
        i = bisect.bisect_right(self.bases, addr) - 1
        if i >= 0 and addr < self.mods[i][0] + self.mods[i][1]:
            return self.mods[i]
        return None


def tool(name):
    for d in os.environ.get("PATH", "").split(os.pathsep) + [r"C:\dev\rel-dl\llvm-mingw-20260922-ucrt-aarch64\bin"]:
        p = os.path.join(d, name + (".exe" if os.name == "nt" else ""))
        if os.path.isfile(p):
            return p
    return shutil.which(name)


def exports(dll, readobj):
    """[(rva, name)] sorted, from the export table."""
    try:
        out = subprocess.run([readobj, "--coff-exports", dll], capture_output=True, text=True, timeout=120).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    res, name = [], None
    for line in out.splitlines():
        m = re.match(r"\s*Name: (\S+)", line)
        if m:
            name = m.group(1)
        m = re.match(r"\s*RVA: 0x([0-9A-Fa-f]+)", line)
        if m and name:
            res.append((int(m.group(1), 16), name))
            name = None
    res.sort()
    return res


def image_base(path, readobj):
    out = subprocess.run([readobj, "--file-headers", path], capture_output=True, text=True).stdout
    m = re.search(r"ImageBase: 0x([0-9A-Fa-f]+)", out)
    return int(m.group(1), 16) if m else 0


class Namer:
    def __init__(self, modules, exe_path):
        self.modules = modules
        self.readobj = tool("llvm-readobj")
        self.symbolizer = tool("llvm-symbolizer")
        self.exe_path = exe_path
        self.export_cache = {}
        self.sym_cache = {}

    def module_name(self, addr):
        m = self.modules.find(addr)
        return os.path.basename(m[2]) if m else "?"

    def is_exe(self, addr):
        m = self.modules.find(addr)
        return bool(m) and os.path.basename(m[2]).lower() == os.path.basename(self.exe_path).lower()

    def symbolize_exe(self, addrs):
        """address -> 'function (file:line)' for the executable, in one llvm-symbolizer run."""
        todo = sorted({a for a in addrs if a not in self.sym_cache})
        if not todo:
            return
        m = next(m for m in self.modules.mods if os.path.basename(m[2]).lower() == os.path.basename(self.exe_path).lower())
        pref = image_base(self.exe_path, self.readobj)
        # return addresses point after the call: look up the call itself (one instruction back)
        query = "\n".join("0x%x" % (a - m[0] + pref - 4) for a in todo) + "\n"
        out = subprocess.run([self.symbolizer, "--obj=" + self.exe_path, "--functions=short", "--demangle",
                              "--no-inlines", "--relativenames"], input=query, capture_output=True, text=True).stdout
        blocks = [b for b in out.split("\n\n") if b.strip()]
        for a, b in zip(todo, blocks):
            lines = b.strip().splitlines()
            fn = lines[0] if lines else "?"
            loc = lines[1] if len(lines) > 1 else ""
            loc = re.sub(r":(\d+):\d+$", r":\1", loc.replace("\\", "/"))
            self.sym_cache[a] = (fn, loc)

    def dll_name(self, addr):
        m = self.modules.find(addr)
        if not m:
            return "?"
        base, _, path = m
        if path not in self.export_cache:
            self.export_cache[path] = exports(path, self.readobj) if self.readobj else []
        ex = self.export_cache[path]
        rva = addr - base
        i = bisect.bisect_right([e[0] for e in ex], rva) - 1
        mod = os.path.basename(path)
        if i < 0:
            return "%s+0x%x" % (mod, rva)
        return "%s!%s+0x%x" % (mod, ex[i][1], rva - ex[i][0])

    def function(self, addr):
        if self.is_exe(addr):
            fn, _ = self.sym_cache.get(addr, ("?", ""))
            return fn
        return re.sub(r"\+0x[0-9a-f]+$", "", self.dll_name(addr))

    def site(self, addr):
        fn, loc = self.sym_cache.get(addr, ("?", ""))
        return "%s  %s" % (fn, loc)


def frames_of(fr):
    """pc, then the caller chain (lr duplicates the first frame-pointer frame in non-leaf functions)."""
    out = [fr[0]]
    for a in fr[1:]:
        if a and a != out[-1]:
            out.append(a)
    return out


def pct(n, total):
    return "%5.1f%%" % (100.0 * n / total) if total else "  -  "


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prefix")
    ap.add_argument("--exe", required=True, help="the profiled game executable (with debug information)")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--thread", action="append", help="only these tags (default: all)")
    a = ap.parse_args()
    tags, samples = read_samples(a.prefix + ".samples")
    mods = Modules(read_modules(a.prefix + ".modules"))
    namer = Namer(mods, a.exe)
    exe_addrs = {x for _, fr in samples for x in frames_of(fr) if namer.is_exe(x)}
    namer.symbolize_exe(exe_addrs)
    for ti, tag in enumerate(tags):
        if a.thread and tag not in a.thread:
            continue
        ss = [frames_of(fr) for t, fr in samples if t == ti]
        total = len(ss)
        print("=" * 100)
        print("thread %s: %d samples (~%.1f s at 1 kHz)" % (tag, total, total / 1000.0))
        if not total:
            continue
        by_mod = collections.Counter(namer.module_name(s[0]) for s in ss)
        print("\n-- where the thread is (the sampled instruction's module) --")
        for k, v in by_mod.most_common(a.top):
            print("  %s  %s" % (pct(v, total), k))
        by_fn = collections.Counter(namer.function(s[0]) for s in ss)
        print("\n-- self time by function --")
        for k, v in by_fn.most_common(a.top):
            print("  %s  %s" % (pct(v, total), k))
        # outside the executable: the executable's call site that led there
        sites = collections.Counter()
        site_mod = collections.defaultdict(collections.Counter)
        for s in ss:
            if namer.is_exe(s[0]):
                continue
            caller = next((x for x in s[1:] if namer.is_exe(x)), None)
            key = namer.site(caller) if caller else "(no caller in the executable)"
            sites[key] += 1
            site_mod[key][namer.function(s[0])] += 1
        outside = sum(sites.values())
        print("\n-- time outside the executable (%s), by the executable's call site --" % pct(outside, total).strip())
        for k, v in sites.most_common(a.top):
            top = ", ".join("%s %s" % (f, pct(c, v).strip()) for f, c in site_mod[k].most_common(3))
            print("  %s  %s\n          -> %s" % (pct(v, total), k, top))
        incl = collections.Counter()
        for s in ss:
            for fn in {namer.sym_cache[x][0] for x in s if namer.is_exe(x)}:
                incl[fn] += 1
        print("\n-- inclusive time by executable function --")
        for k, v in incl.most_common(a.top):
            print("  %s  %s" % (pct(v, total), k))
        print()


if __name__ == "__main__":
    main()
