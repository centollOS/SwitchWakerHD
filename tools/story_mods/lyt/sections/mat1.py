"""mat1: materials. count u16, pad u16, u32 offsets (relative to the section start), then the materials.

A material: name[28], black colour rgba, white colour rgba, flags u32, then the blocks the flags announce.
Flag bits used here (from the game's own materials): bits 0-1 texture map count, bits 2-3 texture SRT
count, bits 4-5 texture coordinate generator count. Other flag bits announce further blocks (TEV stages,
alpha compare, blend modes, ...); they are passed through as "flags_extra" with their raw blocks in
"raw_tail" (hex) until a writer for them exists.

Blocks: texture maps (u16 txl1 index, u8 wrap/filter S, u8 wrap/filter T; 4/4 is by far the most common
value in the game), texture SRTs (translate x y, rotate, scale x y: 5 f32), texture coordinate generators
(8 bytes: type 0x01 matrix 0x1E... written as zeros like the prototype, which the game accepts).

Colours: the game applies material colours as linear values while the ^l images are sRGB (lyt-window's
game run); with the description's "color_space": "srgb" (default) black / white are given in sRGB and
converted here, so a colour matches the same colour in an image.

Description:
  {"textures": [{"texture": "StoryPlate_00", "wrap": [4, 4],
                 "srt": {"translate": [0, 0], "rotate": 0, "scale": [1, 1]}}],
   "black": [0, 0, 0, 0], "white": [255, 255, 255, 255], "flags_extra": 0, "raw_tail": ""}
"""
import struct

from .. import binio


def encode(mat_name, spec, ctx):
    texs = spec.get("textures", [])
    if len(texs) > 3:
        raise ValueError("material %r: at most 3 textures" % mat_name)
    n = len(texs)
    flags = n | (n << 2) | (n << 4) | int(spec.get("flags_extra", 0))
    colour = binio.rgba_linear if ctx.desc.color_space == "srgb" else binio.rgba
    out = (binio.name(mat_name, 28) + colour(spec.get("black", [0, 0, 0, 0])) +
           colour(spec.get("white", [255, 255, 255, 255])) + struct.pack(">I", flags))
    for t in texs:
        ws, wt = t.get("wrap", [4, 4])
        out += struct.pack(">HBB", ctx.texture(t["texture"]), ws, wt)
    for t in texs:
        srt = t.get("srt", {})
        tx, ty = srt.get("translate", [0, 0])
        sx, sy = srt.get("scale", [1, 1])
        out += binio.f32(tx) + binio.f32(ty) + binio.f32(srt.get("rotate", 0)) + binio.f32(sx) + binio.f32(sy)
    for _ in texs:
        out += b"\0" * 8
    return out + bytes.fromhex(spec.get("raw_tail", ""))


def write(materials):
    """materials: [(name, encoded bytes)] from the build context."""
    head = struct.pack(">HH", len(materials), 0)
    first = 8 + len(head) + 4 * len(materials)
    offs, blob = [], b""
    for _, m in materials:
        offs.append(first + len(blob))
        blob += binio.pad4(m)
    return binio.section(b"mat1", head + b"".join(struct.pack(">I", o) for o in offs) + blob)
