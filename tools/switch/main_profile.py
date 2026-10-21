#!/usr/bin/env python3
"""Report for the Switch's thread sampler (runtime/src/platform/thread_sampler_switch.cpp, the debug server's
"profile <seconds>"; docs/cpu-threading-plan.md step 0.2): where the main ("game") and render threads spend their
time, by function.

  tools/switch/wwhd_debug.py raw profile 10            # on the console, in the place to measure
  tools/switch/wwhd_debug.py raw profile status        # -> logs/prof_<n>.samples
  tools/switch/wwhd_debug.py get logs/prof_<n>.samples build/prof/sea.samples
  tools/switch/main_profile.py build/prof/sea.samples --elf build/switch-dk/wwhd.elf [--thread game] [--top 40]

The ELF must be the one of the build that ran (the samples hold the executable's base, not a build id).
Self time: the function holding the pc. Inclusive time: every distinct function in the sample's pc, lr and
frame-pointer chain (the chain is short where code is built without frame pointers: the inclusive column is a lower
bound). Game functions are the recompiled ones (f_XXXXXXXX: the game's canonical address); names from the decomp's
verify units and names.tsv when present (--names).
"""
import argparse
import bisect
import collections
import os
import re
import struct
import subprocess
import sys


def read_samples(path):
    with open(path, "rb") as f:
        data = f.read()
    lines = []
    pos = 0
    head = data[: data.index(b"\n")].decode()
    m = re.match(r"WWHDSAMP1 (\d+)$", head)
    if not m:
        sys.exit("not a WWHDSAMP1 file")
    pos = data.index(b"\n") + 1
    tags = []
    for _ in range(int(m.group(1))):
        nl = data.index(b"\n", pos)
        tags.append(data[pos:nl].decode())
        pos = nl + 1
    base = 0
    if data[pos:pos + 5] == b"base ":
        nl = data.index(b"\n", pos)
        base = int(data[pos + 5:nl], 16)
        pos = nl + 1
    samples = []
    while pos + 8 <= len(data):
        thread, count = struct.unpack_from("<II", data, pos)
        pos += 8
        if count > 64 or pos + 8 * count > len(data):
            break
        frames = struct.unpack_from("<%dQ" % count, data, pos)
        pos += 8 * count
        samples.append((thread, frames))
    return tags, base, samples


def read_symbols(elf):
    nm = None
    for tool in ("nm", "llvm-nm", "aarch64-none-elf-nm"):
        try:
            nm = subprocess.run([tool, "-n", "--defined-only", elf], capture_output=True, text=True, check=True).stdout
            break
        except (OSError, subprocess.CalledProcessError):
            continue
    if nm is None:
        sys.exit("no nm could read " + elf)
    addrs, names = [], []
    for line in nm.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "tTwW":
            addrs.append(int(p[0], 16))
            names.append(p[2])
    return addrs, names


ADDR_RE = re.compile(r"^/\* ([0-9A-F]{8})\b")
DEF_RE = re.compile(r"([A-Za-z_][\w:~<>]*)\s*\(")


def read_names(path, decomp):
    """address -> name: names.tsv (--names), then the decomp's sources (a "/* ADDR */" line above a definition)"""
    names = {}
    if path and os.path.exists(path):
        for line in open(path):
            f = line.rstrip("\n").split("\t")
            if len(f) >= 2 and re.match(r"[0-9A-F]{8}$", f[0]):
                names[f[0]] = f[1]
    src = os.path.join(decomp, "wwhd_src")
    for root, _, files in os.walk(src):
        for fn in files:
            if not fn.endswith((".cpp", ".c", ".h")):
                continue
            pending = None
            for line in open(os.path.join(root, fn), errors="replace"):
                m = ADDR_RE.match(line)
                if m:
                    pending = m.group(1)
                    continue
                if pending:
                    d = DEF_RE.search(line)
                    if d and not line.lstrip().startswith(("//", "/*", "#")):
                        names.setdefault(pending, "%s (%s)" % (d.group(1), os.path.splitext(fn)[0]))
                        pending = None
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("samples")
    ap.add_argument("--elf", default="build/switch-dk/wwhd.elf")
    ap.add_argument("--thread", default="game")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--names", default="build/verify/names.tsv")
    ap.add_argument("--decomp", default="decomp", help="the decomp checkout (names from its sources)")
    args = ap.parse_args()

    tags, base, samples = read_samples(args.samples)
    addrs, syms = read_symbols(args.elf)
    names = read_names(args.names, args.decomp)
    text_lo, text_hi = addrs[0], addrs[-1] + 0x100000

    def sym(pc):
        a = pc - base
        if a < text_lo or a > text_hi:
            return None
        i = bisect.bisect_right(addrs, a) - 1
        return syms[i] if i >= 0 else None

    def label(s):
        m = re.match(r"f_([0-9A-F]{8})", s)
        if m and m.group(1) in names:
            return "%s %s" % (s, names[m.group(1)])
        return s

    for ti, tag in enumerate(tags):
        if args.thread != "all" and tag != args.thread:
            continue
        mine = [fr for t, fr in samples if t == ti]
        if not mine:
            continue
        self_c = collections.Counter()
        incl_c = collections.Counter()
        kind = collections.Counter()
        outside = 0
        depth = 0
        for fr in mine:
            s = sym(fr[0])
            if s is None:
                outside += 1
                kind["outside the executable (kernel, libraries)"] += 1
            else:
                self_c[s] += 1
                kind["game code (f_)" if s.startswith("f_") else "runtime and libraries"] += 1
            seen = set()
            for pc in fr:
                q = sym(pc)
                if q and q not in seen:
                    seen.add(q)
                    incl_c[q] += 1
            depth += len(fr)
        n = len(mine)
        print("== thread %s: %d samples, mean chain %.1f frames" % (tag, n, depth / n))
        for k, v in kind.most_common():
            print("   %5.1f%%  %s" % (100.0 * v / n, k))
        print("-- self time")
        for s, v in self_c.most_common(args.top):
            print("   %5.2f%%  %s" % (100.0 * v / n, label(s)))
        print("-- inclusive (lower bound)")
        for s, v in incl_c.most_common(args.top):
            print("   %5.2f%%  %s" % (100.0 * v / n, label(s)))
        print()


if __name__ == "__main__":
    main()
