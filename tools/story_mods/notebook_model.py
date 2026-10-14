#!/usr/bin/env python3
"""Build the story notebook's 3D model for the get-item scene, on the player's machine.

The notebook is handed out with the get-item demo of a stand-in game item (the Cabana Deed, item 0x9C,
archive Object/Vbeso.szs). This tool builds a replacement for that archive that shows the notebook:

  - the mesh and texture come from the Story Notebook package (its manifest's "notebook" block names the
    .glb; the port runs this tool when the package is loaded and keeps the result in the package's data
    folder);
  - everything that defines how the game draws an item model (the model's material and its shader
    assignment, parameters and samplers, the skeleton, the shader archive vbeso.sharcfb) comes from the
    player's own copy of the game, read at build time. The output is written next to the player's other
    generated files and is never part of the repository or a release.

How: Vbeso.bfres (Wii U BFRES 3.4.0.1) is used as a template. Its one vertex buffer (float3 position,
float3 normal, float2 texture coordinate, stride 48), its one mesh (u32 triangle list) and its colour
texture are replaced by the notebook's: the new data is appended (aligned) and the offsets, counts and
bounds are updated; the texture becomes an uncompressed sRGB RGBA8 surface in linear-aligned tiling with
one mip level (the game initialises the texture registers itself when it sets the file up). The archive
is written as a Yaz0 SARC with the same file names, so the game's resource table for "Vbeso" finds the
model ("vbeso") and the shader archive as before.

usage: notebook_model.py --game GAME_DIR --glb notebook_game.glb --out FILE.szs [--scale S]
"""
import argparse
import json
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools"))
import shaderprep  # noqa: E402  (Yaz0 and SARC readers)

ARCHIVE = "content/Common/Object/Vbeso.szs"
TEXTURE = "koni_BessoKenrosyo_128x128"  # the template's colour texture (its toon ramp ZAtoon is kept)
FMT_RGBA8_SRGB, TILE_LINEAR_ALIGNED = 0x41A, 1


class BuildError(Exception):
    pass


# ---- input: the notebook (glTF binary) ----

def read_glb(path):
    with open(path, "rb") as fh:
        b = fh.read()
    if b[:4] != b"glTF":
        raise BuildError("not a GLB file: " + path)
    jlen = struct.unpack_from("<I", b, 12)[0]
    doc = json.loads(b[20:20 + jlen])
    blen = struct.unpack_from("<I", b, 20 + jlen)[0]
    binary = b[28 + jlen:28 + jlen + blen]

    def view(i):
        v = doc["bufferViews"][i]
        o = v.get("byteOffset", 0)
        return binary[o:o + v["byteLength"]]

    def accessor(i):
        a = doc["accessors"][i]
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[a["type"]]
        fmt = {5126: "f", 5123: "H", 5125: "I"}[a["componentType"]]
        data = view(a["bufferView"])[a.get("byteOffset", 0):]
        flat = struct.unpack_from("<%d%s" % (a["count"] * n, fmt), data)
        return [flat[k * n:(k + 1) * n] for k in range(a["count"])] if n > 1 else list(flat)

    prims = [p for m in doc["meshes"] for p in m["primitives"]]
    if len(prims) != 1 or prims[0].get("mode", 4) != 4:
        raise BuildError("the GLB must hold one triangle mesh")
    att = prims[0]["attributes"]
    pos, nrm, uv = accessor(att["POSITION"]), accessor(att["NORMAL"]), accessor(att["TEXCOORD_0"])
    idx = accessor(prims[0]["indices"])
    image = view(doc["images"][0]["bufferView"])
    return pos, nrm, uv, idx, image


def decode_png(data):
    """8-bit RGB/RGBA, non-interlaced PNG -> (width, height, RGBA bytes)."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise BuildError("the texture is not a PNG")
    o, idat, w = 8, b"", None
    while o < len(data):
        n, kind = struct.unpack_from(">I4s", data, o)
        body = data[o + 8:o + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise BuildError("the texture must be an 8-bit RGB or RGBA PNG")
        elif kind == b"IDAT":
            idat += body
        o += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw, stride = zlib.decompress(idat), w * bpp
    out, prev = bytearray(), bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + prev[i]) & 255
            elif f == 3: line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
            elif f == 4:
                p = a + prev[i] - c
                pa, pb, pc = abs(p - a), abs(p - prev[i]), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else prev[i] if pb <= pc else c)) & 255
        out += line if bpp == 4 else b"".join(bytes(line[i:i + 3]) + b"\xff" for i in range(0, stride, 3))
        prev = line
    return w, h, bytes(out)


# ---- the template (the player's own Vbeso.bfres) ----

class Bfres:
    def __init__(self, data):
        self.b = bytearray(data)
        if self.b[:4] != b"FRES" or self.u32(4) != 0x03040001:
            raise BuildError("Vbeso.bfres is not the expected BFRES 3.4.0.1")

    def u32(self, o): return struct.unpack_from(">I", self.b, o)[0]
    def s32(self, o): return struct.unpack_from(">i", self.b, o)[0]
    def u16(self, o): return struct.unpack_from(">H", self.b, o)[0]
    def put(self, fmt, o, *v): struct.pack_into(">" + fmt, self.b, o, *v)
    def rel(self, o):
        v = self.s32(o)
        return o + v if v else 0
    def set_rel(self, o, target): self.put("i", o, target - o)
    def cstr(self, o): return self.b[o:self.b.index(b"\0", o)].decode()

    def dict(self, o):
        out = []
        for i in range(1, self.s32(o + 4) + 1):
            e = o + 8 + 16 * i
            out.append((self.cstr(self.rel(e + 8)), self.rel(e + 12)))
        return out

    def append(self, data, align):
        while len(self.b) % align:
            self.b.append(0)
        at = len(self.b)
        self.b += data
        return at


def build_bfres(template, glb, scale=None):
    pos, nrm, uv, idx, png = glb
    f = Bfres(template)
    models = f.dict(f.rel(0x20))
    if len(models) != 1:
        raise BuildError("unexpected model count in Vbeso.bfres")
    fm = models[0][1]
    if f.u16(fm + 0x20) != 1 or f.u16(fm + 0x22) != 1:
        raise BuildError("unexpected vertex buffer or shape count in Vbeso.bfres")
    fvtx = f.rel(fm + 0x10)
    attrs = f.rel(fvtx + 0x10)
    layout = [(f.cstr(f.rel(attrs + 12 * a)), f.b[attrs + 12 * a + 4], f.u16(attrs + 12 * a + 6), f.u32(attrs + 12 * a + 8))
              for a in range(f.b[fvtx + 4])]
    if layout != [("_p0", 0, 0, 0x811), ("_n0", 0, 16, 0x811), ("_u0", 0, 32, 0x80D)] or f.b[fvtx + 5] != 1:
        raise BuildError("unexpected vertex layout in Vbeso.bfres: %r" % (layout,))
    vbuf = f.rel(fvtx + 0x18)
    if f.u16(vbuf + 0xC) != 48:
        raise BuildError("unexpected vertex stride in Vbeso.bfres")
    shape = f.dict(f.rel(fm + 0x14))[0][1]
    mesh = f.rel(shape + 0x24)
    if f.u32(mesh) != 4 or f.u32(mesh + 4) != 9 or f.b[shape + 0x17] != 1:
        raise BuildError("unexpected mesh in Vbeso.bfres (one u32 triangle list expected)")
    box = f.rel(shape + 0x34)
    center = struct.unpack_from(">3f", f.b, box)
    extent = struct.unpack_from(">3f", f.b, box + 12)

    # place the notebook where the deed is: same center, scaled to the deed's height
    lo = [min(p[k] for p in pos) for k in range(3)]
    hi = [max(p[k] for p in pos) for k in range(3)]
    mid = [(lo[k] + hi[k]) / 2 for k in range(3)]
    s = scale or (2 * extent[1]) / (hi[1] - lo[1])
    placed = [tuple(center[k] + (p[k] - mid[k]) * s for k in range(3)) for p in pos]
    vdata = bytearray()
    for p, n, t in zip(placed, nrm, uv):
        vdata += struct.pack(">3f4x3f4x2f8x", *p, *n, *t)
    idata = struct.pack(">%dI" % len(idx), *idx)

    # vertex buffer, index buffer
    at = f.append(bytes(vdata), 0x1000)
    f.put("I", vbuf + 4, len(vdata))
    f.set_rel(vbuf + 0x14, at)
    f.put("I", fvtx + 8, len(pos))
    f.put("I", fm + 0x28, len(pos))
    ibuf = f.rel(mesh + 0x14)
    at = f.append(idata, 0x1000)
    f.put("I", ibuf + 4, len(idata))
    f.set_rel(ibuf + 0x14, at)
    f.put("I", mesh + 8, len(idx))
    groups = f.rel(mesh + 0x10)
    f.put("H", mesh + 0xC, 1)
    f.put("II", groups, 0, len(idx))
    f.put("I", mesh + 0x18, 0)
    # bounds: the box and the radius (the game culls with them)
    newext = [(hi[k] - lo[k]) / 2 * s for k in range(3)]
    f.put("3f", box, *center)
    f.put("3f", box + 12, *newext)
    f.put("f", shape + 0x1C, (newext[0] ** 2 + newext[1] ** 2 + newext[2] ** 2) ** 0.5)

    # the colour texture
    textures = dict(f.dict(f.rel(0x24)))
    if TEXTURE not in textures:
        raise BuildError("Vbeso.bfres has no texture " + TEXTURE)
    t = textures[TEXTURE]
    w, h, rgba = decode_png(png)
    if w % 64 or h % 8:
        raise BuildError("the texture size must be a multiple of 64 x 8")
    at = f.append(rgba, 0x2000)
    surf = t + 4
    f.put("I", surf + 0x04, w)
    f.put("I", surf + 0x08, h)
    f.put("I", surf + 0x10, 1)                 # mip levels
    f.put("I", surf + 0x14, FMT_RGBA8_SRGB)
    f.put("I", surf + 0x20, len(rgba))         # image size
    f.put("I", surf + 0x28, 0)                 # mip size
    f.put("I", surf + 0x30, TILE_LINEAR_ALIGNED)
    f.put("I", surf + 0x34, 0)                 # swizzle
    f.put("I", surf + 0x38, 0x100)             # alignment
    f.put("I", surf + 0x3C, w)                 # pitch (texels)
    for k in range(13):
        f.put("I", surf + 0x40 + 4 * k, 0)     # mip offsets
    f.put("I", t + 0x7C, 1)                    # view: mip levels
    for k in range(5):
        f.put("I", t + 0x8C + 4 * k, 0)        # registers: the game initialises them
    f.set_rel(t + 0xB0, at)
    f.put("i", t + 0xB4, 0)                    # no mip data

    while len(f.b) % 0x1000:
        f.b.append(0)
    f.put("I", 0xC, len(f.b))
    return bytes(f.b), s


# ---- output: Yaz0 SARC ----

def sarc(files, align=0x2000):
    def name_hash(name):
        h = 0
        for c in name.encode():
            h = (h * 0x65 + c) & 0xFFFFFFFF
        return h
    entries = sorted(files.items(), key=lambda kv: name_hash(kv[0]))
    names, offs = b"", []
    for name, _ in entries:
        offs.append(len(names))
        names += name.encode() + b"\0"
        names += b"\0" * (-len(names) % 4)
    sfat = struct.pack(">4sHHI", b"SFAT", 0xC, len(entries), 0x65)
    data, spans = b"", []
    for name, body in entries:
        data += b"\0" * (-len(data) % align)
        spans.append((len(data), len(data) + len(body)))
        data += body
    for (name, _), noff, (s, e) in zip(entries, offs, spans):
        sfat += struct.pack(">IIII", name_hash(name), 0x01000000 | (noff // 4), s, e)
    sfnt = struct.pack(">4sHH", b"SFNT", 8, 0) + names
    head_len = 0x14 + len(sfat) + len(sfnt)
    doff = (head_len + align - 1) // align * align
    total = doff + len(data)
    head = struct.pack(">4sHHIIHH", b"SARC", 0x14, 0xFEFF, total, doff, 0x0100, 0)
    return head + sfat + sfnt + b"\0" * (doff - head_len) + data


def yaz0_stored(data):
    """Yaz0 without compression (every group of eight bytes stored as literals): valid for any reader."""
    out = bytearray(b"Yaz0" + struct.pack(">I", len(data)) + b"\0" * 8)
    for i in range(0, len(data), 8):
        chunk = data[i:i + 8]
        out.append(0xFF)
        out += chunk
    return bytes(out)


def build(game, out, glb_path, scale=None):
    src = os.path.join(game, ARCHIVE)
    if not os.path.isfile(src):
        raise BuildError("game archive not found: " + src)
    with open(src, "rb") as fh:
        files = dict(shaderprep.sarc(shaderprep.yaz0(fh.read())))
    if set(files) != {"Vbeso.bfres", "vbeso.sharcfb"}:
        raise BuildError("unexpected contents of Vbeso.szs: %s" % sorted(files))
    bfres, s = build_bfres(files["Vbeso.bfres"], read_glb(glb_path), scale)
    archive = yaz0_stored(sarc({"Vbeso.bfres": bfres, "vbeso.sharcfb": files["vbeso.sharcfb"]}))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(archive)
    os.replace(tmp, out)
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--game", required=True, help="the game folder (with content/)")
    ap.add_argument("--out", required=True, help="the archive to write (Vbeso.szs for the notebook)")
    ap.add_argument("--glb", required=True, help="the notebook's model (the Story Notebook package's)")
    ap.add_argument("--scale", type=float, help="model units per GLB unit (default: the deed's height)")
    a = ap.parse_args()
    try:
        s = build(a.game, a.out, a.glb, a.scale)
    except BuildError as e:
        sys.exit("notebook_model: " + str(e))
    print("notebook_model: wrote %s (scale %.3f)" % (a.out, s))


if __name__ == "__main__":
    main()
