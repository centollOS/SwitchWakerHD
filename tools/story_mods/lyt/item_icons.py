"""The extended item icon archive (mod items): the game's BtnItemIcon_00 with more icon frames.

Built on the player's machine from the player's own BtnItemIcon_00 (in the 2D language pack); only this
code ships, never the game's file or a derived copy. The C++ twin is runtime/src/mods/item_icons.cpp;
test_lyt.py checks that both write the same bytes (on a synthetic archive of the same shape).

What the game's archive holds (read from the player's file): per icon frame n (0..54) a texture
Icon128_nn^l (RGBA8 128 x 128, the icon) and Float128_nn^t (BC5 luminance + alpha 128 x 128, a white
glow of the icon's outline, drawn behind a lifted icon and as the selection glow), and the animation
IconChange: one texture pattern (FLTP) track per material, P_Btn_00 (the Icon128 textures) and
P_BtnFloat_00 / P_BtnGlow_00 (the Float128 textures), frame n -> the textures of icon n.

The extension appends frames: frame 55 is a dummy (the game's "no item" value is 55, so a real icon there
would never show; it gets a transparent icon), then one frame per mod item from 56. For each new frame it
adds the two textures (tiled, swizzle 0) and a key to each IconChange track; the texture list of the
animation grows at its end, so the game's own keys keep their indices. Every other file stays byte for
byte as it was.

The glow (Float128) of a mod icon is made from its alpha: texels with alpha >= 128, widened by a disc of
radius 2, softened by three 5 x 5 box blurs (integer, rounded half up), white. Fitted to the game's own
glows (mean difference about 5 of 255 over a sample of its icons).
"""
import struct

from . import bflim
from .anim import tracks

FIRST_NEW_FRAME = 55   # the dummy; mod items from 56
ICON_SIZE = 128
FLOAT_MATERIALS = ("P_BtnFloat_00", "P_BtnGlow_00")
ICON_MATERIAL = "P_Btn_00"
ANIM = "anim/BtnItemIcon_00_IconChange.bflan"


def icon_name(n):
    return "Icon128_%02d^l.bflim" % n


def float_name(n):
    return "Float128_%02d^t.bflim" % n


# ---- the glow ----

def _dilate(m, w, h, r):
    offs = [(dx, dy) for dy in range(-r, r + 1) for dx in range(-r, r + 1) if dx * dx + dy * dy <= r * r + r]
    out = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            v = 0
            for dx, dy in offs:
                xx, yy = x + dx, y + dy
                if 0 <= xx < w and 0 <= yy < h and m[yy * w + xx] > v:
                    v = m[yy * w + xx]
            out[y * w + x] = v
    return out


def _box(m, w, h, r):
    k = 2 * r + 1
    n = k * k
    out = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            s = 0
            for yy in range(y - r, y + r + 1):
                if 0 <= yy < h:
                    row = yy * w
                    for xx in range(x - r, x + r + 1):
                        if 0 <= xx < w:
                            s += m[row + xx]
            out[y * w + x] = (2 * s + n) // (2 * n)
    return out


def glow(rgba, w=ICON_SIZE, h=ICON_SIZE):
    """RGBA of the Float128 picture for an icon's RGBA (white, alpha = the softened outline)."""
    m = [255 if rgba[4 * i + 3] >= 128 else 0 for i in range(w * h)]
    m = _dilate(m, w, h, 2)
    for _ in range(3):
        m = _box(m, w, h, 2)
    out = bytearray(w * h * 4)
    for i, a in enumerate(m):
        out[4 * i:4 * i + 4] = bytes((255, 255, 255, a))
    return bytes(out)


# ---- the animation ----

def _u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def _u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def parse_pai1(sec):
    """pai1 section bytes -> (frames, loop, textures, contents); contents: [(name, type, [(tag, [(index,
    target, curve, keys)])])], keys as (frame, value) pairs (step: value an int, hermite: (value, slope))."""
    frames, loop = _u16(sec, 8), sec[10]
    ntex, ncont, table = _u16(sec, 12), _u16(sec, 14), _u32(sec, 16)
    textures = []
    for i in range(ntex):
        o = 20 + _u32(sec, 20 + 4 * i)
        textures.append(sec[o:sec.index(b"\0", o)].decode())
    contents = []
    for ci in range(ncont):
        c = _u32(sec, table + 4 * ci)
        name = sec[c:c + 28].split(b"\0")[0].decode()
        ntag, ctype = sec[c + 28], sec[c + 29]
        tags = []
        for ti in range(ntag):
            t = c + _u32(sec, c + 32 + 4 * ti)
            entries = []
            for ei in range(sec[t + 4]):
                e = t + _u32(sec, t + 8 + 4 * ei)
                index, target, curve = sec[e], sec[e + 1], sec[e + 2]
                nk, ko = _u16(sec, e + 4), _u32(sec, e + 8)
                keys = []
                for k in range(nk):
                    if curve == tracks.STEP:
                        keys.append(struct.unpack_from(">fH", sec, e + ko + 8 * k))
                    else:
                        f, v, s = struct.unpack_from(">fff", sec, e + ko + 12 * k)
                        keys.append((f, v, s))
                entries.append((index, target, curve, keys))
            tags.append((sec[t:t + 4], entries))
        contents.append((name, ctype, tags))
    return frames, loop, textures, contents


def _encode_entry(index, target, curve, keys):
    if curve == tracks.STEP:
        data = b"".join(struct.pack(">fHH", f, v, 0) for f, v in keys)
    else:
        data = b"".join(struct.pack(">fff", *k) for k in keys)
    return bytes([index, target, curve, 0]) + struct.pack(">HHI", len(keys), 0, 0xC) + data


def write_pai1(frames, loop, textures, contents):
    from .anim import pai1
    encoded = [pai1.content(name, ctype, [(tag, [_encode_entry(*e) for e in entries]) for tag, entries in tags])
               for name, ctype, tags in contents]
    return pai1.write(frames, loop, textures, encoded)


def _sections(flan):
    """(header bytes, [section bytes]) of a FLAN file."""
    head = _u16(flan, 6)
    n = _u16(flan, 0x10)
    out, o = [], head
    for _ in range(n):
        size = _u32(flan, o + 4)
        out.append(flan[o:o + size])
        o += size
    return flan[:head], out


def _join(head, sections):
    body = b"".join(sections)
    return head[:0x0C] + struct.pack(">I", len(head) + len(body)) + head[0x10:] + body


def extend_animation(flan, count):
    """IconChange with `count` frames after the game's (the dummy first): keys and textures appended."""
    head, secs = _sections(flan)
    out = []
    for s in secs:
        if s[:4] != b"pai1":
            out.append(s)
            continue
        frames, loop, textures, contents = parse_pai1(s)
        last = frames  # the game's last frame (keys 0..frames)
        textures = list(textures)
        new_contents = []
        for name, ctype, tags in contents:
            new_tags = []
            for tag, entries in tags:
                new_entries = []
                for index, target, curve, keys in entries:
                    if tag == b"FLTP" and curve == tracks.STEP and keys:
                        is_float = textures[int(keys[-1][1])].startswith("Float128_")
                        keys = list(keys)
                        for i in range(count):
                            n = last + 1 + i
                            t = float_name(n) if is_float else icon_name(n)
                            if t not in textures:
                                textures.append(t)
                            keys.append((float(n), textures.index(t)))
                    new_entries.append((index, target, curve, keys))
                new_tags.append((tag, new_entries))
            new_contents.append((name, ctype, new_tags))
        out.append(write_pai1(last + count, loop, textures, new_contents))
    return _join(head, out)


def game_frames(flan):
    """The number of icon frames of the game's IconChange (its last frame + 1)."""
    _, secs = _sections(flan)
    for s in secs:
        if s[:4] == b"pai1":
            return _u16(s, 8) + 1
    raise ValueError("no pai1 in IconChange")


# ---- the archive ----

def extend(files, icons):
    """files: the game archive's {path: bytes}; icons: RGBA bytes (128 x 128) of the mod items in frame
    order (56, 57, ...). Returns the extended {path: bytes}. The dummy frame 55 is added first."""
    first = game_frames(files[ANIM])
    if first != FIRST_NEW_FRAME:
        raise ValueError("unexpected IconChange: %d frames" % first)
    out = dict(files)
    pictures = [bytes(ICON_SIZE * ICON_SIZE * 4)] + [bytes(i) for i in icons]
    for i, rgba in enumerate(pictures):
        if len(rgba) != ICON_SIZE * ICON_SIZE * 4:
            raise ValueError("an icon must be 128 x 128 RGBA")
        n = first + i
        out["timg/" + icon_name(n)] = bflim.encode(rgba, ICON_SIZE, ICON_SIZE, "rgba8", "tiled", 0)
        out["timg/" + float_name(n)] = bflim.encode(glow(rgba), ICON_SIZE, ICON_SIZE, "bc5la", "tiled", 0)
    out[ANIM] = extend_animation(files[ANIM], len(pictures))
    return out


# ---- SARC, Yaz0, the pack ----

def _hash(name):
    h = 0
    for c in name.encode():
        h = (h * 0x65 + c) & 0xFFFFFFFF
    return h


def _alignment(name, data):
    """A texture as its .bflim asks (the footer's alignment), other files 4 bytes."""
    if name.endswith(".bflim") and len(data) >= 0x28 and data[-0x28:-0x24] == b"FLIM":
        return max(4, bflim.parse_footer(data)[2])
    return 4


def archive(files, align_of=_alignment, data_align=0x2000):
    """SARC of {name: bytes}: entries by name hash, each file aligned as align_of says (the game's own
    layout archives: textures as their .bflim asks, others 4); the data starts at data_align."""
    names = sorted(files, key=_hash)
    sfnt, offs = b"", []
    for n in names:
        offs.append(len(sfnt))
        sfnt += n.encode() + b"\0"
        sfnt += b"\0" * (-len(sfnt) % 4)
    data, spans = b"", []
    for n in names:
        a = align_of(n, files[n])
        data += b"\0" * (-len(data) % a)
        spans.append((len(data), len(data) + len(files[n])))
        data += files[n]
    sfat = b"SFAT" + struct.pack(">HHI", 0xC, len(names), 0x65)
    for n, o, (s, e) in zip(names, offs, spans):
        sfat += struct.pack(">IIII", _hash(n), 0x01000000 | (o // 4), s, e)
    sfnt = b"SFNT" + struct.pack(">HH", 8, 0) + sfnt
    head = 0x14 + len(sfat) + len(sfnt)
    start = -(-head // data_align) * data_align
    out = b"SARC" + struct.pack(">HHIIHH", 0x14, 0xFEFF, start + len(data), start, 0x0100, 0) + sfat + sfnt
    return out + b"\0" * (start - len(out)) + data


def read_sarc(d):
    if d[:4] != b"SARC":
        raise ValueError("not a SARC")
    head = _u16(d, 4)
    start = _u32(d, 12)
    n = _u16(d, head + 6)
    ents = [struct.unpack_from(">IIII", d, head + 12 + 16 * i) for i in range(n)]
    names = head + 12 + 16 * n + 8
    out = {}
    for _, attr, s, e in ents:
        o = names + (attr & 0xFFFF) * 4
        out[d[o:d.index(b"\0", o)].decode()] = d[start + s:start + e]
    return out


def yaz0_decode(d):
    if d[:4] != b"Yaz0":
        return d
    size = _u32(d, 4)
    out = bytearray()
    i = 16
    while len(out) < size:
        code = d[i]
        i += 1
        for b in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> b):
                out.append(d[i])
                i += 1
            else:
                dist = ((d[i] & 15) << 8 | d[i + 1]) + 1
                n = d[i] >> 4
                i += 2
                if n == 0:
                    n = d[i] + 0x12
                    i += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


def yaz0_store(data):
    """Yaz0 of literal bytes only (as lyt_build's yaz0_stored)."""
    out = b"Yaz0" + struct.pack(">I", len(data)) + bytes(8)
    for i in range(0, len(data), 8):
        out += b"\xff" + data[i:i + 8]
    return out


YAZ0_WINDOW, YAZ0_MAX, YAZ0_CHAIN = 0x1000, 0x111, 64


def yaz0_compress(data):
    """Yaz0 with a greedy LZ search (the C++ twin writes the same bytes): at each position the longest
    match of at least 3 bytes among the last 64 earlier positions with the same 3 bytes within 0x1000 bytes
    (the nearest wins a tie), else a literal."""
    n = len(data)
    out = bytearray(b"Yaz0" + struct.pack(">I", n) + bytes(8))
    heads = {}
    pos = 0
    while pos < n:
        flag_at = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if pos >= n:
                break
            best, best_d = 0, 0
            if pos + 2 < n:
                chain = heads.get(data[pos:pos + 3])
                if chain:
                    limit = min(YAZ0_MAX, n - pos)
                    tried = 0
                    for cand in reversed(chain):
                        if pos - cand > YAZ0_WINDOW or tried >= YAZ0_CHAIN:
                            break
                        tried += 1
                        k = 3
                        while k < limit and data[cand + k] == data[pos + k]:
                            k += 1
                        if k > best:
                            best, best_d = k, pos - cand
                            if k == limit:
                                break
            if best >= 3:
                d = best_d - 1
                if best <= 0x11:
                    out += bytes((((best - 2) << 4) | (d >> 8), d & 0xFF))
                else:
                    out += bytes((d >> 8, d & 0xFF, best - 0x12))
                step = best
            else:
                flags |= 0x80 >> bit
                out.append(data[pos])
                step = 1
            for q in range(pos, pos + step):
                if q + 2 < n:
                    heads.setdefault(data[q:q + 3], []).append(q)
            pos += step
        out[flag_at] = flags
    return bytes(out)


MEMBER = "BtnItemIcon_00.szs"


def extend_pack(pack, icons):
    """The 2D language pack with BtnItemIcon_00.szs extended (Yaz0-compressed); other members as they were
    (the pack: every member aligned to 0x2000, as the game's)."""
    members = read_sarc(pack)
    if MEMBER not in members:
        raise ValueError("no %s in the pack" % MEMBER)
    files = read_sarc(yaz0_decode(members[MEMBER]))
    members[MEMBER] = yaz0_compress(archive(extend(files, icons)))
    return archive(members, align_of=lambda n, d: 0x2000)


def frame_of(index):
    """The animation frame of the index-th mod item (0-based) in the extended archive."""
    return FIRST_NEW_FRAME + 1 + index
