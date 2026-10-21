"""Texture writer: .bflim (GX2 surface data followed by the FLIM footer).

Footer, as in the game's own .bflim files: "FLIM", BOM, header size 0x14, version, file size, 1 section,
then "imag" (size 0x10), width u16, height u16, alignment u16, format u8, tiling byte, image size u32.
The image data (one 2D surface, no mips) comes before the footer.

Tiling byte: low nibble = GX2 tile mode, bits 5-7 = the surface swizzle (pipe + 2 x bank, GX2 swizzle
bits 8-10). Every layout texture of the game decodes cleanly with that reading, and the sizes and
alignments of all 310 of them match gx2tile.surface_size.

Formats, identified from the game's own layout textures (block size from the image sizes; contents
decoded with bc.py):
  key     FLIM code  suffix  GX2 format   notes
  rgba8   0x14       ^l      0x1A         RGBA8 (prototype, verified in game)
  bc1     0x15       ^o      0x31 BC1     colour + 1-bit alpha
  bc3     0x17       ^q      0x33 BC3     colour + smooth alpha
  bc4a    0x10       ^s      0x34 BC4     alpha only (drawn white with the channel as alpha)
  bc4l    0x0F       ^r      0x34 BC4     luminance only (drawn grey, opaque)
  bc5la   0x11       ^t      0x35 BC5     luminance (red) + alpha (green)
How the game draws the one- and two-channel codes was checked in a game run (all formats and tile modes
side by side on the item screen); the encoder fills the BC channels from the picture accordingly
(luminance = (r + g + b) / 3 weighted as 0.299 / 0.587 / 0.114).
Tile modes: "tiled" (4, 2D tiled thin, the game's usual mode), "1d" (2), "linear" (1, linear aligned).
"""
import struct
from collections import namedtuple

from . import binio

# bc: BC key or None; bpp: bits per element; channels: what the BC channels hold ("rgba", "a", "l", "la")
Format = namedtuple("Format", "code suffix gx2 bc bpp channels")

FORMATS = {
    "rgba8": Format(0x14, "l", 0x1A, None, 32, "rgba"),
    "bc1": Format(0x15, "o", 0x31, "bc1", 64, "rgba"),
    "bc3": Format(0x17, "q", 0x33, "bc3", 128, "rgba"),
    "bc4a": Format(0x10, "s", 0x34, "bc4", 64, "a"),
    "bc4l": Format(0x0F, "r", 0x34, "bc4", 64, "l"),
    "bc5la": Format(0x11, "t", 0x35, "bc5", 128, "la"),
}

TILE_MODES = {"linear": 1, "1d": 2, "tiled": 4}


def register_format(key, code, suffix, gx2, bc, bpp, channels="rgba"):
    FORMATS[key] = Format(code, suffix, gx2, bc, bpp, channels)


def _to_channels(rgba, channels):
    """RGBA -> RGBA whose first channels hold what the format stores (a: alpha, l: luminance, la: both)."""
    if channels == "rgba":
        return bytes(rgba)
    import numpy as np
    a = np.frombuffer(bytes(rgba), np.uint8).reshape(-1, 4)
    lum = np.clip(np.round(a[:, :3].astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32)), 0, 255)
    out = np.zeros_like(a)
    if channels == "a":
        out[:, 0] = a[:, 3]
    elif channels == "l":
        out[:, 0] = lum
    else:
        out[:, 0], out[:, 1] = lum, a[:, 3]
    return out.tobytes()


def _from_channels(rgba, channels):
    """Inverse of _to_channels for previews: how the game draws the format."""
    if channels == "rgba":
        return rgba
    import numpy as np
    a = np.frombuffer(rgba, np.uint8).reshape(-1, 4)
    out = np.full_like(a, 255)
    if channels == "a":
        out[:, 3] = a[:, 0]
    elif channels == "l":
        out[:, :3] = a[:, :1]
    else:
        out[:, :3], out[:, 3] = a[:, :1], a[:, 1]
    return out.tobytes()


def elements(rgba, w, h, fmt):
    """(element bytes, element columns, element rows): texels for RGBA8, 4 x 4 blocks for BC."""
    f = FORMATS[fmt]
    if f.bc is None:
        return bytes(rgba), w, h
    from . import bc
    return bc.encode(f.bc, _to_channels(rgba, f.channels), w, h), -(-w // 4), -(-h // 4)


def encode(rgba, w, h, fmt="rgba8", tile="linear", swizzle=0):
    """RGBA bytes -> .bflim. swizzle: 0-7 (pipe + 2 x bank), tiled mode only."""
    if len(rgba) != w * h * 4:
        raise ValueError("expected %d RGBA bytes, got %d" % (w * h * 4, len(rgba)))
    f = FORMATS[fmt]
    mode = TILE_MODES[tile]
    if not 0 <= swizzle <= 7 or (swizzle and mode != 4):
        raise ValueError("swizzle %d needs the tiled mode and 0-7" % swizzle)
    data, ew, eh = elements(rgba, w, h, fmt)
    from . import gx2tile
    _, _, size, align = gx2tile.surface_size(ew, eh, f.bpp, mode)
    image = gx2tile.tile(data, ew, eh, f.bpp, mode, swizzle << 8)
    assert len(image) == size
    footer = (b"FLIM" + struct.pack(">HHIIHH", 0xFEFF, 0x14, binio.VERSION, len(image) + 0x28, 1, 0) +
              b"imag" + struct.pack(">IHHHBBI", 0x10, w, h, align, f.code, mode | (swizzle << 5), len(image)))
    return image + footer


def parse_footer(data):
    """(width, height, alignment, format code, tiling byte, image size) of a .bflim."""
    f = data[-0x28:]
    assert f[:4] == b"FLIM" and f[0x14:0x18] == b"imag", "not a bflim"
    return struct.unpack(">HHHBBI", f[0x1C:0x28])


def decode(data):
    """.bflim -> (RGBA bytes, width, height), for the formats above (tests and previews)."""
    w, h, _, code, tile_byte, size = parse_footer(data)
    fmt = next(k for k, f in FORMATS.items() if f.code == code)
    f = FORMATS[fmt]
    from . import gx2tile
    ew, eh = (w, h) if f.bc is None else (-(-w // 4), -(-h // 4))
    lin = gx2tile.untile(data[:size], ew, eh, f.bpp, tile_byte & 15, (tile_byte >> 5) << 8)
    if f.bc is None:
        return lin, w, h
    from . import bc
    return _from_channels(bc.decode(f.bc, lin, w, h), f.channels), w, h
