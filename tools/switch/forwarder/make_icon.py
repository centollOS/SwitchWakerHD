#!/usr/bin/env python3
"""Generate 256x256 JPEG icon from game's 128x128 icon (TGA, PNG, JPG).

    make_icon.py INPUT.{tga,png,jpg} OUT.jpg

The source icon is scaled up (Lanczos) onto an opaque black background,
since the HOME menu wants a JPEG. Matches forwarder's icon generation exactly.
"""
import io
import sys

from PIL import Image


def main():
    if len(sys.argv) != 3:
        print("Usage: make_icon.py INPUT.{tga,png,jpg} OUT.jpg", file=sys.stderr)
        sys.exit(1)
    src, out = sys.argv[1:3]
    im = Image.open(src).convert("RGBA")
    im = im.resize((256, 256), Image.LANCZOS)
    bg = Image.new("RGB", im.size, (0, 0, 0))
    bg.paste(im, mask=im.split()[3])
    buf = io.BytesIO()
    bg.save(buf, "JPEG", quality=92, optimize=True)
    # an unchanged icon keeps its timestamp: the build packs the NRO again only when the icon changed
    try:
        with open(out, "rb") as f:
            same = f.read() == buf.getvalue()
    except OSError:
        same = False
    if not same:
        with open(out, "wb") as f:
            f.write(buf.getvalue())
    print(f"make_icon: {out} from {src}")


if __name__ == "__main__":
    main()
