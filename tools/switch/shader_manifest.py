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
    shader_manifest.py distill MANIFEST GAME_DIR OUT [--work DIR]
        (maintainers, issue #88) a console's manifest made into the code-free template a release ships: every variant
        translated (dksh_cache translate --map), one state kept per distinct GLSL source and stage, its fetch shader as
        semantic attributes, its registers as tools/shaderprep_format.py allows (no program addresses, no constant
        banks), written as WWHT0003 (tools/shaderprep_format.py). Then checked: the template translated again gives
        the same sources, and no 16 bytes of the dump's shader programs occur in it (game_bytes_found).
    shader_manifest.py from-template TEMPLATE GAME_DIR OUT_MANIFEST OUT_PROGRAMS
        a code-free template (sealed or not) back into a manifest (fetch shaders as semantic attributes, compact flag
        2) and the programs it names from the dump's archives, for speculate / dksh_cache translate. Both stay on your
        computer.
    shader_manifest.py seal TEMPLATE GAME_DIR OUT
    shader_manifest.py open SEALED GAME_DIR OUT
        the release's form of a template (WSME0001, issue #88): encrypted with a key made from the dump's own shader
        files (SEAL_KEY_FILES), so without the game it is meaningless bytes. Not secrecy against anyone: only someone
        with the game can read it at all. A dump whose files differ is told so (an HMAC tag), never decrypted wrong.

The manifest (runtime/src/gfx/deko/shader_manifest.cpp) names each program by hash_bytes() of its bytes, its
size and an FNV-1a hash of its first 64 bytes; this script computes the same hashes over the files.
"""
import json
import os
import re
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


# ---------------------------------------------------------------- the code-free template (issue #88)

# the registers a code-free state may hold (tools/shaderprep_format.py validate_state): config and context registers
# and the texture/sampler resources, without the program addresses (SQ_PGM_START_VS/PS/FS) and the constant banks
TEMPLATE_DROP = (0xA210, 0xA216, 0xA225)


def template_registers(regs):
    return [[a, v] for a, v in sorted(regs.items()) if a not in TEMPLATE_DROP and
            (0x2000 <= a < 0x4000 or 0xA000 <= a < 0xC000 or 0xE000 <= a < 0x10000)]


def template_format():
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    import shaderprep_format
    return shaderprep_format


def state_of(rec, tf):
    """a WSM1 record (read_manifest_full) as a code-free state"""
    vertex, phash, _size, _prefix, fetch, regs, _compact = rec
    return {"hash": phash, "vertex": int(vertex), "registers": template_registers(regs),
            "fetch": tf.parse_fetch(fetch, regs, None) if vertex and fetch else None}


def states_to_records(states, programs, tf):
    """code-free states -> WSM1 records (compact flag 2, the fetch as WWFA0001) and the programs they name.
    programs: {(hash, size): code} of the dump's archives. States whose program is not there are left out."""
    by_hash = {}
    for (h, n), code in programs.items():
        if h in by_hash and by_hash[h][0] != n:
            sys.exit("two archive programs with the hash %016x: the template cannot tell them apart" % h)
        by_hash[h] = (n, code)
    recs, used, missing = [], {}, 0
    for st in states:
        h, vertex, fetch, regs = (st[0], st[1], st[2], dict(st[3])) if isinstance(st, tuple) else (
            st["hash"], st["vertex"], st["fetch"], {a: v for a, v in st["registers"]})
        if h not in by_hash:
            missing += 1
            continue
        n, code = by_hash[h]
        fb = tf.fetch_payload(fetch) if vertex and fetch else b""
        recs.append((bool(vertex), h, n, fnv(code[:64]), fb, regs, 2 if fb else 0))
        used[(h, n)] = code
    return recs, used, missing


# characters of the template's JSON text (tools/shaderprep_format.py encode: sort_keys, compact separators)
_JSON_RUN = re.compile(rb'[-0-9a-z_,:\[\]{}"]{16,}')


def game_bytes_found(texts, blobs, game):
    """the first place where 16 bytes of the dump's shader programs (archive programs and shader files) occur in one
    of `texts` (ASCII: only runs of the game's bytes made of JSON characters can occur there, searched as they are)
    or `blobs` (binary: every 8th 8-byte word of the blob looked up in the game's bytes at every offset, then
    checked to 16 bytes); None when there is none"""
    sources = list(archive_programs(game).values()) + [open(f, "rb").read() for f in shader_files(game)]
    for text in texts:
        for src in sources:
            for m in _JSON_RUN.finditer(src):
                run = m.group(0)
                for i in range(len(run) - 15):
                    if run[i:i + 16] in text:
                        return "a run of game bytes %r in the template's text" % run[i:i + 16]
    for blob in blobs:
        words = {}
        for p in range(0, len(blob) - 7, 8):
            words.setdefault(blob[p:p + 8], []).append(p)
        for src in sources:
            for g in range(len(src) - 7):
                for p in words.get(src[g:g + 8], ()):
                    for k in range(9):
                        if p - k >= 0 and g - k >= 0 and blob[p - k:p - k + 16] == src[g - k:g - k + 16] and \
                                len(blob[p - k:p - k + 16]) == 16:
                            return "16 game bytes at offset %d of a binary blob" % (p - k)
    return None


def dksh_cache(args):
    """tools/switch/dksh_cache/build.sh (native, else its container): paths relative to the repository root"""
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    import subprocess
    rc = subprocess.call(["bash", os.path.join("tools", "switch", "dksh_cache", "build.sh")] + args, cwd=root)
    if rc not in (0, 2):
        sys.exit("dksh_cache %s failed (%d)" % (args[0], rc))


def read_map(path):
    out = []
    for line in open(path):
        i, stage, h = line.split()
        out.append((int(i), stage == "v", int(h, 16)))
    return out


# ---------------------------------------------------------------- the sealed template (issue #88)

SEAL_MAGIC = b"WSME0001"
# the dump's files the key is made from: the game's shader archives, byte-identical in the USA and European games
SEAL_KEY_FILES = ("content/Common/Shaders/Prim/wii_pipeline.sharcfb",
                  "content/Common/Shaders/particle/wii_pipeline.sharcfb",
                  "content/Common/Shaders/render_buffer/render_buffer.sharcfb")


class SealError(Exception):
    pass


def seal_key(game):
    import hashlib
    h = hashlib.sha256(SEAL_MAGIC)
    for rel in SEAL_KEY_FILES:
        path = os.path.join(game, *rel.split("/"))
        if not os.path.isfile(path):
            raise SealError("%s is not in the game folder" % rel)
        with open(path, "rb") as f:
            data = f.read()
        h.update(struct.pack("<Q", len(data)) + data)
    return h.digest()


def _keystream(key, nonce, n):
    import hashlib
    return hashlib.shake_256(SEAL_MAGIC + key + nonce).digest(n)


def seal(plain, game):
    """WSME0001: magic, 16-byte nonce (from the plaintext: the same template seals to the same file), 32-byte
    HMAC-SHA-256 of nonce + ciphertext, ciphertext (plaintext XOR a SHAKE-256 keystream of key + nonce)"""
    import hashlib
    import hmac
    key = seal_key(game)
    nonce = hashlib.sha256(b"nonce" + plain).digest()[:16]
    cipher = bytes(a ^ b for a, b in zip(plain, _keystream(key, nonce, len(plain))))
    return SEAL_MAGIC + nonce + hmac.new(key, nonce + cipher, hashlib.sha256).digest() + cipher


def unseal(data, game):
    import hashlib
    import hmac
    if not data.startswith(SEAL_MAGIC) or len(data) < 56:
        raise SealError("not a sealed template (WSME0001)")
    key = seal_key(game)
    nonce, tag, cipher = data[8:24], data[24:56], data[56:]
    if not hmac.compare_digest(tag, hmac.new(key, nonce + cipher, hashlib.sha256).digest()):
        raise SealError("this template does not open with this game's files (another game, a damaged dump, or a "
                        "damaged download)")
    return bytes(a ^ b for a, b in zip(cipher, _keystream(key, nonce, len(cipher))))


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
    elif cmd == "distill":
        args = sys.argv[2:]
        work = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "manifest-distill")
        if "--work" in args:
            work = args[args.index("--work") + 1]
            del args[args.index("--work"):args.index("--work") + 2]
        if len(args) != 3:
            sys.exit(__doc__)
        manifest, game, out_path = args
        tf = template_format()
        os.makedirs(work, exist_ok=True)
        recs = read_manifest_full(manifest)
        programs = archive_programs(game)
        wanted = {(r[1], r[2]) for r in recs}
        found = {k: v for k, v in programs.items() if k in wanted}
        # the variants with their fetch shaders as semantic attributes (what the template keeps): translated as such,
        # so a state whose attributes would translate differently from the console's microcode would show
        sem = []
        for r in recs:
            st = state_of(r, tf)
            fb = tf.fetch_payload(st["fetch"]) if st["fetch"] else b""
            sem.append((r[0], r[1], r[2], r[3], fb, r[5], 2 if fb else 0))
        m, progs, gl, mp = (os.path.abspath(os.path.join(work, n)) for n in ("variants.bin", "programs.bin", "gl.bin", "variants.map"))
        write_manifest(m, sem)
        write_programs(progs, found)
        dksh_cache(["translate", m, progs, gl, "--map", mp])
        first = {}
        for i, vertex, h in read_map(mp):
            if h:
                first.setdefault((vertex, h), i)
        keep = sorted(first.values())
        states = [state_of(recs[i], tf) for i in keep]
        doc = {"version": 3, "states": states, "pipelines": []}
        data = tf.encode(doc)
        # the template translated again: every kept state's source, nothing lost by the registers left out
        back, used, missing = states_to_records(tf.decode(data)["states"], programs, tf)
        m2, p2, gl2, mp2 = (os.path.abspath(os.path.join(work, n)) for n in ("template.bin", "template_programs.bin", "template_gl.bin", "template.map"))
        write_manifest(m2, back)
        write_programs(p2, used)
        dksh_cache(["translate", m2, p2, gl2, "--map", mp2])
        again = [h for _, _, h in read_map(mp2)]
        want = [h for (_, h) in sorted(first, key=lambda k: first[k])]
        if missing or sorted(again) != sorted(want):
            sys.exit("the template does not translate to the same sources (%d missing programs, %d of %d sources the same)"
                     % (missing, len(set(again) & set(want)), len(want)))
        text = json.dumps(doc, sort_keys=True, separators=(",", ":")).encode("ascii")
        problem = game_bytes_found([text], [data], game)
        if problem:
            sys.exit("game bytes in the template: " + problem)
        with open(out_path, "wb") as f:
            f.write(data)
        print("%d variants -> %d states (%d vertex, %d pixel; one per distinct GLSL source), %d bytes of JSON, %d bytes "
              "-> %s; translated again: the same %d sources; no game bytes" % (
                  len(recs), len(states), sum(s["vertex"] for s in states), sum(not s["vertex"] for s in states),
                  len(text), len(data), out_path, len(want)))
    elif cmd == "from-template":
        if len(sys.argv) != 6:
            sys.exit(__doc__)
        tf = template_format()
        with open(sys.argv[2], "rb") as f:
            data = f.read()
        if data.startswith(SEAL_MAGIC):
            try:
                data = unseal(data, sys.argv[3])
            except SealError as e:
                sys.exit("%s: %s" % (sys.argv[2], e))
        doc = tf.decode(data, compact=True)
        recs, used, missing = states_to_records(doc["states"], archive_programs(sys.argv[3]), tf)
        write_manifest(sys.argv[4], recs)
        write_programs(sys.argv[5], used)
        print("%d states -> %d variants, %d programs (%d states without their program in the dump) -> %s, %s" % (
            len(doc["states"]), len(recs), len(used), missing, sys.argv[4], sys.argv[5]))
    elif cmd in ("seal", "open"):
        if len(sys.argv) != 5:
            sys.exit(__doc__)
        with open(sys.argv[2], "rb") as f:
            data = f.read()
        try:
            if cmd == "seal":
                template_format().decode(data)  # only a valid code-free template is sealed
                out = seal(data, sys.argv[3])
            else:
                out = unseal(data, sys.argv[3])
        except (SealError, ValueError) as e:
            sys.exit("%s: %s" % (sys.argv[2], e))
        with open(sys.argv[4], "wb") as f:
            f.write(out)
        print("%s: %d bytes -> %s (%d bytes)" % (cmd, len(data), sys.argv[4], len(out)))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
