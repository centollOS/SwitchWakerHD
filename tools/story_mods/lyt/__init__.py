"""Story UI layouts in the game's own layout format (local only, story mods are WIP).

Package layout (one module per section writer, so the window / text pane work can plug in separately):
  description.py   the JSON description (schema.json) and its validation
  context.py       resource tables shared by the writers (txl1 textures, fnl1 fonts, mat1 materials)
  panes.py         pane type registry: null (pan1), picture (pic1), window (wnd1), text (txt1)
  sections/        lyt1 txl1 fnl1 mat1 pan1 pic1 wnd1 txt1 grp1
  anim/            pat1 pai1 tracks (animation targets and key encoding)
  bflyt.py bflan.py bflim.py   file writers;  archive.py   description -> Layout/<name>.szs
  bc.py            BC1 / BC3 / BC4 / BC5 block compression;  gx2tile.py   GX2 surface tiling
  tests/           gx2_detile.cpp: reads our textures back through the port's address library

Format knowledge comes only from our decomp and the game's own layout files read locally.
"""
from .archive import build, build_files  # noqa: F401
from .description import Description  # noqa: F401
