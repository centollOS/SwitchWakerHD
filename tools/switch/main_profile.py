#!/usr/bin/env python3
"""Report for the Switch's thread sampler (runtime/src/platform/thread_sampler_switch.cpp, the debug server's
"profile <seconds>"; docs/cpu-threading-plan.md step 0.2): where the main ("game") and render threads spend their
time, by function.

  tools/switch/wwhd_debug.py raw profile 10            # on the console, in the place to measure
  tools/switch/wwhd_debug.py raw profile status        # -> logs/prof_<n>.samples
  tools/switch/wwhd_debug.py get logs/prof_<n>.samples build/prof/sea.samples
  tools/switch/main_profile.py build/prof/sea.samples --elf build/switch-dk/wwhd.elf [--thread game] [--top 40]

Hitch mode (logs/hitch_profile.flag on the SD card, docs/hitch-profiler-plan.md): logs/hitch_<n>.samples holds only
the frames longer than the threshold, each after a marker record; --hitches reports what each thread did in them:

  tools/switch/main_profile.py build/prof/hitch_123.samples --hitches [--thread all] [--each 10]

The ELF must be the one of the build that ran (the samples hold the executable's base, not a build id).
Self time: the function holding the pc. Inclusive time: every distinct function in the sample's pc, lr and
frame-pointer chain (the chain is short where code is built without frame pointers: the inclusive column is a lower
bound). When the sampler also walked the game's own stack (the main thread: its PowerPC back chain and the return
addresses saved in it), those frames count too, and the inclusive column holds every game function on the call stack.
Game functions are the recompiled ones (f_XXXXXXXX: the game's canonical address); names from the decomp's
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


GUEST = 1 << 63  # a sample frame of the guest stack: GUEST | the game's return address


def is_guest(pc):
    return pc >> 32 == GUEST >> 32


def guest_functions(syms):
    """the game functions' canonical addresses, sorted (from the f_XXXXXXXX symbols)"""
    return sorted({int(m.group(1), 16) for m in (re.match(r"f_([0-9A-F]{8})", s) for s in syms) if m})


def guest_sym(starts, pc):
    """the game function holding a guest return address"""
    i = bisect.bisect_right(starts, (pc & 0xFFFFFFFF) - 1) - 1  # (the address after the call; a call may end it)
    return "f_%08X" % starts[i] if i >= 0 else None


def canon(q):
    """one name per game function: f_XXXXXXXX (its _orig and .localalias symbols, and its guest frames, alike)"""
    m = re.match(r"(f_[0-9A-F]{8})", q)
    return m.group(1) if m else q


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


def compare(args, addrs, syms, names):
    starts = guest_functions(syms)
    text_lo, text_hi = addrs[0], addrs[-1] + 0x100000
    cols, tables = [], []
    for path in args.samples:
        tags, base, samples = read_samples(path)
        ti = tags.index(args.thread)
        mine = [fr for t, fr in samples if t == ti]
        c = collections.Counter()
        for fr in mine:
            seen = set()
            for k, pc in enumerate(fr):
                if is_guest(pc):
                    if args.inclusive:
                        q = guest_sym(starts, pc)
                        if q and q not in seen:
                            seen.add(q)
                            c[q] += 1
                    continue
                a = pc - base
                if a < text_lo or a > text_hi:
                    if k == 0 and not args.inclusive:
                        c["(outside the executable)"] += 1
                    if not args.inclusive:
                        break
                    continue
                q = syms[bisect.bisect_right(addrs, a) - 1].split(".")[0]
                if args.inclusive:
                    q = canon(q)
                    if q not in seen:
                        seen.add(q)
                        c[q] += 1
                else:
                    c[q] += 1
                    break
        cols.append(os.path.splitext(os.path.basename(path))[0])
        tables.append({k: 100.0 * v / len(mine) for k, v in c.items()})
    keys = set().union(*tables)
    ranked = sorted(keys, key=lambda k: -sum(t.get(k, 0) for t in tables))
    print("%-58s %s  mean" % ("function (%s time, thread %s)" % ("inclusive" if args.inclusive else "self", args.thread),
                               " ".join("%11s" % c[:11] for c in cols)))
    for k in ranked[: args.top]:
        m = re.match(r"f_([0-9A-F]{8})", k)
        lab = "%s %s" % (k, names.get(m.group(1), "")) if m else k
        vals = [t.get(k, 0) for t in tables]
        print("%-58s %s %5.2f" % (lab[:58], " ".join("%10.2f%%" % v for v in vals), sum(vals) / len(vals)))


MARKER = 0xFFFFFFFE  # a hitch file's marker record: frames (start tick, end tick, frame, rounds)
TICK_HZ = 19_200_000


def hitch_windows(samples):
    """the hitch file's slow frames: [(frame, ms, rounds, [(thread, frames), ...]), ...] in the file's order"""
    out = []
    for t, fr in samples:
        if t == MARKER:
            out.append((fr[2], (fr[1] - fr[0]) * 1000.0 / TICK_HZ, fr[3], []))
        elif out:
            out[-1][3].append((t, fr))
    return out


def hitches(args, tags, sym, label):
    wins = hitch_windows(read_samples(args.samples[0])[2])
    if not wins:
        print("no slow frames in the file")
        return
    ms = sorted(w[1] for w in wins)
    print("== %d slow frames: %.0f ms median, %.0f ms longest; frames shown for 3+ vblanks need > 33.4 ms" % (
        len(wins), ms[len(ms) // 2], ms[-1]))

    def tables(rows):
        self_c, incl_c = collections.Counter(), collections.Counter()
        for fr in rows:
            s = sym(fr[0])
            self_c[s or "(outside the executable)"] += 1
            seen = set()
            for pc in fr:
                q = sym(pc)
                q = canon(q) if q else q
                if q and q not in seen:
                    seen.add(q)
                    incl_c[q] += 1
        return self_c, incl_c

    def below(incl_c, n, limit):
        """inclusive entries under 98%: the thread's entry chain (on every sample's stack) says nothing"""
        return [(k, v) for k, v in incl_c.most_common() if v < 0.98 * n][:limit]

    empty = [w for w in wins if not w[3]]
    if empty:
        print("   %d of them without samples (the sampler thread did not run in them: %s ms)" % (
            len(empty), ", ".join("%.0f" % w[1] for w in sorted(empty, key=lambda w: -w[1])[:8])))
    threads = [i for i, tag in enumerate(tags) if args.thread == "all" or tag == args.thread]
    for ti in threads:
        rows = [fr for w in wins for t, fr in w[3] if t == ti]
        if not rows:
            continue
        self_c, incl_c = tables(rows)
        n = len(rows)
        print("\n== thread %s, all slow frames together: %d samples" % (tags[ti], n))
        print("-- self time")
        for k, v in self_c.most_common(args.top):
            print("   %5.2f%%  %s" % (100.0 * v / n, label(k)))
        print("-- inclusive (without the functions on 98%+ of the stacks)")
        for k, v in below(incl_c, n, args.top):
            print("   %5.2f%%  %s" % (100.0 * v / n, label(k)))
    print("\n== the %d longest frames with samples, one by one (per thread: self, then inclusive under 98%%)" %
          min(args.each, len(wins) - len(empty)))
    for frame, wms, rounds, recs in sorted([w for w in wins if w[3]], key=lambda w: -w[1])[: args.each]:
        print("-- frame %d: %.1f ms, %d rounds sampled" % (frame, wms, rounds))
        for ti in threads:
            rows = [fr for t, fr in recs if t == ti]
            if not rows:
                continue
            self_c, incl_c = tables(rows)
            n = len(rows)
            print("   %-6s self: %s" % (tags[ti], ", ".join("%s %.0f%%" % (label(k), 100.0 * v / n)
                                                          for k, v in self_c.most_common(4))))
            print("   %-6s in:   %s" % ("", ", ".join("%s %.0f%%" % (label(k), 100.0 * v / n)
                                                      for k, v in below(incl_c, n, 6))))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("samples", nargs="+", help="one file, or several with --compare")
    ap.add_argument("--compare", action="store_true", help="a table: self time per file, functions by their mean")
    ap.add_argument("--inclusive", action="store_true", help="with --compare: inclusive time instead of self")
    ap.add_argument("--elf", default="build/switch-dk/wwhd.elf")
    ap.add_argument("--thread", default="game")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--names", default="build/verify/names.tsv")
    ap.add_argument("--decomp", default="decomp", help="the decomp checkout (names from its sources)")
    ap.add_argument("--callers", help="a function (f_XXXXXXXX or a symbol): the chains above it, by caller")
    ap.add_argument("--hitches", action="store_true", help="a hitch file (logs/hitch_*.samples): the slow frames")
    ap.add_argument("--each", type=int, default=10, help="with --hitches: the longest frames shown one by one")
    args = ap.parse_args()

    addrs, syms = read_symbols(args.elf)
    names = read_names(args.names, args.decomp)
    if args.compare:
        return compare(args, addrs, syms, names)
    tags, base, samples = read_samples(args.samples[0])
    text_lo, text_hi = addrs[0], addrs[-1] + 0x100000

    starts = guest_functions(syms)

    def sym(pc):
        if is_guest(pc):
            return guest_sym(starts, pc)
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

    if args.hitches:
        return hitches(args, tags, sym, label)

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
        depth = gdepth = 0
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
                q = canon(q) if q else q
                if q and q not in seen:
                    seen.add(q)
                    incl_c[q] += 1
            depth += sum(1 for pc in fr if not is_guest(pc))
            gdepth += sum(1 for pc in fr if is_guest(pc))
        n = len(mine)
        if args.callers:
            chains = collections.Counter()
            hits = 0
            for fr in mine:
                names_ = []
                for pc in fr:
                    q = sym(pc)
                    q = canon(q) if q else q
                    if q and (not names_ or names_[-1] != q):
                        names_.append(q)
                idx = [i for i, q in enumerate(names_) if q.split(".")[0].startswith(args.callers)]
                if not idx:
                    continue
                hits += 1
                i = idx[0]
                chains[" < ".join(label(q) for q in names_[i:i + 4])] += 1
            print("== thread %s: %s in %.2f%% of samples; chains (it < caller < ...)" % (tag, args.callers, 100.0 * hits / n))
            for c, v in chains.most_common(args.top):
                print("   %5.2f%%  %s" % (100.0 * v / n, c))
            continue
        print("== thread %s: %d samples, mean chain %.1f frames%s" % (
            tag, n, depth / n, ", %.1f guest frames" % (gdepth / n) if gdepth else ""))
        for k, v in kind.most_common():
            print("   %5.1f%%  %s" % (100.0 * v / n, k))
        print("-- self time")
        for s, v in self_c.most_common(args.top):
            print("   %5.2f%%  %s" % (100.0 * v / n, label(s)))
        print("-- inclusive" if gdepth else "-- inclusive (lower bound)")
        for s, v in incl_c.most_common(args.top):
            print("   %5.2f%%  %s" % (100.0 * v / n, label(s)))
        print()


if __name__ == "__main__":
    main()
