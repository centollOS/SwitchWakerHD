"""prt1: parts pane, an instance of another layout ("part") placed in this one, e.g. the game's button
layouts (BtnController_00, ...) placed as L_Btn_00 panes in their screens.

Layout, from the game's own prt1 sections: pane fields (pan1), property count u32, part scale x y (f32),
the properties (40 bytes each: pane name[24], usage flags u8, basic usage flags u8, pad u16, then u32
offsets of an overriding pane section, of user data and of basic info, 0 = none), then the part's layout
name (NUL-terminated, padded to 4), then the overriding sections the properties point to. The game's
parts without overrides are pan1 + 0 + 1.0 1.0 + name (e.g. size 0x6C for a 10-character name).
The part's layout (blyt/<part>.bflyt) is looked up through the layout's resource accessor.

Description keys (type "parts", besides pan1's): "part" layout name, "part_scale" [x, y] (1, 1).
Property overrides are not written yet.
"""
import struct

from .. import binio
from . import pan1


def write(p, ctx):
    sx, sy = p.get("part_scale", [1, 1])
    return pan1.body(p) + struct.pack(">I", 0) + binio.f32(sx) + binio.f32(sy) + binio.cstr4(p["part"])
