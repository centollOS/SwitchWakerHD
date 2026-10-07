#!/usr/bin/env python3
"""The forwarder's 256x256 HOME-menu icon from the game's own icon (meta/iconTex.tga of your dump).

    make_icon.py ICONTEX.tga OUT.jpg

The Wii U icon is 128x128; it is scaled up (Lanczos) onto an opaque background, since the HOME menu
wants a JPEG. The result stays in build/ (it comes from your dump: never committed).
"""
import sys

from PIL import Image

src, out = sys.argv[1:3]
im = Image.open(src).convert("RGBA")
im = im.resize((256, 256), Image.LANCZOS)
bg = Image.new("RGB", im.size, (0, 0, 0))
bg.paste(im, mask=im.split()[3])
bg.save(out, "JPEG", quality=92, optimize=True)
print(f"make_icon: {out} from {src}")
