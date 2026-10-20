#!/usr/bin/env python3
"""Generate the 256x256 JPEG icon of the NRO and the HOME-screen forwarder.

    make_icon.py SOURCE OUT.jpg

SOURCE is the extracted game folder (meta/, content/) or an image (TGA, PNG, JPG).

From the game folder: the title-screen logo (THE LEGEND OF ZELDA, the Wind Waker, HD; content/Common/Layout/
Title_00.szs) over the panel of Link raising the sword in the boot screen (meta/bootTvTex.tga), darkened.
Without those files, the game's own 128x128 meta/iconTex.tga. Everything comes from your dump; none of the
game's art is in this repository.

An image is scaled to 256x256 (Lanczos) onto an opaque black background, since the HOME menu wants a JPEG.
"""
import io
import os
import struct
import sys

from PIL import Image

SIZE = 256
# bootTvTex.tga (1280x720): the frame of the centre panel is x 509..784, y 207..473; with a 7 px margin on every
# side the box is 290x281, scaled to the square icon (3% taller)
PANEL = (502, 200, 792, 481)
DIM = ((10, 20, 60), 0.55)  # the panel blended this much towards navy, so that the logo reads over it
# the logo's textures in Title_00.szs, their width in the icon and the gap above each, stacked and centred
LOGO = (("timg/TitleLogoZelda_00^l.bflim", 232, 0),
        ("timg/TitleLogoWindwaker_00^l.bflim", 212, 4),
        ("timg/TitleLogoHD_00^l.bflim", 72, 8))


def bflim_rgba8(data):
    """A BFLIM's image: RGBA8 (sRGB) in GX2's 2D_TILED_THIN1 layout, the only kind the logo uses."""
    imag = data[-0x14:]
    if data[-0x28:-0x24] != b"FLIM" or imag[:4] != b"imag":
        raise ValueError("not a BFLIM")
    w, h = struct.unpack(">HH", imag[8:12])
    fmt, tile, swizzle = imag[14], imag[15] & 0x1F, imag[15] >> 5
    if fmt != 20 or tile != 4:
        raise ValueError("BFLIM format %d, tile mode %d" % (fmt, tile))
    pitch = (w + 31) & ~31  # whole macro tiles (32x16 at 32 bpp, 2 pipes and 4 banks)
    pipe_swizzle, bank_swizzle = swizzle & 1, swizzle >> 1
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            # AddrLib (R600): the pixel's place in its 8x8 micro tile, then the pipe and bank of its macro tile
            pixel = (x & 1) | (x & 2) | (y & 1) << 2 | (x & 4) << 1 | (y & 2) << 3 | (y & 4) << 3
            pipe = ((y >> 3) ^ (x >> 3)) & 1
            bank = ((y >> 5) ^ (x >> 3)) & 1 | (((y >> 4) ^ (x >> 4)) & 1) << 1
            bank_pipe = (pipe + 2 * bank) ^ (pipe_swizzle + 2 * bank_swizzle)
            pipe, bank = bank_pipe & 1, (bank_pipe >> 1) & 3
            macro = (x // 32 + pitch // 32 * (y // 16)) * (32 * 16 * 4)
            offset = pixel * 4 + (macro >> 3)
            a = bank << 9 | pipe << 8 | offset & 0xFF | (offset & ~0xFF) << 3
            out[(y * w + x) * 4:(y * w + x) * 4 + 4] = data[a:a + 4]
    return Image.frombytes("RGBA", (w, h), bytes(out))


def title_icon(game):
    """The title-screen logo over the boot screen's panel, or None without the files."""
    tv = os.path.join(game, "meta", "bootTvTex.tga")
    szs = os.path.join(game, "content", "Common", "Layout", "Title_00.szs")
    if not (os.path.isfile(tv) and os.path.isfile(szs)):
        return None
    try:
        # Yaz0 and SARC of the game's archives; imported here, so that an image source never needs it
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
        import shaderprep
        boot = Image.open(tv).convert("RGB")
        if boot.size != (1280, 720):
            return None
        with open(szs, "rb") as f:
            files = dict(shaderprep.sarc(shaderprep.yaz0_python(f.read())))
        logos = []
        for name, width, gap in LOGO:
            im = bflim_rgba8(files[name])
            logos.append((im.resize((width, round(im.height * width / im.width)), Image.LANCZOS), gap))
    except Exception as e:  # an archive or texture other than the expected one: the fallback, not a failed build
        print("make_icon: no title-screen icon (%s); the game's iconTex.tga instead" % e, file=sys.stderr)
        return None
    icon = boot.crop(PANEL).resize((SIZE, SIZE), Image.LANCZOS)
    icon = Image.blend(icon, Image.new("RGB", icon.size, DIM[0]), DIM[1])
    y = (SIZE - sum(im.height + gap for im, gap in logos)) // 2
    for im, gap in logos:
        y += gap
        icon.paste(im, ((SIZE - im.width) // 2, y), im)
        y += im.height
    return icon


def scaled(src):
    im = Image.open(src).convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)
    bg = Image.new("RGB", im.size, (0, 0, 0))
    bg.paste(im, mask=im.split()[3])
    return bg


def main():
    if len(sys.argv) != 3:
        print("Usage: make_icon.py GAME_FOLDER|IMAGE OUT.jpg", file=sys.stderr)
        sys.exit(1)
    src, out = sys.argv[1:3]
    if os.path.isdir(src):
        icon = title_icon(src)
        what = "the title screen of " + src
        if icon is None:
            fallback = os.path.join(src, "meta", "iconTex.tga")
            if not os.path.isfile(fallback):
                print("make_icon: %s has neither the title screen nor meta/iconTex.tga" % src, file=sys.stderr)
                sys.exit(1)
            icon, what = scaled(fallback), fallback
    else:
        icon, what = scaled(src), src
    buf = io.BytesIO()
    icon.save(buf, "JPEG", quality=92, optimize=True)
    # an unchanged icon keeps its timestamp: the build packs the NRO again only when the icon changed
    try:
        with open(out, "rb") as f:
            same = f.read() == buf.getvalue()
    except OSError:
        same = False
    if not same:
        with open(out, "wb") as f:
            f.write(buf.getvalue())
    print(f"make_icon: {out} from {what}")


if __name__ == "__main__":
    main()
