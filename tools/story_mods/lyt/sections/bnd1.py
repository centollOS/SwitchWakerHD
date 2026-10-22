"""bnd1: bounding pane, the touch / hit area of the game's buttons (e.g. "B_Btn_00" in the game's button
layouts, named first in their cnt1 control). Only the pane fields (pan1); it is never drawn.

Description keys: pan1's (type "bounding")."""
from . import pan1


def write(p, ctx):
    return pan1.body(p)
