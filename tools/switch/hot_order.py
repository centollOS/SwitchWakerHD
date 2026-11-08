#!/usr/bin/env python3
"""(this fork) The Switch NRO's hot code layout: the functions the frame threads spend their time in, from thread
sampler profiles (docs/cpu-threading-plan.md, tools/switch/main_profile.py), placed together at the start of .text
through the linker's --section-ordering-file (cmake/SwitchPort.cmake uses tools/switch/hot_sections.ld when it
exists). Runtime and game functions are compiled one per section (-ffunction-sections), so a function that no longer
exists matches nothing and the file stays valid when the code changes; regenerate it after big changes.

  tools/switch/hot_order.py OUT.ld PROFILE.samples ELF [PROFILE.samples ELF ...] [--cover 0.995]

Each profile is read with the ELF of the build that ran it; the functions are ranked by their samples (game and
render threads together, every profile weighted alike) and kept until --cover of all samples are in.
"""
import argparse
import bisect
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import main_profile as mp  # noqa: E402


def weights(samples_path, elf):
    tags, base, samples = mp.read_samples(samples_path)
    addrs, syms = mp.read_symbols(elf)
    hi = addrs[-1] + 0x100000
    count = collections.Counter()
    total = 0
    for _, frames in samples:
        if not frames:
            continue
        total += 1
        a = frames[0] - base
        if a < addrs[0] or a > hi:
            continue
        name = syms[bisect.bisect_right(addrs, a) - 1]
        count[name.split(".")[0]] += 1  # .localalias, .part.0, .constprop.0, .isra.0 live in the base's section
    return {k: v / total for k, v in count.items()} if total else {}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("pairs", nargs="+", help="PROFILE.samples ELF ...")
    ap.add_argument("--cover", type=float, default=0.995)
    a = ap.parse_args()
    if len(a.pairs) % 2:
        sys.exit("profiles and ELFs come in pairs")
    total = collections.Counter()
    for path, elf in zip(a.pairs[::2], a.pairs[1::2]):
        for k, v in weights(path, elf).items():
            total[k] += v
    ranked = total.most_common()
    s = sum(v for _, v in ranked)
    kept, acc = [], 0.0
    for k, v in ranked:
        kept.append(k)
        acc += v
        if acc >= a.cover * s:
            break
    with open(a.out, "w") as f:
        f.write("/* (this fork) tools/switch/hot_order.py: the frame threads' hot functions first, %d functions */\n"
                % len(kept))
        f.write(".text : {\n")
        f.write("  KEEP (*(.crt0))  /* first, as in libnx's switch.ld: the NRO's entry and MOD0 header */\n")
        for k in kept:
            # a function's own section, and the clones GCC makes of it (.part, .constprop, .isra)
            f.write("  *(.text.%s .text.%s.*)\n" % (k, k))
        f.write("}\n")
    print("%s: %d functions, %.1f%% of the samples" % (a.out, len(kept), 100 * acc / s))


if __name__ == "__main__":
    main()
