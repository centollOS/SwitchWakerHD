"""Layout writer: description -> .bflyt.

Section order, as in the game's own layouts: lyt1, txl1, fnl1, mat1, the pane tree (RootPane, then pas1 /
pae1 around each pane's children), grp1 RootGroup with the description's groups between grs1 and gre1.
Panes are written first so that their writers can add textures, fonts and materials to the context.
"""
from . import binio
from .context import Context
from .panes import PANE_TYPES
from .sections import fnl1, grp1, lyt1, mat1, pan1, txl1, usd1


def _pane_tree(panes, ctx):
    out = []
    for p in panes:
        magic, writer = PANE_TYPES[p.get("type", "null")]
        out.append(binio.section(magic, writer(p, ctx)))
        out += usd1.for_pane(p)  # extended user data follows its pane (e.g. a text pane's "Scalable")
        if p.get("children"):
            out.append(binio.section(b"pas1", b""))
            out += _pane_tree(p["children"], ctx)
            out.append(binio.section(b"pae1", b""))
    return out


def build(desc, ctx=None):
    ctx = ctx or Context(desc)
    for f in desc.fonts:
        ctx.font(f)
    root = {"name": "RootPane", "size": desc.size, "part_flags": 0x00}
    tree = [binio.section(b"pan1", pan1.body(root))]
    if desc.panes:
        tree.append(binio.section(b"pas1", b""))
        tree += _pane_tree(desc.panes, ctx)
        tree.append(binio.section(b"pae1", b""))
    secs = [lyt1.write(desc)]
    if ctx.textures:
        secs.append(txl1.write(ctx.textures))
    if ctx.fonts:
        secs.append(fnl1.write(ctx.fonts))
    if ctx.materials:
        secs.append(mat1.write(ctx.materials))
    secs += tree + grp1.write(desc.groups)
    return binio.file_header(b"FLYT", secs), ctx
