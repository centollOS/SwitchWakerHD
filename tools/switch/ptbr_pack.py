#!/usr/bin/env python3
"""The Brazilian Portuguese fan translation, rebuilt on the player's own game files (docs/ptbr-translation-plan.md).

Triforce-Heroes' PT-BR translation (triforce-heroes.com.br) is one 2D language pack,
content/Common/Pack/permanent_2d_<Us|Eu><English|Spanish>.pack, that the player downloads from their page. Copied
over the game as it is, it is a repacked archive (16-byte aligned entries where the game's are 0x2000, other Yaz0
header values, a layout cut two bytes short). This tool takes from it only what the translation changes (the MSBT
message archives, the MSBT project and the fonts) and lays them out in the player's own English pack, as the game's
pack is laid out; everything else stays the player's original bytes. Nothing of the translation or the game is in
this repository.

usage:
  ptbr_pack.py --translation DOWNLOAD --base ENGLISH_PACK --out PACK [--info JSON]
      DOWNLOAD: the .rar (or .zip/.7z) as downloaded, the folder it was extracted to, or a .pack
      ENGLISH_PACK: the game's content/Common/Pack/permanent_2d_UsEnglish.pack (or EuEnglish)
      PACK: the pack to write (the port reads it in place of the English one); JSON: its description
             (default: translation.json next to PACK)
  ptbr_pack.py --check DOWNLOAD
      only whether DOWNLOAD holds a translation pack (before the slow steps of an install)

Exit status 0 on success; 2 with a message for the player when the download is not what is expected.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

ARCHIVES = (".rar", ".zip", ".7z")
PAGE = "https://triforce-heroes.com.br/traducoes/the-legend-of-zelda-the-wind-waker-hd"


class Refused(Exception):
    """The download is not a translation this tool can use (the message is for the player)."""


# --- formats -------------------------------------------------------------------------------------------------------

def yaz0(d):
    """Yaz0-decompressed bytes (other data as it is)"""
    if d[:4] != b"Yaz0":
        return d
    n = int.from_bytes(d[4:8], "big")
    out = bytearray()
    p = 16
    ln = len(d)
    try:
        while len(out) < n:
            f = d[p]
            p += 1
            for i in range(8):
                if len(out) >= n:
                    break
                if f & (0x80 >> i):
                    out.append(d[p])
                    p += 1
                else:
                    b1, b2 = d[p], d[p + 1]
                    p += 2
                    dist = ((b1 & 0xF) << 8 | b2) + 1
                    c = b1 >> 4
                    if c == 0:
                        c = d[p] + 0x12
                        p += 1
                    else:
                        c += 2
                    if dist > len(out):
                        raise Refused("damaged compressed data")
                    if dist >= c:
                        out += out[-dist:len(out) - dist + c]
                    else:
                        for _ in range(c):
                            out.append(out[-dist])
            if p > ln:
                raise IndexError
    except IndexError:
        raise Refused("damaged compressed data")
    return bytes(out[:n])


class Sarc:
    """A big-endian SARC: entries (name, start, end) in node order, offsets absolute in `data`"""

    def __init__(self, data, what="the pack"):
        if len(data) < 0x20 or data[:4] != b"SARC" or data[6:8] != b"\xfe\xff":
            raise Refused("%s is not a big-endian SARC archive" % what)
        self.data = data
        self.data_offset = struct.unpack(">I", data[12:16])[0]
        if data[0x14:0x18] != b"SFAT":
            raise Refused("%s has no file table" % what)
        n = struct.unpack(">H", data[0x1A:0x1C])[0]
        names = 0x20 + 16 * n
        if data[names:names + 4] != b"SFNT":
            raise Refused("%s has no name table" % what)
        names += 8
        self.entries = []
        for i in range(n):
            node = 0x20 + 16 * i
            _, attr, s, e = struct.unpack(">IIII", data[node:node + 16])
            if not attr >> 24:
                raise Refused("%s has an entry without a name" % what)
            o = names + (attr & 0xFFFFFF) * 4
            z = data.index(b"\0", o)
            start, end = self.data_offset + s, self.data_offset + e
            if not (self.data_offset <= start <= end <= len(data)):
                raise Refused("%s is cut off or damaged" % what)
            self.entries.append((data[o:z].decode("latin1"), start, end))
        self.by_name = {name: (s, e) for name, s, e in self.entries}

    def file(self, name):
        s, e = self.by_name[name]
        return self.data[s:e]

    def alignment(self):
        """the largest power of two (up to 0x2000) every entry's offset is a multiple of"""
        a = 0x2000
        for _, s, _ in self.entries:
            while a > 1 and s % a:
                a //= 2
        return a


def msbt_layout(m, what):
    """an MSBT's labels {name: index}, attributes and text count (what the game looks messages up by)"""
    if m[:8] != b"MsgStdBn":
        raise Refused("%s is not an MSBT" % what)
    blocks = {}
    p = 0x20
    while p + 16 <= len(m):
        t = m[p:p + 4].decode("latin1")
        sz = struct.unpack(">I", m[p + 4:p + 8])[0]
        blocks[t] = m[p + 16:p + 16 + sz]
        p = (p + 16 + sz + 15) & ~15
    for t in ("LBL1", "ATR1", "TXT2"):
        if t not in blocks:
            raise Refused("%s has no %s block" % (what, t))
    lbl = blocks["LBL1"]
    labels = {}
    for i in range(struct.unpack(">I", lbl[:4])[0]):
        cnt, off = struct.unpack(">II", lbl[4 + 8 * i:12 + 8 * i])
        for _ in range(cnt):
            ln = lbl[off]
            labels[lbl[off + 1:off + 1 + ln]] = struct.unpack(">I", lbl[off + 1 + ln:off + 5 + ln])[0]
            off += 5 + ln
    return labels, blocks["ATR1"], struct.unpack(">I", blocks["TXT2"][:4])[0], sorted(blocks)


def same_messages(base_szs, tr_szs, name):
    """whether a translated MSBT archive keeps the base one's files, labels, attributes and text count"""
    b, t = Sarc(yaz0(base_szs), name), Sarc(yaz0(tr_szs), name)
    if sorted(b.by_name) != sorted(t.by_name):
        return False
    for inner in b.by_name:
        if not inner.endswith(".msbt"):
            if inner not in t.by_name:
                return False
            continue
        lb, ab, nb, kb = msbt_layout(b.file(inner), inner)
        lt, at, nt, kt = msbt_layout(t.file(inner), inner)
        if lb != lt or ab != at or nb != nt or kb != kt:
            return False
    return True


# --- the download --------------------------------------------------------------------------------------------------

def bsdtar():
    """a tar that reads RAR (libarchive's bsdtar: macOS /usr/bin/tar, Windows 10+ tar.exe), or None"""
    for name in ("bsdtar", "tar"):
        exe = shutil.which(name)
        if not exe:
            continue
        try:
            v = subprocess.run([exe, "--version"], capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if "bsdtar" in (v.stdout + v.stderr) or "libarchive" in (v.stdout + v.stderr):
            return exe
    return None


def extract(archive, into):
    """the archive's files into `into`"""
    if archive.lower().endswith(".zip"):
        try:
            with zipfile.ZipFile(archive) as z:
                z.extractall(into)
            return
        except (zipfile.BadZipFile, OSError, RuntimeError, NotImplementedError):
            pass  # bsdtar below (another zip method), or the message
    exe = bsdtar()
    if exe:
        r = subprocess.run([exe, "-xf", archive, "-C", into], capture_output=True, text=True)
        if r.returncode == 0:
            return
        detail = (r.stderr or r.stdout).strip()[-300:]
    else:
        detail = "no program that opens it was found on this computer"
    raise Refused("%s could not be opened (%s). Extract it with a program such as 7-Zip or WinRAR and choose the "
                  "extracted folder instead." % (os.path.basename(archive), detail))


def find_packs(folder):
    out = []
    for dp, dirs, fns in os.walk(folder):
        dirs.sort()
        for fn in sorted(fns):
            if fn.lower().startswith("permanent_2d_") and fn.lower().endswith(".pack"):
                out.append(os.path.join(dp, fn))
    return out


def read_rules(folder):
    """rules.txt's name and path lines (Cemu graphic pack), or {}"""
    for dp, dirs, fns in os.walk(folder):
        for fn in fns:
            if fn.lower() == "rules.txt":
                out = {}
                try:
                    with open(os.path.join(dp, fn), encoding="utf-8", errors="replace") as f:
                        for line in f:
                            k, sep, v = line.partition("=")
                            if sep and k.strip().lower() in ("name", "path", "titleids", "version"):
                                out[k.strip().lower()] = v.strip().strip('"')
                except OSError:
                    pass
                return out
    return {}


def open_download(path, scratch):
    """(pack bytes, pack file name, rules) from the download"""
    if not os.path.exists(path):
        raise Refused("%s is not there" % path)
    folder = None
    if os.path.isdir(path):
        folder = path
    elif path.lower().endswith(ARCHIVES):
        folder = os.path.join(scratch, "download")
        os.makedirs(folder)
        extract(path, folder)
    elif path.lower().endswith(".pack"):
        with open(path, "rb") as f:
            return f.read(), os.path.basename(path), {}
    else:
        raise Refused("%s is not the translation's download (a .rar, its extracted folder or its .pack)" %
                      os.path.basename(path))
    packs = find_packs(folder)
    if not packs:
        raise Refused("%s has no permanent_2d_*.pack: it is not the translation of The Wind Waker HD (on %s, the "
                      "\"Emulador\" download)" % (os.path.basename(path.rstrip("/\\")), PAGE))
    datas = {}
    for p in packs:
        with open(p, "rb") as f:
            datas[p] = f.read()
    english = [p for p in packs if "english" in os.path.basename(p).lower()]
    pick = english[0] if english else packs[0]
    if len(set(hashlib.sha256(d).digest() for d in datas.values())) > 1 and not english:
        raise Refused("%s holds several different language packs and none for English" % os.path.basename(path))
    return datas[pick], os.path.basename(pick), read_rules(folder)


# --- the rebuild ---------------------------------------------------------------------------------------------------

def is_font(name):
    return name.lower().endswith("_bffnt.szs")


def is_messages(name):
    return name.lower().endswith("_msbt.szs")


def is_project(name):
    return name.lower().endswith("_msbp.szs")


def rebuild(base_data, tr_data):
    """(the new pack, the entries taken from the translation, the changed entries left as the base's)"""
    base = Sarc(base_data, "the game's English pack")
    tr = Sarc(tr_data, "the translation's pack")
    if sorted(base.by_name) != sorted(tr.by_name):
        raise Refused("the translation's pack does not hold the same files as the game's (%d and %d): it is not a "
                      "translation of this game's version" % (len(tr.entries), len(base.entries)))
    taken, kept = [], []
    take = {}
    for name, s, e in base.entries:
        b = base_data[s:e]
        t = tr.file(name)
        if b == t:
            continue
        if b[:4] != b"Yaz0" or t[:4] != b"Yaz0":
            raise Refused("%s in the translation's pack is not as the game stores it" % name)
        if b[16:] == t[16:]:
            continue  # only the header's alignment value differs
        bd, td = yaz0(b), yaz0(t)
        if bd == td:
            continue  # only compressed again: the game's bytes stay
        if is_font(name):
            if len(td) != len(bd):
                raise Refused("the font %s in the translation's pack is not the game's size" % name)
        elif is_messages(name):
            if not same_messages(b, t, name):
                raise Refused("the messages of %s do not match the game's: the translation is for another version "
                              "of the game" % name)
        elif is_project(name):
            yaz0(t)  # whole
        else:
            kept.append(name)  # a layout (Text.szs: the translation's copy is cut short); the game's stays
            continue
        take[name] = t[:8] + b[8:16] + t[16:]  # the game's Yaz0 alignment value
        taken.append(name)
    if not taken:
        raise Refused("the translation's pack changes no text: it is the game's own English pack")
    if not any(is_messages(n) for n in taken):
        raise Refused("the translation's pack changes no messages")
    align = base.alignment()
    head = bytearray(base_data[:base.data_offset])
    body = bytearray()
    for i, (name, s, e) in enumerate(base.entries):
        content = take.get(name, base_data[s:e])
        pad = (-(base.data_offset + len(body))) % align
        body += b"\0" * pad
        start = len(body)
        body += content
        node = 0x20 + 16 * i
        struct.pack_into(">II", head, node + 8, start, len(body))
    out = bytes(head) + bytes(body)
    out = out[:8] + struct.pack(">I", len(out)) + out[12:]
    # what the game will read: the same archive, the translation's files in it
    check = Sarc(out, "the new pack")
    for name in base.by_name:
        want = take.get(name, base.file(name))
        if check.file(name) != want or check.by_name[name][0] % align:
            raise Refused("the new pack could not be written correctly (%s)" % name)
    return out, taken, kept


def sha(d):
    return hashlib.sha256(d).hexdigest()


def label(rules, download):
    path = rules.get("path", "")
    if path:
        return path.replace("\\", "/").rstrip("/").split("/")[-1]
    return rules.get("name") or os.path.basename(download.rstrip("/\\"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--translation", help="the download: .rar/.zip/.7z, its folder or its .pack")
    ap.add_argument("--base", help="the game's content/Common/Pack/permanent_2d_UsEnglish.pack (or EuEnglish)")
    ap.add_argument("--out", help="the pack to write")
    ap.add_argument("--info", help="its description (default: translation.json next to --out)")
    ap.add_argument("--check", metavar="DOWNLOAD", help="only check the download")
    args = ap.parse_args()
    scratch = tempfile.mkdtemp(prefix="wwhd-ptbr-")
    try:
        if args.check:
            data, fname, rules = open_download(os.path.abspath(args.check), scratch)
            Sarc(data, "the translation's pack")
            print("ptbr: %s: %s (%s)" % (os.path.basename(args.check.rstrip("/\\")), label(rules, args.check), fname))
            return 0
        if not (args.translation and args.base and args.out):
            ap.error("--translation, --base and --out (or --check)")
        data, fname, rules = open_download(os.path.abspath(args.translation), scratch)
        with open(args.base, "rb") as f:
            base = f.read()
        out, taken, kept = rebuild(base, data)
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        tmp = args.out + ".part"
        with open(tmp, "wb") as f:
            f.write(out)
        os.replace(tmp, args.out)
        info = {
            "language": "pt-BR",
            "name": label(rules, args.translation),
            "download": os.path.basename(os.path.abspath(args.translation).rstrip("/\\")),
            "translation_pack": fname,
            "translation_sha256": sha(data),
            "base_pack": os.path.basename(args.base),
            "base_sha256": sha(base),
            "pack_sha256": sha(out),
            "taken": len(taken),
            "kept_from_game": kept,
        }
        info_path = args.info or os.path.join(os.path.dirname(os.path.abspath(args.out)), "translation.json")
        with open(info_path, "w", encoding="utf-8") as f:
            json.dump(info, f, ensure_ascii=False, indent=1)
            f.write("\n")
        print("ptbr: %s: %d files of the translation in the game's own pack (%s)%s" % (
            info["name"], len(taken), os.path.basename(args.base),
            "; kept the game's %s (the translation's copy differs without text)" % ", ".join(kept) if kept else ""))
        return 0
    except Refused as e:
        print("ptbr: %s" % e, file=sys.stderr)
        return 2
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
