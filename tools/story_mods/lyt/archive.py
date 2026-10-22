"""Description -> layout archive (Layout/<name>.szs): a Yaz0 SARC with
  blyt/<name>.bflyt, anim/<name>_<animation>.bflan, timg/<texture>^<suffix>.bflim
Textures come from our own PNGs (description "textures"), resized to "width" (height follows the aspect
ratio unless "height" is given)."""
import os
import sys

from . import bflan, bflim, bflyt
from .description import Description

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from notebook_model import sarc, yaz0_stored  # noqa: E402


def fill_image(spec):
    """A plain texture described in the description itself (no image file): "fill" is
    {"color": rgba, "border": rgba, "border_width": n}; "width" and "height" are required."""
    from PIL import Image, ImageDraw
    f = spec["fill"]
    w, h = int(spec["width"]), int(spec["height"])
    im = Image.new("RGBA", (w, h), tuple(f["color"]))
    n = int(f.get("border_width", 0))
    if n:
        d = ImageDraw.Draw(im)
        for i in range(n):
            d.rectangle([i, i, w - 1 - i, h - 1 - i], outline=tuple(f["border"]))
    return im


def load_texture(spec, desc):
    from PIL import Image
    im = fill_image(spec) if "fill" in spec else Image.open(desc.path(spec["png"])).convert("RGBA")
    w = int(spec.get("width", im.width))
    h = int(spec.get("height", max(1, round(im.height * w / im.width))))
    if (w, h) != im.size:
        im = im.resize((w, h), Image.LANCZOS)
    return im.tobytes(), w, h


def build_files(desc, texture_loader=load_texture):
    layout, ctx = bflyt.build(desc)
    files = {"blyt/%s.bflyt" % desc.name: layout}
    for tex, spec in desc.textures.items():
        rgba, w, h = texture_loader(spec, desc)
        files["timg/%s" % desc.texture_file(tex)] = bflim.encode(
            rgba, w, h, spec.get("format", "rgba8"), spec.get("tile", "linear"), int(spec.get("swizzle", 0)))
    for anim_name, spec in desc.animations.items():
        files["anim/%s_%s.bflan" % (desc.name, anim_name)] = bflan.build(anim_name, spec, desc)
    return files


def build(desc_or_path, out_path=None, texture_loader=load_texture):
    desc = desc_or_path if isinstance(desc_or_path, Description) else Description.load(desc_or_path)
    data = yaz0_stored(sarc(build_files(desc, texture_loader)))
    if out_path:
        with open(out_path, "wb") as f:
            f.write(data)
    return data
