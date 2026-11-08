#!/usr/bin/env python3
"""The story notebook detail view's mini sea chart as panes of the notebook's own layout (local only,
story mods are WIP).

    build_chart(nb, art, out, card_box, parent=None) -> dict(panes, textures, materials, groups, animations)

nb        the make_notebook_layout module (its pane helpers pic / null / text / centre)
art       the painted art folder (unused: the chart draws only pictures made here and one game texture)
out       the layout folder (gets the small pictures drawn here: disc, ring, line, compass)
card_box  the chart card's page box (x, y, w, h; 1280 x 720 page pixels): frame_part(art::detail::kCard)
parent    the pane the chart goes into: its layout centre and rotation (x, y) or (x, y, degrees,
          counter-clockwise), as nb.centre gives it (its ancestors unrotated); default: the card box's
          centre. In the detail page: D_ChartSlot (make_notebook_detail.py card_sea(): on the sea, tilted).
          C_Chart undoes the parent's rotation, so the chart is placed by this module alone (the exact
          bilinear sea below), whatever the slot's own fit

The HUD's mini chart (runtime/src/mods/story_notebook_page.h card_chart) maps a share (u, v) of the 7 x 7
squares (chart_share) bilinearly onto the card's tilted blue sea (story_notebook_art.h card::kTL..kBR, as
shares of the card picture), inset by 8 % on each side. Every line of that grid is straight (a bilinear map
keeps the lines u = const and v = const straight), so each grid line here is a thin picture pane rotated
onto the HUD's exact line: the grid matches the HUD to the pixel, unlike one null pane whose rotation and
scale approximate the quad (its affine fit is 1.7 px off at worst, 0.5 px on average, and needs a shear a
pane's rotate + scale cannot express; fit_report()). The icons stay upright and round as on the HUD: the
runtime (story_notebook_layout_chart.cpp) moves them every frame to the bilinear place of Link and of the
quest's place, from the four corner panes C_SeaTL / C_SeaTR / C_SeaBL / C_SeaBR (the inset quad, in
C_Chart's coordinates).

Panes (all named C_*), drawn in the HUD's order:
  C_Chart                the chart (a null pane over the card box)
    C_SeaTL .. C_SeaBR   the inset sea quad's corners (hidden null panes, read by the runtime)
    C_Grid               C_GridU0..7 (lines u = i/7), C_GridV0..7 (lines v = i/7)
    C_Place              the quest's place (moved, shown by the runtime): C_Glow (pulses: ChartGlow),
                         C_Ring, C_PlaceDot
    C_Route              C_Dot00..C_Dot39: the route's dots (a pool, moved and shown by the runtime)
    C_Link               Link (moved, shown by the runtime): C_LinkFace, the sea chart's own Link icon
                         (the game's texture MapLinkS_00^l from its resident archive LinkPos_00, attached
                         to the notebook's parts by name: no game data in the package); C_LinkMark, our own
                         marker in the HUD fallback's look, shown only when that texture isn't there
    C_Compass            C_CompassRose (a picture drawn here), C_CompassN (the "N")
Animation ChartGlow (group G_ChartGlow = C_Glow): the glow's radius 15 + 4 * pulse, 0.8 times a second,
looping (the HUD's pulse).
"""
import math
import os

from PIL import Image, ImageDraw

# the chart card: the corners of its blue sea as shares of the card picture (story_notebook_art.h card::)
CARD_TL, CARD_TR, CARD_BL, CARD_BR = (0.124, 0.063), (0.960, 0.193), (0.044, 0.783), (0.898, 0.942)
INSET = 0.08  # card_chart's inset: u = 0.08 + 0.84 * u
SQUARES = 7
DOTS = 40  # the route's dot pool (the longest route across the inset sea is ~ 33 dots, one per 9 px)
GAME_LINK_TEXTURE = "MapLinkS_00^l.bflim"  # the sea chart's Link icon (LinkPos_00.szs, P_MapLinkFace_00)
FPS = 30

# the HUD's colours (story_notebook_page.h card_chart, sRGB RGBA)
GRID, GLOW, RED, WHITE = 0xFFFFFF70, 0xFFF4B0A0, 0xD12E1EFF, 0xFFFFFFFF
COMPASS_FILL, COMPASS_INK, COMPASS_N = 0xF4E9C8D0, 0x5A7F9AFF, 0x2C5C7CFF
LINK_FALLBACK = 0x2E8A2EFF  # style::shapes "nb:linkface" without the icon: a green disc of 0.32 x the box


def rgba(c):
    return [(c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, c & 255]


def sea_at(card_box, u, v, inset=True):
    """card_chart's at(): a share of the 7 x 7 squares to the page (pixels)."""
    x, y, w, h = card_box
    if inset:
        u, v = INSET + (1 - 2 * INSET) * u, INSET + (1 - 2 * INSET) * v
    tx, ty = CARD_TL[0] + (CARD_TR[0] - CARD_TL[0]) * u, CARD_TL[1] + (CARD_TR[1] - CARD_TL[1]) * u
    bx, by = CARD_BL[0] + (CARD_BR[0] - CARD_BL[0]) * u, CARD_BL[1] + (CARD_BR[1] - CARD_BL[1]) * u
    return x + (tx + (bx - tx) * v) * w, y + (ty + (by - ty) * v) * h


def fit_report(card_box):
    """How well one null pane (an affine map of its local square) fits the HUD's bilinear sea: the least
    squares affine map over a 15 x 15 sample, its worst and mean error in page pixels, and its singular
    values / rotations (an R * S * R: a single pane's rotate + scale is R * S, so the shear needs two)."""
    import numpy as np
    P, Q = [], []
    for u in np.linspace(0, 1, 15):
        for v in np.linspace(0, 1, 15):
            P.append([u, v, 1])
            Q.append(sea_at(card_box, u, v))
    P, Q = np.array(P), np.array(Q)
    A = np.linalg.lstsq(P, Q, rcond=None)[0]
    err = np.linalg.norm(P @ A - Q, axis=1)
    U, S, Vt = np.linalg.svd(A[:2].T)
    deg = lambda m: math.degrees(math.atan2(m[1, 0], m[0, 0]))
    return {"max_px": float(err.max()), "mean_px": float(err.mean()), "scale": [float(s) for s in S],
            "rotate_outer": deg(U), "rotate_inner": deg(Vt)}


# ---- our own small pictures (drawn here, 4x supersampled) ----
def _supersampled(size, draw, k=4):
    im = Image.new("RGBA", (size * k, size * k), (0, 0, 0, 0))
    draw(ImageDraw.Draw(im), size * k, k)
    return im.resize((size, size), Image.LANCZOS)


# The pictures are drawn at about twice the size they are shown at: the layout samples them bilinearly
# without mipmaps, so a picture much larger than its pane aliases (a 64 px disc shown at 4 px is a square).
DISCS = {"dot": 9, "spot": 16, "glow": 68, "mark": 41}  # the route's dots (4.4 px), the place's dot (8),
#                                                         the glow (34 at scale 1), the fallback marker (20.5)


def write_chart_art(out):
    # white discs (one per use, see DISCS) and a white ring (the place's ring: radius 9, 3 px wide, so 10.5
    # outside and 7.5 inside; 21 px shown, 42 drawn)
    for name, size in DISCS.items():
        _supersampled(size, lambda d, s, k: d.ellipse([0, 0, s - 1, s - 1], fill=(255, 255, 255, 255))).save(
            os.path.join(out, "chart_%s.png" % name))

    def ring(d, s, k):
        d.ellipse([0, 0, s - 1, s - 1], fill=(255, 255, 255, 255))
        r = s / 2 * 7.5 / 10.5
        d.ellipse([s / 2 - r, s / 2 - r, s / 2 + r, s / 2 + r], fill=(0, 0, 0, 0))
    _supersampled(42, ring).save(os.path.join(out, "chart_ring.png"))
    # a line's cross-section: a tent over the pane's height (twice the line's width), so that every pixel
    # column of a slightly tilted line gets the same coverage (a hard-edged middle band draws dashes)
    n = 16
    im = Image.new("RGBA", (4, n), (255, 255, 255, 0))
    for y in range(n):
        a = max(0.0, 1.0 - abs((y + 0.5) / n - 0.5) * 2)
        for x in range(4):
            im.putpixel((x, y), (255, 255, 255, round(255 * a)))
    im.save(os.path.join(out, "chart_line.png"))

    # the compass (radius 17, 1.5 px rim, a cross of 2 px lines, the north half-line red, 3 px), drawn at
    # 2 x its 36 px pane
    def compass(d, s, k):
        px = s / 36.0  # texture pixels per page pixel
        c, r = s / 2, 17 * px
        d.ellipse([c - r - 0.75 * px, c - r - 0.75 * px, c + r + 0.75 * px, c + r + 0.75 * px], fill=tuple(rgba(COMPASS_INK)))
        d.ellipse([c - r + 0.75 * px, c - r + 0.75 * px, c + r - 0.75 * px, c + r - 0.75 * px], fill=tuple(rgba(COMPASS_FILL)))
        ink = tuple(rgba(COMPASS_INK))
        d.rectangle([c - 1 * px, c - r + 3 * px, c + 1 * px, c + r - 3 * px], fill=ink)
        d.rectangle([c - r + 3 * px, c - 1 * px, c + r - 3 * px, c + 1 * px], fill=ink)
        d.rectangle([c - 1.5 * px, c - r + 3 * px, c + 1.5 * px, c - 2 * px], fill=tuple(rgba(RED)))
    _supersampled(72, compass).save(os.path.join(out, "chart_compass.png"))


def build_chart(nb, art, out, card_box, parent=None):
    """The chart's panes and resources, to merge into the notebook's description (see the module doc)."""
    del art  # the chart uses no painted art
    os.makedirs(out, exist_ok=True)
    write_chart_art(out)
    x, y, w, h = card_box
    cc = nb.centre(x, y, w, h)  # C_Chart's centre (unrotated): its children's parent
    px_, py_, tilt = (list(cc) + [0.0]) if parent is None else (list(parent) + [0.0])[:3]

    def lay(px, py):  # a page point in C_Chart's coordinates (layout: y up)
        return [round(px - nb.W / 2 - cc[0], 3), round(nb.H / 2 - py - cc[1], 3)]

    def at_pane(name, px, py, size, material, **kw):  # a picture centred on a page point
        p = {"type": "picture", "name": name, "size": [round(size[0], 3), round(size[1], 3)], "translate": lay(px, py),
             "material": material}
        p.update(kw)
        return p

    def null_at(name, px, py, children, **kw):
        p = {"type": "null", "name": name, "size": [1, 1], "translate": lay(px, py), "influence_alpha": True,
             "children": children}
        p.update(kw)
        return p

    tex = lambda png: {"png": png, "tile": "tiled"}
    textures = {"NbChartRing_00": tex("chart_ring.png"), "NbChartLine_00": tex("chart_line.png"),
                "NbChartCompass_00": tex("chart_compass.png")}
    for name in DISCS:
        textures["NbChart%s_00" % name.capitalize()] = tex("chart_%s.png" % name)
    mat = lambda t, white=None: {"textures": [{"texture": t}], **({"white": rgba(white)} if white else {})}
    materials = {"M_CGrid": mat("NbChartLine_00", GRID), "M_CGlow": mat("NbChartGlow_00", GLOW),
                 "M_CRing": mat("NbChartRing_00", WHITE), "M_CDot": mat("NbChartDot_00", RED),
                 "M_CSpot": mat("NbChartSpot_00", RED), "M_CLinkMark": mat("NbChartMark_00", LINK_FALLBACK),
                 "M_CCompass": mat("NbChartCompass_00"),
                 # the game's own texture, by its file name: resolved through the archives attached to the
                 # notebook's parts (LinkPos_00 is one of the main scene's resident layout archives)
                 "M_CLinkFace": {"textures": [{"texture": GAME_LINK_TEXTURE}]}}

    corners = [null_at("C_Sea" + n, *sea_at(card_box, u, v), [], visible=False)
               for n, (u, v) in (("TL", (0, 0)), ("TR", (1, 0)), ("BL", (0, 1)), ("BR", (1, 1)))]
    # the grid: 8 lines each way, about 1.5 px (the pane 3 px, the line picture a tent across it)
    lines = []
    for axis in ("U", "V"):
        for i in range(SQUARES + 1):
            t = i / SQUARES
            (x0, y0), (x1, y1) = (sea_at(card_box, t, 0), sea_at(card_box, t, 1)) if axis == "U" else \
                (sea_at(card_box, 0, t), sea_at(card_box, 1, t))
            length = math.hypot(x1 - x0, y1 - y0)
            angle = math.degrees(math.atan2(-(y1 - y0), x1 - x0))  # layout: y up, counter-clockwise
            lines.append(at_pane("C_Grid%s%d" % (axis, i), (x0 + x1) / 2, (y0 + y1) / 2, (length + 1.5, 3.0), "M_CGrid",
                                 rotate=[0, 0, round(angle, 4)]))
    grid = null_at("C_Grid", x + w / 2, y + h / 2, [], influence_alpha=True)
    grid["translate"] = [0, 0]
    grid["children"] = lines
    # the quest's place (C_Place moved by the runtime): the glow (radius 15 + 4 * pulse: 34 px at scale 1 =
    # radius 17, the pulse's middle), the white ring (radius 9, 3 px), the red dot (radius 4)
    origin = (x + w / 2, y + h / 2)
    place = null_at("C_Place", *origin, [at_pane("C_Glow", *origin, (34, 34), "M_CGlow"),
                                         at_pane("C_Ring", *origin, (21, 21), "M_CRing"),
                                         at_pane("C_PlaceDot", *origin, (8, 8), "M_CSpot")], visible=False)
    route = null_at("C_Route", *origin, [at_pane("C_Dot%02d" % i, *origin, (4.4, 4.4), "M_CDot", visible=False)
                                         for i in range(DOTS)])
    link = null_at("C_Link", *origin, [at_pane("C_LinkFace", *origin, (32, 32), "M_CLinkFace"),
                                       at_pane("C_LinkMark", *origin, (32 * 0.64, 32 * 0.64), "M_CLinkMark", visible=False)],
                   visible=False)
    # the children of the moved null panes sit at its origin
    for n in (place, route, link):
        n["translate"] = [0, 0]
        for k in n["children"]:
            k["translate"] = [0, 0]
    # the compass near the card's lower right corner, and its N (the HUD's text at (cx - 5, cy - 17 - 17))
    cx, cy = sea_at(card_box, 0.86, 0.84)
    ccn = nb.centre(cx - 18, cy - 18, 36, 36)
    compass = nb.null("C_Compass", cx - 18, cy - 18, 36, 36, [
        nb.pic("C_CompassRose", cx - 18, cy - 18, 36, 36, "M_CCompass", ccn),
        nb.text("C_CompassN", cx - 5, cy - 17 - 17, 15, COMPASS_N, "N", bold=True, width=24, parent=ccn, capacity=4, k=1.0)], cc)

    # C_Chart in its parent: the card's centre in the parent's (rotated) frame, the rotation undone
    a = math.radians(tilt)
    dx, dy = cc[0] - px_, cc[1] - py_
    chart = nb.null("C_Chart", x, y, w, h, corners + [grid, place, route, link, compass],
                    rotate=[0, 0, round(-tilt, 4)])
    chart["translate"] = [round(math.cos(a) * dx + math.sin(a) * dy, 3), round(-math.sin(a) * dx + math.cos(a) * dy, 3)]

    n_frames = max(1, round(FPS / 0.8))
    pulse = [[k, round((15 + 4 * (0.5 + 0.5 * math.sin(2 * math.pi * k / n_frames))) / 17.0, 4)] for k in range(n_frames + 1)]
    animations = {"ChartGlow": {"frames": n_frames, "loop": True, "groups": ["G_ChartGlow"], "tracks": [
        {"pane": "C_Glow", "target": "sx", "keys": pulse, "interp": "linear"},
        {"pane": "C_Glow", "target": "sy", "keys": pulse, "interp": "linear"}]}}
    return {"panes": [chart], "textures": textures, "materials": materials, "groups": {"G_ChartGlow": ["C_Glow"]},
            "animations": animations}


if __name__ == "__main__":  # the fit of one affine pane against the HUD's sea, for the record
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import make_notebook_layout as nb_
    box = nb_.frame_part(nb_.R(925, 405, 1412, 815))
    print("card box %.1f %.1f %.1f %.1f" % box)
    print(fit_report(box))
