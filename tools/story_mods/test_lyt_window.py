#!/usr/bin/env python3
"""Unit tests for the window (wnd1) and bounding (bnd1) writers and the button demo (tools/story_mods/lyt).
The expected layouts are the ones documented in lyt/sections/wnd1.py (learned from the game's own files);
no game data is used here."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lyt  # noqa: E402
from lyt import bflan, bflyt, binio  # noqa: E402
from lyt.sections import wnd1  # noqa: E402


def desc(panes, materials=None, groups=None, animations=None):
    return lyt.Description({
        "name": "StoryLyt_00",
        "color_space": "linear",  # raw encodings: colours written as given
        "textures": {"Frame_00": {"png": "frame.png"}, "Edge_00": {"png": "edge.png"}},
        "materials": materials or {},
        "panes": panes, "groups": groups or {}, "animations": animations or {},
    })


def sections(d):
    data, ctx = bflyt.build(d)
    return [(m, m + struct.pack(">I", 8 + len(b)) + b) for m, b in binio.read_sections(data)], ctx


def window_sections(d):
    secs, ctx = sections(d)
    return [s for m, s in secs if m == b"wnd1"], ctx


def material_names(ctx):
    return [n for n, _ in ctx.materials]


class Window(unittest.TestCase):
    def test_one_frame_layout(self):
        (s,), ctx = window_sections(desc([
            {"type": "window", "name": "W_Base_00", "size": [300, 150], "frame_size": 40,
             "frames": [{"texture": "Frame_00"}]}]))
        self.assertEqual(len(s), 0x8C)  # as the game's 1-frame windows without content UVs
        w = wnd1.parse(s)
        self.assertEqual(w["name"], "W_Base_00")
        self.assertEqual(w["size"], [300, 150])
        self.assertEqual(w["frame_size"], [40, 40, 40, 40])
        self.assertEqual(w["inflation"], [0, 0, 0, 0])
        self.assertEqual((w["flags"], w["content_offset"], w["frame_table_offset"]), (0x01, 0x70, 0x84))
        self.assertEqual(w["vertex_colors"], [[255] * 4] * 4)
        self.assertEqual(w["uvs"], [])
        self.assertEqual(material_names(ctx), ["W_Base_00C", "W_Base_00LT"])
        self.assertEqual(w["content_material"], 0)
        self.assertEqual(w["frames"], [(1, 0)])
        self.assertEqual(ctx.textures, ["Frame_00^l.bflim"])

    def test_material_encoding(self):
        _, ctx = window_sections(desc([
            {"type": "window", "name": "W_Base_00", "size": [10, 10], "frame_size": 4,
             "frames": [{"texture": "Frame_00", "white": [1, 2, 3, 4]}],
             "content": {"black": [9, 9, 9, 9], "white": [8, 8, 8, 8]}}]))
        content, frame = (m for _, m in ctx.materials)
        # untextured content material: name[28], black, white, flags 0 (as the game's "...C" materials)
        self.assertEqual(len(content), 40)
        self.assertEqual(content[28:40], bytes([9] * 4 + [8] * 4) + b"\0\0\0\0")
        # frame material: 1 texture map (wrap 4/4), 1 SRT, 1 generator: flags 0x15
        self.assertEqual(frame[28:36], bytes([0, 0, 0, 0, 1, 2, 3, 4]))
        self.assertEqual(struct.unpack(">IHBB", frame[36:44]), (0x15, 0, 4, 4))

    def test_eight_frames_flips_and_content_uvs(self):
        frames = [{"texture": "Frame_00", "flip": f} for f in ("none", "rot90", "rot270", "rot180")]
        frames += [{"texture": "Edge_00", "flip": f} for f in ("none", "flip_h", "rot90", "flip_v")]
        (s,), ctx = window_sections(desc([
            {"type": "window", "name": "W_Base_00", "size": [640, 360], "frame_size": [30, 31, 32, 33],
             "frames": frames, "content": {"texture": "Edge_00", "vertex_colors": [[1, 2, 3, 4]] * 4,
                                           "inflation": [-2, 3, -4, 5]}}]))
        self.assertEqual(len(s), 0xE4)  # as the game's 8-frame windows with one content UV set
        w = wnd1.parse(s)
        self.assertEqual(w["frame_size"], [30, 31, 32, 33])
        self.assertEqual(w["inflation"], [-2, 3, -4, 5])
        self.assertEqual((w["content_offset"], w["frame_table_offset"]), (0x70, 0xA4))
        self.assertEqual(w["uvs"], [[[0, 0], [1, 0], [0, 1], [1, 1]]])
        self.assertEqual(w["vertex_colors"], [[1, 2, 3, 4]] * 4)
        self.assertEqual([f for _, f in w["frames"]], [0, 3, 5, 4, 0, 1, 3, 2])
        self.assertEqual(material_names(ctx), ["W_Base_00" + x for x in ("C", "LT", "RT", "LB", "RB", "L", "R", "T", "B")])
        self.assertEqual([m for m, _ in w["frames"]], list(range(1, 9)))
        # the frame table holds offsets from the section start, the frames follow it
        offs = struct.unpack(">8I", s[0xA4:0xC4])
        self.assertEqual(offs, tuple(0xC4 + 4 * i for i in range(8)))

    def test_shared_materials_two_sizes(self):
        mats = {"Frame_00C": {}, "Frame_00LT": {"textures": [{"texture": "Frame_00"}]}}
        win = lambda n, w, h: {"type": "window", "name": n, "size": [w, h], "frame_size": 32,
                               "frames": [{"material": "Frame_00LT"}], "content": {"material": "Frame_00C"}}
        ws, ctx = window_sections(desc([win("W_A_00", 100, 60), win("W_B_00", 700, 400)], mats))
        a, b = (wnd1.parse(s) for s in ws)
        self.assertEqual(material_names(ctx), ["Frame_00C", "Frame_00LT"])
        self.assertEqual((a["content_material"], a["frames"]), (b["content_material"], b["frames"]))
        self.assertEqual((a["size"], b["size"]), ([100, 60], [700, 400]))

    def test_materials_used(self):
        p = {"frames": [{"material": "A"}, {"texture": "T"}, {"material": "B"}, {"material": "A"}],
             "content": {"material": "C"}}
        self.assertEqual(wnd1.materials(p), ["A", "B", "C"])

    def test_flags(self):
        f = lambda **k: wnd1.flags_of(dict({"name": "W"}, **k))
        self.assertEqual(f(), 0x01)
        self.assertEqual(f(kind="horizontal"), 0x09)
        self.assertEqual(f(draw_content=False), 0x11)
        self.assertEqual(f(window_flags=0x13), 0x13)
        with self.assertRaises(ValueError):
            f(kind="vertical")

    def test_errors(self):
        bad = [
            {"frames": []},
            {"frames": [{"texture": "Frame_00"}] * 3},
            {"frames": [{"texture": "Frame_00", "flip": 6}]},
            {"frames": [{"texture": "Frame_00", "flip": "mirror"}]},
            {"frames": [{"texture": "Frame_00"}], "frame_size": [1, 2, 3]},
            {"frames": [{"texture": "Frame_00"}], "frame_size": -1},
            {"frames": [{"material": "Missing"}]},
        ]
        for extra in bad:
            p = dict({"type": "window", "name": "W_Base_00", "size": [10, 10]}, **extra)
            with self.assertRaises((ValueError, KeyError), msg=extra):
                bflyt.build(desc([p]))


class Bounding(unittest.TestCase):
    def test_bnd1(self):
        secs, _ = sections(desc([{"type": "bounding", "name": "B_Btn_00", "size": [260, 90]}]))
        (s,) = [s for m, s in secs if m == b"bnd1"]
        self.assertEqual(len(s), 0x54)  # pane fields only, as the game's B_Btn_00
        self.assertEqual(s[8:12], bytes([1, 0, 255, 2]))
        self.assertEqual(struct.unpack(">2f", s[0x4C:0x54]), (260, 90))


class Parts(unittest.TestCase):
    def test_prt1_without_properties(self):
        secs, _ = sections(desc([{"type": "parts", "name": "L_Btn_00", "size": [260, 90], "part": "StoryBtn_0"}]))
        (s,) = [s for m, s in secs if m == b"prt1"]
        self.assertEqual(len(s), 0x6C)  # as the game's parts without overrides and a 10-character part name
        self.assertEqual(struct.unpack(">I2f", s[0x54:0x60]), (0, 1.0, 1.0))
        self.assertEqual(s[0x60:0x6C], b"StoryBtn_0\0\0")


class ButtonDemo(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "lyt", "examples"))
        import window_demo
        self.d = lyt.Description(window_demo.description("/nonexistent"))

    def test_layout(self):
        secs, ctx = sections(self.d)
        order = [m for m, _ in secs]
        self.assertEqual(order.count(b"wnd1"), 5)
        self.assertEqual(order.count(b"bnd1"), 1)
        # one texture set (corner + edge) for the decorated windows, one for the button
        self.assertEqual(ctx.textures, ["StoryFrame_00^l.bflim", "StoryEdge_00^l.bflim",
                                        "StoryBtnFrame_00^l.bflim", "StoryBtnEdge_00^l.bflim"])
        # the button's 8 frames share two materials (corner, edge)
        (btn,) = [wnd1.parse(s) for m, s in secs if m == b"wnd1" and b"W_Btn_00" in s[12:36]]
        names = [n for n, _ in ctx.materials]
        self.assertEqual([names[m] for m, _ in btn["frames"]], ["W_Btn_00LT"] * 4 + ["W_Btn_00T"] * 4)

    def test_schema(self):
        try:
            import jsonschema
        except ImportError:
            self.skipTest("jsonschema not installed")
        import json
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "lyt", "schema.json")) as f:
            jsonschema.validate(self.d.data, json.load(f))

    def test_state_animations(self):
        for name in ("Select", "Press", "Normal"):
            data = bflan.build(name, self.d.animations[name], self.d)
            (_, pat), (_, pai) = binio.read_sections(data)
            groups = struct.unpack(">H", pat[2:4])[0]
            self.assertEqual(groups, 1)
            self.assertIn(b"G_Btn_00", pat)
            sec = b"pai1" + struct.pack(">I", 8 + len(pai)) + pai
            ncont, table = struct.unpack(">HI", pai[6:12])
            names = [sec[o:o + 28].split(b"\0")[0] for o in struct.unpack(">%dI" % ncont, sec[table:table + 4 * ncont])]
            self.assertEqual(names, [b"N_Btn_00", b"W_Btn_00LT", b"W_Btn_00T", b"W_Btn_00C"])
        # consecutive states meet: Select ends where Press starts, Press ends where Normal starts
        ends = {}
        for name in ("Select", "Press", "Normal"):
            sx = [t for t in self.d.animations[name]["tracks"] if t.get("target") == "sx"][0]["keys"]
            ends[name] = (sx[0][1], sx[-1][1])
        self.assertEqual(ends["Select"][1], ends["Press"][0])
        self.assertEqual(ends["Press"][1], ends["Normal"][0])
        self.assertEqual(ends["Normal"][1], ends["Select"][0])


if __name__ == "__main__":
    unittest.main()
