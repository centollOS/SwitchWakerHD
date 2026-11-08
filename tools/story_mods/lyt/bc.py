"""BC (S3TC / RGTC) block compression for layout textures: encoders and decoders, numpy-vectorised.

Blocks are 4 x 4 texels; BC1 and BC4 blocks are 8 bytes, BC2 / BC3 / BC5 blocks 16 bytes (little-endian
fields, as the GPU reads them). The encoders use integer arithmetic only, so the port's own writer
(runtime/src/mods/lyt_build.cpp) produces the same bytes (tools/story_mods/test_lyt.py compares them):
  colour (BC1, BC3's colour): the texels' per-channel range, inset by 1/16, with each channel's direction
    taken from the sign of its covariance with the widest channel (a range fit on the box's diagonal),
    endpoints in 5:6:5; every texel takes the nearest palette entry (ties: the lower index)
  one channel (BC4, BC3's alpha): min and max as the endpoints (8-value mode), nearest entry
Good enough for UI art; the decoders exist for the tests and for previews.
"""
import numpy as np


def to_blocks(rgba, w, h):
    """RGBA bytes (w x h) -> array (by, bx, 16, 4) of 4 x 4 blocks; edges padded by repetition."""
    a = np.frombuffer(bytes(rgba), np.uint8).reshape(h, w, 4)
    bw, bh = -(-w // 4), -(-h // 4)
    a = np.pad(a, ((0, bh * 4 - h), (0, bw * 4 - w), (0, 0)), mode="edge")
    return a.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh, bw, 16, 4)


def from_blocks(blocks, w, h):
    bh, bw = blocks.shape[:2]
    a = blocks.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return np.ascontiguousarray(a[:h, :w]).tobytes()


# ---------------------------------------------------------------- BC1 (colour, 1-bit alpha)
def _565(c):
    c = c.astype(np.int32)
    return ((c[..., 0] * 31 + 127) // 255) << 11 | ((c[..., 1] * 63 + 127) // 255) << 5 | ((c[..., 2] * 31 + 127) // 255)


def _unpack565(v):
    v = v.astype(np.int32)
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1)


def _bc1_colour(px, alpha_mask=None):
    """px (..., 16, 3) -> (..., 8) BC1 colour blocks. alpha_mask (..., 16) bool: transparent texels
    (BC1 3-colour mode with index 3 = transparent), None for the 4-colour mode (BC2 / BC3 colour part)."""
    f = px.astype(np.float32)
    opaque = np.ones(px.shape[:-1], bool) if alpha_mask is None else ~alpha_mask
    wsum = np.maximum(opaque.sum(-1, keepdims=True), 1)
    mean = (f * opaque[..., None]).sum(-2) / wsum
    d = (f - mean[..., None, :]) * opaque[..., None]
    cov = np.einsum("...ni,...nj->...ij", d, d)
    axis = np.ones(mean.shape, np.float32)
    for _ in range(8):  # power iteration for the main axis
        axis = np.einsum("...ij,...j->...i", cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=-1, keepdims=True), 1e-6)
    t = np.einsum("...ni,...i->...n", d, axis)
    big = np.where(opaque, t, -np.inf).max(-1)
    small = np.where(opaque, t, np.inf).min(-1)
    big = np.where(np.isfinite(big), big, 0)
    small = np.where(np.isfinite(small), small, 0)
    c0 = np.clip(mean + axis * big[..., None], 0, 255)
    c1 = np.clip(mean + axis * small[..., None], 0, 255)
    e0, e1 = _565(c0), _565(c1)
    if alpha_mask is None:
        # 4-colour mode needs e0 > e1; equal endpoints: all texels use index 0
        swap = e0 < e1
        e0, e1 = np.where(swap, e1, e0), np.where(swap, e0, e1)
    else:
        has_alpha = alpha_mask.any(-1)
        # 3-colour mode (e0 <= e1) for blocks with transparent texels, 4-colour mode otherwise
        lo, hi = np.minimum(e0, e1), np.maximum(e0, e1)
        e0 = np.where(has_alpha, lo, hi)
        e1 = np.where(has_alpha, hi, lo)
    p0, p1 = _unpack565(e0).astype(np.float32), _unpack565(e1).astype(np.float32)
    four = e0 > e1
    pal = np.stack([p0, p1,
                    np.where(four[..., None], (2 * p0 + p1) / 3, (p0 + p1) / 2),
                    np.where(four[..., None], (p0 + 2 * p1) / 3, np.float32(1e9))], -2)
    dist = ((f[..., :, None, :] - pal[..., None, :, :]) ** 2).sum(-1)
    idx = dist.argmin(-1).astype(np.uint32)
    if alpha_mask is not None:
        idx = np.where(alpha_mask, 3, idx)
    if alpha_mask is None:
        idx = np.where((e0 == e1)[..., None], 0, idx)
    bits = (idx << (2 * np.arange(16, dtype=np.uint32))).sum(-1, dtype=np.uint64).astype(np.uint32)
    out = np.zeros(px.shape[:-2] + (8,), np.uint8)
    out[..., 0], out[..., 1] = e0 & 0xFF, e0 >> 8
    out[..., 2], out[..., 3] = e1 & 0xFF, e1 >> 8
    for i in range(4):
        out[..., 4 + i] = (bits >> (8 * i)) & 0xFF
    return out


def _bc1_decode_colour(blk, allow_three=True):
    e0 = blk[..., 0].astype(np.int32) | blk[..., 1].astype(np.int32) << 8
    e1 = blk[..., 2].astype(np.int32) | blk[..., 3].astype(np.int32) << 8
    p0, p1 = _unpack565(e0), _unpack565(e1)
    four = (e0 > e1) | (not allow_three)
    p2 = np.where(four[..., None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[..., None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], -2)
    a = np.stack([np.full(e0.shape, 255)] * 3 + [np.where(four, 255, 0)], -1)
    bits = sum(blk[..., 4 + i].astype(np.uint32) << (8 * i) for i in range(4))
    idx = (bits[..., None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    rgb = np.take_along_axis(pal, idx[..., None].astype(np.int64), -2)
    alpha = np.take_along_axis(a, idx.astype(np.int64), -1)
    return rgb, alpha


# ---------------------------------------------------------------- BC4 (one channel)
def _bc4(v):
    """v (..., 16) uint8 -> (..., 8) BC4 blocks (8-value mode: e0 > e1)."""
    v = v.astype(np.int32)
    hi, lo = v.max(-1), v.min(-1)
    e0 = hi
    e1 = np.where(hi == lo, np.maximum(lo - 1, 0), lo)
    e0 = np.where(hi == lo, np.maximum(e1 + 1, hi), e0)
    w0 = np.array([7, 0, 6, 5, 4, 3, 2, 1])
    pal = (e0[..., None] * w0 + e1[..., None] * (7 - w0)) / 7.0
    idx = np.abs(v[..., :, None] - pal[..., None, :]).argmin(-1).astype(np.uint64)
    bits = (idx << (3 * np.arange(16, dtype=np.uint64))).sum(-1, dtype=np.uint64)
    out = np.zeros(v.shape[:-1] + (8,), np.uint8)
    out[..., 0], out[..., 1] = e0, e1
    for i in range(6):
        out[..., 2 + i] = (bits >> np.uint64(8 * i)) & np.uint64(0xFF)
    return out


def _bc4_decode(blk):
    e0, e1 = blk[..., 0].astype(np.int32), blk[..., 1].astype(np.int32)
    eight = e0 > e1
    w0 = np.array([7, 0, 6, 5, 4, 3, 2, 1]); w1 = 7 - w0
    p8 = (e0[..., None] * w0 + e1[..., None] * w1 + 3) // 7
    w0b = np.array([5, 0, 4, 3, 2, 1]); w1b = 5 - w0b
    p6 = (e0[..., None] * w0b + e1[..., None] * w1b + 2) // 5
    p6 = np.concatenate([p6, np.zeros(e0.shape + (1,), np.int32), np.full(e0.shape + (1,), 255)], -1)
    pal = np.where(eight[..., None], p8, p6)
    bits = sum(blk[..., 2 + i].astype(np.uint64) << np.uint64(8 * i) for i in range(6))
    idx = (bits[..., None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)
    return np.take_along_axis(pal, idx.astype(np.int64), -1)


# ---------------------------------------------------------------- formats
def _q565(c):
    return ((c[0] * 31 + 127) // 255) << 11 | ((c[1] * 63 + 127) // 255) << 5 | ((c[2] * 31 + 127) // 255)


def _x565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)]


def colour_block(px, transparent=None):
    """8 bytes of BC1 colour for 16 RGBA texels (lists). transparent: per texel, None for the 4-colour mode
    (BC3); with any transparent texel the 3-colour mode with index 3 transparent (BC1)."""
    use = [i for i in range(16) if not (transparent and transparent[i])]
    if transparent and not use:  # all transparent
        return bytes([0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF])
    n = len(use)
    mn = [min(px[i][c] for i in use) for c in range(3)]
    mx = [max(px[i][c] for i in use) for c in range(3)]
    for c in range(3):  # inset by 1/16 of the range
        d = (mx[c] - mn[c]) >> 4
        mn[c] += d
        mx[c] -= d
    rng = [mx[c] - mn[c] for c in range(3)]
    p = rng.index(max(rng))
    mean = [sum(px[i][c] for i in use) // n for c in range(3)]
    hi, lo = list(mx), list(mn)
    for c in range(3):
        if c == p:
            continue
        cov = sum((px[i][p] - mean[p]) * (px[i][c] - mean[c]) for i in use)
        if cov < 0:
            hi[c], lo[c] = mn[c], mx[c]
    a, b = _q565(hi), _q565(lo)
    three = bool(transparent) and len(use) < 16
    if three:
        e0, e1 = min(a, b), max(a, b)
    else:
        e0, e1 = max(a, b), min(a, b)
    p0, p1 = _x565(e0), _x565(e1)
    if three:
        pal = [p0, p1, [(p0[c] + p1[c]) // 2 for c in range(3)]]
    elif e0 == e1:
        pal = [p0]
    else:
        pal = [p0, p1, [(2 * p0[c] + p1[c]) // 3 for c in range(3)], [(p0[c] + 2 * p1[c]) // 3 for c in range(3)]]
    bits = 0
    for i in range(16):
        if transparent and transparent[i]:
            k = 3
        else:
            best, k = None, 0
            for j, q in enumerate(pal):
                d = sum((px[i][c] - q[c]) ** 2 for c in range(3))
                if best is None or d < best:
                    best, k = d, j
        bits |= k << (2 * i)
    return bytes([e0 & 0xFF, e0 >> 8, e1 & 0xFF, e1 >> 8]) + bits.to_bytes(4, "little")


def channel_block(v):
    """8 bytes of BC4 for 16 values."""
    mx, mn = max(v), min(v)
    if mx == mn:
        return bytes([mx, mn, 0, 0, 0, 0, 0, 0])
    w0 = [7, 0, 6, 5, 4, 3, 2, 1]
    pal = [(mx * w + mn * (7 - w) + 3) // 7 for w in w0]
    bits = 0
    for i in range(16):
        best, k = None, 0
        for j, q in enumerate(pal):
            d = abs(v[i] - q)
            if best is None or d < best:
                best, k = d, j
        bits |= k << (3 * i)
    return bytes([mx, mn]) + bits.to_bytes(6, "little")


def encode(fmt, rgba, w, h):
    if fmt in ("bc1", "bc3", "bc4", "bc5"):
        b = to_blocks(rgba, w, h).tolist()
        out = bytearray()
        for row in b:
            for blk in row:
                if fmt == "bc1":
                    out += colour_block(blk, [t[3] < 128 for t in blk])
                elif fmt == "bc3":
                    vis = [t for t in blk if t[3] > 0]
                    out += channel_block([t[3] for t in blk])
                    out += colour_block(blk if len(vis) in (0, 16) else [t if t[3] > 0 else vis[0] for t in blk])
                elif fmt == "bc4":
                    out += channel_block([t[0] for t in blk])
                else:
                    out += channel_block([t[0] for t in blk]) + channel_block([t[1] for t in blk])
        return bytes(out)
    return _encode_float(fmt, rgba, w, h)


def _encode_float(fmt, rgba, w, h):
    b = to_blocks(rgba, w, h)
    if fmt == "bc1":
        return _bc1_colour(b[..., :3], b[..., 3] < 128).tobytes()
    if fmt == "bc2":
        a4 = (b[..., 3].astype(np.uint32) * 15 + 127) // 255
        ab = np.zeros(b.shape[:2] + (8,), np.uint8)
        for i in range(8):
            ab[..., i] = a4[..., 2 * i] | (a4[..., 2 * i + 1] << 4)
        return np.concatenate([ab, _bc1_colour(b[..., :3])], -1).tobytes()
    if fmt == "bc3":
        return np.concatenate([_bc4(b[..., 3]), _bc1_colour(b[..., :3])], -1).tobytes()
    if fmt == "bc4":
        return _bc4(b[..., 0]).tobytes()
    if fmt == "bc5":
        return np.concatenate([_bc4(b[..., 0]), _bc4(b[..., 1])], -1).tobytes()
    raise ValueError(fmt)


BLOCK_BYTES = {"bc1": 8, "bc2": 16, "bc3": 16, "bc4": 8, "bc5": 16}


def decode(fmt, data, w, h):
    """BC blocks (row-major, untiled) -> RGBA bytes. BC4 decodes to grey (r = g = b), BC5 to (r, g, 0)."""
    bw, bh = -(-w // 4), -(-h // 4)
    blk = np.frombuffer(bytes(data), np.uint8)[:bw * bh * BLOCK_BYTES[fmt]].reshape(bh, bw, BLOCK_BYTES[fmt])
    out = np.zeros((bh, bw, 16, 4), np.int32)
    if fmt == "bc1":
        rgb, a = _bc1_decode_colour(blk)
        out[..., :3], out[..., 3] = rgb, a
    elif fmt == "bc2":
        rgb, _ = _bc1_decode_colour(blk[..., 8:], False)
        a = np.stack([(blk[..., i // 2] >> (4 * (i % 2))) & 15 for i in range(16)], -1).astype(np.int32)
        out[..., :3], out[..., 3] = rgb, a * 17
    elif fmt == "bc3":
        rgb, _ = _bc1_decode_colour(blk[..., 8:], False)
        out[..., :3], out[..., 3] = rgb, _bc4_decode(blk[..., :8])
    elif fmt == "bc4":
        v = _bc4_decode(blk)
        out[..., 0] = out[..., 1] = out[..., 2] = v
        out[..., 3] = 255
    elif fmt == "bc5":
        out[..., 0], out[..., 1] = _bc4_decode(blk[..., :8]), _bc4_decode(blk[..., 8:])
        out[..., 3] = 255
    return from_blocks(out.astype(np.uint8), w, h)
