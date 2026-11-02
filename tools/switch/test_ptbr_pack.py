#!/usr/bin/env python3
"""ptbr_pack.py with made-up packs (no game data): what it takes, what it keeps, what it refuses.

With WWHD_TEST_PTBR=<the download> and WWHD_TEST_PTBR_BASE=<the game's permanent_2d_UsEnglish.pack> it also
rebuilds the real translation and checks the result's layout.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ptbr_pack  # noqa: E402

TOOL = os.path.join(HERE, "ptbr_pack.py")


def stored_yaz0(data, align=0):
    out = bytearray(b"Yaz0" + struct.pack(">II", len(data), align) + b"\0" * 4)
    for i in range(0, len(data), 8):
        chunk = data[i:i + 8]
        out.append((0xFF << (8 - len(chunk))) & 0xFF)
        out += chunk
    return bytes(out)


def sarc(files, align=0x2000, data_offset=None):
    """a big-endian SARC of {name: bytes} in name order"""
    names = sorted(files)
    sfnt = bytearray()
    offs = []
    for n in names:
        offs.append(len(sfnt) // 4)
        sfnt += n.encode() + b"\0"
        sfnt += b"\0" * ((-len(sfnt)) % 4)
    head_len = 0x20 + 16 * len(names) + 8 + len(sfnt)
    data_offset = data_offset or (head_len + align - 1) // align * align
    body = bytearray()
    nodes = bytearray()
    for i, n in enumerate(names):
        body += b"\0" * ((-(data_offset + len(body))) % align)
        s = len(body)
        body += files[n]
        nodes += struct.pack(">IIII", i, 0x01000000 | offs[i], s, len(body))
    head = bytearray(b"SARC" + struct.pack(">HH", 0x14, 0xFEFF) + b"\0" * 4 + struct.pack(">IHH", data_offset, 0x100, 0))
    head += b"SFAT" + struct.pack(">HHI", 0xC, len(names), 0x65)
    head += nodes + b"SFNT" + struct.pack(">HH", 8, 0) + sfnt
    head += b"\0" * (data_offset - len(head))
    out = bytes(head) + bytes(body)
    return out[:8] + struct.pack(">I", len(out)) + out[12:]


def msbt(labels, texts):
    """an MSBT with LBL1 (one hash slot), ATR1 and TXT2"""
    def block(tag, payload):
        b = tag + struct.pack(">I", len(payload)) + b"\0" * 8 + payload
        return b + b"\xab" * ((-len(b)) % 16)
    lbl = struct.pack(">I", 1) + struct.pack(">II", len(labels), 12)
    for i, l in enumerate(labels):
        lbl += bytes([len(l)]) + l + struct.pack(">I", i)
    atr = struct.pack(">II", len(labels), 0)
    txt = struct.pack(">I", len(texts))
    strings = [t.encode("utf-16-be") + b"\0\0" for t in texts]
    off = 4 + 4 * len(texts)
    for s in strings:
        txt += struct.pack(">I", off)
        off += len(s)
    txt += b"".join(strings)
    body = block(b"LBL1", lbl) + block(b"ATR1", atr) + block(b"TXT2", txt)
    return b"MsgStdBn" + b"\xfe\xff" + b"\0" * 22 + body


def messages(texts, labels=(b"A", b"B"), align=0x2000):
    return stored_yaz0(sarc({"Main_00.msbt": msbt(list(labels), texts)}, align=align))


def game_pack():
    return sarc({
        "Main_00_msbt.szs": messages(["Hello", "Bye"]),
        "Shop_00_msbt.szs": messages(["Buy"]),
        "CKing_msbp.szs": stored_yaz0(b"MsgPrjBn" + b"\0" * 40),
        "CKingMain_bffnt.szs": stored_yaz0(b"FFNT" + b"\x11" * 300),
        "CKingMsg_bffnt.szs": stored_yaz0(b"FFNT" + b"\x22" * 100),
        "Text.szs": stored_yaz0(b"layout" * 20, align=0x2000),
    })


def translation_pack(**changes):
    files = {
        "Main_00_msbt.szs": messages(["Olá", "Tchau"], align=0x10),
        "Shop_00_msbt.szs": messages(["Buy"]),  # untranslated
        "CKing_msbp.szs": stored_yaz0(b"MsgPrjBn" + b"\1" * 36, align=4),
        "CKingMain_bffnt.szs": stored_yaz0(b"FFNT" + b"\x11" * 290 + b"\x33" * 10, align=0x2000),
        "CKingMsg_bffnt.szs": stored_yaz0(b"FFNT" + b"\x22" * 100, align=0x2000),  # same glyphs, other header
        "Text.szs": stored_yaz0((b"layout" * 20)[:-2], align=4),  # cut short, as in the real download
    }
    files.update(changes)
    return sarc(files, align=0x10)


def run(*args):
    return subprocess.run([sys.executable, "-I", TOOL] + list(args), capture_output=True, text=True)


def main():
    base = game_pack()
    tr = translation_pack()
    out, taken, kept = ptbr_pack.rebuild(base, tr)
    assert sorted(taken) == ["CKingMain_bffnt.szs", "CKing_msbp.szs", "Main_00_msbt.szs"], taken
    assert kept == ["Text.szs"], kept
    s, b, t = ptbr_pack.Sarc(out), ptbr_pack.Sarc(base), ptbr_pack.Sarc(tr)
    assert s.alignment() == 0x2000 and s.data_offset == b.data_offset
    assert [n for n, _, _ in s.entries] == [n for n, _, _ in b.entries]
    for name in ("Shop_00_msbt.szs", "CKingMsg_bffnt.szs", "Text.szs"):
        assert s.file(name) == b.file(name), name  # the game's own bytes
    for name in taken:
        got, theirs, ours = s.file(name), t.file(name), b.file(name)
        assert got[16:] == theirs[16:] and got[:8] == theirs[:8] and got[8:16] == ours[8:16], name
    assert ptbr_pack.yaz0(s.file("Main_00_msbt.szs")) == ptbr_pack.yaz0(t.file("Main_00_msbt.szs"))
    assert struct.unpack(">I", out[8:12])[0] == len(out)

    def refused(pack, words):
        try:
            ptbr_pack.rebuild(base, pack)
        except ptbr_pack.Refused as e:
            assert words in str(e), (words, str(e))
            return
        raise AssertionError("not refused: " + words)

    refused(sarc({"Main_00_msbt.szs": messages(["x", "y"])}), "same files")
    refused(translation_pack(**{"Main_00_msbt.szs": messages(["Olá", "Tchau"], labels=(b"A", b"C"))}),
            "another version")
    refused(translation_pack(**{"Main_00_msbt.szs": messages(["Olá"], labels=(b"A", b"B"))}), "another version")
    refused(translation_pack(**{"CKingMain_bffnt.szs": stored_yaz0(b"FFNT" + b"\x11" * 200)}), "size")
    refused(base, "changes no text")
    refused(b"PK\3\4", "not a big-endian SARC")

    # the command: a folder, a .zip, a .pack; the description; --check; a wrong file
    with tempfile.TemporaryDirectory() as tmp:
        game = os.path.join(tmp, "UsEnglish.pack")
        with open(game, "wb") as f:
            f.write(base)
        folder = os.path.join(tmp, "Tradução (3.0)", "content", "Common", "Pack")
        os.makedirs(folder)
        for n in ("permanent_2d_UsSpanish.pack", "permanent_2d_UsEnglish.pack"):
            with open(os.path.join(folder, n), "wb") as f:
                f.write(tr)
        with open(os.path.join(tmp, "Tradução (3.0)", "rules.txt"), "w", encoding="utf-8") as f:
            f.write('[Definition]\ntitleIds = 0005000010143500\nname = "X"\npath = "WWHD/Translations/Tradução v1"\n')
        z = os.path.join(tmp, "download.zip")
        with zipfile.ZipFile(z, "w") as zf:
            for dp, _, fns in os.walk(os.path.join(tmp, "Tradução (3.0)")):
                for fn in fns:
                    p = os.path.join(dp, fn)
                    zf.write(p, os.path.relpath(p, tmp))
        for i, src in enumerate((os.path.join(tmp, "Tradução (3.0)"), z, os.path.join(folder, "permanent_2d_UsEnglish.pack"))):
            dst = os.path.join(tmp, "out%d" % i, "permanent_2d_ptBR.pack")
            r = run("--translation", src, "--base", game, "--out", dst)
            assert r.returncode == 0, r.stderr
            with open(dst, "rb") as f:
                assert f.read() == out
            import json
            with open(os.path.join(os.path.dirname(dst), "translation.json"), encoding="utf-8") as f:
                info = json.load(f)
            assert info["taken"] == 3 and info["kept_from_game"] == ["Text.szs"]
            if i < 2:
                assert info["name"] == "Tradução v1" and info["translation_pack"] == "permanent_2d_UsEnglish.pack", info
        r = run("--check", z)
        assert r.returncode == 0 and "Tradução v1" in r.stdout, (r.stdout, r.stderr)
        junk = os.path.join(tmp, "junk")
        os.makedirs(junk)
        r = run("--check", junk)
        assert r.returncode == 2 and "no permanent_2d_" in r.stderr, r.stderr
        r = run("--check", os.path.join(tmp, "missing.rar"))
        assert r.returncode == 2 and "not there" in r.stderr, r.stderr

    real, real_base = os.environ.get("WWHD_TEST_PTBR"), os.environ.get("WWHD_TEST_PTBR_BASE")
    if real and real_base:
        with tempfile.TemporaryDirectory() as tmp:
            dst = os.path.join(tmp, "permanent_2d_ptBR.pack")
            r = run("--translation", real, "--base", real_base, "--out", dst)
            assert r.returncode == 0, r.stderr
            with open(dst, "rb") as f:
                new = ptbr_pack.Sarc(f.read())
            with open(real_base, "rb") as f:
                game = ptbr_pack.Sarc(f.read())
            assert new.alignment() == game.alignment() == 0x2000
            assert [n for n, _, _ in new.entries] == [n for n, _, _ in game.entries]
            print("real download:", r.stdout.strip())
    print("test_ptbr_pack: OK")


if __name__ == "__main__":
    main()
