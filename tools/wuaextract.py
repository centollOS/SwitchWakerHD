#!/usr/bin/env python3
"""Pure-Python Cemu .wua (ZArchive) extractor: a stand-in for the C++ wwhd-extract on machines without it.
Port of tools/wudextract/zarchive.cpp. Needs: pip install zstandard

usage: wuaextract.py ARCHIVE.wua list
       wuaextract.py ARCHIVE.wua extract OUTDIR [TITLE]    TITLE: 16 hex digits or folder name
                                                           (default: the USA or European base game, 0005000010143500 or 0005000010143600)
"""
import hashlib
import os
import re
import struct
import sys

import zstandard

MAGIC, VERSION1, FOOTER, BLOCK = 0x169F52D6, 0x61BF3A01, 144, 65536
DEFAULT_TITLES = ("0005000010143500", "0005000010143600")  # the base games the port builds from (USA, Europe)


def be(b):
    return int.from_bytes(b, "big")


class Zar:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.size = self.f.tell()
        ft = self.read(self.size - FOOTER, FOOTER)
        if be(ft[140:144]) != MAGIC or be(ft[136:140]) != VERSION1:
            sys.exit("not a Cemu Wii U archive (.wua)")
        if be(ft[128:136]) != self.size:
            sys.exit("the archive is truncated (size mismatch): incomplete copy?")
        self.hash = ft[96:128]
        sec = [(be(ft[16 * i:16 * i + 8]), be(ft[16 * i + 8:16 * i + 16])) for i in range(6)]
        self.data_off = sec[0][0]
        rec = self.read(*sec[1])
        self.boff, self.blen = [], []
        for r in range(len(rec) // 40):
            q = rec[r * 40:r * 40 + 40]
            off = be(q[:8])
            for k in range(16):
                ln = be(q[8 + 2 * k:10 + 2 * k]) + 1
                self.boff.append(off)
                self.blen.append(ln)
                off += ln
        names = self.read(*sec[2])
        tree = self.read(*sec[3])
        self.nodes = []
        for i in range(len(tree) // 16):
            w0, w1, w2, w3 = struct.unpack(">IIII", tree[i * 16:i * 16 + 16])
            is_file, no = w0 >> 31, w0 & 0x7FFFFFFF
            name = ""
            if i:
                ln = names[no] & 0x7F
                if names[no] & 0x80:
                    ln |= names[no + 1] << 7
                    no += 2
                else:
                    no += 1
                raw = names[no:no + ln]
                try:
                    name = raw.decode("utf-8")
                except UnicodeDecodeError:
                    name = raw.decode("latin-1")
            if is_file:
                self.nodes.append((name, True, w1 | (w3 & 0xFFFF) << 32, w2 | (w3 & 0xFFFF0000) << 16))
            else:
                self.nodes.append((name, False, w1, w2))
        self.dz = zstandard.ZstdDecompressor()
        self.cached = None, None

    def read(self, off, n):
        self.f.seek(off)
        d = self.f.read(n)
        if len(d) != n:
            sys.exit("the archive is truncated or unreadable")
        return d

    def block(self, i):
        if self.cached[0] == i:
            return self.cached[1]
        raw = self.read(self.data_off + self.boff[i], self.blen[i])
        if self.blen[i] != BLOCK:
            raw = self.dz.decompress(raw, max_output_size=BLOCK)
            if len(raw) != BLOCK:
                sys.exit("block %d is damaged" % i)
        self.cached = i, raw
        return raw

    def write_file(self, node, dst):
        _, _, pos, remaining = self.nodes[node]
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as out:
            while remaining > 0:
                w = pos % BLOCK
                n = min(remaining, BLOCK - w)
                out.write(self.block(pos // BLOCK)[w:w + n])
                pos += n
                remaining -= n

    def kids(self, d):
        _, _, first, count = self.nodes[d]
        return range(first, first + count)

    def walk(self, d, rel=""):
        for c in self.kids(d):
            name, is_file = self.nodes[c][:2]
            p = rel + name
            if is_file:
                yield p, c
            else:
                yield from self.walk(c, p + "/")

    def verify(self):
        h, pos, at = hashlib.sha256(), 0, self.size - FOOTER + 96
        self.f.seek(0)
        while pos < self.size:
            buf = bytearray(self.f.read(min(4 << 20, self.size - pos)))
            for i in range(max(pos, at), min(pos + len(buf), at + 32)):
                buf[i - pos] = 0
            h.update(buf)
            pos += len(buf)
        return h.digest() == self.hash


def titles(z):
    out = []
    for c in z.kids(0):
        n = z.nodes[c][0]
        m = re.fullmatch(r"([0-9a-fA-F]{16})_v(\d+)", n)
        if m and not z.nodes[c][1]:
            out.append((m.group(1).lower(), int(m.group(2)), n, c))
    return out


def main():
    if len(sys.argv) < 3 or sys.argv[2] not in ("list", "extract"):
        sys.exit(__doc__)
    z = Zar(sys.argv[1])
    ts = titles(z)
    if sys.argv[2] == "list":
        for t in ts:
            print(t[2], sum(1 for _ in z.walk(t[3])), "files")
        return
    out = sys.argv[3]
    wants = [sys.argv[4].lower()] if len(sys.argv) > 4 else list(DEFAULT_TITLES)
    cand = next(([t for t in ts if w in (t[0], t[2].lower())] for w in wants
                 if any(w in (t[0], t[2].lower()) for t in ts)), [])
    if not cand:
        sys.exit("title %s not in the archive (it has: %s)" % (" or ".join(wants), ", ".join(t[2] for t in ts)))
    t = max(cand, key=lambda x: x[1])
    print("checking the archive hash ...", flush=True)
    if not z.verify():
        sys.exit("the archive's SHA-256 does not match: damaged archive")
    files = list(z.walk(t[3]))
    print("extracting %s (%d files) ..." % (t[2], len(files)), flush=True)
    for i, (p, c) in enumerate(files):
        # names come from the archive: keep every file under OUT (`..`, `\` or a drive letter would escape it)
        parts = p.split("/")
        if any(n in ("", ".", "..") or "\\" in n or ":" in n for n in parts):
            sys.exit("unsafe file name in the archive: %r" % p)
        dst = os.path.join(out, *parts)
        root = os.path.realpath(out)
        if os.path.commonpath([root, os.path.realpath(dst)]) != root:
            sys.exit("unsafe file name in the archive: %r" % p)
        z.write_file(c, dst)
        if i % 500 == 0:
            print("  %d/%d" % (i, len(files)), flush=True)
    print("done")


if __name__ == "__main__":
    main()
