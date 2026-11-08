"""GX2 surface tiling for 2D textures (one slice, one sample, no mips), as used by the game's layout
textures: tile mode 1 (linear aligned), 2 (1D tiled thin) and 4 (2D tiled thin, the game's usual mode).

The address arithmetic mirrors the port's vendored Cemu LatteAddrLib (runtime/third_party/cemu/Cafe/HW/
Latte/LatteAddrLib/LatteAddrLib_Coord.cpp: ComputeSurfaceAddrFromCoord{Linear,MicroTiled,MacroTiled}),
the code the port itself uses to read guest textures, restricted to the cases above (2 pipes, 4 banks,
256-byte pipe interleave). Elements are texels, or 4x4 blocks for BC formats; bpp is bits per element.

tile(data, w, h, bpp, mode, swizzle) places a linear row-major element array into a tiled surface;
untile() is the inverse (used by the tests and to read surfaces back).
"""
import numpy as np

PIPES, BANKS, PIPE_INTERLEAVE = 2, 4, 256
MACRO_PITCH, MACRO_HEIGHT = 8 * BANKS, 8 * PIPES  # 32 x 16 elements for 2D tiled thin


def surface_size(w, h, bpp, mode):
    """(pitch, height, image bytes, alignment) in elements for a surface of w x h elements."""
    if mode == 1:  # linear aligned: 64-element pitch (as the prototype's RGBA8 textures), alignment 0x100
        align = max(64, PIPE_INTERLEAVE * 8 // bpp)
        pitch = -(-w // align) * align
        return pitch, h, pitch * h * bpp // 8, 0x100
    if mode == 2:  # 1D tiled thin: 8 x 8 micro tiles
        pitch, height = -(-w // 8) * 8, -(-h // 8) * 8
        return pitch, height, pitch * height * bpp // 8, PIPE_INTERLEAVE
    if mode == 4:
        pitch, height = -(-w // MACRO_PITCH) * MACRO_PITCH, -(-h // MACRO_HEIGHT) * MACRO_HEIGHT
        return pitch, height, pitch * height * bpp // 8, MACRO_PITCH * MACRO_HEIGHT * bpp // 8
    raise ValueError("tile mode %d is not supported" % mode)


def _pixel_index(x, y, bpp):
    """Element index inside an 8 x 8 micro tile (non-depth, thin), by bits per element."""
    b = lambda v, i: (v >> i) & 1
    if bpp == 8:
        bits = (b(x, 0), b(x, 1), b(x, 2), b(y, 1), b(y, 0), b(y, 2))
    elif bpp == 16:
        bits = (b(x, 0), b(x, 1), b(x, 2), b(y, 0), b(y, 1), b(y, 2))
    elif bpp == 64:
        bits = (b(x, 0), b(y, 0), b(x, 1), b(x, 2), b(y, 1), b(y, 2))
    elif bpp == 128:
        bits = (b(y, 0), b(x, 0), b(x, 1), b(x, 2), b(y, 1), b(y, 2))
    else:  # 32 bits (and 96)
        bits = (b(x, 0), b(x, 1), b(y, 0), b(x, 2), b(y, 1), b(y, 2))
    return sum(v << i for i, v in enumerate(bits))


def addresses(w, h, bpp, mode, swizzle=0):
    """Byte address of every element (y, x) of a w x h element surface."""
    pitch, height, _, _ = surface_size(w, h, bpp, mode)
    y, x = np.mgrid[0:h, 0:w].astype(np.int64)
    if mode == 1:
        return (x + pitch * y) * bpp // 8
    pix = _pixel_index(x, y, bpp) * bpp // 8
    if mode == 2:
        micro_bytes = 64 * bpp // 8
        return pix + micro_bytes * ((x >> 3) + (pitch >> 3) * (y >> 3))
    # mode 4: 2D tiled thin (macro tiles of 32 x 16 elements, pipe/bank rotation, swizzle)
    pipe = ((y >> 3) ^ (x >> 3)) & 1
    bank = (y >> 4) & 3
    bank = ((bank >> 1) | (bank << 1)) & 3
    bank = (bank ^ (x >> 3)) & 3
    pipe_swizzle, bank_swizzle = (swizzle >> 8) & 1, (swizzle >> 9) & 3
    bank_pipe = (pipe + PIPES * bank) ^ (pipe_swizzle + PIPES * bank_swizzle)
    bank_pipe %= PIPES * BANKS
    pipe, bank = bank_pipe % PIPES, bank_pipe // PIPES
    macro_bytes = MACRO_PITCH * MACRO_HEIGHT * bpp // 8
    macro_off = ((x // MACRO_PITCH) + (pitch // MACRO_PITCH) * (y // MACRO_HEIGHT)) * macro_bytes
    off = (macro_off >> 3) + pix
    off = ((off & ~0xFF) << 3) | (off & 0xFF)
    return off | (pipe << 8) | (bank << 9)


def tile(data, w, h, bpp, mode, swizzle=0):
    """Linear row-major elements (w x h, bpp bits each) -> tiled surface bytes."""
    esize = bpp // 8
    _, _, size, _ = surface_size(w, h, bpp, mode)
    src = np.frombuffer(bytes(data), np.uint8).reshape(h * w, esize)
    out = np.zeros(size, np.uint8)
    addr = addresses(w, h, bpp, mode, swizzle).reshape(-1)
    idx = addr[:, None] + np.arange(esize)[None, :]
    out[idx] = src
    return out.tobytes()


def untile(surface, w, h, bpp, mode, swizzle=0):
    """Tiled surface bytes -> linear row-major elements."""
    esize = bpp // 8
    src = np.frombuffer(bytes(surface), np.uint8)
    addr = addresses(w, h, bpp, mode, swizzle).reshape(-1)
    return src[addr[:, None] + np.arange(esize)[None, :]].tobytes()
