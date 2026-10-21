"""pan1: the pane fields every pane type starts with (also the body of null panes).

flags u8 (bit 0 visible, bit 1 influences children's alpha), origin u8 (raw; 0 = centre, the value of
nearly all of the game's panes), alpha u8, part flags u8 (2 in the game's panes, 0 in RootPane), name[24],
user data[8], translate xyz, rotate xyz, scale xy, size wh (f32).

Description keys (all pane types): name, size [w, h], translate [x, y(, z)], rotate [x, y, z] or z,
scale [x, y], alpha 0-255, visible (true), influence_alpha (false), origin (0), user (""), children [].
"""
from .. import binio


def body(p, extra=0x02):
    flags = (1 if p.get("visible", True) else 0) | (2 if p.get("influence_alpha", False) else 0)
    t = list(p.get("translate", [0, 0])) + [0] * 3
    r = p.get("rotate", [0, 0, 0])
    r = [0, 0, r] if isinstance(r, (int, float)) else list(r)
    s = p.get("scale", [1, 1])
    w, h = p["size"]
    return (bytes([flags, int(p.get("origin", 0)), int(p.get("alpha", 255)), p.get("part_flags", extra)]) +
            binio.name(p["name"], 24) + binio.name(p.get("user", ""), 8)[:8] +
            b"".join(binio.f32(v) for v in t[:3] + r[:3] + list(s) + [w, h]))


def write(p, ctx):
    return body(p)
