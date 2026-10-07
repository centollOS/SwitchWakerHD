#!/usr/bin/env python3
"""Merge shadercache_gl.bin files (the OpenGL renderer's shader cache) into one, without duplicates.

    python3 tools/switch/merge_shadercache.py out.bin first.bin [more.bin ...]

The first file's records come first (give the console's file first). The output holds every GLSL
source once (record 1, by hash), then every translation once (record 3, by stage and key), then
every linked pair once (record 2); translations and pairs whose GLSL is in none of the inputs are
dropped. Sources come first because the loader (gfx/gl/shaders.cpp load_shader_cache) takes a
translation only if its GLSL was read before it. Prints the counts per input and the new ones each
added. The format is described in tools/switch/extract_shadercache.py."""
import struct
import sys
import zlib


def records(path):
    data = open(path, "rb").read()
    if data[:4] != b"WGS1":
        sys.exit(f"{path}: not a shadercache_gl.bin (no WGS1 magic)")
    i = 4
    while i < len(data):
        kind = data[i]
        if kind == 2 and i + 17 <= len(data):
            yield 2, data[i:i + 17]
            i += 17
        elif kind == 3 and i + 5 <= len(data):
            (size,) = struct.unpack_from("<I", data, i + 1)
            if i + 5 + size > len(data):
                break
            yield 3, data[i:i + 5 + size]
            i += 5 + size
        elif kind == 1 and i + 18 <= len(data):
            (packed,) = struct.unpack_from("<I", data, i + 10)
            if i + 18 + packed > len(data):
                break
            yield 1, data[i:i + 18 + packed]
            i += 18 + packed
        else:
            break  # a record cut short ends the file


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    sources, translations, pairs = {}, {}, {}
    for path in sys.argv[2:]:
        counts = {1: 0, 2: 0, 3: 0}
        new = {1: 0, 2: 0, 3: 0}
        for kind, rec in records(path):
            counts[kind] += 1
            if kind == 1:
                key, table = rec[2:10], sources
                zlib.decompress(rec[18:])  # (a damaged source would fail every link that needs it)
            elif kind == 3:
                key, table = rec[5:14], translations  # stage byte and the u64 translation key
            else:
                key, table = rec[1:17], pairs
            if key not in table:
                table[key] = rec
                new[kind] += 1
        print(f"{path}: {counts[1]} sources, {counts[3]} translations, {counts[2]} pairs; "
              f"new: {new[1]} sources, {new[3]} translations, {new[2]} pairs")
    known = set(sources)
    kept_t = [r for r in translations.values() if r[22:30] in known]  # payload + 1 + 8 + 8: GLSL hash
    kept_p = [r for r in pairs.values() if r[1:9] in known and r[9:17] in known]
    with open(sys.argv[1], "wb") as f:
        f.write(b"WGS1")
        for r in sources.values():
            f.write(r)
        for r in kept_t:
            f.write(r)
        for r in kept_p:
            f.write(r)
    print(f"{sys.argv[1]}: {len(sources)} sources, {len(kept_t)} translations, {len(kept_p)} pairs "
          f"(dropped for missing GLSL: {len(translations) - len(kept_t)} translations, {len(pairs) - len(kept_p)} pairs)")


if __name__ == "__main__":
    main()
