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

Comparing two builds (issue #66): two runs can differ more from the scene than from the code. --normalise scales
each file by the functions whose code is the same in both builds (one --elf per file; the first file is the
reference), so a function's normalised share is its cost in the reference run's time:

  tools/switch/main_profile.py --compare --normalise main.samples sync.samples --elf main.elf --elf sync.elf
"""
import argparse
import bisect
import collections
import hashlib
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


def read_elf_functions(elf):
    """[(name, address, code bytes)] from the ELF's symbol table (FUNC symbols with a size; a static function's
    name can appear more than once)"""
    with open(elf, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        sys.exit(elf + ": not a 64-bit little-endian ELF")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    out = []
    for sec in secs:
        if sec[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[sec[6]]
        for off in range(sec[4], sec[4] + sec[5], 24):
            name, info, _, shndx, value, size = struct.unpack_from("<IBBHQQ", data, off)
            if info & 0xF != 2 or not size or not 0 < shndx < shnum:  # STT_FUNC
                continue
            code = secs[shndx]
            if code[1] != 1:  # SHT_PROGBITS
                continue
            end = data.index(b"\0", strtab[4] + name)
            start = code[4] + value - code[3]
            out.append((data[strtab[4] + name:end].decode(errors="replace"), value, data[start:start + size]))
    return out


def masked_code(code):
    """the code with the immediates that depend on where things were linked set to 0 (AArch64): branches,
    ADR/ADRP and literal loads, and the ADD/LDR/STR offsets from an ADRP's page or an address made from it (the
    :lo12: part, and offsets into merged strings or a section anchor); what is left changes with the code only.
    (An address kept on the stack and loaded again is not followed: such a function can look changed, which only
    leaves it out of --normalise's yardstick; between main and main plus a one-file change, 2 of 59938 differed,
    one of them really changed.)"""
    words = struct.unpack("<%dI" % (len(code) // 4), code[: len(code) // 4 * 4])
    pages = set()  # registers an ADRP wrote, or an ADD or MOV made from one
    out = []
    for w in words:
        if w & 0x7C000000 == 0x14000000:  # B, BL
            w &= 0xFC000000
        elif w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000:  # B.cond, CBZ/CBNZ
            w &= 0xFF00001F
        elif w & 0x7E000000 == 0x36000000:  # TBZ/TBNZ
            w &= 0xFFF8001F
        elif w & 0x1F000000 == 0x10000000:  # ADR, ADRP
            if w & 0x80000000:
                pages.add(w & 0x1F)
            w &= 0x9F00001F
        elif w & 0x3B000000 == 0x18000000:  # LDR (literal)
            w &= 0xFF00001F
        elif w & 0xFFE0FFE0 == 0xAA0003E0:  # MOV (register): a copy of an address is one
            if (w >> 16) & 0x1F in pages:
                pages.add(w & 0x1F)
        elif (w & 0xFF800000 == 0x91000000 or w & 0x3B000000 == 0x39000000) and (w >> 5) & 0x1F in pages:
            if w & 0xFF800000 == 0x91000000:  # ADD (immediate): its result is an address too
                pages.add(w & 0x1F)
            w &= 0xFFC003FF  # ADD (immediate) / LDR, STR (unsigned offset): the 12-bit offset
        out.append(w)
    return struct.pack("<%dI" % len(out), *out)


def code_fingerprints(elf):
    """a function's name (as the self-time table writes it: before any ".") -> a hash of its masked code, with
    its .cold and other parts"""
    parts = collections.defaultdict(list)
    for name, _, code in read_elf_functions(elf):
        base, _, suffix = name.partition(".")
        if suffix.startswith("localalias"):  # the same code as the function
            continue
        parts[base].append((suffix, hashlib.sha1(masked_code(code)).digest()))
    return {k: hashlib.sha1(b"".join(s.encode() + h for s, h in sorted(v))).hexdigest() for k, v in parts.items()}


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


def compare(args, names):
    elfs = args.elf or ["build/switch-dk/wwhd.elf"]
    if len(elfs) not in (1, len(args.samples)):
        sys.exit("--compare takes one --elf, or one per samples file (in the same order)")
    elfs = elfs * len(args.samples) if len(elfs) == 1 else elfs
    symbols, prints = {}, {}
    cols, tables, selfs = [], [], []
    for path, elf in zip(args.samples, elfs):
        if elf not in symbols:
            symbols[elf] = read_symbols(elf)
        addrs, syms = symbols[elf]
        starts = guest_functions(syms)
        text_lo, text_hi = addrs[0], addrs[-1] + 0x100000
        tags, base, samples = read_samples(path)
        ti = tags.index(args.thread)
        mine = [fr for t, fr in samples if t == ti]
        c, own = collections.Counter(), collections.Counter()  # the table's time; self time (for --normalise)
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
                    if k == 0:
                        own["(outside the executable)"] += 1
                        if not args.inclusive:
                            c["(outside the executable)"] += 1
                    if not args.inclusive:
                        break
                    continue
                q = syms[bisect.bisect_right(addrs, a) - 1].split(".")[0]
                if k == 0:
                    own[q] += 1
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
        selfs.append({k: 100.0 * v / len(mine) for k, v in own.items()})
    scale = [1.0] * len(tables)
    if args.normalise:
        for elf in set(elfs):
            prints[elf] = code_fingerprints(elf)
        # unchanged: the same code in every file's build; their self time is the yardstick (each sample counts
        # once in self time, so the total is not inflated by call chains)
        same = {k for k, h in prints[elfs[0]].items() if all(prints[e].get(k) == h for e in elfs[1:])}
        changed = sum(1 for k in prints[elfs[0]] if k not in same)
        totals = [sum(v for k, v in t.items() if k in same) for t in selfs]
        if not all(totals):
            sys.exit("--normalise: no sampled function has the same code in every build")
        scale = [totals[0] / u for u in totals]
        sampled = sum(1 for k in same if any(k in t for t in selfs))
        print("normalised by the %d functions with the same code in every build (%d of them sampled; %d changed or "
              "missing in a build)" % (len(same), sampled, changed))
        print("their self time: %s" % ", ".join("%s %.2f%%" % (c, u) for c, u in zip(cols, totals)))
        print("each file's time for the same unchanged work, against %s's (the n: columns' scale): %s" % (
            cols[0], ", ".join("%.1f%%" % (100.0 * f) for f in scale)))
        print()
    keys = set().union(*tables)
    ranked = sorted(keys, key=lambda k: -sum(t.get(k, 0) for t in tables))
    head = " ".join("%11s" % c[:11] for c in cols)
    if args.normalise:
        head += "  " + " ".join("%11s" % ("n:" + c)[:11] for c in cols[1:])
    print("%-58s %s  mean" % ("function (%s time, thread %s)" % ("inclusive" if args.inclusive else "self", args.thread),
                               head))
    for k in ranked[: args.top]:
        m = re.match(r"f_([0-9A-F]{8})", k)
        lab = "%s %s" % (k, names.get(m.group(1), "")) if m else k
        vals = [t.get(k, 0) for t in tables]
        row = " ".join("%10.2f%%" % v for v in vals)
        if args.normalise:
            # the function's share scaled to the reference run's time; * a function whose code changed
            mark = "" if k in same or canon(k) in same or k.startswith("(") else "*"
            row += "  " + " ".join("%10.2f%%" % (v * f) for v, f in zip(vals[1:], scale[1:])) + mark
        print("%-58s %s %5.2f" % (lab[:58], row, sum(vals) / len(vals)))


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
    ap.add_argument("--normalise", action="store_true",
                    help="with --compare: scale each file by the functions whose code is the same in every build "
                         "(the first file is the reference; --elf once per file when the builds differ)")
    ap.add_argument("--elf", action="append",
                    help="the build's ELF (default build/switch-dk/wwhd.elf); with --compare, once per file")
    ap.add_argument("--thread", default="game")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--names", default="build/verify/names.tsv")
    ap.add_argument("--decomp", default="decomp", help="the decomp checkout (names from its sources)")
    ap.add_argument("--callers", help="a function (f_XXXXXXXX or a symbol): the chains above it, by caller")
    ap.add_argument("--hitches", action="store_true", help="a hitch file (logs/hitch_*.samples): the slow frames")
    ap.add_argument("--each", type=int, default=10, help="with --hitches: the longest frames shown one by one")
    args = ap.parse_args()

    names = read_names(args.names, args.decomp)
    if args.compare:
        return compare(args, names)
    if args.normalise:
        sys.exit("--normalise goes with --compare")
    if args.elf and len(args.elf) > 1:
        sys.exit("one --elf without --compare")
    addrs, syms = read_symbols(args.elf[0] if args.elf else "build/switch-dk/wwhd.elf")
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
