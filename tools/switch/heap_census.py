#!/usr/bin/env python3
"""Names the call sites of the debug server's `heap diff` / `heap top` reply (WWHD_HEAP_CENSUS builds,
runtime/src/platform/heap_census_switch.cpp) with addr2line on the build's wwhd.elf (docs/memory-round2-plan.md).

usage: heap_census.py wwhd.elf reply.txt     (addr2line from devkitPro: native, or the devkita64 container)"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

elf, reply = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).read_text()
tool = "aarch64-none-elf-"
native = shutil.which(tool + "addr2line") or (Path("/opt/devkitpro/devkitA64/bin") / (tool + "addr2line"))


def run(name, *args):
    if Path(native).exists():
        return subprocess.run([str(Path(native).with_name(tool + name)), *args], capture_output=True, text=True).stdout
    return subprocess.run(["docker", "run", "--rm", "-v", f"{elf.parent}:/e", "devkitpro/devkita64",
                           f"/opt/devkitpro/devkitA64/bin/{tool}{name}", *[a.replace(str(elf), f"/e/{elf.name}") for a in args]],
                          capture_output=True, text=True).stdout


anchor_rt = int(re.search(r"anchor (0x[0-9a-f]+)", reply).group(1), 16)
anchor_elf = next(int(l.split()[0], 16) for l in run("nm", "-C", str(elf)).splitlines() if "heap_census" in l and "anchor()" in l)
base = anchor_rt - anchor_elf
rows = [l for l in reply.splitlines() if re.match(r"\s*-?\d+ ", l)]
pcs = sorted({int(p, 16) - base for l in rows for p in re.findall(r"0x[0-9a-f]+", l) if int(p, 16) > base})
names = {}
if pcs:
    out = run("addr2line", "-f", "-C", "-s", "-e", str(elf), *[hex(p - 4) for p in pcs]).splitlines()  # (the call, not the return)
    for i, p in enumerate(pcs):
        fn, where = out[2 * i], out[2 * i + 1]
        names[p] = f"{fn[:70]} ({where})"
print(reply.splitlines()[0])
for l in rows:
    cols = l.split()
    print(f"{int(cols[0]) / 1048576:9.2f} MiB {int(cols[1]) / 1048576:+9.2f} MiB {cols[2]:>8} {cols[3]:>8}")
    for p in re.findall(r"0x[0-9a-f]+", l):
        a = int(p, 16) - base
        if a > 0:
            print(f"        {names.get(a, hex(a))}")
