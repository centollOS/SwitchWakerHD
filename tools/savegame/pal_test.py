#!/usr/bin/env python3
"""Synthetic PAL conversion regressions; no game files or real saves."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

import gc2hd
import hd2gc
import wwsave as W


def slots():
    # Deliberately exercise every packed field, including the last slot and reserved data.
    result = [bytearray((i * 17 + s * 31) % 256 for i in range(W.GAMEDATA_SIZE))
              for s in range(3)]
    for s, g in enumerate(result):
        W.put(g, W.GC_FIELDS, "info.player_name", [b"Andr\xe9", b"\x8c\x9c", b""][s].ljust(17, b"\0"))
        W.put(g, W.GC_FIELDS, "info.save_count", b"\0\0")
        W.put(g, W.GC_FIELDS, "item.slot07", b"\xff")
    return result


class PalTest(unittest.TestCase):
    def test_languages_roundtrip_and_all_fields(self):
        original = slots()
        for language in range(5):
            with self.subTest(language=language):
                raw = W.write_gci(b"GZLP01", original, pal_language=language)
                for k in (1, 2):
                    block = raw[W.GCI_HEADER + k * W.CARD_BLOCK:W.GCI_HEADER + (k + 1) * W.CARD_BLOCK]
                    self.assertEqual(struct.unpack_from(">II", block), (language, 0))
                    self.assertEqual(struct.unpack_from(">I", block, 0x1FFC)[0], W.gc_checksum_card(block))
                self.assertEqual(W.gc_pal_language(raw), language)
                code, hd, report = gc2hd.convert_gci(bytes(raw))
                self.assertEqual(code, b"GZLP01")
                converted, extras, _ = W.read_hd(hd)
                self.assertEqual([W.hd_name(e) for e in extras], ["André", "Œœ", "Link"])
                self.assertIn(W.PAL_LANGUAGES[language], report[0][4][-1])
                self.assertEqual([ex["player"][4] for ex in extras], [language] * 3)
                self.assertEqual(hd2gc.hd_pal_language(hd), language)
                for s in range(3):
                    for field in W.FIELD_NAMES:
                        expected = b"\0\1" if s < 2 and field == "info.save_count" else W.get(original[s], W.GC_FIELDS, field)
                        self.assertEqual(W.get(converted[s], W.HD_FIELDS, field), expected, (s, field))
                back = W.write_gci(code, hd2gc.hd_to_gc_slots(hd, code=code), pal_language=hd2gc.hd_pal_language(hd))
                _, restored = W.read_gci(back)
                self.assertEqual(restored, converted)
                self.assertEqual(W.gc_pal_language(back), language)
                self.assertEqual(gc2hd.convert_gci(back)[1], hd)

    def test_card_language_is_not_slot_counter(self):
        raw = bytearray(W.write_gci(b"GZLP01", slots(), pal_language=4))
        struct.pack_into(">I", raw, W.GCI_HEADER + W.CARD_BLOCK, 0xFFFFFFFF)
        self.assertEqual(W.gc_pal_language(raw), 0)  # tww Restore clamps invalid language
        converted, _, _ = W.read_hd(gc2hd.convert_gci(bytes(raw))[1])
        self.assertEqual(W.get(converted[0], W.HD_FIELDS, "info.save_count"), b"\0\1")
        self.assertEqual(W.get(converted[2], W.HD_FIELDS, "info.save_count"), b"\0\0")

    def test_slot_backup_and_bad_checksums(self):
        raw = bytearray(W.write_gci(b"GZLP01", slots()))
        off = W.GCI_HEADER + W.CARD_BLOCK + 8 + 0x770
        raw[off] ^= 1
        self.assertIsNotNone(W.read_gci(bytes(raw))[1][1])
        gc2hd.convert_gci(bytes(raw))
        raw[off + W.CARD_BLOCK] ^= 1
        self.assertIsNone(W.read_gci(bytes(raw))[1][1])
        with self.assertRaises(W.GciError):
            gc2hd.convert_gci(bytes(raw))

    def test_region_template_and_other_regions(self):
        template = W.write_gci(b"GZLE01", slots())
        pal = W.write_gci(b"GZLP01", slots(), template=template)
        self.assertEqual(pal[:6], b"GZLP01")
        self.assertEqual(pal[6:W.GCI_HEADER + W.CARD_BLOCK], template[6:W.GCI_HEADER + W.CARD_BLOCK])
        for code in (b"GZLE01", b"GZLJ01"):
            W.read_hd(gc2hd.convert_gci(W.write_gci(code, slots()))[1])
        with self.assertRaises(W.GciError):
            W.read_gci(pal, allow=(b"GZLE01",))
        for language in (-1, 5):
            with self.assertRaises(W.GciError):
                W.write_gci(b"GZLP01", slots(), pal_language=language)

    def test_mixed_hd_languages_require_override(self):
        hd = gc2hd.convert_gci(W.write_gci(b"GZLP01", slots()))[1]
        data, extras, _ = W.read_hd(hd)
        player = bytearray(extras[1]["player"])
        player[4] = 3
        extras[1]["player"] = bytes(player)
        with self.assertRaisesRegex(W.HdError, "different languages"):
            hd2gc.hd_pal_language(W.write_hd(data, extras))

    def test_cli_and_pal_name_output(self):
        base = Path(__file__).parent
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "pal.gci"
            source.write_bytes(W.write_gci(b"GZLP01", slots(), pal_language=2))
            subprocess.run([sys.executable, str(base / "gc2hd.py"), str(source), "-o", str(root / "hd")], check=True, capture_output=True)
            subprocess.run([sys.executable, str(base / "hd2gc.py"), str(root / "hd/cking.sav"), "-o", str(root / "back.gci"), "--code", "GZLP01", "--pal-language", "german"], check=True, capture_output=True)
            self.assertEqual(W.gc_pal_language((root / "back.gci").read_bytes()), 1)
            # Use a normal HD file whose internal GameCube name is empty.
            normal, extras, _ = W.read_hd((root / "hd/cking.sav").read_bytes())
            for i in (0, 1):
                slot = bytearray(normal[i])
                W.put(slot, W.HD_FIELDS, "info.player_name", bytes(17))
                normal[i] = bytes(slot)
            back = hd2gc.hd_to_gc_slots(W.write_hd(normal, extras), True, b"GZLP01")
            self.assertEqual(W.get(back[0], W.GC_FIELDS, "info.player_name"), b"Andr\xe9".ljust(17, b"\0"))
            self.assertEqual(W.get(back[1], W.GC_FIELDS, "info.player_name"), b"\x8c\x9c".ljust(17, b"\0"))
            default = [sys.executable, str(base / "hd2gc.py"), str(root / "hd/cking.sav"),
                       "-o", str(root / "default.gci"), "--code", "GZLP01"]
            subprocess.run(default, check=True, capture_output=True)
            self.assertEqual(W.gc_pal_language((root / "default.gci").read_bytes()), 2)
            rejected = subprocess.run(default[:-2] + ["--pal-language", "german"], capture_output=True)
            self.assertNotEqual(rejected.returncode, 0)


if __name__ == "__main__":
    unittest.main()
