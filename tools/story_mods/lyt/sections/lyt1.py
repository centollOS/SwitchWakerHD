"""lyt1: layout header. drawFromCenter u8, pad[3], width f32, height f32, part size w / h f32, layout name
(NUL-terminated, padded to 4). The part size is 0 in the game's screen layouts and the part's real size in
its part layouts (the ones with a cnt1 section, e.g. buttons): description "part_size"."""
from .. import binio


def write(desc):
    w, h = desc.size
    pw, ph = desc.part_size
    body = (bytes([1, 0, 0, 0]) + binio.f32(w) + binio.f32(h) + binio.f32(pw) + binio.f32(ph) +
            binio.cstr4(desc.name))
    return binio.section(b"lyt1", body)
