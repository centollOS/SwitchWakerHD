#!/usr/bin/env python3
"""Unit tests for the story layout writers (tools/story_mods/lyt)."""
import hashlib
import json
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lyt  # noqa: E402
from lyt import bflan, bflim, bflyt, binio  # noqa: E402
from lyt.anim import tracks  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLE = os.path.join(HERE, "lyt", "examples", "plate.json")
sha = lambda b: hashlib.sha256(b).hexdigest()


def load_json(path):
    with open(path) as f:
        return json.load(f)


def example(**changes):
    d = load_json(EXAMPLE)
    d.update(changes)
    return lyt.Description(d, os.path.dirname(EXAMPLE))


class PrototypeEquivalence(unittest.TestCase):
    """The package reproduces the prototype's files byte for byte (the prototype ran in the game)."""

    def test_bflyt(self):
        data, ctx = bflyt.build(example())
        self.assertEqual(sha(data), "624c0c536cd8cec256c0b9ae70b6d6f7c9cb918947150659182648eeca37496d")
        self.assertEqual(ctx.textures, ["StoryPlate_00^l.bflim"])

    def test_bflan(self):
        d = example()
        data = bflan.build("InAll", d.animations["InAll"], d)
        self.assertEqual(sha(data), "c2e35a89a86be2edc124bab7883e882ee2657214ba61d5a5cef55aabe6a8f63c")

    def test_bflim(self):
        data = bflim.encode(bytes(range(256)) * 2, 64, 2)
        self.assertEqual(sha(data), "124fe89ae4fffd22650e84241b26bfffd98f658b984b6a346cb3ab11e0470804")
        self.assertEqual(bflim.parse_footer(data), (64, 2, 0x100, 0x14, 0x01, 512))


class Structure(unittest.TestCase):
    def test_section_order(self):
        data, _ = bflyt.build(example())
        order = [m for m, _ in binio.read_sections(data)]
        self.assertEqual(order, [b"lyt1", b"txl1", b"mat1", b"pan1", b"pas1", b"pan1", b"pas1", b"pic1",
                                 b"pae1", b"pae1", b"grp1", b"grs1", b"grp1", b"gre1"])

    def test_materials_shared_and_ordered(self):
        d = load_json(EXAMPLE)
        d["textures"]["Other_00"] = {"png": "x.png"}
        d["materials"]["Other_00"] = {"textures": [{"texture": "Other_00"}]}
        kids = d["panes"][0]["children"]
        kids.append({"type": "picture", "name": "P_B", "size": [4, 4], "material": "Other_00"})
        kids.append({"type": "picture", "name": "P_C", "size": [4, 4], "material": "StoryPlate_00"})
        data, ctx = bflyt.build(lyt.Description(d))
        self.assertEqual([n for n, _ in ctx.materials], ["StoryPlate_00", "Other_00"])
        self.assertEqual(ctx.textures, ["StoryPlate_00^l.bflim", "Other_00^l.bflim"])
        secs = dict((m, b) for m, b in binio.read_sections(data) if m in (b"txl1", b"mat1"))
        n, = struct.unpack(">H", secs[b"txl1"][:2])
        o0, o1 = struct.unpack(">II", secs[b"txl1"][4:12])
        self.assertEqual((n, o0, o1), (2, 8, 8 + len("StoryPlate_00^l.bflim") + 1))
        mat = secs[b"mat1"]
        offs = struct.unpack(">II", mat[4:12])
        self.assertEqual(offs[0], 0x14)  # relative to the section start (8-byte header + count + 2 offsets)
        second = mat[offs[1] - 8:]
        self.assertEqual(second[:8], b"Other_00")
        self.assertEqual(struct.unpack(">IHBB", second[36:44]), (0x15, 1, 4, 4))

    def test_hooks_not_written_yet(self):
        for kind in ():  # every pane writer is implemented (window: part B, text: part C)
            d = load_json(EXAMPLE)
            d["panes"].append({"type": kind, "name": "X_00", "size": [1, 1]})
            with self.assertRaises(NotImplementedError):
                bflyt.build(lyt.Description(d))

    def test_validation(self):
        with self.assertRaises(ValueError):
            example(groups={"G": ["Missing"]})
        with self.assertRaises(ValueError):
            example(panes=[{"type": "sprite", "name": "A", "size": [1, 1]}])
        with self.assertRaises(ValueError):
            example(panes=[{"name": "A", "size": [1, 1]}, {"name": "A", "size": [1, 1]}], groups={},
                    animations={})
        with self.assertRaises(ValueError):
            binio.name("x" * 24, 24)
        with self.assertRaises(ValueError):  # a track for a pane outside the animation's groups
            d = load_json(EXAMPLE)
            d["animations"]["InAll"]["tracks"].append({"pane": "P_Plate_00", "target": "sx", "keys": [[0, 1]]})
            lyt.Description(d)

    def test_schema(self):
        try:
            import jsonschema
        except ImportError:
            self.skipTest("jsonschema not installed")
        schema = load_json(os.path.join(HERE, "lyt", "schema.json"))
        jsonschema.validate(load_json(EXAMPLE), schema)


class Colours(unittest.TestCase):
    def test_part_size(self):
        data, _ = bflyt.build(example(part_size=[240, 140]))
        lyt1 = binio.read_sections(data)[0][1]
        self.assertEqual(struct.unpack(">ff", lyt1[12:20]), (240, 140))

    def test_srgb_materials(self):
        d = load_json(EXAMPLE)
        d["materials"]["StoryPlate_00"]["white"] = [128, 255, 0, 200]
        d["animations"]["InAll"]["tracks"].append(
            {"material": "StoryPlate_00", "target": "white.r", "keys": [[0, 128], [10, 255]]})
        d["groups"]["G_All_00"].append("P_Plate_00")
        for space, white, key in (("srgb", bytes([55, 255, 0, 200]), 55.0), ("linear", bytes([128, 255, 0, 200]), 128.0)):
            d["color_space"] = space
            desc = lyt.Description(d)
            _, ctx = bflyt.build(desc)
            self.assertEqual(ctx.materials[0][1][32:36], white, space)
            anim = bflan.build("InAll", desc.animations["InAll"], desc)
            self.assertIn(b"FLMC", anim)
            i = anim.index(b"FLMC")
            e = i + struct.unpack(">I", anim[i + 8:i + 12])[0]
            self.assertAlmostEqual(struct.unpack(">f", anim[e + 16:e + 20])[0], key, 0)


class MaterialUsers(unittest.TestCase):
    def test_window_frame_material_track(self):
        """An animated frame material of an 8-frame window in the groups is accepted (lyt-window)."""
        from lyt.sections import wnd1
        if not hasattr(wnd1, "materials"):
            self.skipTest("window panes not merged yet")
        d = load_json(EXAMPLE)
        names = ["W_Btn_00" + s for s in ("LT", "RT", "LB", "RB", "L", "R", "T", "B")]
        for n in names:
            d["materials"][n] = {"textures": [{"texture": "StoryPlate_00"}]}
        d["panes"].append({"type": "window", "name": "W_Btn_00", "size": [200, 80], "frame_size": 16,
                           "frames": [{"material": n} for n in names]})
        d["groups"]["G_Btn_00"] = ["W_Btn_00"]
        track = {"material": "W_Btn_00LT", "target": "white.r", "keys": [[0, 255], [4, 128]]}
        d["animations"]["Select"] = {"frames": 4, "groups": ["G_Btn_00"], "tracks": [track]}
        lyt.Description(d)  # accepted
        d["animations"]["Select"]["groups"] = ["G_All_00"]
        with self.assertRaises(ValueError):
            lyt.Description(d)

    def test_registered_users(self):
        from lyt import panes
        d = load_json(EXAMPLE)
        d["materials"]["M_X"] = {"textures": [{"texture": "StoryPlate_00"}]}
        d["panes"][0]["children"][0]["material"] = "M_X"
        d["groups"]["G_All_00"].append("P_Plate_00")
        d["animations"]["InAll"]["tracks"].append({"material": "M_X", "target": "white.r", "keys": [[0, 1]]})
        lyt.Description(d)
        self.assertEqual(panes.materials_of({"type": "null", "name": "N"}), [])


class TextPane(unittest.TestCase):
    """txt1 / fnl1 / usd1 writers (layout checked against the game's own sections locally; no game data here)."""

    def build(self, *panes, **top):
        d = load_json(EXAMPLE)
        d["panes"][0]["children"] += list(panes)
        d.update(top)
        return bflyt.build(lyt.Description(d))

    def test_fields(self):
        from lyt.sections import txt1
        data, ctx = self.build({"type": "text", "name": "T_Line_00", "size": [600, 60], "text": "Hi\nyou",
                                "capacity": 40, "font_size": [30, 36], "line_space": 4, "text_position": ["left", "top"],
                                "line_alignment": "center", "colors": [[1, 2, 3, 4], [5, 6, 7, 8]], "text_id": "L_00",
                                "shadow": {"offset": [2, -2], "colors": [[0, 0, 0, 255], [9, 9, 9, 255]],
                                           "white": [255, 255, 255, 100]}})
        secs = binio.read_sections(data)
        body = [b for m, b in secs if m == b"txt1"][0]
        sec = b"txt1" + struct.pack(">I", 8 + len(body)) + body
        self.assertEqual(sec[0xC:0x16], b"T_Line_00\0")
        buf, tlen, mat, font, pos, la, fl, pad = struct.unpack(">HHHHBBBB", sec[0x54:0x60])
        self.assertEqual((buf, tlen, mat, font, pos, la, fl, pad), (82, 14, 1, 0, 5, 2, 1, 0))
        self.assertEqual(struct.unpack(">I", sec[0x64:0x68])[0], 0xA0)
        self.assertEqual(sec[0x68:0x70], bytes(range(1, 9)))
        self.assertEqual(struct.unpack(">4f", sec[0x70:0x80]), (30, 36, 0, 4))
        self.assertEqual(sec[0xA0:0xAE].decode("utf-16-be"), "Hi\nyou\0")
        tid = struct.unpack(">I", sec[0x80:0x84])[0]
        self.assertEqual(sec[tid:tid + 5], b"L_00\0")
        self.assertEqual(struct.unpack(">4f", sec[0x84:0x94]), (2, -2, 1, 1))
        self.assertEqual(sec[0x94:0x9C], bytes([0, 0, 0, 255, 9, 9, 9, 255]))
        self.assertEqual(len(sec) % 4, 0)
        self.assertEqual(ctx.fonts, [txt1.DEFAULT_FONT])
        self.assertEqual(ctx.materials[1][0], "T_Line_00")
        # the text pane's own material: black alpha 0, white, the shadow-colour block
        self.assertEqual(ctx.materials[1][1][28:], bytes([0, 0, 0, 0, 255, 255, 255, 255]) +
                         struct.pack(">I", 0x20000) + bytes([0, 0, 0, 255, 255, 255, 100, 0]))

    def test_defaults_and_utf16(self):
        data, ctx = self.build({"type": "text", "name": "T_A", "size": [10, 10], "text": "\u00e9\U0001F600"})
        body = [b for m, b in binio.read_sections(data) if m == b"txt1"][0]
        buf, tlen = struct.unpack(">HH", body[0x4C:0x50])
        self.assertEqual((buf, tlen), (66, 8))  # capacity 32 characters; 3 UTF-16 units + NUL
        self.assertEqual(body[0x98:0xA0], bytes([0, 0xE9, 0xD8, 0x3D, 0xDE, 0x00, 0, 0]))
        self.assertEqual(body[0x56], 0)  # no shadow
        self.assertEqual(ctx.materials[1][1][28:], bytes([0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0]))

    def test_fnl1_two_fonts(self):
        data, ctx = self.build({"type": "text", "name": "T_A", "size": [10, 10], "font": "CKingMain.bffnt"},
                               {"type": "text", "name": "T_B", "size": [10, 10]})
        fnl = [b for m, b in binio.read_sections(data) if m == b"fnl1"][0]
        self.assertEqual(fnl, struct.pack(">HHII", 2, 0, 8, 0x18) + b"CKingMain.bffnt\0CKingMsg.bffnt\0\0")
        order = [m for m, _ in binio.read_sections(data)]
        self.assertEqual(order[:4], [b"lyt1", b"txl1", b"fnl1", b"mat1"])

    def test_usd1_scalable(self):
        data, _ = self.build({"type": "text", "name": "T_A", "size": [10, 10], "scalable": 80,
                              "user_data": [{"name": "Fix", "type": "int", "value": 0}]})
        secs = binio.read_sections(data)
        i = [m for m, _ in secs].index(b"txt1")
        self.assertEqual(secs[i + 1][0], b"usd1")
        self.assertEqual(secs[i + 1][1], bytes.fromhex("000200000000002000000018000101000000001800000010000102000000000042a00000"
                                                       "466978005363616c61626c6500000000"))

    def test_usd1_string(self):
        from lyt.sections import usd1
        self.assertEqual(usd1.write([{"name": "Copy", "type": "string", "value": "T_XXX_00"}])[8:],
                         struct.pack(">HHIIHBB", 1, 0, 0x15, 0xC, 8, 0, 0) + b"T_XXX_00\0Copy\0\0\0")

    def test_capacity_limits(self):
        with self.assertRaises(ValueError):
            self.build({"type": "text", "name": "T_A", "size": [10, 10], "capacity": 40000})


class Animation(unittest.TestCase):
    def test_hermite_slopes(self):
        keys = tracks.hermite_keys([[0, 0], [10, 100], [20, 100]])
        self.assertEqual(struct.unpack(">9f", keys), (0, 0, 10, 10, 100, 5, 20, 100, 0))
        self.assertEqual(struct.unpack(">6f", tracks.hermite_keys([[0, 0], [5, 1]], "ease"))[2::3], (0, 0))
        self.assertEqual(struct.unpack(">3f", tracks.hermite_keys([[0, 1, 7]])), (0, 1, 7))

    def test_pattern_and_tags(self):
        d = load_json(EXAMPLE)
        d["textures"]["Alt_00"] = {"png": "alt.png"}
        d["groups"]["G_All_00"].append("P_Plate_00")
        d["animations"]["Select"] = {"frames": 4, "loop": True, "groups": ["G_All_00"], "tracks": [
            {"material": "StoryPlate_00", "target": "tex.pattern", "keys": [[0, "StoryPlate_00"], [2, "Alt_00"]]},
            {"pane": "P_Plate_00", "target": "sx", "keys": [[0, 1], [4, 1.2]]},
            {"pane": "P_Plate_00", "target": "sy", "keys": [[0, 1], [4, 1.2]]},
            {"pane": "P_Plate_00", "target": "alpha", "keys": [[0, 255]]},
        ]}
        desc = lyt.Description(d)
        data = bflan.build("Select", desc.animations["Select"], desc)
        (_, pat), (_, pai) = binio.read_sections(data)
        frames, loop, _, ntex, ncont, table = struct.unpack(">HBBHHI", pai[:12])
        self.assertEqual((frames, loop, ntex, ncont), (4, 1, 2, 2))
        sec = b"pai1" + struct.pack(">I", 8 + len(pai)) + pai  # offsets are relative to the section start
        names = [sec[20 + o:].split(b"\0")[0] for o in struct.unpack(">II", sec[20:28])]
        self.assertEqual(names, [b"StoryPlate_00^l.bflim", b"Alt_00^l.bflim"])
        c0, c1 = struct.unpack(">II", sec[table:table + 8])
        self.assertEqual(sec[c0:c0 + 13], b"StoryPlate_00")
        self.assertEqual(sec[c0 + 28:c0 + 30], bytes([1, tracks.MATERIAL]))
        self.assertEqual(sec[c1:c1 + 10], b"P_Plate_00")
        ntags = sec[c1 + 28]
        tag_offs = struct.unpack(">%dI" % ntags, sec[c1 + 32:c1 + 32 + 4 * ntags])
        self.assertEqual([sec[c1 + o:c1 + o + 4] for o in tag_offs], [b"FLPA", b"FLVC"])
        flpa = c1 + tag_offs[0]
        self.assertEqual(sec[flpa + 4], 2)  # sx and sy share one FLPA block
        e0 = flpa + struct.unpack(">I", sec[flpa + 8:flpa + 12])[0]
        self.assertEqual(sec[e0:e0 + 4], bytes([0, 6, 2, 0]))
        # step keys of the pattern: frame f32, value u16, pad u16
        tp = c0 + struct.unpack(">I", sec[c0 + 32:c0 + 36])[0]
        e = tp + struct.unpack(">I", sec[tp + 8:tp + 12])[0]
        self.assertEqual(sec[e:e + 8], bytes([0, 0, 1, 0, 0, 2, 0, 0]))
        self.assertEqual(struct.unpack(">fHHfHH", sec[e + 12:e + 28]), (0, 0, 0, 2, 1, 0))
        self.assertEqual(pat[0x14:0x14 + 7], b"Select\0")


class Archive(unittest.TestCase):
    def test_files(self):
        loader = lambda spec, desc: (bytes(512 * 171 * 4), 512, 171)
        files = lyt.build_files(example(), loader)
        self.assertEqual(sorted(files), ["anim/StoryLyt_00_InAll.bflan", "blyt/StoryLyt_00.bflyt",
                                         "timg/StoryPlate_00^l.bflim"])

    def test_rgba_size_checked(self):
        with self.assertRaises(ValueError):
            bflim.encode(bytes(4 * 10), 10, 2)


def gradient(w, h, alpha=True):
    """A synthetic test picture: colour gradients, a hard edge and (optionally) an alpha ramp."""
    import numpy as np
    y, x = np.mgrid[0:h, 0:w]
    a = np.zeros((h, w, 4), np.uint8)
    a[..., 0] = x * 255 // max(w - 1, 1)
    a[..., 1] = y * 255 // max(h - 1, 1)
    a[..., 2] = np.where(x > w // 2, 200, 40)
    a[..., 3] = (x * 255 // max(w - 1, 1)) if alpha else 255
    return a.tobytes()


def rmse(a, b, channels=(0, 1, 2, 3)):
    import numpy as np
    x = np.frombuffer(a, np.uint8).reshape(-1, 4)[:, channels].astype(float)
    y = np.frombuffer(b, np.uint8).reshape(-1, 4)[:, channels].astype(float)
    return float(np.sqrt(((x - y) ** 2).mean()))


class Channels(unittest.TestCase):
    def test_game_drawing(self):
        """bc4a is drawn white with alpha, bc4l grey and opaque, bc5la grey with alpha (game run)."""
        img = bytes([200, 100, 50, 77] * 16)
        lum = round(200 * 0.299 + 100 * 0.587 + 50 * 0.114)
        for fmt, texel in (("bc4a", (255, 255, 255, 77)), ("bc4l", (lum, lum, lum, 255)),
                           ("bc5la", (lum, lum, lum, 77))):
            back, _, _ = bflim.decode(bflim.encode(img, 4, 4, fmt, "tiled"))
            self.assertEqual(tuple(back[:4]), texel, fmt)


class Tiling(unittest.TestCase):
    # (texels w, h, FLIM format code, image size, alignment) of game layout textures (tile mode 4); all
    # 308 2D-tiled game textures match, these rows cover every format and alignment case
    GAME = [(300, 300, 0x14, 389120, 2048), (72, 72, 0x14, 30720, 2048), (300, 300, 0x15, 61440, 4096),
            (160, 90, 0x15, 16384, 4096), (160, 8, 0x10, 8192, 4096), (300, 300, 0x0F, 61440, 4096),
            (1010, 150, 0x11, 196608, 8192), (505, 75, 0x11, 65536, 8192), (80, 80, 0x17, 16384, 8192),
            (2300, 500, 0x15, 589824, 4096)]

    def test_game_sizes(self):
        from lyt import gx2tile
        bpp = {0x14: 32, 0x15: 64, 0x10: 64, 0x0F: 64, 0x11: 128, 0x17: 128}
        for w, h, code, size, align in self.GAME:
            ew, eh = (w, h) if code == 0x14 else (-(-w // 4), -(-h // 4))
            self.assertEqual(gx2tile.surface_size(ew, eh, bpp[code], 4)[2:], (size, align), (w, h, code))
        self.assertEqual(gx2tile.surface_size(2, 2, 64, 2)[2:], (512, 256))  # 8 x 8 texels, 1D tiled

    def test_round_trip(self):
        from lyt import gx2tile
        for bpp in (32, 64, 128):
            for mode in (1, 2, 4):
                for swz in ((0, 3, 7) if mode == 4 else (0,)):
                    w, h = 37, 21
                    data = bytes((i * 7 + 3) & 0xFF for i in range(w * h * bpp // 8))
                    t = gx2tile.tile(data, w, h, bpp, mode, swz << 8)
                    self.assertEqual(gx2tile.untile(t, w, h, bpp, mode, swz << 8), data, (bpp, mode, swz))
                    addr = gx2tile.addresses(w, h, bpp, mode, swz << 8).reshape(-1)
                    self.assertEqual(len(set(addr.tolist())), w * h)  # no two elements share an address


class Textures(unittest.TestCase):
    def test_bc_quality(self):
        from lyt import bc
        w, h = 64, 32
        img = gradient(w, h)
        limits = {"bc1": (12, (0, 1, 2)), "bc3": (12, (0, 1, 2, 3)), "bc4": (4, (0,)), "bc5": (4, (0, 1))}
        for fmt, (limit, ch) in limits.items():
            src = gradient(w, h, alpha=False) if fmt == "bc1" else img
            back = bc.decode(fmt, bc.encode(fmt, src, w, h), w, h)
            img_ = src
            self.assertLess(rmse(img_, back, ch), limit, fmt)

    def test_bc1_punch_through(self):
        import numpy as np
        from lyt import bc
        img = np.frombuffer(gradient(8, 8, alpha=False), np.uint8).reshape(8, 8, 4).copy()
        img[:4, :4, 3] = 0
        back = np.frombuffer(bc.decode("bc1", bc.encode("bc1", img.tobytes(), 8, 8), 8, 8), np.uint8)
        back = back.reshape(8, 8, 4)
        self.assertTrue((back[:4, :4, 3] == 0).all() and (back[4:, :, 3] == 255).all())

    def test_bflim_formats(self):
        w, h = 100, 50
        img = gradient(w, h)
        for fmt in bflim.FORMATS:
            for tile, swz in (("tiled", 0), ("tiled", 5), ("1d", 0), ("linear", 0)):
                data = bflim.encode(img, w, h, fmt, tile, swz)
                fw, fh, align, code, tile_byte, size = bflim.parse_footer(data)
                self.assertEqual((fw, fh, code, tile_byte), (w, h, bflim.FORMATS[fmt].code,
                                                            bflim.TILE_MODES[tile] | swz << 5))
                self.assertEqual(len(data), size + 0x28)
                back, _, _ = bflim.decode(data)
                if fmt == "rgba8":
                    self.assertEqual(back, img)
                else:
                    from lyt import bc
                    f = bflim.FORMATS[fmt]
                    lin = bflim.elements(img, w, h, fmt)[0]
                    self.assertEqual(back, bflim._from_channels(bc.decode(f.bc, lin, w, h), f.channels))
                    if fmt in ("bc3", "bc4a", "bc5la"):  # smooth alpha survives
                        self.assertLess(rmse(img, back, (3,)), 10, fmt)


def cemu_latte_lib():
    """The port's libcemu_latte.a (address library) from WWHD_CEMU_LATTE_LIB or a build next to the repo."""
    root = os.path.dirname(os.path.dirname(HERE))
    for p in (os.environ.get("WWHD_CEMU_LATTE_LIB", ""), os.path.join(root, "build", "cmake", "libcemu_latte.a"),
              os.path.join(root, "build", "libcemu_latte.a")):
        if p and os.path.exists(p):
            return p, root
    return None, root


class PortAddrLib(unittest.TestCase):
    """Our tiled textures read back through the port's own address library (LatteAddrLib)."""

    @classmethod
    def setUpClass(cls):
        import shutil
        import subprocess
        import tempfile
        lib, root = cemu_latte_lib()
        cxx = shutil.which("clang++") or shutil.which("c++")
        if not lib or not cxx:
            raise unittest.SkipTest("no libcemu_latte.a (set WWHD_CEMU_LATTE_LIB) or no C++ compiler")
        cls.tmp = tempfile.mkdtemp()
        cemu = os.path.join(root, "runtime", "third_party", "cemu")
        cls.exe = os.path.join(cls.tmp, "gx2_detile")
        subprocess.run([cxx, "-std=c++20", "-w", "-O1", "-include", os.path.join(cemu, "cemu_shim.h"),
                        "-I" + cemu, "-I" + os.path.join(cemu, "Cafe"),
                        "-I" + os.path.join(root, "runtime", "third_party", "fmt", "include"),
                        os.path.join(HERE, "lyt", "tests", "gx2_detile.cpp"), lib, "-o", cls.exe], check=True)

    @classmethod
    def tearDownClass(cls):
        import shutil
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_detile(self):
        import subprocess
        for (w, h) in ((100, 50), (300, 300), (512, 171), (8, 8)):
            img = gradient(w, h)
            for fmt, f in bflim.FORMATS.items():
                for tile, swz in (("tiled", 0), ("tiled", 3), ("tiled", 6), ("1d", 0), ("linear", 0)):
                    data = bflim.encode(img, w, h, fmt, tile, swz)
                    _, _, align, _, _, size = bflim.parse_footer(data)
                    src, dst = os.path.join(self.tmp, "in"), os.path.join(self.tmp, "out")
                    with open(src, "wb") as fh:
                        fh.write(data[:size])
                    out = subprocess.run([self.exe, hex(f.gx2), str(w), str(h), str(bflim.TILE_MODES[tile]),
                                          hex(swz << 8), src, dst], check=True, capture_output=True, text=True)
                    pitch, height, psize, palign = map(int, out.stdout.split())
                    self.assertEqual((psize, palign), (size, align), (w, h, fmt, tile))
                    with open(dst, "rb") as fh:
                        self.assertEqual(fh.read(), bflim.elements(img, w, h, fmt)[0], (w, h, fmt, tile, swz))


class Fill(unittest.TestCase):
    def test_fill_texture(self):
        from lyt.archive import fill_image
        im = fill_image({"fill": {"color": [1, 2, 3, 4], "border": [9, 9, 9, 255], "border_width": 2}, "width": 8, "height": 6})
        self.assertEqual(im.size, (8, 6))
        self.assertEqual(im.getpixel((0, 0)), (9, 9, 9, 255))
        self.assertEqual(im.getpixel((1, 3)), (9, 9, 9, 255))
        self.assertEqual(im.getpixel((3, 3)), (1, 2, 3, 4))

def lyt_build_test():
    """The port's layout writer test tool (runtime/tools/lyt_build_test.cpp), from WWHD_LYT_BUILD_TEST or a
    build next to the repo."""
    root = os.path.dirname(os.path.dirname(HERE))
    for p in (os.environ.get("WWHD_LYT_BUILD_TEST", ""), os.path.join(root, "build", "cmake", "lyt_build_test"),
              os.path.join(root, "build", "lyt_build_test")):
        if p and os.path.exists(p):
            return p
    return None


# A description that uses every part the port's writer supports (art made in the test, no files)
EVERYTHING = {
    "name": "StoryAll_00", "size": [1280, 720], "part_size": [320, 200],
    "textures": {
        "Art_00": {"png": "art.png", "tile": "tiled", "swizzle": 5},
        "Art_01": {"png": "art.png", "tile": "1d"},
        "Art_02": {"png": "art.png"},
        "Fill_00": {"fill": {"color": [10, 20, 30], "border": [200, 100, 50, 128], "border_width": 3},
                    "width": 70, "height": 33, "tile": "tiled"},
        "Bc1_00": {"png": "art.png", "format": "bc1", "tile": "tiled", "swizzle": 3},
        "Bc1_01": {"png": "art.png", "format": "bc1", "tile": "1d"},
        "Bc3_00": {"png": "art.png", "format": "bc3", "tile": "tiled"},
        "Bc3_01": {"png": "art.png", "format": "bc3", "tile": "linear"},
    },
    "fonts": ["CKingMain.bffnt"],
    "materials": {
        "M_Art": {"textures": [{"texture": "Art_00", "wrap": [5, 4],
                                "srt": {"translate": [0.25, -1], "rotate": 30, "scale": [2, 0.5]}}],
                  "black": [12, 34, 56, 0], "white": [128, 200, 64], "flags_extra": 0x40000, "raw_tail": "0102aabb"},
        "M_Two": {"textures": [{"texture": "Art_01"}, {"texture": "Fill_00"}]},
        "M_Plain": {"white": [255, 128, 0, 77]},
        "M_Bc": {"textures": [{"texture": "Bc1_00"}, {"texture": "Bc1_01"}, {"texture": "Bc3_00"}]},
        "M_Bc3": {"textures": [{"texture": "Bc3_01"}]},
    },
    "panes": [
        {"type": "null", "name": "N_All_00", "size": [30, 40], "influence_alpha": True, "rotate": 12.5, "children": [
            {"type": "picture", "name": "P_Art_00", "size": [128, 64], "translate": [-100, 50, 3], "material": "M_Art",
             "alpha": 200, "origin": 4, "visible": False, "user": "abc",
             "vertex_colors": [[255, 0, 0, 255], [0, 255, 0], [0, 0, 255, 10], [1, 2, 3, 4]]},
            {"type": "picture", "name": "P_Two_00", "size": [64, 64], "material": "M_Two", "scale": [1.5, -1],
             "uvs": [[[0, 0], [2, 0], [0, 2], [2, 2]], [[0.5, 0.5], [1, 0.5], [0.5, 1], [1, 1]]]},
            {"type": "window", "name": "W_Win_00", "size": [300, 120], "frame_size": [16, 16, 12, 12],
             "content": {"material": "M_Plain", "inflation": [1, -2, 3, -4]},
             "frames": [{"texture": "Art_02", "flip": f} for f in ("none", "flip_h", "flip_v", "rot180")] +
                       [{"material": "M_Art", "flip": 3}, {"texture": "Fill_00", "white": [10, 200, 30, 255]},
                        {"texture": "Fill_00", "black": [1, 1, 1, 1], "flip": "rot270"}, {"material": "M_Two"}]},
            {"type": "window", "name": "W_Bar_00", "size": [200, 20], "frame_size": 8, "kind": "horizontal",
             "draw_content": False, "frames": [{"texture": "Art_02"}],
             "content": {"texture": "Fill_00", "vertex_colors": [[9, 9, 9, 9]] * 4}},
            {"type": "text", "name": "T_Title_00", "size": [400, 50], "font": "CKingMain.bffnt", "font_size": [40, 48],
             "text": "Notebook \u00e9\u00e0 \U0001F600 line\nbreak", "capacity": 60, "text_position": ["left", "top"],
             "line_alignment": "center", "italic": 0.25, "char_space": 1.5, "line_space": -2, "text_id": "MSG_01",
             "colors": [[255, 236, 170, 255], [255, 200, 120]], "scalable": 75,
             "shadow": {"offset": [2, -2], "scale": [1.1, 0.9], "colors": [[0, 0, 0, 255], [10, 10, 10, 128]],
                        "black": [5, 6, 7], "white": [255, 255, 255, 100], "italic": 0.1},
             "text_material": {"black": [1, 2, 3, 4], "white": [250, 240, 230, 220]}},
            {"type": "text", "name": "T_Plain_00", "size": [300, 40], "text": "", "text_position": 5,
             "line_alignment": 3, "text_flags": 4, "user_data": [{"name": "Copy", "type": "string", "value": "T_Title_00"},
                                                                {"name": "N", "type": "int", "value": [1, -2, 3]},
                                                                {"name": "F", "type": "float", "value": 0.5}]},
            {"type": "text", "name": "T_NoShadow_00", "size": [300, 40], "text": "x", "shadow": {"enabled": False}},
            {"type": "bounding", "name": "B_Btn_00", "size": [100, 40]},
            {"type": "picture", "name": "P_Bc_00", "size": [96, 40], "material": "M_Bc"},
            {"type": "picture", "name": "P_Bc3_00", "size": [96, 40], "material": "M_Bc3"},
            {"type": "parts", "name": "L_Btn_00", "size": [100, 40], "part": "BtnSample_00", "part_scale": [0.5, 2]},
        ]},
    ],
    "groups": {"G_All_00": ["N_All_00", "P_Art_00", "W_Win_00"], "G_Two_00": ["P_Two_00", "T_Title_00"]},
    "animations": {
        "InAll": {"frames": 10, "groups": ["G_All_00"], "tracks": [
            {"pane": "N_All_00", "target": "alpha", "keys": [[0, 0], [10, 255]]},
            {"pane": "P_Art_00", "target": "tx", "keys": [[0, -10], [4, 30], [10, 0]], "interp": "ease"},
            {"pane": "P_Art_00", "target": "vc.br.a", "keys": [[0, 255, 3.5], [10, 0]]},
            {"material": "M_Art", "target": "white.g", "keys": [[0, 128], [10, 255]]},
            {"material": "M_Art", "target": "white.a", "keys": [[0, 128], [10, 255]]},
            {"material": "M_Art", "target": "tex.sx", "keys": [[0, 1], [10, 2]]},
            {"pane": "P_Art_00", "target": "visible", "keys": [[0, 0], [5, 1]]},
        ]},
        "Select": {"frames": 4, "start": 2, "loop": True, "order": 1, "groups": ["G_Two_00"], "tracks": [
            {"material": "M_Two", "target": "tex.pattern", "index": 1, "keys": [[0, "Fill_00"], [2, "Art_01"], [3, "x^l.bflim"]]},
            {"pane": "T_Title_00", "tag": "FLFS", "target": 3, "type": 0, "keys": [[0, 1], [4, 0]]},
            {"pane": "P_Two_00", "target": "w", "keys": [[0, 64]]},
        ]},
        "Free": {"frames": 3, "child_binding": 1, "groups": ["G_All_00"], "tracks": [
            {"pane": "T_Plain_00", "target": "h", "keys": [[0, 40], [3, 80]]}]},
    },
}


class CppWriter(unittest.TestCase):
    """The port's layout writer (C++, lyt_build.cpp) writes the same bytes as this generator."""

    @classmethod
    def setUpClass(cls):
        import tempfile
        cls.exe = lyt_build_test()
        if not cls.exe:
            raise unittest.SkipTest("no lyt_build_test (build it, or set WWHD_LYT_BUILD_TEST)")
        cls.tmp = tempfile.mkdtemp()

    @classmethod
    def tearDownClass(cls):
        import shutil
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def compare(self, d, art_dir, tag):
        import subprocess
        from notebook_model import sarc, yaz0_stored
        path = os.path.join(art_dir, tag + ".json")
        with open(path, "w") as f:
            json.dump(d, f, indent=1)
        out = os.path.join(self.tmp, tag)
        os.makedirs(out, exist_ok=True)
        r = subprocess.run([self.exe, path, out], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        files = lyt.build_files(lyt.Description.load(path))
        for name, data in files.items():
            with open(os.path.join(out, name.replace("/", "_")), "rb") as f:
                self.assertEqual(f.read(), data, (tag, name))
        with open(os.path.join(out, "archive.szs"), "rb") as f:
            self.assertEqual(f.read(), yaz0_stored(sarc(files)), tag)
        self.assertEqual(sorted(os.listdir(out)), sorted([n.replace("/", "_") for n in files] + ["archive.szs"]))

    def art(self, name, w, h):
        from PIL import Image
        Image.frombytes("RGBA", (w, h), gradient(w, h)).save(os.path.join(self.tmp, name))

    def test_everything(self):
        self.art("art.png", 96, 40)
        d = json.loads(json.dumps(EVERYTHING))
        self.compare(d, self.tmp, "everything")
        d["color_space"] = "linear"
        self.compare(d, self.tmp, "everything_linear")

    def test_examples(self):
        self.art("plate.png", 512, 171)
        for ex in ("plate.json", "text.json"):
            d = load_json(os.path.join(HERE, "lyt", "examples", ex))
            self.compare(d, self.tmp, ex[:-5])

    def test_window_demo(self):
        import subprocess
        art = os.path.join(self.tmp, "wdemo")
        js = os.path.join(art, "demo.json")
        os.makedirs(art, exist_ok=True)
        subprocess.run([sys.executable, os.path.join(HERE, "lyt", "examples", "window_demo.py"),
                        os.path.join(art, "demo.szs"), "--art", art, "--json", js], check=True, capture_output=True)
        d = load_json(js)
        for spec in d["textures"].values():  # the port reads art only by paths relative to the package
            if "png" in spec:
                spec["png"] = os.path.relpath(spec["png"], art)
        self.compare(d, art, "window_demo")

    def test_refusals(self):
        import subprocess
        self.art("plate.png", 512, 171)
        for change in ({"format": "bc4a"}, {"width": 256}):
            d = load_json(os.path.join(HERE, "lyt", "examples", "plate.json"))
            d["textures"]["StoryPlate_00"].update(change)
            path = os.path.join(self.tmp, "refused.json")
            with open(path, "w") as f:
                json.dump(d, f)
            r = subprocess.run([self.exe, path, self.tmp], capture_output=True, text=True)
            self.assertEqual(r.returncode, 3, change)


def item_icons_test():
    """The C++ item icon builder's test tool (runtime/tools/item_icons_test.cpp)."""
    exe = lyt_build_test()
    if exe:
        exe = os.path.join(os.path.dirname(exe), "item_icons_test")
    return exe if exe and os.path.exists(exe) else os.environ.get("WWHD_ITEM_ICONS_TEST") or None


def fake_icon_archive():
    """A synthetic archive shaped like the game's BtnItemIcon_00 (no game data): IconChange with 55 frames
    over Float128 / Icon128 textures (three pattern tracks as the game's), two of the textures, a layout."""
    from lyt import item_icons as ii
    from lyt.anim import pat1
    textures = [ii.float_name(n) for n in range(55)] + [ii.icon_name(n) for n in range(55)]
    contents = [("P_Btn_00", 1, [(b"FLTP", [(0, 0, tracks.STEP, [(float(n), 55 + n) for n in range(55)])])]),
                ("P_BtnFloat_00", 1, [(b"FLTP", [(0, 0, tracks.STEP, [(float(n), n) for n in range(55)])])]),
                ("P_BtnGlow_00", 1, [(b"FLTP", [(0, 0, tracks.STEP, [(float(n), n) for n in range(55)])])])]
    anim = binio.file_header(b"FLAN", [pat1.write("IconChange", ["G_IconChange_00"], -200, -146, 8),
                                       ii.write_pai1(54, False, textures, contents)])
    return {ii.ANIM: anim, "blyt/BtnItemIcon_00.bflyt": b"FLYT" + bytes(60),
            "anim/BtnItemIcon_00_In.bflan": b"FLAN" + bytes(28),
            "timg/" + ii.icon_name(54): bflim.encode(gradient(128, 128), 128, 128, "rgba8", "tiled", 3),
            "timg/" + ii.float_name(54): bflim.encode(gradient(128, 128), 128, 128, "bc5la", "tiled", 5)}


class ItemIcons(unittest.TestCase):
    """The extended item icon archive (lyt/item_icons.py) and its C++ twin (item_icons.cpp)."""

    def test_animation(self):
        from lyt import item_icons as ii
        files = fake_icon_archive()
        anim = files[ii.ANIM]
        self.assertEqual(ii.extend_animation(anim, 0), anim)  # parsed and written back unchanged
        out = ii.extend(files, [gradient(128, 128)])
        frames, _, textures, contents = ii.parse_pai1(ii._sections(out[ii.ANIM])[1][1])
        self.assertEqual(frames, 56)
        self.assertEqual(textures[:110], [ii.float_name(n) for n in range(55)] + [ii.icon_name(n) for n in range(55)])
        keys = {name: tags[0][1][0][3] for name, _, tags in contents}
        self.assertEqual(textures[keys["P_Btn_00"][-1][1]], ii.icon_name(56))
        self.assertEqual(textures[keys["P_BtnGlow_00"][-2][1]], ii.float_name(55))
        self.assertEqual(ii.frame_of(0), 56)
        for n in (55, 56):
            self.assertEqual(bflim.parse_footer(out["timg/" + ii.icon_name(n)])[3], 0x14)
            self.assertEqual(bflim.parse_footer(out["timg/" + ii.float_name(n)])[3], 0x11)
        self.assertEqual(bflim.decode(out["timg/" + ii.icon_name(55)])[0], bytes(128 * 128 * 4))  # the dummy
        with self.assertRaises(ValueError):
            ii.extend({ii.ANIM: ii.extend_animation(anim, 1)}, [])  # not the game's 55 frames

    def test_archive_layout(self):
        from lyt import item_icons as ii
        files = fake_icon_archive()
        sarc = ii.archive(files)
        self.assertEqual(ii.read_sarc(sarc), files)
        self.assertEqual(ii.yaz0_decode(ii.yaz0_store(sarc)), sarc)
        for data in (b"", b"a", b"abcabcabcabcabcabcabc" * 50, bytes(5000), gradient(64, 64), sarc):
            packed = ii.yaz0_compress(data)
            self.assertEqual(ii.yaz0_decode(packed), data)
        self.assertLess(len(ii.yaz0_compress(sarc)), len(sarc) * 2 // 3)

    def test_cpp_same_bytes(self):
        import subprocess
        import tempfile
        from lyt import item_icons as ii
        exe = item_icons_test()
        if not exe:
            self.skipTest("no item_icons_test (build it)")
        tmp = tempfile.mkdtemp()
        member = ii.yaz0_store(ii.archive(fake_icon_archive()))
        pack = ii.archive({ii.MEMBER: member, "Other_00.szs": b"Yaz0" + bytes(40)}, align_of=lambda n, d: 0x2000)
        icons = [gradient(128, 128), gradient(128, 128, alpha=False)]
        paths = []
        for i, icon in enumerate(icons):
            paths.append(os.path.join(tmp, "icon%d.rgba" % i))
            with open(paths[-1], "wb") as f:
                f.write(icon)
        with open(os.path.join(tmp, "in.pack"), "wb") as f:
            f.write(pack)
        r = subprocess.run([exe, os.path.join(tmp, "in.pack"), os.path.join(tmp, "out.pack")] + paths, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(os.path.join(tmp, "out.pack"), "rb") as f:
            self.assertEqual(f.read(), ii.extend_pack(pack, icons))
        r = subprocess.run([exe], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    unittest.main()
