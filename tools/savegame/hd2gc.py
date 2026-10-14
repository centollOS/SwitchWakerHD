#!/usr/bin/env python3
"""Convert a Wind Waker HD save (cking.sav) into a GameCube-layout .gci (USA GZLE01 by default).

usage: hd2gc.py cking.sav -o OUT.gci [--code GZLE01|GZLJ01|GZLP01] [--pal-language english] [--set-gc-name] [--template SOME.gci]

Mainly the reverse direction for the round-trip check of gc2hd.py: the shared fields are copied
field by field; HD-only data (UTF-16 names, the HD per-file area, the picture album) has no
GameCube place and is dropped. --set-gc-name writes the HD player name into the GameCube name
field when that is empty (HD leaves it empty; GameCube shows such a file as "New Game").
--template copies the banner/icon block from an existing .gci.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wwsave as W  # noqa: E402


def hd_to_gc_slots(raw, set_gc_name=False, code=b"GZLE01"):
    slots, extras, _ = W.read_hd(raw)
    out = []
    for g, ex in zip(slots, extras):
        gc = bytearray(W.GAMEDATA_SIZE)
        for f in W.FIELD_NAMES:
            W.put(gc, W.GC_FIELDS, f, W.get(g, W.HD_FIELDS, f))
        used = W.get(g, W.HD_FIELDS, "info.save_count") != b"\0\0"   # HD "New Game" test
        if set_gc_name and used and W.get(gc, W.GC_FIELDS, "info.player_name")[0] == 0:
            name = W.hd_name(ex)
            encoding = "cp1252" if code == b"GZLP01" else "ascii"
            try:
                encoded = name.encode(encoding)
            except UnicodeEncodeError:
                encoded = b""
            if encoded:
                W.put(gc, W.GC_FIELDS, "info.player_name", encoded[:16].ljust(17, b"\0"))
        out.append(bytes(gc))
    return out


def hd_pal_language(raw):
    """PAL's card-wide language from HD per-slot option byte 4; reject ambiguous used slots."""
    slots, extras, _ = W.read_hd(raw)
    languages = {ex["player"][4] for g, ex in zip(slots, extras)
                 if W.get(g, W.HD_FIELDS, "info.save_count") != b"\0\0"}
    if not languages:
        languages = {extras[0]["player"][4]}
    if len(languages) != 1:
        raise W.HdError("HD Quest Logs have different languages; choose --pal-language for the PAL card")
    language = languages.pop()
    return language if language < len(W.PAL_LANGUAGES) else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("sav")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--code", default="GZLE01", choices=[c.decode() for c in W.SUPPORTED_GC])
    ap.add_argument("--pal-language", choices=W.PAL_LANGUAGES, default=None,
                    help="override PAL card-wide language (default: HD per-slot options; mixed used slots require an override)")
    ap.add_argument("--set-gc-name", action="store_true")
    ap.add_argument("--template")
    a = ap.parse_args()
    if a.pal_language is not None and a.code != "GZLP01":
        ap.error("--pal-language requires --code GZLP01")
    raw = open(a.sav, "rb").read()
    try:
        language = (W.PAL_LANGUAGES.index(a.pal_language) if a.pal_language is not None else
                    hd_pal_language(raw) if a.code == "GZLP01" else 0)
        slots = hd_to_gc_slots(raw, a.set_gc_name, a.code.encode())
    except W.HdError as e:
        ap.error(str(e))
    tpl = open(a.template, "rb").read() if a.template else None
    with open(a.out, "wb") as f:
        f.write(W.write_gci(a.code.encode(), slots, template=tpl,
                            pal_language=language))
    print("%s -> %s" % (a.sav, a.out))


if __name__ == "__main__":
    main()
