"""pic1: picture pane. pane fields (pan1), vertex colours[4] rgba (top-left, top-right, bottom-left,
bottom-right), material index u16, texture coordinate set count u8, pad u8, then per set 4 UV pairs
(TL, TR, BL, BR).

Description keys (besides pan1's): material (name in "materials"), vertex_colors [4 x rgba] (white),
uvs [[TL, TR, BL, BR] per texture] (default: the full texture, one set per material texture).
"""
import struct

from .. import binio
from . import pan1

FULL = [[0, 0], [1, 0], [0, 1], [1, 1]]


def write(p, ctx):
    mat = ctx.material(p["material"])
    ntex = len(ctx.desc.materials[p["material"]].get("textures", []))
    uvs = p.get("uvs", [FULL] * max(ntex, 1))
    vc = p.get("vertex_colors", [[255, 255, 255, 255]] * 4)
    out = pan1.body(p) + b"".join(binio.rgba(c) for c in vc) + struct.pack(">HBB", mat, len(uvs), 0)
    for s in uvs:
        out += b"".join(binio.f32(u) + binio.f32(v) for u, v in s)
    return out
