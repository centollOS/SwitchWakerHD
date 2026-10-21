"""txt1: text pane drawn with one of the game's own fonts (referenced by name in fnl1, never shipped).

Layout (offsets from the section start; learned from the game's own txt1 sections and from the text box
constructor the game's layouts are built with, 0287795C, plus the HD text pane 027030FC / 02703538):

  0x00 pan1 fields (see pan1.py)
  0x54 u16 text buffer size in bytes, terminator included: the pane holds size / 2 - 1 characters
  0x56 u16 default text size in bytes, terminator included (the constructor raises the capacity to it)
  0x58 u16 material index (mat1)            0x5A u16 font index (fnl1)
  0x5C u8  text position: where the text block sits in the pane (see TEXT_POSITION)
  0x5D u8  line alignment (0-3, see LINE_ALIGN; the game's files use 0 and 2)
  0x5E u8  text flags: bit 0 shadow on, bit 1 restrict the text to the buffer, bits 2-3 go to the
           pane's draw flags (the game's panes use 0, 1, 4 and 5)
  0x5F u8  pad
  0x60 f32 italic ratio
  0x64 u32 default text offset (UTF-16 big endian, NUL-terminated; from the section start)
  0x68 rgba text colour top, 0x6C rgba text colour bottom
  0x70 f32 font width, 0x74 f32 font height (46 x 54 in most of the game's panes)
  0x78 f32 character spacing, 0x7C f32 line spacing
  0x80 u32 text id offset (the message label; 0 = none: the pane name is the label)
  0x84 f32 x2 shadow offset, 0x8C f32 x2 shadow scale
  0x94 rgba shadow colour top, 0x98 rgba shadow colour bottom, 0x9C f32 shadow italic ratio
  0xA0 default text, then the text id (NUL-terminated); the section is padded to 4

Each of the game's text panes has a material of its own named after the pane: black colour (alpha 0), white
colour, flags 0, or for panes with a shadow optionally flag 0x20000 and an 8-byte shadow-colour block
(black rgb, white rgba, pad).

Description keys (besides pan1's):
  text            default string (UTF-8; "\\n" is the game's line break)            ""
  capacity        buffer size in characters                                    max(len(text), 32)
                  (the game's text panes resize their buffer when the layout is built, from the pane
                  width; runtime text that needs more calls story_lyt_text::set_capacity)
  font            the game's font file name                                     "CKingMsg.bffnt"
  font_size       [w, h]                                                        [46, 54]
  char_space, line_space, italic                                              0
  text_position   0-10 or [h, v] with h in center/left/right, v in center/top/bottom    0 (centre)
  line_alignment  0-3 or "pane"/"left"/"center"/"right"                        0
  colors          [top, bottom] rgba                                            white
  shadow          {"offset": [x, y], "scale": [x, y], "colors": [top, bottom], "italic": 0,
                   "black": rgb, "white": rgba, "enabled": true}; on unless "enabled" is false
                  (offset [1, 1], scale [1, 1], black colours: the values the game's unshadowed panes keep)
  text_flags      extra raw bits for 0x5E (OR-ed in)                            0
  text_id         message label                                                 none
  text_material   {"black": rgba, "white": rgba}                               0/0/0/0, white
  scalable        the game's shrink-to-fit: minimum horizontal scale in percent (user data "Scalable",
                  written by usd1.py)
"""
import struct

from .. import binio
from . import pan1

DEFAULT_FONT = "CKingMsg.bffnt"
HORIZONTAL = {"center": 0, "left": 1, "right": 2}
VERTICAL = {"center": 0, "top": 1, "bottom": 2}
LINE_ALIGN = {"pane": 0, "left": 1, "center": 2, "right": 3}
SHADOW_ON, RESTRICT = 0x01, 0x02
MAT_SHADOW_BLOCK = 0x20000
TEXT_OFFSET = 0xA0


def encode_text(s):
    """UTF-16 big endian, NUL-terminated (characters outside the BMP become surrogate pairs)."""
    return s.encode("utf-16-be") + b"\0\0"


def text_position(v):
    if isinstance(v, (list, tuple)):
        h, vert = v
        return HORIZONTAL.get(h, h) + 4 * VERTICAL.get(vert, vert)
    return int(v)


def material(p):
    m = p.get("text_material", {})
    out = binio.rgba(m.get("black", [0, 0, 0, 0])) + binio.rgba(m.get("white", [255, 255, 255, 255]))
    sh = p.get("shadow")
    if sh is not None and sh.get("enabled", True) and ("black" in sh or "white" in sh):
        black = bytes(int(x) & 0xFF for x in list(sh.get("black", [0, 0, 0]))[:3])
        out += struct.pack(">I", MAT_SHADOW_BLOCK) + black + binio.rgba(sh.get("white", [255, 255, 255, 255])) + b"\0"
    else:
        out += struct.pack(">I", 0)
    return binio.name(p.get("material_name", p["name"]), 28) + out


def write(p, ctx):
    text = encode_text(p.get("text", ""))
    chars = len(text) // 2 - 1
    capacity = max(int(p.get("capacity", max(chars, 32))), chars)
    if 2 * (capacity + 1) > 0xFFFF:
        raise ValueError("text pane %r: capacity %d is too large" % (p["name"], capacity))
    mat = ctx.add_material(p.get("material_name", p["name"]), material(p))
    font = ctx.font(p.get("font", DEFAULT_FONT))
    sh = p.get("shadow")
    flags = int(p.get("text_flags", 0)) | (SHADOW_ON if sh is not None and sh.get("enabled", True) else 0)
    la = p.get("line_alignment", 0)
    colors = p.get("colors", [[255, 255, 255, 255]] * 2)
    fw, fh = p.get("font_size", [46, 54])
    sh = sh or {}
    shc = sh.get("colors", [[0, 0, 0, 255]] * 2)
    tid = p.get("text_id")
    out = pan1.body(p)
    out += struct.pack(">HHHHBBBB", 2 * (capacity + 1), len(text), mat, font,
                       text_position(p.get("text_position", 0)), LINE_ALIGN.get(la, la), flags, 0)
    out += binio.f32(p.get("italic", 0)) + struct.pack(">I", TEXT_OFFSET)
    out += binio.rgba(colors[0]) + binio.rgba(colors[1])
    out += binio.f32(fw) + binio.f32(fh) + binio.f32(p.get("char_space", 0)) + binio.f32(p.get("line_space", 0))
    out += struct.pack(">I", TEXT_OFFSET + len(text) if tid else 0)
    out += b"".join(binio.f32(v) for v in list(sh.get("offset", [1, 1])) + list(sh.get("scale", [1, 1])))
    out += binio.rgba(shc[0]) + binio.rgba(shc[1]) + binio.f32(sh.get("italic", 0))
    assert len(out) + 8 == TEXT_OFFSET
    out += text
    if tid:
        out += tid.encode() + b"\0"
    return out
