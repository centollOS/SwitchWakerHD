#!/usr/bin/env python3
"""Release notes from README.md: install instructions + the "What's new" section + checksums.

usage: notes.py README.md VERSION SHA256SUMS.txt > notes.md
"""
import re
import sys


def section(text, title_prefix):
    m = re.search(r"^## %s.*?$\n(.*?)(?=^## )" % re.escape(title_prefix), text, re.S | re.M)
    return m.group(1).strip() if m else ""


def main():
    readme, version, sums = sys.argv[1:4]
    with open(readme, encoding="utf-8") as f:
        text = f.read()
    new = section(text, "What's new")
    with open(sums) as f:
        checksums = f.read().strip()
    print("""**The Wind Waker HD, native PC port, %s**

This release contains **no game files, no game code and no keys**. You need your own disc dump
(.wux/.wud with its disc key, plus the Wii U common key from your console), or an already
extracted game folder. The installer builds the game from it on your machine.

**Install:** download the zip for your system, unzip it, and run **Wind Waker HD Setup** (the
Terminal installer next to it is the fallback):
- macOS (Apple Silicon, macOS 14+): `Wind Waker HD Setup.app`. The release is not signed by Apple:
  macOS 15+: System Settings > Privacy & Security > Open Anyway; macOS 14: right-click > Open
- Windows (x86-64): `Wind Waker HD Setup.exe` (SmartScreen: "More info" > "Run anyway")
- Linux (x86-64, glibc 2.35+, Vulkan): `wwhd-setup`

Setup takes a few minutes; see "Install (releases)" in the README for details.

## What's new

%s

## Checksums (SHA-256)

```
%s
```
""" % (version, new or "See the README.", checksums))


if __name__ == "__main__":
    main()
