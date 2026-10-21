#!/usr/bin/env python3
"""Window and button demo (local only): our own frame art, two windows of different sizes from one texture
set (and its size animated), and a button with Normal / Select / Press state animations in one animation set.

usage: window_demo.py OUT.szs [--art DIR] [--json DESC.json]
  writes the PNGs (drawn here, no game data) to --art (default: next to OUT), builds Layout StoryLyt_00.
Runtime test (the prototype's item-screen hook, story_lyt_states.cpp):
  WWHD_STORY_LYT=OUT.szs
  WWHD_STORY_LYT_ANIMS=InAll@G_All_00:0,Normal@G_Btn_00:1,Select@G_Btn_00:1,Press@G_Btn_00:1,Grow@G_Grow_00:2
  WWHD_STORY_LYT_STATES=Select:40,Press:30,Normal:40
  WWHD_STORY_LYT_PLAY=Grow
"""
import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import lyt  # noqa: E402



def linear(rgb):
    """Material colours are applied as linear values, while the "^l" RGBA8 textures are sRGB (seen in game
    runs: a constant 206 shows as about 231 next to a texture's 206). An sRGB art colour -> material colour."""
    out = [round(255 * (c / 255 if c / 255 <= 0.04045 else ((c / 255 + 0.055) / 1.055) ** 2.4)) for c in rgb[:3]]
    return out + list(rgb[3:])


CREAM_ART = [246, 236, 206, 255]   # the frame image's fill
CREAM = linear(CREAM_ART)          # the same colour for the untextured content
BTN_NORMAL, BTN_SELECT, BTN_PRESS = (linear(c) for c in ([90, 150, 230], [255, 210, 80], [40, 80, 150]))
NORMAL, SELECTED, PRESSED = (1.0, BTN_NORMAL), (1.12, BTN_SELECT), (0.9, BTN_PRESS)  # button poses


def _smooth(edge0, edge1, x):
    t = min(1.0, max(0.0, (x - edge0) / (edge1 - edge0)))
    return t * t * (3 - 2 * t)


def corner_png(path, size=64, radius=40, rings=None):
    """A top-left window corner: rounded outer edge, coloured rings from the outside in, then the fill.
    rings: [(width, rgba)], the last entry is the fill. Pixels right of / below the arc continue the
    straight edges, so mirrored corners and stretched edges meet seamlessly."""
    from PIL import Image
    rings = rings or [(5, (70, 44, 22, 255)), (3, (205, 160, 70, 255)), (0, tuple(CREAM_ART))]
    im = Image.new("RGBA", (size, size))
    px = im.load()
    c = radius  # arc centre (c, c); beyond it the edge is straight
    for y in range(size):
        for x in range(size):
            if x < c and y < c:  # the rounded corner: distance inward from the arc
                d = radius - math.hypot(c - x - 0.5, c - y - 0.5)
            elif y < c:          # top edge
                d = y + 0.5
            elif x < c:          # left edge
                d = x + 0.5
            else:
                d = min(x, y) + 0.5
            inside = _smooth(-0.5, 0.5, d)
            depth, col = d, rings[-1][1]
            for w, rgba in rings[:-1]:
                if depth < w:
                    col = rgba
                    break
                depth -= w
            r, g, b, a = col
            px[x, y] = (r, g, b, int(round(a * inside)))
    im.save(path)


CORNER_FLIPS = ("none", "flip_h", "flip_v", "rot180")  # LT RT LB RB from one top-left corner image
EDGE_FLIPS = ("rot270", "rot90", "none", "rot180")     # L R T B from one top-edge image


def nine_slice(corner, edge):
    """8 frames (corners, then edges) from one corner and one edge: a texture set or material names."""
    key = "material" if corner.startswith("W_") else "texture"
    return ([{key: corner, "flip": f} for f in CORNER_FLIPS] + [{key: edge, "flip": f} for f in EDGE_FLIPS])


def description(art):
    tex = lambda name: {"png": os.path.join(art, name + ".png"), "format": "rgba8"}
    frame_mat = lambda t: {"textures": [{"texture": t}], "white": BTN_NORMAL + [255]}
    btn_mats = {"W_Btn_00C": {"white": BTN_NORMAL + [255]},
                "W_Btn_00LT": frame_mat("StoryBtnFrame_00"), "W_Btn_00T": frame_mat("StoryBtnEdge_00")}

    def state(frames, pose_from, pose_to):
        """a transition between two button poses (scale, colour), as the game's Select / UnSelect / Decide"""
        (s0, c0), (s1, colour) = pose_from, pose_to
        tracks = [{"pane": "N_Btn_00", "target": t, "keys": [[0, s0], [frames, s1]], "interp": "ease"}
                  for t in ("sx", "sy")]
        for m in ("W_Btn_00LT", "W_Btn_00T", "W_Btn_00C"):
            for i, ch in enumerate("rgb"):
                tracks.append({"material": m, "target": "white.%s" % ch,
                               "keys": [[0, c0[i]], [frames, colour[i]]], "interp": "ease"})
        return {"frames": frames, "groups": ["G_Btn_00"], "tracks": tracks}

    cream = {"white": CREAM}  # untextured content: its colour is the white colour (x vertex colours)
    return {
        "name": "StoryLyt_00",
        "color_space": "linear",  # the colours below are already converted with linear()
        "size": [1280, 720],
        "textures": {n: tex(n) for n in ("StoryFrame_00", "StoryEdge_00", "StoryBtnFrame_00", "StoryBtnEdge_00")},
        "materials": btn_mats,
        "panes": [
            {"type": "null", "name": "N_All_00", "size": [30, 40], "influence_alpha": True, "children": [
                # one texture set (corner + edge), two sizes
                {"type": "window", "name": "W_Small_00", "size": [300, 150], "translate": [-400, 170],
                 "frame_size": 64, "frames": nine_slice("StoryFrame_00", "StoryEdge_00"), "content": cream},
                {"type": "window", "name": "W_Large_00", "size": [620, 330], "translate": [170, 110],
                 "frame_size": 64, "frames": nine_slice("StoryFrame_00", "StoryEdge_00"), "content": cream},
                # the same set, its size animated (Grow): the frame keeps its size while the pane stretches
                {"type": "window", "name": "W_Grow_00", "size": [160, 160], "translate": [-400, -110],
                 "frame_size": 48, "frames": nine_slice("StoryFrame_00", "StoryEdge_00"), "content": cream},
                # one frame, mirrored by the game into the other corners (clean when frame size = image size)
                {"type": "window", "name": "W_Mirror_00", "size": [200, 140], "translate": [-500, -260],
                 "frame_size": 64, "frames": [{"texture": "StoryFrame_00"}], "content": cream},
                # the button: corner and edge materials shared by the 4 corners and the 4 edges
                {"type": "null", "name": "N_Btn_00", "size": [260, 90], "translate": [170, -200], "children": [
                    {"type": "bounding", "name": "B_Btn_00", "size": [260, 90]},
                    {"type": "window", "name": "W_Btn_00", "size": [260, 90], "frame_size": 32,
                     "frames": nine_slice("W_Btn_00LT", "W_Btn_00T"), "content": {"material": "W_Btn_00C"}},
                ]},
            ]},
        ],
        "groups": {"G_All_00": ["N_All_00"], "G_Btn_00": ["N_Btn_00", "W_Btn_00"], "G_Grow_00": ["W_Grow_00"]},
        "animations": {
            "InAll": {"frames": 10, "groups": ["G_All_00"],
                      "tracks": [{"pane": "N_All_00", "target": "alpha", "keys": [[0, 0], [10, 255]]}]},
            # Select: normal -> selected; Press: selected -> pressed; Normal: back from pressed / selected
            "Select": state(4, NORMAL, SELECTED),
            "Press": state(3, SELECTED, PRESSED),
            "Normal": state(6, PRESSED, NORMAL),
            "Grow": {"frames": 120, "loop": True, "groups": ["G_Grow_00"], "tracks": [
                {"pane": "W_Grow_00", "target": "w", "keys": [[0, 110], [60, 330], [120, 110]], "interp": "ease"},
                {"pane": "W_Grow_00", "target": "h", "keys": [[0, 110], [60, 200], [120, 110]], "interp": "ease"}]},
        },
    }


def edge_png(corner_path, path):
    """The top edge: the corner image's cross-section right of its arc (its last column), repeated."""
    from PIL import Image
    c = Image.open(corner_path).convert("RGBA")
    col = c.crop((c.width - 1, 0, c.width, c.height))
    col.resize((c.width, c.height), Image.NEAREST).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--art")
    ap.add_argument("--json")
    a = ap.parse_args()
    art = os.path.abspath(a.art or os.path.dirname(os.path.abspath(a.out)))
    os.makedirs(art, exist_ok=True)
    corner_png(os.path.join(art, "StoryFrame_00.png"))
    # the button's frame is white: its materials' colours tint it (and the state animations change them)
    corner_png(os.path.join(art, "StoryBtnFrame_00.png"), radius=28,
               rings=[(3, (40, 40, 40, 255)), (4, (255, 255, 255, 255)), (0, (215, 215, 215, 255))])
    edge_png(os.path.join(art, "StoryFrame_00.png"), os.path.join(art, "StoryEdge_00.png"))
    edge_png(os.path.join(art, "StoryBtnFrame_00.png"), os.path.join(art, "StoryBtnEdge_00.png"))
    d = description(art)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(d, f, indent=1)
    data = lyt.build(lyt.Description(d), a.out)
    print("%s: %d bytes" % (a.out, len(data)))


if __name__ == "__main__":
    main()
