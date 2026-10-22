"""Animation track targets and key encoding.

A track block (tag) inside pai1 content: magic (e.g. "FLVC"), entry count u8, pad[3], u32 entry offsets
(relative to the tag start). An entry: index u8, target u8, curve u8 (1 step, 2 hermite), pad u8,
key count u16, pad u16, key offset u32 (0xC), then the keys:
  hermite: frame f32, value f32, slope f32      step: frame f32, value u16, pad u16

Tags and targets seen in the game's own .bflan files (content type 0 = pane, 1 = material):
  FLPA pane SRT     targets 0-9 in pan1 field order (translate xyz, rotate xyz, scale xy, size wh)
  FLVC pane colour  targets 0-15 vertex colours (4 corners x rgba), 16 = pane alpha
  FLVI pane visibility (step)
  FLMC material colours targets 0-3 black rgba, 4-7 white rgba
  FLTS texture SRT (index = texture map) targets 0-4: translate x y, rotate, scale x y
  FLTP texture pattern (step; index = texture map, value = pai1 texture list index)
  FLFS (font shadow, text panes; lyt-text)
The last field of TARGETS says whether the target was checked in a game run (end state of an animation
on the item screen, 2026-10-14): translate, rotate z, scale, size w / h, all 16 vertex colours (corner
order top-left, top-right, bottom-left, bottom-right; r g b a each), pane alpha, material white r g b
(also lyt-window's run) and black.g, texture scale x. The unchecked rest follows the same field order as its checked neighbours.
"""
import struct

PANE, MATERIAL = 0, 1
HERMITE, STEP = 2, 1

_CORNERS = ("tl", "tr", "bl", "br")
_CH = ("r", "g", "b", "a")

# name -> (tag, content type, target, curve, verified)
TARGETS = {
    "tx": (b"FLPA", PANE, 0, HERMITE, True), "ty": (b"FLPA", PANE, 1, HERMITE, True),
    "tz": (b"FLPA", PANE, 2, HERMITE, False), "rx": (b"FLPA", PANE, 3, HERMITE, False),
    "ry": (b"FLPA", PANE, 4, HERMITE, False), "rz": (b"FLPA", PANE, 5, HERMITE, True),
    "sx": (b"FLPA", PANE, 6, HERMITE, True), "sy": (b"FLPA", PANE, 7, HERMITE, True),
    "w": (b"FLPA", PANE, 8, HERMITE, True), "h": (b"FLPA", PANE, 9, HERMITE, True),
    "alpha": (b"FLVC", PANE, 16, HERMITE, True),
    "visible": (b"FLVI", PANE, 0, STEP, True),
}
for i, c in enumerate(_CORNERS):
    for j, ch in enumerate(_CH):
        TARGETS["vc.%s.%s" % (c, ch)] = (b"FLVC", PANE, 4 * i + j, HERMITE, True)
for j, ch in enumerate(_CH):
    TARGETS["black.%s" % ch] = (b"FLMC", MATERIAL, j, HERMITE, ch == "g")
    TARGETS["white.%s" % ch] = (b"FLMC", MATERIAL, 4 + j, HERMITE, ch in "rgb")
for j, k in enumerate(("tx", "ty", "rotate", "sx", "sy")):
    TARGETS["tex.%s" % k] = (b"FLTS", MATERIAL, j, HERMITE, k == "sx")
TARGETS["tex.pattern"] = (b"FLTP", MATERIAL, 0, STEP, True)


def register(name, tag, content_type, target, curve, verified=False):
    """Add a target (e.g. the text pane work's FLFS targets)."""
    TARGETS[name] = (tag, content_type, target, curve, verified)


def resolve(track):
    """(tag, content type, index, target, curve) of a description track."""
    if "tag" in track:  # raw form: {"tag": "FLFS", "target": 3, "type": 0, "index": 0, "curve": 2}
        return (track["tag"].encode(), track.get("type", PANE if "pane" in track else MATERIAL),
                track.get("index", 0), track["target"], track.get("curve", HERMITE))
    tag, ctype, target, curve, _ = TARGETS[track["target"]]
    return tag, ctype, track.get("index", 0), target, curve


def hermite_keys(keys, interp="linear"):
    """keys: [[frame, value(, slope)]]. Missing slopes: "linear" = mean of the neighbouring segment slopes
    (2 keys: the straight line), "ease" = 0."""
    out = b""
    fs = [k[0] for k in keys]
    vs = [k[1] for k in keys]
    for i, k in enumerate(keys):
        if len(k) > 2:
            slope = k[2]
        elif interp == "ease" or len(keys) == 1:
            slope = 0.0
        else:
            segs = []
            if i > 0:
                segs.append((vs[i] - vs[i - 1]) / (fs[i] - fs[i - 1]))
            if i + 1 < len(keys):
                segs.append((vs[i + 1] - vs[i]) / (fs[i + 1] - fs[i]))
            slope = sum(segs) / len(segs)
        out += struct.pack(">fff", k[0], k[1], slope)
    return out


def step_keys(keys):
    return b"".join(struct.pack(">fHH", k[0], int(k[1]), 0) for k in keys)


def entry(index, target, curve, keys):
    data = hermite_keys(keys[0], keys[1]) if curve == HERMITE else step_keys(keys[0])
    return bytes([index, target, curve, 0]) + struct.pack(">HHI", len(keys[0]), 0, 0xC) + data


def tag_block(tag, entries):
    """entries: [encoded entry bytes]."""
    head = tag + bytes([len(entries), 0, 0, 0])
    offs, blob = [], b""
    first = len(head) + 4 * len(entries)
    for e in entries:
        offs.append(first + len(blob))
        blob += e
    return head + b"".join(struct.pack(">I", o) for o in offs) + blob
