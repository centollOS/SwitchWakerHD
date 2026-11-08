"""Shared binary helpers for the layout writers (big-endian, as in the game's layout files)."""
import struct

VERSION = 0x02020000  # the version of the game's .bflyt / .bflan / .bflim files


def f32(v):
    return struct.pack(">f", v)


def name(s, n):
    """A NUL-padded fixed-size name field."""
    b = s.encode() + b"\0"
    if len(b) > n:
        raise ValueError("name %r is longer than %d bytes" % (s, n - 1))
    return b + b"\0" * (n - len(b))


def cstr4(s):
    """A NUL-terminated string padded to a multiple of 4 bytes."""
    b = s.encode() + b"\0"
    return b + b"\0" * (-len(b) % 4)


def pad4(b):
    return b + b"\0" * (-len(b) % 4)


def section(magic, body):
    """A section: magic, u32 size (including the 8-byte header), body padded to 4 bytes."""
    assert len(magic) == 4
    body = pad4(body)
    return magic + struct.pack(">I", 8 + len(body)) + body


def file_header(magic, sections):
    """A file: magic, BOM, header size 0x14, version, file size, section count, then the sections."""
    body = b"".join(sections)
    return magic + struct.pack(">HHIIHH", 0xFEFF, 0x14, VERSION, 0x14 + len(body), len(sections), 0) + body


def string_table(names):
    """u32 offsets (relative to the table start) followed by the NUL-terminated names, padded to 4.
    Used by txl1, fnl1 and the texture list of pai1."""
    offs, blob = [], b""
    base = 4 * len(names)
    for n in names:
        offs.append(base + len(blob))
        blob += n.encode() + b"\0"
    return pad4(b"".join(struct.pack(">I", o) for o in offs) + blob)


def srgb_to_linear(v):
    """An sRGB channel value 0-255 as the linear value 0-255 (alpha is never converted)."""
    x = v / 255.0
    x = x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4
    return 255.0 * x


def rgba_linear(c):
    """rgba() of an sRGB colour, its r g b converted to linear (rounded)."""
    c = list(c) + [255] * (4 - len(c))
    return rgba([round(srgb_to_linear(x)) for x in c[:3]] + [c[3]])


def rgba(c):
    """A colour from [r, g, b, a] (or [r, g, b]; alpha 255) as 4 bytes."""
    c = list(c) + [255] * (4 - len(c))
    return bytes(int(x) & 0xFF for x in c[:4])


def read_sections(data):
    """[(magic, body)] of a .bflyt / .bflan file; for tests and for comparing with the game's own files."""
    magic, bom, hsize, version, size, count = struct.unpack(">4sHHIIH", data[:0x12])
    assert bom == 0xFEFF and size == len(data), "bad header"
    out, off = [], hsize
    for _ in range(count):
        m, n = struct.unpack(">4sI", data[off:off + 8])
        out.append((m, data[off + 8:off + n]))
        off += n
    assert off == len(data), "trailing data"
    return out
