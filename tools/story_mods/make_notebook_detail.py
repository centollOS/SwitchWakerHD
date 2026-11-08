#!/usr/bin/env python3
"""The story notebook's detail view (one quest) as panes of the notebook's game layout (local only, story
mods are WIP).

build_detail(nb, art, out, track_box=None) -> {"panes", "textures", "materials", "groups", "animations", "track_plate"}
  nb   the make_notebook_layout module (its helpers and constants: pic, null, text, frame_part, R, ...)
  art  the painted art (sheet.png, card.png, header.png, underline.png, band_cream/blue/green.png, frame.png
       / frame_notabs.png: the user's artwork, never in this repository)
  out  the layout folder: gets the processed pictures this module names (and our own small drawn ones)

"panes" are the children of the null pane N_Detail, which the notebook's generator places inside N_Page
at the page's centre (null("N_Detail", 0, 0, W, H, panes): translates here are relative to the screen
centre), above the frame and under the bottom plates. "track_plate": panes for N_PlateTrack (over its
plate, under its texts are fine): the chart scroll and the cursor brackets the detail page shows around
the A plate. Pane names start with "D_" and are the contract with
runtime/src/mods/story_notebook_layout_detail.cpp.

Geometry: story_notebook_page.h framed_detail() (the HUD's detail page with the painted pieces,
story_notebook_art.h namespace detail), on the same 1280 x 720 page.

The header band (header.png, the King of Red Lions on the sea): only the boat is kept, cut out of the
band's own waves and birds, scaled into the frame's green band between the title and the close button, and
cut off along the frame's own white wave line, so the boat sails in the frame's waves: the band's waves and
birds would double the frame's (and its opaque wave strip met the frame's green with a hard edge). The cut
is measured on the frame picture itself (its white line, column by column), so it fits the frame exactly.
"""
import math
import os

from PIL import Image, ImageDraw

FPS = 30

# the detail page's parts in the frame picture (story_notebook_art.h namespace detail; 1448 x 1086)
D_SHEET = (120, 250, 1385, 905)
D_TITLE = (160, 268, 914, 338)
D_UNDERLINE = (155, 342, 914, 371)
D_BANDS = ((198, 527, 905, 613), (202, 667, 898, 740), (202, 793, 903, 866))
D_CARD = (925, 405, 1412, 815)
CARD_SEA = {"tl": (0.124, 0.063), "tr": (0.960, 0.193), "bl": (0.044, 0.783), "br": (0.898, 0.942)}  # art::card

# colours of story_notebook_page.h (RGBA)
FROM_BLUE, INK_LIGHT = 0x2C6CA8FF, 0x5E7F4AFF
HEADS = ("Next step", "Where", "Reward")
HEAD_COLOURS = (0xC0392BFF, 0x1F6FB2FF, 0x3A8A2AFF)
EMBLEM_INKS = (0x7D3B2EFF, 0x2E5E6EFF, 0x5B6B2EFF, 0x6A3F72FF, 0x8A5A1EFF, 0x34507EFF)  # page.h emblem_colour
STEPS = 12  # progress markers (page.h progress(): at most 12)

# the header boat: its size in the frame picture and where it sits (the close button: frame.kSlot 1327..)
BOAT_HEIGHT = 0.38   # header picture -> frame picture scale
BOAT_RIGHT = 1312    # the boat's right edge (frame pixels), clear of the close button (round, from 1324)
BOAT_WATER = 335     # the boat's water line in header.png (where its own waves meet the hull)


HUD_WIDTH_K = 1.04  # the HUD page's letters are as wide as the layout's at k = 1.04 (and as high at k)


def text(nb, *args, **kw):
    """nb.text with the HUD page's letter widths: nb.text's size factor k matches the HUD's letter heights
    (cap height, x-height); the HUD's letters are narrower (measured in game against the HUD detail page:
    the lines were 1.14 - 1.16 times as long at k = 1.2, 1.05 at k = 1.1, the same at 1.0), so the font's
    width is scaled by 1.04 / k (now in nb.text, as the plain letters' border)."""
    return nb.text(*args, **kw)  # nb.text has the HUD's letter widths and the plain letters' border now


def box(nb, r):
    """A part of the frame picture (pixels) on the page: x, y, w, h."""
    return nb.frame_part(nb.R(*r))


def open_art(art, name):
    return Image.open(os.path.join(art, name)).convert("RGBA")


def cleaned(nb, art, name, height=None):
    """story_notebook.cpp load_user_art(): edges cleaned (alpha < 96), cropped, at most `height` high
    (None: the source's own size, fitted into the 2048 texture limit and later to twice its display
    size by make_notebook_layout.fit_textures_to_display)."""
    im = nb.clean_edges(open_art(art, name))
    if height and im.height > height:
        im = im.resize((round(im.width * height / im.height), height), Image.LANCZOS)
    return nb.fit_texture(im)


def slice3(nb, name, x, y, w, h, material, size, share, parent, **kw):
    """art::stretch3 as panes: the picture's ends (`share` of its width each) keep their shape at this
    height, the middle stretches. A null pane with L / M / R pictures."""
    pw, ph = size
    end = pw * share * h / ph
    if 2 * end >= w:
        return nb.pic(name, x, y, w, h, material, parent, **kw)
    c = nb.centre(x, y, w, h)
    s0, s1 = share, 1 - share
    return nb.null(name, x, y, w, h, [
        nb.pic(name + "L", x, y, end, h, material, c, [[0, 0], [s0, 0], [0, 1], [s0, 1]]),
        nb.pic(name + "M", x + end, y, w - 2 * end, h, material, c, [[s0, 0], [s1, 0], [s0, 1], [s1, 1]]),
        nb.pic(name + "R", x + w - end, y, end, h, material, c, [[s1, 0], [1, 0], [s1, 1], [1, 1]])], parent, **kw)


# ---- our own small pictures (drawn here, as the HUD page draws them) ----
def disc(path, fill, outline=None, ring=None, check=False, s=64):
    """A marker of radius 8 with a 2 px outline (page.h progress()), or a plain disc: drawn 4x."""
    im = Image.new("RGBA", (s * 4, s * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    S = s * 4
    if outline is None:
        d.ellipse([2, 2, S - 3, S - 3], fill=fill)
    else:  # r = 8, outline 2 centred on it: outer radius 9 = the picture's half
        k = S / 18.0
        d.ellipse([0, 0, S - 1, S - 1], fill=outline)
        d.ellipse([2 * k, 2 * k, S - 1 - 2 * k, S - 1 - 2 * k], fill=fill)
        if check:  # page.h: (5, 11) -> (8, 15) -> (14, 7) of an 18 px box
            d.line([5 * k, 11 * k, 8 * k, 15 * k, 14 * k, 7 * k], fill=(255, 255, 255, 255), width=round(2.5 * k), joint="curve")
    if ring is not None:  # the seal's ring: radius r - 4 of r = 20, 1.5 px
        colour, r0, width = ring
        d.ellipse([S / 2 - r0 * S / 2, S / 2 - r0 * S / 2, S / 2 + r0 * S / 2, S / 2 + r0 * S / 2], outline=colour,
                  width=max(1, round(width * S / 40)))
    im.resize((s, s), Image.LANCZOS).save(path)


def icon(path, kind, colour, s=64):
    """page.h icon(): 0 a swirl (a disc with a spiral cut), 1 a map pin, 2 a small chest. Drawn 4x."""
    S = s * 4
    k = S / 22.0  # the HUD draws them 22 px
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    white = (255, 255, 255, 255)
    cx = cy = S / 2
    if kind == 0:
        r = S * 0.48
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=colour)
        pts = [(cx, cy)]
        for i in range(1, 15):
            a, rr = i * 0.55, S * 0.03 * i
            pts.append((cx + math.cos(a) * rr, cy + math.sin(a) * rr))
        d.line(pts, fill=white, width=round(S * 0.09), joint="curve")
    elif kind == 1:
        r = S * 0.34
        d.polygon([(cx - S * 0.30, S * 0.48), (cx + S * 0.30, S * 0.48), (cx, S - 2)], fill=colour)
        d.ellipse([cx - r, S * 0.36 - r, cx + r, S * 0.36 + r], fill=colour)
        r = S * 0.13
        d.ellipse([cx - r, S * 0.36 - r, cx + r, S * 0.36 + r], fill=white)
    else:
        d.rectangle([S * 0.06, S * 0.30, S * 0.94, S * 0.92], fill=(0x8A, 0x5A, 0x1E, 255))
        d.rectangle([S * 0.06, S * 0.18, S * 0.94, S * 0.42], fill=(0xA8, 0x74, 0x2C, 255))
        d.rectangle([S * 0.06, S * 0.40, S * 0.94, S * 0.47], fill=colour)
        d.rectangle([S * 0.42, S * 0.36, S * 0.58, S * 0.56], fill=(0xE8, 0xC3, 0x4A, 255))
    del k
    im.resize((s, s), Image.LANCZOS).save(path)


def chart_scroll(path):
    """page.h chart_scroll(): a small chart scroll (34 x 30 and its rods) with a red cross, drawn 4x."""
    k = 4
    W_, H_ = 40, 34  # the scroll with its rods (x - 3 .. x + w + 3, y - 2 .. y + h + 2)
    im = Image.new("RGBA", (W_ * k, H_ * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    ox, oy = 3, 2
    sc = lambda *v: [c * k for c in v]
    d.rectangle(sc(ox, oy, ox + 34, oy + 30), fill=(0xE9, 0xD9, 0xA8, 255), outline=(0x8A, 0x6A, 0x3A, 255), width=2 * k)
    d.rectangle(sc(ox - 3, oy - 2, ox + 2, oy + 32), fill=(0xC9, 0xA6, 0x6B, 255))
    d.rectangle(sc(ox + 32, oy - 2, ox + 37, oy + 32), fill=(0xC9, 0xA6, 0x6B, 255))
    for i in range(3):
        d.line(sc(ox + 5 + i * 6, oy + 21 - i * 3, ox + 8 + i * 6, oy + 19 - i * 3), fill=(0xB2, 0x39, 0x2B, 255), width=2 * k)
    d.line(sc(ox + 21, oy + 7, ox + 29, oy + 15), fill=(0xD1, 0x2E, 0x1E, 255), width=3 * k)
    d.line(sc(ox + 29, oy + 7, ox + 21, oy + 15), fill=(0xD1, 0x2E, 0x1E, 255), width=3 * k)
    im.resize((W_ * 2, H_ * 2), Image.LANCZOS).save(path)


# ---- the header band: the boat, sailing in the frame's own waves ----
def frame_wave_line(frame, x0, x1):
    """The top of the frame's white wave line (frame picture rows) for the columns x0..x1: the first
    white texel under the green band; columns without one (the line thins out) take their neighbours'."""
    import numpy as np
    f = np.asarray(frame.convert("RGBA")).astype(int)
    top = []
    for x in range(x0, x1):
        col = f[140:240, x]
        white = (col[:, :3].min(axis=1) > 205) & (col[:, 3] > 200)
        ys = np.nonzero(white)[0]
        top.append(140 + int(ys[0]) if len(ys) else None)
    known = [(i, t) for i, t in enumerate(top) if t is not None]
    for i, t in enumerate(top):  # gaps: the nearest known column
        if t is None:
            top[i] = min(known, key=lambda k: abs(k[0] - i))[1]
    # a spiral of the line rises in a few columns: the boat's cut follows the line, not the spiral's top
    out = []
    for i in range(len(top)):
        near = sorted(top[max(0, i - 6):i + 7])
        out.append(near[len(near) // 2])
    return out


def header_boat(art, frame, out):
    """header.png's boat (the King of Red Lions with his sail) as a picture in the frame picture's own
    pixels: (file name, (x0, y0, x1, y1) in frame pixels). The band's waves and birds are left out, the
    hull is continued under its painted waves down to the frame's wave line and cut off there."""
    import numpy as np
    # header_v2.png (the user's band without the Triforce, 2172 x 724) when present, else header.png; at the
    # 1600 x 533 the boat's measures below were taken on (the same 3:1 picture)
    name = "header_v2.png" if os.path.exists(os.path.join(art, "header_v2.png")) else "header.png"
    band = open_art(art, name)
    if band.size != (1600, 533):
        band = band.convert("RGBa").resize((1600, 533), Image.LANCZOS).convert("RGBA")
    h = np.asarray(band).astype(int)
    r, g, b, a = h[..., 0], h[..., 1], h[..., 2], h[..., 3]
    dark = (np.maximum(np.maximum(r, g), b) < 125) & (a > 96)                       # the hull, the head, the sail
    teal = (r > 45) & (r < 170) & (g > 140) & (b < 205) & (b > 110) & (a > 96)       # the Triforce, the stripes
    mask = Image.fromarray(((dark | teal) * 255).astype(np.uint8), "L").copy()
    # the boat: what is joined to its hull (the birds and the waves' teal flecks are not)
    seed = (1000, 300)
    assert mask.getpixel(seed) == 255, "header.png: no boat at %s" % (seed,)
    comp = mask.copy()
    ImageDraw.floodfill(comp, seed, 128)
    boat = np.asarray(comp) == 128
    # its holes (the sail's lines, the light bits of the hull's paint) filled
    inv = Image.fromarray(((~boat) * 255).astype(np.uint8), "L").copy()  # writable, for floodfill
    ImageDraw.floodfill(inv, (0, 0), 128)  # the outside
    boat |= np.asarray(inv) == 255
    ys, xs = np.nonzero(boat)
    bx0, bx1, by0 = xs.min(), xs.max() + 1, ys.min()
    # the hull continues under the band's waves: from its water line down, what isn't painted hull (the
    # band's waves in front of it) takes the hull's colour (the mean of the column's hull texels above, so
    # its shading goes on); the frame's waves cover it again
    rgba = h.copy()
    alpha = np.where(boat, a, 0)
    hull_from = BOAT_WATER - 40
    painted = dark | teal
    overall = h[dark & boat][:, :3].mean(axis=0)
    for x in range(bx0, bx1):
        col = np.nonzero(boat[:, x])[0]
        col = col[col >= hull_from]
        if not len(col):
            continue
        above = dark[max(0, col[0] - 30):BOAT_WATER, x] & boat[max(0, col[0] - 30):BOAT_WATER, x]
        texels = h[max(0, col[0] - 30):BOAT_WATER, x][above][:, :3]
        colour = texels.mean(axis=0) if len(texels) >= 4 else overall
        for y in range(col[0], h.shape[0]):
            if not (painted[y, x] and y < BOAT_WATER + 20):
                rgba[y, x, :3] = colour
                alpha[y, x] = 255
    rgba[..., 3] = alpha
    piece = Image.fromarray(rgba.astype(np.uint8), "RGBA").crop((bx0, by0, bx1, h.shape[0]))
    # into the frame picture: the boat's right edge at BOAT_RIGHT, its water line on the frame's wave line
    k = BOAT_HEIGHT
    pw, ph = round(piece.width * k), round(piece.height * k)
    piece = piece.convert("RGBa").resize((pw, ph), Image.LANCZOS).convert("RGBA")
    fx0 = BOAT_RIGHT - pw
    line = frame_wave_line(frame, fx0, BOAT_RIGHT)
    water = sum(line) / len(line)
    fy0 = round(water - (BOAT_WATER - by0) * k)
    p = np.asarray(piece).astype(float)
    # cut along the wave line (1 texel soft): the line itself and the frame's sea below it stay in front
    for x in range(pw):
        cut = line[x] - fy0
        for y in range(max(0, cut - 1), ph):
            p[y, x, 3] *= 0.5 if y == cut - 1 else 0.0
    ys = np.nonzero(p[..., 3].max(axis=1) > 0)[0]
    p = p[: ys.max() + 1]
    Image.fromarray(p.astype(np.uint8), "RGBA").save(os.path.join(out, "d_header.png"))
    return "d_header.png", (fx0, fy0, BOAT_RIGHT, fy0 + p.shape[0])


def card_sea(nb):
    """The chart card's sea on the page: centre, size and tilt (degrees, layout: counter-clockwise) of the
    rectangle that fits its four corners (art::card; the HUD maps the chart bilinearly into them)."""
    x, y, w, h = box(nb, D_CARD)
    pt = {k: (x + u * w, y + v * h) for k, (u, v) in CARD_SEA.items()}
    cx = sum(p[0] for p in pt.values()) / 4
    cy = sum(p[1] for p in pt.values()) / 4
    dist = lambda p, q: math.hypot(q[0] - p[0], q[1] - p[1])
    sw = (dist(pt["tl"], pt["tr"]) + dist(pt["bl"], pt["br"])) / 2
    sh = (dist(pt["tl"], pt["bl"]) + dist(pt["tr"], pt["br"])) / 2
    top = math.degrees(math.atan2(pt["tr"][1] - pt["tl"][1], pt["tr"][0] - pt["tl"][0]))
    left = math.degrees(math.atan2(pt["tl"][0] - pt["bl"][0], pt["bl"][1] - pt["tl"][1]))
    return cx, cy, sw, sh, -(top + left) / 2


def build_detail(nb, art, out, track_box=None):
    """track_box: the A plate on the page (x, y, w, h; N_PlateTrack's box), default the frame's right plate."""
    os.makedirs(out, exist_ok=True)
    W, H = nb.W, nb.H
    root = (0.0, 0.0)  # N_Detail: centred on the page
    textures, materials, panes = {}, {}, []
    tex = lambda png: {"png": png, "tile": "tiled"}
    mat = lambda t, white=None: {"textures": [{"texture": t}], **({"white": white} if white else {})}

    def picture(name, im):
        im.save(os.path.join(out, name + ".png"))
        tname = "NbD%s_00" % name.split("_", 1)[1].title().replace("_", "")
        textures[tname] = tex(name + ".png")
        materials["M_D" + name.split("_", 1)[1].title().replace("_", "")] = mat(tname)
        return "M_D" + name.split("_", 1)[1].title().replace("_", ""), im.size

    # ---- the painted pieces (load_user_art(): cleaned, cropped, sized) ----
    m_sheet, s_sheet = picture("d_sheet", cleaned(nb, art, "sheet.png"))
    m_card, s_card = picture("d_card", cleaned(nb, art, "card.png", 512))
    m_uline, s_uline = picture("d_underline", cleaned(nb, art, "underline.png", 48))
    bands = [picture("d_band_" + n, cleaned(nb, art, "band_%s.png" % n, 120)) for n in ("cream", "blue", "green")]
    frame_file = "frame_notabs.png" if os.path.exists(os.path.join(art, "frame_notabs.png")) else "frame.png"
    frame = nb.clean_edges(open_art(art, frame_file), crop=False)
    head_png, head_box = header_boat(art, frame, out)
    textures["NbDHeader_00"] = tex(head_png)
    materials["M_DHeader"] = mat("NbDHeader_00")

    # ---- our own small pictures ----
    small = {
        "d_step_done.png": lambda p: disc(p, (0x3A, 0x8A, 0x2A, 255), (0x2A, 0x6A, 0x1E, 255), check=True),
        "d_step_now.png": lambda p: disc(p, (0xC0, 0x39, 0x2B, 255), (0x8A, 0x2A, 0x1E, 255)),
        "d_step_todo.png": lambda p: disc(p, (255, 255, 255, 255), (0xA8, 0x9A, 0x7A, 255)),
        "d_seal.png": lambda p: disc(p, (255, 255, 255, 255)),
        "d_seal_ring.png": lambda p: disc(p, (0, 0, 0, 0), ring=(tuple(nb.rgba(nb.SEAL)), 0.8, 1.5)),
        "d_icon_next.png": lambda p: icon(p, 0, tuple(nb.rgba(HEAD_COLOURS[0]))),
        "d_icon_where.png": lambda p: icon(p, 1, tuple(nb.rgba(HEAD_COLOURS[1]))),
        "d_icon_reward.png": lambda p: icon(p, 2, tuple(nb.rgba(HEAD_COLOURS[2]))),
        "d_scroll.png": chart_scroll,
    }
    for f, draw in small.items():
        draw(os.path.join(out, f))
        n = f[2:-4].title().replace("_", "")
        textures["NbD%s_00" % n] = tex(f)
        materials["M_D" + n] = mat("NbD%s_00" % n)
    # the seal's disc in each of the mods' inks (one is shown), the progress line's links
    for i, ink in enumerate(EMBLEM_INKS):
        materials["M_DSeal%d" % i] = mat("NbDSeal_00", nb.rgba(ink))
    materials["M_DStepLink"] = mat("NbWhite_00", [0xA8, 0x9A, 0x7A, 0xFF])

    # ---- the sheet, the title, its wave line ----
    x, y, w, h = box(nb, D_SHEET)
    panes.append(slice3(nb, "D_Sheet", x, y, w, h, m_sheet, s_sheet, 0.15, root))
    tx, ty, tw, th = box(nb, D_TITLE)
    panes.append(text(nb, "D_Title", tx, ty + (th - 34) / 2, 34, nb.INK, "", bold=True, width=tw, capacity=96, k=1.1))
    ux, uy, uw, uh = box(nb, D_UNDERLINE)
    panes.append(slice3(nb, "D_Underline", ux, uy, uw, uh, m_uline, s_uline, 0.05, root))

    # ---- the sender (page.h portrait_place(): under the wave line), the progress, the step before ----
    pcx, pcy, pr = ux + 22, uy + uh + 26, 20.0
    seal = [nb.pic("D_Seal%d" % i, pcx - pr, pcy - pr, 2 * pr, 2 * pr, "M_DSeal%d" % i, nb.centre(pcx - pr, pcy - pr, 2 * pr, 2 * pr),
                   visible=False) for i in range(len(EMBLEM_INKS))]
    # the quest's portrait (the layout's portrait slot 5, written by the port), else the seal
    seal.append(nb.pic("D_SenderPortrait", pcx - pr, pcy - pr, 2 * pr, 2 * pr, "M_Portrait5", nb.centre(pcx - pr, pcy - pr, 2 * pr, 2 * pr),
                       visible=False))
    seal.append(nb.pic("D_SealRing", pcx - pr, pcy - pr, 2 * pr, 2 * pr, "M_DSealRing", nb.centre(pcx - pr, pcy - pr, 2 * pr, 2 * pr)))
    seal.append(text(nb, "D_SenderInitial", pcx - pr, pcy - 11, 22, nb.SEAL, "?", bold=True, width=2 * pr, align="center",
                        parent=nb.centre(pcx - pr, pcy - pr, 2 * pr, 2 * pr), capacity=4, k=1.0))
    panes.append(nb.null("D_Sender", pcx - pr, pcy - pr, 2 * pr, 2 * pr, seal, root))
    from_w = 40  # "From" in the bold menu style at 18 (the HUD's measure of it)
    panes.append(text(nb, "D_From", pcx + pr + 10, pcy - 11, 18, FROM_BLUE, "From", bold=True, width=from_w + 8, capacity=8))
    nx = pcx + pr + 10 + from_w + 8
    panes.append(text(nb, "D_SenderName", nx, pcy - 12, 20, nb.INK, "", width=uw - 2 * pr - 70, capacity=96))
    # "Step n of m" ends at the wave line's right end; the markers run left from it, one per step
    label_w = 118  # "Step 99 of 99" bold at 20 (the HUD keeps this much room for the label)
    lx = ux + uw - label_w
    panes.append(text(nb, "D_Progress", lx, pcy - 11, 20, nb.INK_SOFT, "", bold=True, width=label_w + 40, capacity=32))
    steps = []
    for j in range(STEPS):  # slot j: the j-th marker left of the label (the last step in slot 0)
        sx = lx - 6 - 26 * (j + 1)
        sc = nb.centre(sx, pcy - 11 + 2, 18, 18)
        kids = [nb.pic("D_Step%d%s" % (j, st), sx, pcy - 11 + 2, 18, 18, "M_DStep" + st, sc, visible=False)
                for st in ("Done", "Now", "Todo")]
        if j > 0:  # the link to the marker on its right
            kids.append(nb.pic("D_Step%dLink" % j, sx + 18, pcy - 1, 6, 2, "M_DStepLink", sc, visible=False))
        steps.append(nb.null("D_Step%d" % j, sx, pcy - 11 + 2, 18, 18, kids, root, visible=False))
    panes += steps
    panes.append(text(nb, "D_DoneBefore", pcx - pr, pcy + pr + 4, 17, INK_LIGHT, "", width=uw, capacity=128, visible=False))

    # ---- the three bands: icon, head, the band, its text (wrapped by the port) ----
    for i, (r_, (m_band, s_band)) in enumerate(zip(D_BANDS, bands)):
        bx, by, bw, bh = box(nb, r_)
        panes.append(nb.pic("D_Icon%d" % i, bx - 4, by - 28, 22, 22, "M_DIcon" + ("Next", "Where", "Reward")[i], root))
        panes.append(text(nb, "D_Head%d" % i, bx + 24, by - 28, 21, HEAD_COLOURS[i], HEADS[i], bold=True, width=300, capacity=32))
        panes.append(slice3(nb, "D_Band%d" % i, bx, by, bw, bh, m_band, s_band, 0.04, root))
        t = text(nb, "D_Body%d" % i, bx + 18, by, 19, nb.INK, "", width=bw - 36, capacity=256)
        t["size"] = [round(bw - 36, 3), round(bh, 3)]
        t["translate"] = nb.place(bx + 18, by, bw - 36, bh)
        t["text_position"] = ["left", "center"]
        panes.append(t)

    # ---- the header band: the boat in the frame's green band ----
    hx0, hy0, hx1, hy1 = head_box
    hx, hy, hw, hh = box(nb, head_box)
    panes.append(nb.pic("D_Header", hx, hy, hw, hh, "M_DHeader", root))

    # ---- the status pill above the card, the chart ring, the card ----
    cx, cy, cw, ch = box(nb, D_CARD)
    pw, ph = 168.0, 50.0
    px, py = cx + cw - pw - 10, cy - ph - 8
    pill_uv = [[nb.B_PILL.x0, nb.B_PILL.y0], [nb.B_PILL.x1, nb.B_PILL.y0], [nb.B_PILL.x0, nb.B_PILL.y1], [nb.B_PILL.x1, nb.B_PILL.y1]]
    pc = nb.centre(px, py, pw, ph)
    pill = [nb.pic("D_PillArt", px, py, pw, ph, "M_Banner", pc, pill_uv)]
    for name in ("New", "NotYet", "Done"):
        pill.append(nb.pic("D_Pill" + name, px + 3, py + 3, pw - 6, ph - 6, "M_Pill" + name, pc, visible=False))
    pill.append(text(nb, "D_Status", px, py + (ph - 20) / 2, 20, nb.WHITE, "", bold=True, width=pw, align="center", parent=pc,
                        capacity=24, k=1.0))
    panes.append(nb.null("D_Pill", px, py, pw, ph, pill, root))
    ring_x = px - 16
    panes.append(nb.pic("D_Ring", ring_x - 11, py + ph / 2 - 11, 22, 22, "M_Ring", root, visible=False))
    panes.append(text(nb, "D_OnChart", ring_x - 12 - 6 - 300, py + ph / 2 - 9, 17, nb.INK, "On the chart", width=300, align="right",
                         capacity=24, visible=False))
    panes.append(nb.pic("D_Card", cx, cy, cw, ch, m_card, root))
    scx, scy, sw, sh, tilt = card_sea(nb)
    slot = nb.null("D_ChartSlot", scx - sw / 2, scy - sh / 2, sw, sh, [], root)
    slot["rotate"] = [0, 0, round(tilt, 3)]
    panes.append(slot)
    # the mini chart on the card's sea (make_notebook_chart.py): it places itself exactly (the HUD's bilinear
    # sea) and undoes the slot's tilt
    import make_notebook_chart
    chart = make_notebook_chart.build_chart(nb, art, out, (cx, cy, cw, ch),
                                            parent=nb.centre(scx - sw / 2, scy - sh / 2, sw, sh) + (tilt,))
    slot["children"] = chart["panes"]
    textures.update(chart["textures"]), materials.update(chart["materials"])

    # ---- for N_PlateTrack: the chart scroll right of its label, the cursor brackets around it ----
    track = []
    if track_box or hasattr(nb, "FR_PLATE_R"):
        tx_, ty_, tw_, th_ = track_box or nb.frame_part(nb.FR_PLATE_R)
        tcen = nb.centre(tx_, ty_, tw_, th_)
        track.append(nb.pic("D_TrackScroll", tx_ + tw_ - 20 - 40, ty_ + th_ / 2 - 17 - 2, 40, 34, "M_DScroll", tcen, visible=False))
        cs, b = 22, 4 + 2
        x0, y0, x1, y1 = tx_ + 2 - b, ty_ + 2 - b, tx_ + tw_ - 2 + b - cs, ty_ + th_ - 2 + b - cs
        for name, uv, (ox, oy) in (("TL", (0, 1, 0, 1), (x0, y0)), ("TR", (1, 0, 0, 1), (x1, y0)),
                                   ("BL", (0, 1, 1, 0), (x0, y1)), ("BR", (1, 0, 1, 0), (x1, y1))):
            u0, u1, v0, v1 = uv
            track.append(nb.pic("D_TrackCursor" + name, ox, oy, cs, cs, "M_Corner", tcen, [[u0, v0], [u1, v0], [u0, v1], [u1, v1]],
                                visible=False))

    # ---- animations (N_Detail's own page turn, DetailIn / DetailOut, is the notebook generator's) ----
    groups, anims = dict(chart["groups"]), dict(chart["animations"])
    if track:  # the cursor around the A plate breathes as the list's does
        groups["G_TrackCursor"] = [p["name"] for p in track if p["name"].startswith("D_TrackCursor")]
        bn = max(1, round(FPS / 0.9))
        off = lambda t: 2.0 * math.sin(t * 2 * math.pi)
        tracks = []
        for p in track:
            if not p["name"].startswith("D_TrackCursor"):
                continue
            sxn = -1 if p["name"][-1] == "L" else 1
            syn = 1 if p["name"][-2] == "T" else -1
            bx_, by_ = p["translate"]
            tracks.append({"pane": p["name"], "target": "tx", "keys": [[k, round(bx_ + sxn * off(k / bn), 4)] for k in range(bn + 1)]})
            tracks.append({"pane": p["name"], "target": "ty", "keys": [[k, round(by_ + syn * off(k / bn), 4)] for k in range(bn + 1)]})
        anims["TrackBreathe"] = {"frames": bn, "loop": True, "groups": ["G_TrackCursor"], "tracks": tracks}
    return {"panes": panes, "textures": textures, "materials": materials, "groups": groups, "animations": anims,
            "track_plate": track}
