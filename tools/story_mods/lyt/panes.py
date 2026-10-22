"""Pane type registry: description "type" -> (section magic, writer(pane, ctx) -> section body).

The window (wnd1) and text (txt1) writers plug in here; further pane types (parts, prt1; bounding, bnd1)
can be added with register()."""
from .sections import bnd1, pan1, pic1, prt1, txt1, wnd1

PANE_TYPES = {
    "null": (b"pan1", pan1.write),
    "picture": (b"pic1", pic1.write),
    "window": (b"wnd1", wnd1.write),
    "text": (b"txt1", txt1.write),
    "bounding": (b"bnd1", bnd1.write),
    "parts": (b"prt1", prt1.write),
}


def register(type_name, magic, writer, materials=None):
    PANE_TYPES[type_name] = (magic, writer)
    if materials:
        MATERIAL_USERS[type_name] = materials


# The description materials a pane uses, by pane type (for the check that an animated material's pane is
# in the animation's groups). A pane type's section module may define materials(p); register() can too.
MATERIAL_USERS = {
    "picture": lambda p: [p["material"]] if p.get("material") else [],
}


def materials_of(p):
    """Names of the description materials pane p uses."""
    kind = p.get("type", "null")
    fn = MATERIAL_USERS.get(kind)
    if fn is None:
        module = {"window": wnd1, "text": txt1}.get(kind)
        fn = getattr(module, "materials", None)
    if fn is None:
        return [p["material"]] if p.get("material") else []
    return list(fn(p))
