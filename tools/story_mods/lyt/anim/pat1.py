"""pat1: animation tag. order u16, group count u16, name offset u32 (0x1C), group names offset u32,
start frame i16, end frame i16, child binding u8, pad[3], name (padded to 4), groups (name[24] +
flags u8 + pad[3] each). Offsets are relative to the section start."""
import struct

from .. import binio


def write(anim_name, groups, start, end, order=0, child_binding=0):
    nm = binio.cstr4(anim_name)
    head = struct.pack(">HHIIhhBBH", order, len(groups), 0x1C, 0x1C + len(nm), start, end, child_binding, 0, 0)
    return binio.section(b"pat1", head + nm + b"".join(binio.name(g, 28) for g in groups))
