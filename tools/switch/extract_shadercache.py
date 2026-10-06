#!/usr/bin/env python3
"""Write every GLSL source in a shadercache_gl.bin (the OpenGL renderer's shader cache, also the one on
the Switch's SD card) to <out dir>/<hash>_vs.glsl or _ps.glsl. The hashes are the ones the log prints,
e.g. the watchdog's "linking vertex shader <hash> with pixel shader <hash>".

    python3 tools/switch/extract_shadercache.py shadercache_gl.bin out/

Records (gfx/gl/shaders.cpp): magic WGS1, then {1, vertex, u64 hash, u32 packed, u32 size, zlib GLSL},
{2, u64 vertex hash, u64 pixel hash} and {3, u32 size, translation}."""
import os
import struct
import sys
import zlib


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    if data[:4] != b"WGS1":
        sys.exit("not a shadercache_gl.bin (no WGS1 magic)")
    os.makedirs(out, exist_ok=True)
    i, sources, pairs = 4, 0, 0
    while i < len(data):
        kind = data[i]
        if kind == 1 and i + 18 <= len(data):
            vertex = data[i + 1]
            (h,) = struct.unpack_from("<Q", data, i + 2)
            packed, _size = struct.unpack_from("<II", data, i + 10)
            if i + 18 + packed > len(data):
                break
            glsl = zlib.decompress(data[i + 18:i + 18 + packed])
            with open(os.path.join(out, f"{h:016x}_{'vs' if vertex else 'ps'}.glsl"), "wb") as f:
                f.write(glsl)
            sources += 1
            i += 18 + packed
        elif kind == 2:
            pairs += 1
            i += 17
        elif kind == 3 and i + 5 <= len(data):
            (size,) = struct.unpack_from("<I", data, i + 1)
            i += 5 + size
        else:
            break  # a record cut short ends the file
    print(f"{sources} sources written to {out}; {pairs} linked pairs recorded")


if __name__ == "__main__":
    main()
