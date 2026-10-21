"""wnd1: window pane (a 9-slice frame around a content quad that stretches with the pane size).

Layout, learned from the game's own wnd1 sections (every window of the game's layout archives decodes this
way; offsets are relative to the section start, i.e. including the 8-byte section header):
  0x08  pane fields (pan1), 0x4C bytes
  0x54  content inflation  s16 x4 (left, right, top, bottom; 0 in all of the game's windows)
  0x5C  frame sizes        u16 x4 (left, right, top, bottom) in layout units
  0x64  frame count u8 (1, 4 or 8), window flags u8, pad u16
  0x68  content offset u32 (0x70)       0x6C  frame table offset u32
  0x70  content: vertex colours rgba x4 (TL, TR, BL, BR), material index u16, texture coordinate set
        count u8, pad u8, then per set 4 UV pairs (TL, TR, BL, BR) as in pic1
  ....  frame table: u32 offset per frame (relative to the section start), then the frames:
        material index u16, flip u8, pad u8

Frames, in the order the game's material names give them: LT, RT, LB, RB (4), then L, R, T, B (8).
  1 frame : one corner material (LT), mirrored into the other corners. The edges are made by stretching
            the corner pieces, so it only looks clean when the frame size equals the image size
            (as in all of the game's 1-frame windows); checked in game runs.
  4 frames: one material per corner (edges stretched from the corners, same caveat).
  8 frames: corners plus one material per edge: the general 9-slice (e.g. one corner image with flips
            plus one edge image with rotations; frames may share materials). Checked in game runs.
Flip (u8, from the game's frame/material pairs, e.g. RT = LT turned 90 degrees): 0 none, 1 flip
horizontally, 2 flip vertically, 3 rotate 90, 4 rotate 180, 5 rotate 270.

Window flags as seen in the game (raw byte; meanings inferred from which windows use them):
  bit 0      set in every window of the game
  bit 1      set in some windows whose content has a texture coordinate set (meaning unknown)
  bits 2-3   frame kind: 0 = frame all around, 2 = horizontal (left/right frames only; the game's
             horizontal windows use frame top = bottom = the pane height)
  bit 4      set in frame-only windows (outlines, edge shadows, glow lines): the content is not drawn
Content colour (game runs): untextured content = the material's white colour x the vertex colours (the
black colour is not used). Material colours are applied as linear values while "^l" RGBA8 images are
sRGB, so a constant colour that should match an image colour needs the sRGB -> linear conversion.
Materials, as in the game: the content material (name ends in "C"; often untextured, its colour
fills the inside) and one material per frame (names ending in LT RT LB RB L R T B), textured with
the frame image whose size usually equals the frame size.

Description keys (besides pan1's):
  "frame_size": n or [left, right, top, bottom]
  "frames": [{"material": name} | {"texture": name, "wrap": [s, t], "black": rgba, "white": rgba},
             "flip": "none" | "flip_h" | "flip_v" | "rot90" | "rot180" | "rot270" | 0-5] (1, 4 or 8)
  "content": {"material": name} | {"texture": ..., "black": ..., "white": ...} | {} (white, untextured),
             plus "vertex_colors" [4 x rgba] (white), "uvs" (as pic1; default: one full set per texture),
             "inflation" [l, r, t, b] (0)
  "kind": "around" (default) | "horizontal";  "draw_content": true (false sets bit 4);
  "window_flags": raw byte (overrides kind / draw_content; default 0x01 + those bits)
Frames and content given by "texture" get a material of their own named <pane name><LT|RT|...|C>,
the game's naming; frames given by "material" use the description's material (and may be shared).
"""
import struct

from .. import binio
from . import mat1, pan1

FRAME_SUFFIXES = ("LT", "RT", "LB", "RB", "L", "R", "T", "B")
FLIPS = {"none": 0, "flip_h": 1, "flip_v": 2, "rot90": 3, "rot180": 4, "rot270": 5}
KINDS = {"around": 0, "horizontal": 2}
FLAG_BASE, FLAG_NO_CONTENT = 0x01, 0x10
FULL_UV = [[0, 0], [1, 0], [0, 1], [1, 1]]


def _sizes(v, what, lo, hi):
    v = [v] * 4 if isinstance(v, (int, float)) else list(v)
    if len(v) != 4 or any(not lo <= int(x) <= hi for x in v):
        raise ValueError("%s: 4 values in %d..%d expected, got %r" % (what, lo, hi, v))
    return [int(x) for x in v]


def _material(spec, auto_name, ctx):
    """(material index, texture count) of a frame / content spec."""
    if "material" in spec:
        name = spec["material"]
        return ctx.material(name), len(ctx.desc.materials.get(name, {}).get("textures", []))
    texs = [{"texture": spec["texture"], "wrap": spec.get("wrap", [4, 4])}] if "texture" in spec else []
    m = {"textures": texs, "black": spec.get("black", [0, 0, 0, 0]),
         "white": spec.get("white", [255, 255, 255, 255])}
    return ctx.add_material(auto_name, mat1.encode(auto_name, m, ctx)), len(texs)


def flags_of(p):
    if "window_flags" in p:
        return int(p["window_flags"]) & 0xFF
    kind = p.get("kind", "around")
    if kind not in KINDS:
        raise ValueError("window %r: unknown kind %r" % (p["name"], kind))
    return FLAG_BASE | (KINDS[kind] << 2) | (0 if p.get("draw_content", True) else FLAG_NO_CONTENT)


def write(p, ctx):
    name = p["name"]
    frames = p.get("frames")
    if not frames or len(frames) not in (1, 4, 8):
        raise ValueError("window %r: 1, 4 or 8 frames expected" % name)
    fsize = _sizes(p.get("frame_size", 0), "window %r frame_size" % name, 0, 0xFFFF)
    content = p.get("content", {})
    infl = _sizes(content.get("inflation", 0), "window %r inflation" % name, -0x8000, 0x7FFF)

    # materials in the game's order: content first, then the frames
    cmat, ctex = _material(content, name + "C", ctx)
    fmats = []
    for i, f in enumerate(frames):
        flip = f.get("flip", "none")
        flip = FLIPS[flip] if isinstance(flip, str) else int(flip)
        if not 0 <= flip <= 5:
            raise ValueError("window %r: flip %r" % (name, flip))
        fmats.append((_material(f, name + FRAME_SUFFIXES[i], ctx)[0], flip))

    uvs = content.get("uvs", [FULL_UV] * ctex)
    vc = content.get("vertex_colors", [[255, 255, 255, 255]] * 4)
    pane = pan1.body(p)
    content_off = 8 + len(pane) + 0x1C
    content_blob = (b"".join(binio.rgba(c) for c in vc) + struct.pack(">HBB", cmat, len(uvs), 0) +
                    b"".join(binio.f32(u) + binio.f32(v) for s in uvs for u, v in s))
    table_off = content_off + len(content_blob)
    first = table_off + 4 * len(fmats)
    table = b"".join(struct.pack(">I", first + 4 * i) for i in range(len(fmats)))
    frame_blob = b"".join(struct.pack(">HBB", m, flip, 0) for m, flip in fmats)
    head = (struct.pack(">4h", *infl) + struct.pack(">4H", *fsize) +
            struct.pack(">BBH", len(fmats), flags_of(p), 0) + struct.pack(">II", content_off, table_off))
    return pane + head + content_blob + table + frame_blob


def parse(section):
    """Decode a whole wnd1 section (8-byte header included) into a dict: for tests and for comparing our
    windows with the game's own."""
    s = section
    n, flags, _ = struct.unpack(">BBH", s[0x64:0x68])
    co, fo = struct.unpack(">II", s[0x68:0x70])
    cmat, ntc, _ = struct.unpack(">HBB", s[co + 16:co + 20])
    frames = []
    for i in range(n):
        x, = struct.unpack(">I", s[fo + 4 * i:fo + 4 * i + 4])
        m, flip, _ = struct.unpack(">HBB", s[x:x + 4])
        frames.append((m, flip))
    return {
        "name": s[12:36].split(b"\0")[0].decode(),
        "size": list(struct.unpack(">2f", s[0x4C:0x54])),
        "inflation": list(struct.unpack(">4h", s[0x54:0x5C])),
        "frame_size": list(struct.unpack(">4H", s[0x5C:0x64])),
        "flags": flags, "content_offset": co, "frame_table_offset": fo,
        "vertex_colors": [list(s[co + 4 * i:co + 4 * i + 4]) for i in range(4)],
        "content_material": cmat,
        "uvs": [[list(struct.unpack(">2f", s[co + 20 + 32 * k + 8 * j:co + 28 + 32 * k + 8 * j]))
                 for j in range(4)] for k in range(ntc)],
        "frames": frames,
    }


def materials(p):
    """Names of the description materials this window uses (frames and content given by "material"):
    for the animation check that a material track's pane is in the animation's groups."""
    specs = list(p.get("frames", [])) + [p.get("content", {})]
    return sorted({s["material"] for s in specs if "material" in s})
