"""usd1: a pane's extended user data, written right after the pane's own section.

Layout (from the game's own usd1 sections): count u16, pad u16, then 12-byte entries
  u32 name offset, u32 data offset (both relative to the entry), u16 value count, u8 type, u8 pad
types 0 string (count = length; the data is NUL-terminated), 1 int (s32), 2 float (f32).
The data blocks follow the entries (ints and floats aligned to 4), then the NUL-terminated names.

The game's text panes read these entries (FW-parts_text_valid): "Scalable" (float, percent) lets the
text shrink horizontally to the pane width down to that scale (hdTextPane_c::fitText); "Copy" (string)
takes the message label of another pane.

Description: a pane's "user_data": [{"name": "Scalable", "type": "float", "value": 80.0}, ...];
text panes' "scalable": 80 is a short form of that entry.
"""
import struct

from .. import binio

TYPES = {"string": 0, "int": 1, "float": 2}


def entries(p):
    out = list(p.get("user_data", []))
    if "scalable" in p and not any(e["name"] == "Scalable" for e in out):
        out.append({"name": "Scalable", "type": "float", "value": float(p["scalable"])})
    return out


def write(items):
    n = len(items)
    data, data_offs = b"", []
    for e in items:
        kind = TYPES[e.get("type", "string")]
        v = e["value"]
        if kind == 0:
            data = data + v.encode() + b"\0"
            data_offs.append((len(data) - len(v.encode()) - 1, len(v.encode()), kind))
            continue
        vals = v if isinstance(v, (list, tuple)) else [v]
        data = binio.pad4(data)
        data_offs.append((len(data), len(vals), kind))
        data += b"".join(struct.pack(">i", int(x)) if kind == 1 else binio.f32(x) for x in vals)
    names, name_offs = b"", []
    for e in items:
        name_offs.append(len(names))
        names += e["name"].encode() + b"\0"
    head = 4 + 12 * n  # body offset of the data block
    body = struct.pack(">HH", n, 0)
    for i, (e, (doff, count, kind)) in enumerate(zip(items, data_offs)):
        entry = 4 + 12 * i
        body += struct.pack(">IIHBB", head + len(data) + name_offs[i] - entry, head + doff - entry, count, kind, 0)
    return binio.section(b"usd1", body + data + names)


def for_pane(p):
    """[usd1 section] when the pane has user data, else []."""
    items = entries(p)
    return [write(items)] if items else []
