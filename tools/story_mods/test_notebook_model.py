"""Tests of notebook_model.py that need no game files: the GLB and PNG readers, the archive writers
(read back with the readers the port's tools use), and the model build on a synthetic template
shaped like the stand-in's BFRES (made up here; no game data)."""
import os
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import notebook_model as m  # noqa: E402
import shaderprep  # noqa: E402


def png(w, h, rgba):
    rows = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    chunk = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def template():
    """A tiny BFRES 3.4.0.1 with the stand-in's shape: one model 'vbeso' (one vertex buffer _p0/_n0/_u0,
    stride 48; one shape with one u32 triangle mesh and a bounding box) and the colour texture."""
    b = bytearray(0x2000)
    put = lambda fmt, o, *v: struct.pack_into(">" + fmt, b, o, *v)
    rel = lambda o, t: put("i", o, t - o)
    strings = {}
    def string(s, at=[0x1800]):
        if s not in strings:
            strings[s] = at[0]
            b[at[0]:at[0] + len(s)] = s.encode()
            at[0] += len(s) + 1
        return strings[s]
    def dictionary(o, entries):
        put("Ii", o, 8 + 16 * (len(entries) + 1), len(entries))
        for i, (name, target) in enumerate(entries, 1):
            e = o + 8 + 16 * i
            rel(e + 8, string(name))
            rel(e + 12, target)
    b[0:4] = b"FRES"
    put("I", 4, 0x03040001)
    fm, fvtx, attrs, vbuf, shape, mesh, groups, ibuf, box, tex = 0x200, 0x300, 0x340, 0x380, 0x400, 0x480, 0x4A0, 0x4B0, 0x4D0, 0x600
    dictionary(0x100, [("vbeso", fm)])
    rel(0x20, 0x100)
    dictionary(0x180, [(m.TEXTURE, tex)])
    rel(0x24, 0x180)
    b[fm:fm + 4] = b"FMDL"
    rel(fm + 0x10, fvtx)
    dictionary(0x500, [("Polygon0", shape)])
    rel(fm + 0x14, 0x500)
    put("HH", fm + 0x20, 1, 1)
    put("I", fm + 0x28, 3)
    b[fvtx:fvtx + 4] = b"FVTX"
    b[fvtx + 4], b[fvtx + 5] = 3, 1
    put("I", fvtx + 8, 3)
    rel(fvtx + 0x10, attrs)
    rel(fvtx + 0x18, vbuf)
    for i, (name, off, fmt) in enumerate([("_p0", 0, 0x811), ("_n0", 16, 0x811), ("_u0", 32, 0x80D)]):
        rel(attrs + 12 * i, string(name))
        b[attrs + 12 * i + 4] = 0
        put("HI", attrs + 12 * i + 6, off, fmt)
    put("IIIHH", vbuf, 0, 144, 0, 48, 1)
    b[shape:shape + 4] = b"FSHP"
    b[shape + 0x17] = 1
    put("f", shape + 0x1C, 10.0)
    rel(shape + 0x24, mesh)
    rel(shape + 0x34, box)
    put("III", mesh, 4, 9, 3)
    put("H", mesh + 0xC, 1)
    rel(mesh + 0x10, groups)
    rel(mesh + 0x14, ibuf)
    put("II", groups, 0, 3)
    put("IIIHH", ibuf, 0, 12, 0, 0, 1)
    put("6f", box, 1.0, 2.0, 3.0, 10.0, 20.0, 1.0)  # the deed's box: center, extent
    b[tex:tex + 4] = b"FTEX"
    put("IIIIII", tex + 4, 1, 64, 64, 1, 7, 0x431)
    put("I", tex + 0x7C, 7)
    return bytes(b)


def glb(png_bytes):
    pos = [(-1.0, -2.0, -0.5), (1.0, 2.0, 0.5), (0.0, 0.0, 0.0), (1.0, -2.0, 0.0)]
    nrm = [(0.0, 0.0, 1.0)] * 4
    uv = [(0.0, 0.0), (1.0, 1.0), (0.5, 0.5), (1.0, 0.0)]
    idx = [0, 1, 2, 0, 3, 1]
    parts = [struct.pack("<12f", *sum(pos, ())), struct.pack("<12f", *sum(nrm, ())), struct.pack("<8f", *sum(uv, ())),
             struct.pack("<6H", *idx), png_bytes]
    views, binary = [], b""
    for p in parts:
        binary += b"\0" * (-len(binary) % 4)
        views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(p)})
        binary += p
    import json
    doc = {"buffers": [{"byteLength": len(binary)}], "bufferViews": views,
           "accessors": [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"},
                         {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
                         {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
                         {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"}],
           "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3}]}],
           "images": [{"bufferView": 4, "mimeType": "image/png"}]}
    j = json.dumps(doc).encode()
    j += b" " * (-len(j) % 4)
    binary += b"\0" * (-len(binary) % 4)
    body = struct.pack("<I4s", len(j), b"JSON") + j + struct.pack("<I4s", len(binary), b"BIN\0") + binary
    return b"glTF" + struct.pack("<II", 2, 12 + len(body)) + body


def write(path, data):
    with open(path, "wb") as fh:
        fh.write(data)


class NotebookModelTest(unittest.TestCase):
    def setUp(self):
        self.rgba = bytes((x * 4 + y * 3 + c) & 255 for y in range(8) for x in range(64) for c in range(4))
        self.png = png(64, 8, self.rgba)

    def test_png(self):
        self.assertEqual(m.decode_png(self.png), (64, 8, self.rgba))
        with self.assertRaises(m.BuildError):
            m.decode_png(b"not a png")

    def test_glb(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "n.glb")
            write(path, glb(self.png))
            pos, nrm, uv, idx, image = m.read_glb(path)
        self.assertEqual(len(pos), 4)
        self.assertEqual(idx, [0, 1, 2, 0, 3, 1])
        self.assertEqual(image, self.png)

    def test_archive_round_trip(self):
        files = {"Vbeso.bfres": b"FRES" + bytes(range(200)), "vbeso.sharcfb": b"BAHS" * 9}
        packed = m.yaz0_stored(m.sarc(files))
        self.assertEqual(packed[:4], b"Yaz0")
        self.assertEqual(dict(shaderprep.sarc(shaderprep.yaz0(packed))), files)

    def test_model(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "n.glb")
            write(path, glb(self.png))
            out, s = m.build_bfres(template(), m.read_glb(path))
        f = m.Bfres(out)
        self.assertEqual(f.u32(0xC), len(out))
        self.assertEqual(len(out) % 0x1000, 0)
        self.assertAlmostEqual(s, 10.0)  # the deed's height (2 x 20) over the notebook's (4)
        fm = f.dict(f.rel(0x20))[0][1]
        fvtx = f.rel(fm + 0x10)
        self.assertEqual((f.u32(fvtx + 8), f.u32(fm + 0x28)), (4, 4))
        vbuf = f.rel(fvtx + 0x18)
        data = f.rel(vbuf + 0x14)
        self.assertEqual(f.u32(vbuf + 4), 4 * 48)
        self.assertEqual(data % 0x1000, 0)
        # vertex 1: (1, 2, 0.5) around the notebook's center (0, 0, 0), scaled 10, placed at the deed's center
        self.assertEqual(struct.unpack_from(">3f", out, data + 48), (11.0, 22.0, 8.0))
        self.assertEqual(struct.unpack_from(">2f", out, data + 48 + 32), (1.0, 1.0))
        shape = f.dict(f.rel(fm + 0x14))[0][1]
        mesh = f.rel(shape + 0x24)
        self.assertEqual(f.u32(mesh + 8), 6)
        ibuf = f.rel(mesh + 0x14)
        self.assertEqual(struct.unpack_from(">6I", out, f.rel(ibuf + 0x14)), (0, 1, 2, 0, 3, 1))
        self.assertEqual(struct.unpack_from(">2I", out, f.rel(mesh + 0x10)), (0, 6))
        box = f.rel(shape + 0x34)
        self.assertEqual(struct.unpack_from(">6f", out, box), (1.0, 2.0, 3.0, 10.0, 20.0, 5.0))
        tex = dict(f.dict(f.rel(0x24)))[m.TEXTURE]
        surf = tex + 4
        self.assertEqual(struct.unpack_from(">IIIII", out, surf + 4), (64, 8, 1, 1, m.FMT_RGBA8_SRGB))
        self.assertEqual(f.u32(surf + 0x30), m.TILE_LINEAR_ALIGNED)
        self.assertEqual(f.u32(tex + 0x7C), 1)
        texels = f.rel(tex + 0xB0)
        self.assertEqual(out[texels:texels + len(self.rgba)], self.rgba)

    def test_unexpected_template(self):
        bad = bytearray(template())
        bad[4:8] = struct.pack(">I", 0x03050001)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "n.glb")
            write(path, glb(self.png))
            with self.assertRaises(m.BuildError):
                m.build_bfres(bytes(bad), m.read_glb(path))
            with self.assertRaises(m.BuildError):
                m.build(d, os.path.join(d, "out.szs"), path)  # no game archive there


if __name__ == "__main__":
    unittest.main()
