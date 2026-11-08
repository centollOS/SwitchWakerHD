#!/usr/bin/env python3
"""The story notebook page as a game layout (local only, story mods are WIP).

usage: make_notebook_layout.py ART_DIR OUT_DIR
  ART_DIR  the painted art (frame.png, banner.png: the user's artwork, never in this repository)
  OUT_DIR  gets layout/notebook.json and the PNGs it names: the Story Notebook package's layout/ folder
           (manifest "notebook": {"layout": "layout/notebook.json"}); the port builds the archive from it
           (runtime/src/mods/lyt_build.h, story_notebook_layout.h). Check it with tools/story_mods/lyt
           (lyt.Description.load) or build it with story_lyt.py.
A dev tool (local only while the story mods are WIP): it writes our own small pictures (white, ring,
cross, cursor corner) and copies the art it is given, cleaned and cropped; no art lives here.

Geometry: the page of runtime/src/mods/story_notebook_page.h (framed look), on a 1280 x 720 screen,
converted to layout coordinates (origin in the centre, y up). Pane names are the contract with
runtime/src/mods/story_notebook_layout.cpp. Animations run at 30 frames a second (one frame per item
screen draw).
"""
import json
import math
import os
import sys

from PIL import Image, ImageDraw

W, H = 1280, 720
FRAME_ASPECT = 1448 / 1086
ROWS = 5
PORTRAIT, PORTRAIT_SLOTS = 64, 6  # portrait slots: one per list row, the last for the detail page
FPS = 30

# colours of story_notebook_page.h (RGBA)
INK, INK_SOFT, PLATE_INK, WHITE, SEAL = 0x003800FF, 0x3C6B2EFF, 0x0B3D06FF, 0xFFFFFFFF, 0xF2E4C2FF
SHADE = 0x0A140A88


def linear(c):
    """An sRGB colour as the linear values the game applies (alpha unchanged)."""
    f = lambda v: round(255 * (v / 255 / 12.92 if v / 255 <= 0.04045 else ((v / 255 + 0.055) / 1.055) ** 2.4))
    return [f(v) for v in c[:3]] + list(c[3:])


def rgba(c):
    return [(c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, c & 255]


# ---- the page's geometry (story_notebook_page.h Page::frame_box / frame_part / geo, art.h) ----
class R:
    def __init__(self, x0, y0, x1, y1, w=1448, h=1086):
        self.x0, self.y0, self.x1, self.y1 = x0 / w, y0 / h, x1 / w, y1 / h


FR_TITLE = R(112, 92, 735, 160)
FR_TAB0, FR_TAB1 = R(748, 123, 1031, 197), R(1043, 123, 1311, 197)
FR_SLOT = R(1327, 94, 1395, 162)
FR_CONTENT = R(105, 228, 1365, 905)
FR_PLATE_L, FR_PLATE_R = R(80, 935, 464, 1023), R(948, 937, 1390, 1024)
BANNER_W, BANNER_H = 2103, 311
B_LEFT, B_RIGHT = 524 / BANNER_W, 1484 / BANNER_W
B_MED_X, B_MED_Y, B_MED_R = 199 / BANNER_W, 164 / BANNER_H, 97 / BANNER_H
B_PILL = R(1552, 106, 1919, 201, BANNER_W, BANNER_H)
B_TEXT = R(340, 30, 1490, 215, BANNER_W, BANNER_H)


def frame_box():
    h = 696.0
    w = h * FRAME_ASPECT
    return 640 - w / 2, 12.0, w, h


def frame_part(r):
    fx, fy, fw, fh = frame_box()
    return fx + r.x0 * fw, fy + r.y0 * fh, (r.x1 - r.x0) * fw, (r.y1 - r.y0) * fh


def geo():
    cx, cy, cw, ch = frame_part(FR_CONTENT)
    return {"content": (cx, cy, cw, ch), "list_x": cx + 8, "list_y": cy + 8, "list_w": cw - 16, "row_h": (ch - 16) / ROWS}


def row_x(share, x, w, h):
    k = h / 311.0 * 2103.0
    left, right = B_LEFT * k, (1 - B_RIGHT) * k
    if left + right >= w:
        return x + share * w
    if share <= B_LEFT:
        return x + share * k
    if share >= B_RIGHT:
        return x + w - (1 - share) * k
    return x + left + (share - B_LEFT) / (B_RIGHT - B_LEFT) * (w - left - right)


# ---- panes ----
def place(x, y, w, h, parent=(0.0, 0.0)):
    """A pane's translate (relative to its parent's centre in layout coordinates) for a page box."""
    return [round(x + w / 2 - W / 2 - parent[0], 3), round(H / 2 - (y + h / 2) - parent[1], 3)]


def pic(name, x, y, w, h, material, parent=(0.0, 0.0), uvs=None, **kw):
    p = {"type": "picture", "name": name, "size": [round(w, 3), round(h, 3)], "translate": place(x, y, w, h, parent),
         "material": material}
    if uvs:
        p["uvs"] = [uvs]
    p.update(kw)
    return p


def null(name, x, y, w, h, children, parent=(0.0, 0.0), **kw):
    p = {"type": "null", "name": name, "size": [round(w, 3), round(h, 3)], "translate": place(x, y, w, h, parent),
         "influence_alpha": True, "children": children}
    p.update(kw)
    return p


def three_slice(name, x, y, w, h, material, size, parent, **kw):
    """A plate picture (size: its pixels) as a null pane with 3 pictures: the ends keep their shape (3/4 of
    the height each), the middle stretches."""
    pw_, ph_ = size
    cap = min(w / 2, h * 0.75)
    share = cap / h * ph_ / pw_
    c = centre(x, y, w, h)
    return null(name, x, y, w, h, [
        pic(name + "L", x, y, cap, h, material, c, [[0, 0], [share, 0], [0, 1], [share, 1]]),
        pic(name + "M", x + cap, y, w - 2 * cap, h, material, c, [[share, 0], [1 - share, 0], [share, 1], [1 - share, 1]]),
        pic(name + "R", x + w - cap, y, cap, h, material, c, [[1 - share, 0], [1, 0], [1 - share, 1], [1, 1]])], parent, **kw)


def centre(x, y, w, h):
    return (x + w / 2 - W / 2, H / 2 - (y + h / 2))


def text(name, x, y, size, colour, value="", bold=False, width=None, align="left", parent=(0.0, 0.0), capacity=96, k=1.2,
         square=False, **kw):
    """A text pane whose text starts at the page's (x, y) (top left of the text) in the given font size."""
    # k: the game font's glyphs are smaller in their cell than the HUD's at the same size (measured
    # against the HUD page: x-height 1.2 - 1.3 times, hudref); the HUD's letters are narrower, so the
    # width grows only to 1.04 (lines as long as the HUD's: detail helper's measurement); square: the
    # icon font's round buttons
    h = size * 1.35 * k
    w = width if width is not None else 600
    pos = {"left": ["left", "top"], "center": ["center", "top"], "right": ["right", "top"]}[align]
    # the game's own text panes: the letters take the material's white colour, the font's border its black
    # colour (alpha 0: no border); the text colours stay white. Bold (the HUD's menu labels): a light border
    # around dark letters, a dark one around light letters. Material colours are linear, the HUD's sRGB.
    fill = rgba(colour)
    light = sum(fill[:3]) > 3 * 160
    # The game's text material mixes black -> white by the glyph's value: the letters (full value) take
    # white, the font's border (about half value) a mix of both. Plain text: black = transparent white, as
    # the game's own plain panes (ffffff00): the border fades out on the paper (black = the ink at alpha 0
    # gives the border the ink at half alpha: heavy letters). Bold: the HUD's menu-label border.
    # plain letters: the game's own plain convention (border transparent white); a light-ink border made
    # names heavy and smeared (lead's decision after the crispness check)
    border = ([44, 58, 46, 255] if light else [255, 255, 255, 255]) if bold else [255, 255, 255, 0]
    p = {"type": "text", "name": name, "size": [round(w, 3), round(h, 3)], "translate": place(x, y, w, h, parent),
         # the HUD page draws all its text in the game's menu font (story_notebook_font.h)
         "font": "CKingMain.bffnt", "font_size": [round(size * min(k, 1.04) * (1.0 if square else 0.92), 2), round(size * k, 2)],
         "text": value, "capacity": capacity, "text_position": pos, "line_alignment": align,
         "text_material": {"black": linear(border), "white": linear(fill)}}
    p.update(kw)
    return p


# ---- our own small pictures (drawn here) ----
def write_small_art(out):
    Image.new("RGBA", (8, 8), (255, 255, 255, 255)).save(os.path.join(out, "white.png"))
    s = 64
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([1, 1, s - 2, s - 2], fill=tuple(rgba(0xD9822BFF)))
    d.ellipse([s * 0.19, s * 0.19, s * 0.81, s * 0.81], outline=(255, 255, 255, 255), width=6)
    im.save(os.path.join(out, "ring.png"))
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = tuple(rgba(INK_SOFT))
    d.line([10, 10, s - 11, s - 11], fill=c, width=7)
    d.line([s - 11, 10, 10, s - 11], fill=c, width=7)
    im.save(os.path.join(out, "cross.png"))
    # the cursor's corner bracket (top-left; the other corners use flipped texture coordinates)
    s = 32
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    brown = (70, 45, 20, 255)  # the HUD's cursor: solid dark brown brackets
    d.rectangle([0, 0, s - 1, 7], fill=brown)
    d.rectangle([0, 0, 7, s - 1], fill=brown)
    im.save(os.path.join(out, "corner.png"))


TEXELS_PER_UNIT = 2.0  # texture texels per layout unit (1 unit = 1 px of the 1280 x 720 TV at scale 1)


def fit_textures_to_display(textures, materials, panes, out):
    """Every texture at most TEXELS_PER_UNIT times its largest use on the page (the pane size over the
    texture coordinates' span): the art's full size would not fit the game's layout memory, and twice the
    display size is sharp up to resolution scale 2 on the TV and 3 on the GamePad."""
    mat_tex = {m: [t["texture"] for t in v.get("textures", [])] for m, v in materials.items()}
    need = {}

    def walk(ps):
        for p in ps:
            yield p
            yield from walk(p.get("children", []))
    for p in walk(panes):
        for t in mat_tex.get(p.get("material"), []):
            uv = p.get("uvs", [[[0, 0], [1, 0], [0, 1], [1, 1]]])[0]
            us = abs(uv[1][0] - uv[0][0]) or 1.0
            vs = abs(uv[2][1] - uv[0][1]) or 1.0
            sx, sy = p.get("scale", [1, 1])
            w, h = p["size"][0] * abs(sx) / us, p["size"][1] * abs(sy) / vs
            nw, nh = need.get(t, (0, 0))
            need[t] = (max(nw, w), max(nh, h))
    done = set()
    for t, spec in textures.items():
        png = spec.get("png")
        if t not in need or not png or png in done or "portrait" in png:
            continue
        path = os.path.join(out, png)
        im = Image.open(path)
        k = max(need[t][0] * TEXELS_PER_UNIT / im.width, need[t][1] * TEXELS_PER_UNIT / im.height)
        if k < 1.0:
            im.convert("RGBA").resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS).save(path)
        done.add(png)


BC_FROM = 256 * 256  # textures with at least this many texels are block-compressed (BC1 opaque, BC3 with alpha)


def compress_large_textures(textures, out):
    """The large pictures (frame, sheet, banner, card, bands, ...) as BC1 / BC3, which the port encodes
    when it builds the archive: a quarter (BC3) or an eighth (BC1) of RGBA8, so the archive stays well
    under the layout cache's limit. Small and runtime-written pictures (portrait slots) stay RGBA8."""
    for t, spec in textures.items():
        png = spec.get("png")
        if not png or "portrait" in png or spec.get("format", "rgba8") != "rgba8":
            continue
        im = Image.open(os.path.join(out, png)).convert("RGBA")
        if im.width * im.height < BC_FROM:
            continue
        opaque = im.getchannel("A").getextrema()[0] == 255
        spec["format"] = "bc1" if opaque else "bc3"


def fit_texture(im, limit=2048):
    """A picture at its own size, scaled down only to fit the texture limit (2048)."""
    if max(im.size) <= limit:
        return im
    k = limit / max(im.size)
    return im.resize((round(im.width * k), round(im.height * k)), Image.LANCZOS)


def clean_edges(im, min_alpha=96, crop=True):
    """story_notebook_art.h clean_edges: texels under min_alpha cleared, cropped to what is left."""
    im = im.convert("RGBA")
    px = im.load()
    w, h = im.size
    x0, y0, x1, y1 = w, h, 0, 0
    for y in range(h):
        for x in range(w):
            if px[x, y][3] >= min_alpha:
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x + 1), max(y1, y + 1)
            else:
                px[x, y] = (0, 0, 0, 0)
    return im.crop((x0, y0, x1, y1)) if crop else im


def painted_piece(old, box):
    """A piece painted on the old frame (its tab plates, the close button's slot): the shape its dark
    outline encloses inside `box` (frame picture pixels), outline included; (RGBA picture, its box)."""
    import numpy as np
    from PIL import ImageFilter
    x0, y0, x1, y1 = box
    o = np.asarray(old.crop(box).convert("RGBA")).astype(int)
    r, g, b = o[..., 0], o[..., 1], o[..., 2]
    outline = (np.maximum(np.maximum(r, g), b) < 90) & (g >= r)  # the plates' dark green outline
    m = Image.fromarray(outline.astype(np.uint8) * 255, "L").filter(ImageFilter.MaxFilter(3))
    # what the outside (flood-filled from the box's corner) doesn't reach is the piece
    inv = Image.fromarray(255 - np.asarray(m), "L").copy()  # a writable copy for floodfill
    ImageDraw.floodfill(inv, (0, 0), 128)
    inside = np.asarray(inv) != 128
    # only the part around the box's centre (strays of a neighbour's outline go)
    part = Image.fromarray(inside.astype(np.uint8) * 255, "L").copy()
    ImageDraw.floodfill(part, (part.width // 2, part.height // 2), 128)
    inside = np.asarray(part) == 128
    mask = np.asarray(Image.fromarray(inside.astype(np.uint8) * 255, "L").filter(ImageFilter.GaussianBlur(0.6)))
    ys, xs = np.nonzero(mask > 8)
    bx0, by0, bx1, by1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
    rgba = o.astype(np.uint8)
    rgba[..., 3] = (mask.astype(int) * o[..., 3] // 255).astype(np.uint8)
    piece = Image.fromarray(rgba[by0:by1, bx0:bx1], "RGBA")
    return piece, (x0 + bx0, y0 + by0, x0 + bx1, y0 + by1)


def painted_plates(frame):
    """The bottom Close / Open plates as the frame picture paints them: the boxes of their dark green
    outlines (rows / columns mostly outline) in the bottom left and right; R in the picture's shares."""
    import numpy as np
    o = np.asarray(frame.convert("RGBA")).astype(int)
    h, w = o.shape[:2]
    found = []
    for x0, x1 in ((0.03, 0.36), (0.62, 0.98)):
        X0, X1, Y0, Y1 = int(x0 * w), int(x1 * w), int(0.80 * h), int(0.98 * h)
        c = o[Y0:Y1, X0:X1]
        r, g, b, a = c[..., 0], c[..., 1], c[..., 2], c[..., 3]
        m = (np.maximum(np.maximum(r, g), b) < 90) & (g >= r) & (a > 200)
        rows = [y for y in range(m.shape[0]) if m[y].sum() > m.shape[1] * 0.4]
        cols = [x for x in range(m.shape[1]) if m[:, x].sum() > m.shape[0] * 0.3]
        if not rows or not cols:
            return None
        found.append(R(X0 + cols[0], Y0 + rows[0], X0 + cols[-1] + 1, Y0 + rows[-1] + 1, w, h))
    return found


def prepare_art(art, out):
    """The frame (frame_notabs.png when the art has it: the frame without its tab plates and close button,
    which then become pieces of their own, cut out of frame.png), the row banner."""
    old = clean_edges(Image.open(os.path.join(art, "frame.png")), crop=False)
    pieces = {}
    notabs = os.path.join(art, "frame_notabs.png")
    user_buttons = all(os.path.exists(os.path.join(art, n + ".png")) for n in ("tab_plate_a", "tab_plate_b", "close_round"))
    if os.path.exists(notabs):
        frame = Image.open(notabs).convert("RGBA")
        assert frame.size == old.size, "frame_notabs.png must match frame.png"
        for name, r, margin in [] if user_buttons else (("tab0", FR_TAB0, 8), ("tab1", FR_TAB1, 8), ("close", FR_SLOT, 4)):
            box = (round(r.x0 * old.width) - margin, round(r.y0 * old.height) - margin,
                   round(r.x1 * old.width) + margin, round(r.y1 * old.height) + margin)
            piece, at = painted_piece(old, box)
            piece.save(os.path.join(out, name + ".png"))
            pieces[name] = at
    else:
        frame = old
    frame.save(os.path.join(out, "frame.png"))
    banner = fit_texture(clean_edges(Image.open(os.path.join(art, "banner.png"))))  # its own size
    banner.save(os.path.join(out, "banner.png"))
    painted = painted_plates(frame)  # where this frame paints its bottom plates (None: it has none)
    return frame.size, banner.size, pieces, painted


def build(art, out_dir):
    out = os.path.join(out_dir, "layout")
    os.makedirs(out, exist_ok=True)
    write_small_art(out)
    (frame_w, frame_h), (banner_w, banner_h), pieces, painted = prepare_art(art, out)
    # the bottom plates: where the frame paints them, else at the old frame's plates (the page's own touch
    # boxes, story_notebook_page.h geo()), drawn as elements of their own
    plate_l, plate_r = painted or (FR_PLATE_L, FR_PLATE_R)
    tex = lambda png: {"png": png, "tile": "tiled"}
    # portrait slots (64 x 64, transparent): the port writes each quest's picture into them at run time
    Image.new("RGBA", (PORTRAIT, PORTRAIT), (0, 0, 0, 0)).save(os.path.join(out, "portrait_slot.png"))
    textures = {"NbFrame_00": tex("frame.png"), "NbBanner_00": tex("banner.png"), "NbWhite_00": tex("white.png"),
                "NbRing_00": tex("ring.png"), "NbCross_00": tex("cross.png"), "NbCorner_00": tex("corner.png")}
    for i in range(PORTRAIT_SLOTS):
        textures["NbPortrait%d_00" % i] = tex("portrait_slot.png")
    mat = lambda t, white=None: {"textures": [{"texture": t}], **({"white": white} if white else {})}
    materials = {"M_Frame": mat("NbFrame_00"), "M_Banner": mat("NbBanner_00"), "M_Ring": mat("NbRing_00"),
                 "M_Cross": mat("NbCross_00"), "M_Corner": mat("NbCorner_00"),
                 "M_Shade": mat("NbWhite_00", rgba(SHADE)), "M_Wash": mat("NbWhite_00", [255, 255, 255, 0x30]),
                 "M_TabOn": mat("NbWhite_00", [255, 255, 255, 0x50]), "M_TabOff": mat("NbWhite_00", [0x1E, 0x3C, 0x14, 0x30]),
                 "M_PillNew": mat("NbWhite_00", [0x2E, 0x8A, 0x2E, 0xC8]), "M_PillNotYet": mat("NbWhite_00", [0x7C, 0x8A, 0x6C, 0xD8]),
                 "M_PillDone": mat("NbWhite_00", [0x9C, 0x3A, 0x2A, 0xD0])}
    for i in range(PORTRAIT_SLOTS):
        materials["M_Portrait%d" % i] = mat("NbPortrait%d_00" % i)
    buttons = {}
    if all(os.path.exists(os.path.join(art, n + ".png")) for n in ("tab_plate_a", "tab_plate_b", "close_round")):
        for key, n in (("a", "tab_plate_a"), ("b", "tab_plate_b"), ("close", "close_round")):
            im = Image.open(os.path.join(art, n + ".png")).convert("RGBA")
            im.save(os.path.join(out, n + ".png"))
            buttons[key] = (n, im.size)
        textures.update({"NbTabA_00": tex("tab_plate_a.png"), "NbTabB_00": tex("tab_plate_b.png"),
                         "NbCloseRound_00": tex("close_round.png")})
        # the lit tab: plate b as painted; the shaded one: plate a, darker
        materials.update({"M_TabPlateOn": mat("NbTabB_00"), "M_TabPlateOff": mat("NbTabA_00", [200, 212, 200, 255]),
                          "M_PlateA": mat("NbTabA_00"),
                          "M_CloseRound": mat("NbCloseRound_00")})
        pieces = {}  # the user's buttons replace the pieces cut out of the old frame
    for name in pieces:
        tname = "Nb%s_00" % name.capitalize()
        textures[tname] = tex(name + ".png")
        materials["M_" + name.capitalize()] = mat(tname)

    fx, fy, fw, fh = frame_box()
    page_c = centre(fx, fy, fw, fh)  # N_Page sits at the screen centre (scales around it)
    page_c = (0.0, 0.0)
    g = geo()
    cont = g["content"]
    content_children = []
    rows = []
    for i in range(ROWS):
        x, w = g["list_x"], g["list_w"]
        y, h = g["list_y"] + g["row_h"] * i + 2, g["row_h"] - 4
        rc = centre(x, y, w, h)
        k = h / banner_h * banner_w  # the banner's width at this height (art: its own aspect)
        lw, rw = B_LEFT * k, (1 - B_RIGHT) * k
        kids = [pic("P_Row%dL" % i, x, y, lw, h, "M_Banner", rc, [[0, 0], [B_LEFT, 0], [0, 1], [B_LEFT, 1]]),
                pic("P_Row%dM" % i, x + lw, y, w - lw - rw, h, "M_Banner", rc, [[B_LEFT, 0], [B_RIGHT, 0], [B_LEFT, 1], [B_RIGHT, 1]]),
                pic("P_Row%dR" % i, x + w - rw, y, rw, h, "M_Banner", rc, [[B_RIGHT, 0], [1, 0], [B_RIGHT, 1], [1, 1]])]
        bx = row_x(B_TEXT.x0 - 0.02, x, w, h)
        kids.append(pic("P_Row%dWash" % i, bx, y + h * 0.12, x + w - h * 0.25 - bx, h * 0.76, "M_Wash", rc, visible=False))
        mcx, mcy, mr = row_x(B_MED_X, x, w, h), y + B_MED_Y * h, B_MED_R * h * 0.86
        kids.append(pic("P_Row%dPortrait" % i, mcx - mr, mcy - mr, 2 * mr, 2 * mr, "M_Portrait%d" % i, rc, visible=False))
        kids.append(text("T_Row%dInitial" % i, mcx - mr, mcy - mr * 0.68, mr * 1.3, SEAL, "?", bold=True, width=2 * mr,
                         align="center", parent=rc, capacity=4, k=1.0))
        tx, px0, px1 = row_x(B_TEXT.x0, x, w, h), row_x(B_PILL.x0, x, w, h), row_x(B_PILL.x1, x, w, h)
        tw = px0 - tx - 20
        for dim, colour in (("", INK), ("Dim", INK_SOFT)):
            kids.append(text("T_Row%dTitle%s" % (i, dim), tx, y + h * 0.13, 25, colour, bold=True, width=tw, parent=rc,
                             visible=False))
            kids.append(text("T_Row%dNext%s" % (i, dim), tx, y + h * 0.47, 19, colour, width=tw, parent=rc, visible=False, k=1.2))
        py, ph = y + B_PILL.y0 * h, (B_PILL.y1 - B_PILL.y0) * h
        for name in ("New", "NotYet", "Done"):
            kids.append(pic("P_Row%dPill%s" % (i, name), px0 + 2, py + 2, px1 - px0 - 4, ph - 4, "M_Pill" + name, rc, visible=False))
        kids.append(text("T_Row%dStatus" % i, px0, py + (ph - 17) / 2, 17, WHITE, bold=True, width=px1 - px0, align="center",
                         parent=rc, capacity=24, k=1.0))
        kids.append(pic("P_Row%dRing" % i, px0 - 18 - 12, py + ph / 2 - 12, 24, 24, "M_Ring", rc, visible=False))
        rows.append(null("N_Row%d" % i, x, y, w, h, kids, page_c, visible=False))
    content_children += rows
    # the empty list
    content_children.append(text("T_Empty1", cont[0] + 40, cont[1] + 60, 30, INK_SOFT, width=cont[2] - 80, visible=False, k=1.18))
    content_children.append(text("T_Empty2", cont[0] + 40, cont[1] + 104, 24, INK_SOFT, width=cont[2] - 80, visible=False, k=1.18))
    # scroll marks
    content_children.append(text("T_Up", g["list_x"] + g["list_w"] / 2 - 20, g["list_y"] - 26, 22, INK_SOFT, "^", width=40,
                                 align="center", visible=False, capacity=4))
    content_children.append(text("T_Down", g["list_x"] + g["list_w"] / 2 - 20, g["list_y"] + g["row_h"] * ROWS - 6, 22, INK_SOFT,
                                 "v", width=40, align="center", visible=False, capacity=4))
    # the cursor: four corner brackets around the selected row (N_Cursor moves to the row)
    cx, cy, cw, chh = g["list_x"] + 4, g["list_y"] + 2 + 2, g["list_w"] - 8, g["row_h"] - 8
    b = 4 + 2
    cur_c = centre(cx, cy, cw, chh)
    cs = 22
    corners = []
    for name, (ux0, ux1, uy0, uy1), (ox, oy) in (("TL", (0, 1, 0, 1), (cx - b, cy - b)), ("TR", (1, 0, 0, 1), (cx + cw + b - cs, cy - b)),
                                                 ("BL", (0, 1, 1, 0), (cx - b, cy + chh + b - cs)),
                                                 ("BR", (1, 0, 1, 0), (cx + cw + b - cs, cy + chh + b - cs))):
        corners.append(pic("P_Cursor" + name, ox, oy, cs, cs, "M_Corner", cur_c, [[ux0, uy0], [ux1, uy0], [ux0, uy1], [ux1, uy1]]))
    content_children.append(null("N_Cursor", cx, cy, cw, chh, corners, page_c, visible=False))
    content = null("N_Content", 0, 0, W, H, content_children, page_c)

    # the frame, title, close mark, tabs, plates
    kids = [pic("P_Frame", fx, fy, fw, fh, "M_Frame", page_c)]
    tx, ty, tw, th = frame_part(FR_TITLE)
    kids.append(text("T_Title", tx, ty, 44, INK, "Story Notebook", bold=True, width=tw + 200, capacity=32, k=1.1))
    def piece_box(name):  # a piece's place on the page (its box in the frame picture's pixels)
        x0, y0, x1, y1 = pieces[name]
        return fx + x0 / frame_w * fw, fy + y0 / frame_h * fh, (x1 - x0) / frame_w * fw, (y1 - y0) / frame_h * fh
    sx, sy, sw, sh = frame_part(FR_SLOT)
    if buttons:  # the user's own buttons: the round close button with the game's x on it
        d = sw + 6
        kids.append(pic("P_CloseButton", sx + sw / 2 - d / 2, sy + sh / 2 - d / 2, d, d, "M_CloseRound", page_c))
        kids.append(text("T_CloseX", sx, sy + sh / 2 - 24, 44, INK_SOFT, "\u00d7", bold=True, width=sw, align="center", capacity=4,
                         k=1.0))
    else:
        if "close" in pieces:  # the close button's round slot as an element of its own (the frame has none)
            kids.append(pic("P_CloseButton", *piece_box("close"), "M_Close", page_c))
        r = sw / 2 * 0.42 * 2 + 6
        kids.append(pic("P_Close", sx + sw / 2 - r / 2, sy + sh / 2 - r / 2, r, r, "M_Cross", page_c))
    for i, fr in enumerate((FR_TAB0, FR_TAB1)):
        x, y, w, h = frame_part(fr)
        if buttons:  # the user's tab plates, 3-slice (the curled ends keep their shape): b lit, a shaded
            tc = centre(x, y, w, h)
            for state, (plate, (pw_, ph_)) in (("On", buttons["b"]), ("Off", buttons["a"])):
                cap = min(w / 2, h * 0.75)  # each end, as wide as 3/4 of the height
                share = cap / h * ph_ / pw_  # the end's share of the plate picture's width
                m = "M_TabPlate" + state
                kids.append(null("P_Tab%d%s" % (i, state), x, y, w, h, [
                    pic("P_Tab%d%sL" % (i, state), x, y, cap, h, m, tc, [[0, 0], [share, 0], [0, 1], [share, 1]]),
                    pic("P_Tab%d%sM" % (i, state), x + cap, y, w - 2 * cap, h, m, tc,
                        [[share, 0], [1 - share, 0], [share, 1], [1 - share, 1]]),
                    pic("P_Tab%d%sR" % (i, state), x + w - cap, y, cap, h, m, tc, [[1 - share, 0], [1, 0], [1 - share, 1], [1, 1]])],
                    page_c, visible=state == "Off"))
        else:
            if "tab%d" % i in pieces:  # the tab plate as an element of its own
                kids.append(pic("P_Tab%dPlate" % i, *piece_box("tab%d" % i), "M_Tab%d" % i, page_c))
            kids.append(pic("P_Tab%dOn" % i, x + 6, y + 6, w - 12, h - 12, "M_TabOn", page_c, visible=False))
            kids.append(pic("P_Tab%dOff" % i, x + 6, y + 6, w - 12, h - 12, "M_TabOff", page_c))
        for state, colour in (("On", PLATE_INK), ("Off", INK_SOFT)):
            kids.append(text("T_Tab%d%s" % (i, state), x, y + (h - 25) / 2, 25, colour, ("Quests", "Archive")[i], bold=True,
                             width=w, align="center", visible=state == "Off", capacity=24))
    plates = []
    for name, fr, glyph, label in (("Back", plate_l, "", "Close"), ("Track", plate_r, "", "Open")):
        x, y, w, h = frame_part(fr)
        pc = centre(x, y, w, h)
        size = 34
        pw = size + 6 + len(label) * size * 0.8 * 0.44  # the label's width as the HUD measures it
        px, py = x + (w - pw) / 2, y + (h - size) / 2 - 2
        plate = []
        if buttons and not painted:  # the frame has no plates: the user's plate (A's plate lit, B's plain)
            key, m = ("b", "M_TabPlateOn") if name == "Track" else ("a", "M_PlateA")
            plate.append(three_slice("P_Plate" + name, x, y, w, h, m, buttons[key][1], pc))
        plates.append(null("N_Plate" + name, x, y, w, h, plate + [
            text("T_%sIcon" % name, px, py, size, WHITE, glyph, width=size + 4, parent=pc, capacity=4, font="CKingPic.bffnt",
                 k=1.0, square=True),
            text("T_%sLabel" % name, px + size + 6, py + size * 0.1, size * 0.8, INK, label, bold=True, width=w, parent=pc,
                 capacity=24)], page_c))
    # the detail view (make_notebook_detail.py, when present): N_Detail above the frame, under the plates;
    # its panes, textures, materials, groups and animations come from that module
    detail = None
    try:
        import make_notebook_detail
    except ImportError:
        make_notebook_detail = None
    if make_notebook_detail:
        detail = make_notebook_detail.build_detail(sys.modules[__name__], art, out)
        textures.update(detail.get("textures", {}))
        materials.update(detail.get("materials", {}))
        kids.append(null("N_Detail", 0, 0, W, H, detail["panes"], page_c, visible=False))
        for plate in plates:  # the A plate's chart scroll and cursor brackets on the detail page
            if plate["name"] == "N_PlateTrack":
                plate["children"] += detail.get("track_plate", [])
    kids += plates
    kids.append(content)
    page = null("N_Page", 0, 0, W, H, kids)
    shade = pic("P_Shade", -400, -200, 2080, 1120, "M_Shade")
    root = null("N_Notebook", 0, 0, W, H, [null("N_Shade", 0, 0, W, H, [shade]), page])

    groups = {"G_Open": ["N_Shade", "N_Page"], "G_Content": ["N_Content"], "G_Back": ["N_PlateBack"], "G_Track": ["N_PlateTrack"],
              "G_Cursor": ["P_CursorTL", "P_CursorTR", "P_CursorBL", "P_CursorBR"]}
    for i in range(ROWS):
        groups["G_Row%d" % i] = ["N_Row%d" % i]

    f = lambda s: max(1, round(s * FPS))
    open_n, close_n, turn_n, press_n, bump_n = f(0.22), f(0.15), f(0.20), f(0.16), f(0.18)

    def ease_out_back(t):
        c, u = 1.6, t - 1.0
        return 1.0 + (c + 1.0) * u ** 3 + c * u * u

    def ease_out(t):
        return 1 - (1 - t) ** 3

    def sampled(fn, n):
        return [[k, round(fn(k / n), 4)] for k in range(n + 1)]

    scale_open = sampled(lambda t: 0.92 + 0.08 * ease_out_back(t), open_n)
    fade_open = sampled(lambda t: 255 * min(1.0, t * 1.6), open_n)
    anims = {
        "Open": {"frames": open_n, "groups": ["G_Open"], "tracks": [
            {"pane": "N_Page", "target": "sx", "keys": scale_open, "interp": "linear"},
            {"pane": "N_Page", "target": "sy", "keys": scale_open, "interp": "linear"},
            {"pane": "N_Page", "target": "alpha", "keys": fade_open},
            {"pane": "N_Shade", "target": "alpha", "keys": fade_open}]},
        "Close": {"frames": close_n, "groups": ["G_Open"], "tracks": [
            {"pane": "N_Page", "target": "sx", "keys": [[0, 1], [close_n, 0.95]]},
            {"pane": "N_Page", "target": "sy", "keys": [[0, 1], [close_n, 0.95]]},
            {"pane": "N_Page", "target": "alpha", "keys": [[0, 255], [close_n, 0]]},
            {"pane": "N_Shade", "target": "alpha", "keys": [[0, 255], [close_n, 0]]}]},
    }
    for name, d in (("TurnRight", 1), ("TurnLeft", -1)):
        anims[name] = {"frames": turn_n, "groups": ["G_Content"], "tracks": [
            {"pane": "N_Content", "target": "tx", "keys": sampled(lambda t: d * (1 - ease_out(t)) * 70, turn_n)},
            {"pane": "N_Content", "target": "alpha", "keys": sampled(lambda t: 255 * ease_out(t), turn_n)}]}
    press = sampled(lambda t: 1.0 - 0.10 * math.sin(t * math.pi), press_n)
    for name in ("Back", "Track"):
        anims["Press" + name] = {"frames": press_n, "groups": ["G_" + name], "tracks": [
            {"pane": "N_Plate" + name, "target": "sx", "keys": press}, {"pane": "N_Plate" + name, "target": "sy", "keys": press}]}
    bump = sampled(lambda t: 1.0 + 0.02 * math.sin(t * math.pi), bump_n)
    for i in range(ROWS):
        anims["Bump%d" % i] = {"frames": bump_n, "groups": ["G_Row%d" % i], "tracks": [
            {"pane": "N_Row%d" % i, "target": "sx", "keys": bump}, {"pane": "N_Row%d" % i, "target": "sy", "keys": bump}]}
    # the cursor breathes: its brackets move out and back (2 to 6 px), 0.9 times a second
    breathe_n = f(1 / 0.9)
    off = lambda t: 2.0 * math.sin(t * 2 * math.pi)
    tracks = []
    for name, (sxn, syn) in (("TL", (-1, 1)), ("TR", (1, 1)), ("BL", (-1, -1)), ("BR", (1, -1))):
        pane = next(p for p in corners if p["name"] == "P_Cursor" + name)
        bx_, by_ = pane["translate"]
        tracks.append({"pane": pane["name"], "target": "tx", "keys": sampled(lambda t: bx_ + sxn * off(t), breathe_n)})
        tracks.append({"pane": pane["name"], "target": "ty", "keys": sampled(lambda t: by_ + syn * off(t), breathe_n)})
    anims["Breathe"] = {"frames": breathe_n, "loop": True, "groups": ["G_Cursor"], "tracks": tracks}

    if detail:
        groups["G_Detail"] = ["N_Detail"]
        groups.update(detail.get("groups", {}))
        # the page turns into the detail view (from the right) and back (to the left), as the list's turn
        for name, d in (("DetailIn", 1), ("DetailOut", -1)):
            anims[name] = {"frames": turn_n, "groups": ["G_Detail"], "tracks": [
                {"pane": "N_Detail", "target": "tx", "keys": sampled(lambda t: d * (1 - ease_out(t)) * 70, turn_n)},
                {"pane": "N_Detail", "target": "alpha", "keys": sampled(lambda t: 255 * ease_out(t) if d > 0 else 255, turn_n)}]}
        anims.update(detail.get("animations", {}))
    fit_textures_to_display(textures, materials, [root], out)
    compress_large_textures(textures, out)
    desc = {"name": "StoryNotebook_00", "size": [W, H], "textures": textures, "materials": materials, "panes": [root],
            "groups": groups, "animations": anims}
    with open(os.path.join(out, "notebook.json"), "w") as fh:
        json.dump(desc, fh, indent=1, ensure_ascii=False)
    return desc


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    d = build(sys.argv[1], sys.argv[2])
    print("layout/notebook.json: %d textures, %d animations" % (len(d["textures"]), len(d["animations"])))
