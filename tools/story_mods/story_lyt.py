#!/usr/bin/env python3
"""Build a story UI layout archive (Layout/<name>.szs) from a description (local only, story mods are WIP).

usage: story_lyt.py DESC.json OUT.szs
       story_lyt.py --png PLATE.png OUT.szs [--width 512]   the prototype's one-picture layout

The writers live in the lyt package (tools/story_mods/lyt); see lyt/description.py for the format.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lyt  # noqa: E402

EXAMPLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lyt", "examples", "plate.json")


def plate_description(png, width):
    from PIL import Image
    with open(EXAMPLE) as f:
        d = json.load(f)
    im = Image.open(png)
    h = max(1, round(im.height * width / im.width))
    d["textures"]["StoryPlate_00"].update(png=os.path.abspath(png), width=width)
    d["panes"][0]["children"][0]["size"] = [width, h]
    return lyt.Description(d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("desc", nargs="?")
    ap.add_argument("out")
    ap.add_argument("--png")
    ap.add_argument("--width", type=int, default=512)
    a = ap.parse_args()
    if a.png:
        desc = plate_description(a.png, a.width)
    elif a.desc:
        desc = lyt.Description.load(a.desc)
    else:
        ap.error("a description or --png is required")
    lyt.build(desc, a.out)
    print("%s: %d bytes" % (a.out, os.path.getsize(a.out)))


if __name__ == "__main__":
    main()
