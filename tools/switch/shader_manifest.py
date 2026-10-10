#!/usr/bin/env python3
"""The shader manifest on the computer (docs/shader-cache-from-dump-plan.md, step 3).

    shader_manifest.py info MANIFEST
        the records: variants, vertex/pixel, distinct programs and their sizes
    shader_manifest.py find MANIFEST GAME_DIR
        looks for every program of the manifest in the dump's shader files (content/**/*.sharcfb, *.gsh):
        where each one is, and which are missing. Exit status 1 when one is missing.
    shader_manifest.py programs MANIFEST GAME_DIR OUT
        the manifest's programs from every shader archive of the dump (upstream's tools/shaderprep.py extractor:
        the .sharcfb files, also inside the SARC and Yaz0 archives), written to OUT (WSP1) for
        `dksh_cache translate`. OUT holds game code: it stays on your computer (build/), never share it.
    shader_manifest.py speculate MANIFEST GAME_DIR OUT_MANIFEST OUT_PROGRAMS [--max N]
        the manifest's variants plus speculative ones (upstream shaderprep's type 3 records): every archive program
        the manifest has not seen takes up to N (default 2) states recorded for programs of its family, i.e. with
        the same register block in their own GX2 structure, so places not visited yet get shaders too. Writes a
        manifest and a programs file for `dksh_cache translate` (both stay on your computer).
    shader_manifest.py selftest GAME_DIR
        checks the search itself: programs cut from the dump's own files at known places are found there

The manifest (runtime/src/gfx/deko/shader_manifest.cpp) names each program by hash_bytes() of its bytes, its
size and an FNV-1a hash of its first 64 bytes; this script computes the same hashes over the files.
"""
import os
import struct
import sys
import zlib

M64 = (1 << 64) - 1


def hash_bytes(b, h=0x9E3779B97F4A7C15):
    """gfx/deko shaders_dk.cpp hash_bytes"""
    n = len(b) - len(b) % 8
    for (word,) in struct.iter_unpack("<Q", b[:n]):
        h = ((h ^ word) * 0xFF51AFD7ED558CCD) & M64
        h ^= h >> 32
    for x in b[n:]:
        h = ((h ^ x) * 0x100000001B3) & M64
    return h ^ (h >> 29)


def fnv(b):
    h = 0xCBF29CE484222325
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & M64
    return h


def read_manifest(path):
    """[(vertex, program hash, size, prefix hash, fetch bytes, {register: value})]"""
    data = open(path, "rb").read()
    if data[:4] != b"WSM1" or struct.unpack_from("<I", data, 4)[0] != 1:
        sys.exit("%s: not a shader manifest (WSM1, version 1)" % path)
    out, at = [], 8
    while at + 9 <= len(data) and data[at] == 1:
        packed, size = struct.unpack_from("<II", data, at + 1)
        d = zlib.decompress(data[at + 9:at + 9 + packed])
        if len(d) != size:
            break
        at += 9 + packed
        vertex, phash, psize, prefix, compact, fsize = struct.unpack_from("<BQIQBI", d, 0)
        o = 26
        fetch = d[o:o + fsize]
        o += fsize
        (count,) = struct.unpack_from("<I", d, o)
        o += 4
        regs = {}
        for _ in range(count):
            i, v = struct.unpack_from("<HI", d, o)
            regs[i] = v
            o += 6
        out.append((bool(vertex), phash, psize, prefix, bytes(fetch), regs))
    return out


def read_manifest_full(path):
    """as read_manifest, plus the fetch shader's compact flag: (vertex, hash, size, prefix, fetch, regs, compact)"""
    data = open(path, "rb").read()
    out, at = [], 8
    while at + 9 <= len(data) and data[at] == 1:
        packed, size = struct.unpack_from("<II", data, at + 1)
        d = zlib.decompress(data[at + 9:at + 9 + packed])
        at += 9 + packed
        vertex, phash, psize, prefix, compact, fsize = struct.unpack_from("<BQIQBI", d, 0)
        o = 26 + fsize
        fetch = d[26:o]
        (count,) = struct.unpack_from("<I", d, o)
        o += 4
        regs = {}
        for _ in range(count):
            i, v = struct.unpack_from("<HI", d, o)
            regs[i] = v
            o += 6
        out.append((bool(vertex), phash, psize, prefix, bytes(fetch), regs, compact))
    return out


def shader_files(game_dir):
    files = []
    for root, _, names in os.walk(os.path.join(game_dir, "content")):
        for n in sorted(names):
            if n.endswith((".sharcfb", ".gsh")):
                files.append(os.path.join(root, n))
    return sorted(files)


def find(programs, game_dir, step=4):
    """programs: {(hash, size, prefix)}. Returns {(hash, size, prefix): (file, offset)}."""
    by_prefix = {}
    for p in programs:
        by_prefix.setdefault(p[2], []).append(p)
    lens = sorted({min(p[1], 64) for p in programs})
    found = {}
    for path in shader_files(game_dir):
        data = open(path, "rb").read()
        for off in range(0, len(data), step):
            for n in lens:
                if off + n > len(data):
                    continue
                cands = by_prefix.get(fnv(data[off:off + n]))
                if not cands:
                    continue
                for p in cands:
                    if p not in found and off + p[1] <= len(data) and hash_bytes(data[off:off + p[1]]) == p[0]:
                        found[p] = (os.path.relpath(path, game_dir), off)
        if len(found) == len(programs):
            break
    return found


def archive_programs(game_dir):
    """{(hash_bytes, size): microcode} of every vertex/pixel shader in the dump's archives"""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    import shaderprep
    out = {}
    for (_typ, code) in shaderprep.game_shaders(game_dir):
        out.setdefault((hash_bytes(code), len(code)), code)
    return out


def own_block(shaderprep, vertex, w):
    """shaderprep.own_block as this runtime's GX2SetVertexShader writes it: SQ_VTX_SEMANTIC_CLEAR clears only the
    shader's semantics (0xFFFFFFFF << count), where shaderprep writes 0xFFFFFFFF; without this no recorded vertex
    state matched its family and the speculation made no vertex shaders"""
    blk = shaderprep.own_block(vertex, w)
    if vertex and shaderprep.mmSQ_VTX_SEMANTIC_CLEAR in blk:
        blk[shaderprep.mmSQ_VTX_SEMANTIC_CLEAR] = (0xFFFFFFFF << min(w[0x40 // 4], 32)) & 0xFFFFFFFF
    return blk


def write_manifest(path, recs):
    """WSM1 as the runtime writes it (runtime/src/gfx/deko/shader_manifest.cpp)"""
    with open(path, "wb") as f:
        f.write(b"WSM1" + struct.pack("<I", 1))
        for vertex, phash, psize, prefix, fetch, regs, compact in recs:
            d = struct.pack("<BQIQBI", vertex, phash, psize, prefix, compact, len(fetch)) + fetch
            items = sorted((i, v) for i, v in regs.items() if v and not 0xC000 <= i < 0xD000)
            d += struct.pack("<I", len(items)) + b"".join(struct.pack("<HI", i, v) for i, v in items)
            z = zlib.compress(d, 6)
            f.write(struct.pack("<BII", 1, len(z), len(d)) + z)


def write_programs(path, programs):
    with open(path, "wb") as f:
        f.write(b"WSP1" + struct.pack("<I", len(programs)))
        for (h, n), code in sorted(programs.items()):
            f.write(struct.pack("<QI", h, n) + code)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == "info":
        recs = read_manifest(sys.argv[2])
        progs = {(r[1], r[2], r[3]) for r in recs}
        print("%d variants (%d vertex, %d pixel), %d distinct programs, sizes %s" % (
            len(recs), sum(r[0] for r in recs), sum(not r[0] for r in recs), len(progs),
            sorted({p[1] for p in progs})[:20]))
    elif cmd == "find":
        recs = read_manifest(sys.argv[2])
        progs = {(r[1], r[2], r[3]) for r in recs}
        found = find(progs, sys.argv[3])
        for p in sorted(progs):
            where = found.get(p)
            print("%016x %6d  %s" % (p[0], p[1], "%s +0x%x" % where if where else "NOT FOUND"))
        print("%d of %d programs found in the dump" % (len(found), len(progs)))
        sys.exit(0 if len(found) == len(progs) else 1)
    elif cmd == "programs":
        if len(sys.argv) != 5:
            sys.exit(__doc__)
        recs = read_manifest(sys.argv[2])
        wanted = {(r[1], r[2]) for r in recs}
        found = {k: v for k, v in archive_programs(sys.argv[3]).items() if k in wanted}
        write_programs(sys.argv[4], found)
        print("%d of %d programs of the manifest found in the dump's archives -> %s" % (len(found), len(wanted), sys.argv[4]))
    elif cmd == "speculate":
        args = sys.argv[2:]
        limit = 2
        if "--max" in args:
            limit = int(args[args.index("--max") + 1])
            del args[args.index("--max"):args.index("--max") + 2]
        if len(args) != 4:
            sys.exit(__doc__)
        manifest, game, out_manifest, out_programs = args
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
        import shaderprep
        recs = read_manifest_full(manifest)
        archive = shaderprep.game_shaders(game)  # (type, code) -> (structure words, path)
        programs, seen = {}, set()
        for (typ, code), (w, path) in archive.items():
            programs[(hash_bytes(code), len(code))] = (typ == shaderprep.VS, code, w)
        for r in recs:
            seen.add((r[1], r[2]))
        # families: the register sets of the archive's blocks; a recorded state belongs to the family whose
        # registers it holds with those values (as shaderprep cmd_build)
        regsets = {(v, tuple(sorted(own_block(shaderprep, v, w)))) for (v, code, w) in programs.values()}
        by_family = {}
        for r in recs:
            vertex, regs = r[0], r[5]
            for (tv, regset) in regsets:
                if tv == vertex:
                    by_family.setdefault((tv, tuple((a, regs.get(a, 0)) for a in regset)), []).append(r)
        pgm = {True: shaderprep.mmSQ_PGM_START_VS, False: shaderprep.mmSQ_PGM_START_PS}
        out = [(r[0], r[1], r[2], r[3], r[4], r[5], r[6]) for r in recs]
        used = {k: programs[k][1] for k in seen if k in programs}
        spec = 0
        for (h, n), (vertex, code, w) in sorted(programs.items()):
            if (h, n) in seen:
                continue
            blk = own_block(shaderprep, vertex, w)
            for t in by_family.get((vertex, tuple(sorted(blk.items()))), [])[:limit]:
                regs = dict(t[5])
                regs.update(blk)
                regs[pgm[vertex] + 1] = n >> 3
                out.append((vertex, h, n, fnv(code[:64]), t[4], regs, t[6]))
                used[(h, n)] = code
                spec += 1
        write_manifest(out_manifest, out)
        write_programs(out_programs, used)
        print("%d recorded variants, %d speculative (up to %d per program) for %d archive programs not seen; "
              "%d programs -> %s, %s" % (len(recs), spec, limit, len({(h, n) for (_, h, n, *_r) in out[len(recs):]}),
                                          len(used), out_manifest, out_programs))
    elif cmd == "selftest":
        files = shader_files(sys.argv[2])
        progs, where = set(), {}
        for path in files:
            data = open(path, "rb").read()
            for off, size in ((0x100, 0x200), (len(data) // 2 & ~3, 0x180), (len(data) - 0x40, 0x40)):
                b = data[off:off + size]
                p = (hash_bytes(b), size, fnv(b[:64]))
                progs.add(p)
                where.setdefault(p, (os.path.relpath(path, sys.argv[2]), off))
        found = find(progs, sys.argv[2])
        ok = all(found.get(p) is not None and hash_bytes(open(os.path.join(sys.argv[2], found[p][0]), "rb").read()[found[p][1]:found[p][1] + p[1]]) == p[0] for p in progs)
        print("selftest: %d of %d cut programs found%s" % (len(found), len(progs), "" if ok else " (MISMATCH)"))
        sys.exit(0 if ok and len(found) == len(progs) else 1)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
