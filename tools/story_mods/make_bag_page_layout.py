#!/usr/bin/env python3
"""The Delivery Bag's extra pages (mod items) as a game layout: StoryBag_00.

usage: make_bag_page_layout.py [OUT.json]   (default: runtime/assets/story_bag/StoryBag_00.json)

The description holds no art: the window, the cursor, the selection disc, the Back button and the page dots
name the game's own textures (MenuItem_00, Cursor_00, BtnItemIcon_00 and BtnBack_00, the item screen's
archives, attached at run time: story_lyt_screen Client::game_archives); the item icons are picture slots
of our own ("fill" textures, transparent) that the port writes at run time (story_lyt_screen::
set_texture_pixels). The port builds the archive on the player's machine (lyt_build.h, story_bag_page.cpp).

Geometry: the game's bag window (MenuItem_00: W_BagItemBase_00 562 x 494, its eight places
L_BtnItemIcon_19..26 and L_BtnBack_00 relative to the window). The pane N_Bag is moved every frame onto the
game's window (story_bag_page.cpp), so the page lies exactly over it, on the GamePad and on the TV. Pane
names are the contract with runtime/src/mods/story_bag_page_layout.cpp (story_bag_page_layout.h).
Animations run at 30 frames a second (one frame per item screen draw).
"""
import json
import os
import sys

NAME = "StoryBag_00"
SLOTS = 8
MAX_DOTS = 17          # the game's page and up to 16 extra pages (story_items kMaxPages)
ICON = 128             # the icon slots' texture size (RGBA8)
WINDOW = (562, 494)    # W_BagItemBase_00
# the eight places and the Back button, relative to the window's centre (L_BtnItemIcon_19..26, L_BtnBack_00)
PLACES = [(-131, 130), (-175, -6), (-127, -142), (-3, 130), (-47, -6), (1, -142), (125, 130), (81, -6)]
BACK = (129, -142)
ICON_SIZE = 104        # the item icon on its place (the game's 128 x 128 icon picture inside a 140 part, scaled)
DOT_Y, DOT_STEP, DOT_SIZE = 210, 24, 18
NAME_Y, MSG_Y = 186, -222
MENU_OPTIONS = 4

# the game's textures, by file name (never shipped)
T_WINDOW = "ItemBackBaseGlow_00^t.bflim"   # MenuItem_00: the bag window's shape
T_CURSOR = "Crsor_00^t.bflim"              # Cursor_00: the menus' corner brackets
T_DISC = "ItemNow_00^t.bflim"              # BtnItemIcon_00: the round selection disc
T_BACK = "Back_00^t.bflim"                 # BtnBack_00: the Back arrow
T_FRAME = ["NotchedWindow%s_00^t.bflim" % s for s in ("LT", "RT", "LB", "RB", "L", "R", "T", "B")]  # MenuItem_00
T_PAPER = "PaperPattern_00^t.bflim"        # MenuItem_00: the small windows' paper
GAME_ARCHIVES = ["MenuItem_00", "Cursor_00", "BtnItemIcon_00", "BtnBack_00"]

# colours (sRGB; the description converts them): the window's paper, the ink of the menu's small windows
PAPER = [228, 229, 238, 255]
INK = [52, 58, 78, 255]
DOT_ON, DOT_OFF = [255, 225, 0, 255], [150, 156, 178, 255]


OUTLINE = [36, 40, 60, 255]  # the dark border of the game's white menu labels ("Back", counts)


def text(name, xy, size, w, value="", align="center", capacity=64, color=INK, border=None, **kw):
    h = size * 1.4
    p = {"type": "text", "name": name, "size": [w, round(h, 2)], "translate": list(xy),
         "font": "CKingMain.bffnt", "font_size": [round(size * 0.92, 2), size], "text": value, "capacity": capacity,
         "text_position": [align, "center"], "line_alignment": align,
         "text_material": {"black": border or [255, 255, 255, 0], "white": color}}
    p.update(kw)
    return p


def pic(name, xy, size, material, **kw):
    p = {"type": "picture", "name": name, "size": list(size), "translate": list(xy), "material": material}
    p.update(kw)
    return p


def null(name, xy, size, children, **kw):
    p = {"type": "null", "name": name, "size": list(size), "translate": list(xy), "influence_alpha": True,
         "children": children}
    p.update(kw)
    return p


def description():
    textures = {"BagIcon%d_00" % i: {"fill": {"color": [0, 0, 0, 0]}, "width": ICON, "height": ICON, "format": "rgba8"}
                for i in range(SLOTS + 1)}  # 0..7 the places, 8 the floating icon (choosing a game place)
    materials = {
        # the window: the game's bag window shape on the paper colour
        "M_Window": {"textures": [{"texture": T_WINDOW}], "black": PAPER[:3] + [0], "white": PAPER},
        "M_Cursor": {"textures": [{"texture": T_CURSOR}], "black": [42, 20, 6, 0], "white": [255, 255, 255, 255]},
        "M_Disc": {"textures": [{"texture": T_DISC}], "black": [255, 255, 255, 0], "white": [255, 255, 255, 255]},
        "M_DotOn": {"textures": [{"texture": T_DISC}], "black": DOT_ON[:3] + [0], "white": DOT_ON},
        "M_DotOff": {"textures": [{"texture": T_DISC}], "black": DOT_OFF[:3] + [0], "white": DOT_OFF},
        "M_Back": {"textures": [{"texture": T_BACK}], "black": [0, 0, 0, 0], "white": [246, 243, 225, 255]},
        "M_Paper": {"textures": [{"texture": T_PAPER, "wrap": [5, 5]}], "black": [196, 198, 214, 0], "white": PAPER},
        "M_MenuSel": {"textures": [{"texture": T_DISC}], "black": [255, 225, 0, 0], "white": [255, 225, 0, 255]},
    }
    for i in range(SLOTS + 1):
        materials["M_Icon%d" % i] = {"textures": [{"texture": "BagIcon%d_00" % i}]}
    for i, t in enumerate(T_FRAME):
        materials["M_Frame%d" % i] = {"textures": [{"texture": t}]}

    places = []
    for i, xy in enumerate(PLACES):
        places.append(null("N_Slot%d" % i, xy, [140, 140], [
            pic("P_Disc%d" % i, [0, 0], [138, 138], "M_Disc", visible=False),
            pic("P_Icon%d" % i, [0, 0], [ICON_SIZE, ICON_SIZE], "M_Icon%d" % i, visible=False),
            text("T_Num%d" % i, [40, -40], 30, 60, capacity=4, align="right", color=[255, 255, 255, 255],
                 border=OUTLINE, visible=False),
        ]))
    back = null("N_Back", BACK, [128, 128], [
        pic("P_Back", [0, 6], [92, 89], "M_Back"),
        text("T_Back", [0, -44], 24, 128, "Back", capacity=16, color=[255, 255, 255, 255], border=OUTLINE),
    ])
    dots = [pic("P_Dot%d" % i, [0, 0], [DOT_SIZE, DOT_SIZE], "M_DotOff", visible=False) for i in range(MAX_DOTS)]
    menu_w, menu_h = 380, 60 + 48 * MENU_OPTIONS
    menu = null("N_Menu", [0, 0], [menu_w, menu_h], [
        {"type": "window", "name": "W_Menu", "size": [menu_w, menu_h], "frame_size": 30,
         "frames": [{"material": "M_Frame%d" % i} for i in range(8)], "content": {"material": "M_Paper"}},
        pic("P_MenuSel", [0, 0], [menu_w - 40, 46], "M_MenuSel", alpha=150),
    ] + [text("T_Opt%d" % i, [0, menu_h / 2 - 54 - 48 * i], 28, menu_w - 40, capacity=40) for i in range(MENU_OPTIONS)],
        visible=False)
    cursor = null("N_Cursor", PLACES[0], [120, 120], [
        {"type": "window", "name": "W_Cursor", "size": [120, 120], "frame_size": 40,
         "frames": [{"material": "M_Cursor"}], "content": {}, "draw_content": False},
    ], visible=False)
    page = null("N_Page", [0, 0], list(WINDOW), [
        {"type": "window", "name": "W_Bag", "size": list(WINDOW), "frame_size": [158, 158, 494, 494],
         "frames": [{"material": "M_Window", "flip": f} for f in ("none", "flip_h", "none", "none")],
         "content": {"material": "M_Window"}, "window_flags": 0x0B},  # as the game's: left and right frames only
        null("N_Slots", [0, 0], list(WINDOW), places + [back]),
        text("T_Name", [0, NAME_Y], 26, 420, capacity=48),
        text("T_Msg", [0, MSG_Y], 22, 460, capacity=96),
    ])
    bag = null("N_Bag", [313, -92], list(WINDOW), [
        page,
        null("N_Dots", [0, DOT_Y], [DOT_STEP * MAX_DOTS, DOT_SIZE], dots),
        text("T_Hint", [0, NAME_Y], 26, 460, capacity=64, color=[255, 255, 255, 255], border=OUTLINE, visible=False),
        cursor,
        pic("P_Float", [0, 0], [ICON_SIZE, ICON_SIZE], "M_Icon8", visible=False),
        menu,
    ])
    turn = lambda name, dx: {"frames": 6, "groups": ["G_Slots"], "tracks": [
        {"pane": "N_Slots", "target": "tx", "keys": [[0, dx], [6, 0]], "interp": "ease"},
        {"pane": "N_Slots", "target": "alpha", "keys": [[0, 0], [6, 255]]}]}
    return {
        "name": NAME,
        "size": [1280, 720],
        "textures": textures,
        "materials": materials,
        "panes": [null("N_All", [0, 0], [1280, 720], [bag])],
        "groups": {"G_Page": ["N_Page"], "G_Slots": ["N_Slots"], "G_Cursor": ["N_Cursor"]},
        "animations": {
            "Open": {"frames": 6, "groups": ["G_Page"], "tracks": [
                {"pane": "N_Page", "target": "alpha", "keys": [[0, 0], [6, 255]]},
                {"pane": "N_Page", "target": "sx", "keys": [[0, 0.92], [6, 1]], "interp": "ease"},
                {"pane": "N_Page", "target": "sy", "keys": [[0, 0.92], [6, 1]], "interp": "ease"}]},
            "TurnLeft": turn("TurnLeft", -60),
            "TurnRight": turn("TurnRight", 60),
            "Breathe": {"frames": 40, "loop": True, "groups": ["G_Cursor"], "tracks": [
                {"pane": "N_Cursor", "target": "sx", "keys": [[0, 1], [20, 1.07], [40, 1]], "interp": "ease"},
                {"pane": "N_Cursor", "target": "sy", "keys": [[0, 1], [20, 1.07], [40, 1]], "interp": "ease"}]},
        },
    }


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "runtime", "assets", "story_bag", NAME + ".json")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w") as f:
        json.dump(description(), f, indent=1)
        f.write("\n")
    print(out)


if __name__ == "__main__":
    main()
