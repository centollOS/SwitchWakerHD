#!/usr/bin/env python3
"""Lenzo's Picto Box test across the conversion (#306): synthetic GameCube saves, one per quest
stage, no game files or real saves.

GameCube: the third picto raises Lenzo's register (event 0xC407) to 6 and gives the Joy Pendant;
the Deluxe Picto Box needs a Forest Firefly afterwards. HD gives the Deluxe Picto Box with the
third picto. Only the stage in between (register 6, black-and-white Picto Box) changes.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gc2hd  # noqa: E402
import hd2gc  # noqa: E402
import wwsave as W  # noqa: E402

REG = W.HD_FIELDS["event.flags"][0] + W.LENZO_REG_BYTE
SLOT8 = W.HD_FIELDS["item.slot08"][0]
GET8 = W.HD_FIELDS["get_item.slot08"][0]


def gc_slot(stage, camera=W.PICTO_BOX, other_bits=0xA8, name=b"Link"):
    """a used GameCube file at Lenzo stage `stage` (register value) with `camera` in slot 8"""
    g = bytearray(W.GAMEDATA_SIZE)
    W.put(g, W.GC_FIELDS, "info.player_name", name.ljust(17, b"\0"))
    W.put(g, W.GC_FIELDS, "item.slot08", bytes([camera]))
    got = {0xFF: 0, W.PICTO_BOX: 0x01, W.DELUXE_PICTO_BOX: 0x03}[camera]
    W.put(g, W.GC_FIELDS, "get_item.slot08", bytes([got]))
    W.put(g, W.GC_FIELDS, "status_a.select_item", bytes([8, 0xFF, 0xFF, 0xFF, 0xFF]))
    g[REG] = other_bits | stage          # the other bits of event byte 0xC4 belong to other flags
    if stage:
        g[W.HD_FIELDS["event.flags"][0] + 0x16] |= 0x01   # 0x1601: Lenzo's test started
    return g


def convert(slots):
    code, hd, report = gc2hd.convert_gci(W.write_gci(b"GZLE01", slots))
    data, _, _ = W.read_hd(hd)                  # HD checksums and CRC verified here
    return hd, data, report


class PictoQuestTest(unittest.TestCase):
    def assert_only_picto_changed(self, before, after, changed):
        diff = [i for i in range(W.GAMEDATA_SIZE) if before[i] != after[i]]
        info_cnt = W.HD_FIELDS["info.save_count"][0]
        diff = [i for i in diff if i not in (info_cnt, info_cnt + 1)]  # 0 -> 1 for every used file
        self.assertEqual(diff, sorted(changed))

    def test_every_stage(self):
        cases = [  # (register, camera item) -> expected camera item, get-item bits
            (0, 0xFF, 0xFF, 0x00),                         # no Picto Box yet
            (0, W.PICTO_BOX, W.PICTO_BOX, 0x01),           # Picto Box, test not started
            (1, W.PICTO_BOX, W.PICTO_BOX, 0x01),           # order 1 given
            (2, W.PICTO_BOX, W.PICTO_BOX, 0x01),           # picto 1 accepted
            (3, W.PICTO_BOX, W.PICTO_BOX, 0x01),
            (4, W.PICTO_BOX, W.PICTO_BOX, 0x01),
            (5, W.PICTO_BOX, W.PICTO_BOX, 0x01),           # order 3 given: HD gives Deluxe at the 3rd
            (6, W.PICTO_BOX, W.DELUXE_PICTO_BOX, 0x03),    # GameCube-only: waiting for the firefly
            (7, W.PICTO_BOX, W.DELUXE_PICTO_BOX, 0x03),    # out of range, same as >= 6 in both games
            (6, W.DELUXE_PICTO_BOX, W.DELUXE_PICTO_BOX, 0x03),  # firefly shown: done in both
        ]
        for reg, camera, want_item, want_get in cases:
            with self.subTest(reg=reg, camera=hex(camera)):
                g = gc_slot(reg, camera)
                hd, data, report = convert([g, gc_slot(0, name=b"Other"), bytes(W.GAMEDATA_SIZE)])
                out = data[0]
                self.assertEqual(out[SLOT8], want_item)
                self.assertEqual(out[GET8], want_get)
                self.assertEqual(out[REG], g[REG])   # the test stays passed, other bits kept
                changed = [SLOT8, GET8] if want_item != camera else []
                self.assert_only_picto_changed(g, out, changed)
                notes = report[0][4]
                self.assertEqual(any("Deluxe Picto Box" in n for n in notes), bool(changed))
                self.assertFalse(W.picto_test_stuck(out, W.HD_FIELDS))
                # file 2 (stage 0) and the empty file 3 are untouched
                self.assertEqual(data[1][SLOT8], W.PICTO_BOX)
                self.assertEqual(bytes(data[2]), bytes(W.GAMEDATA_SIZE))

    def test_empty_gc_file_untouched(self):
        g = gc_slot(6, name=b"")                     # unused file: copied as it is
        _, data, _ = convert([g, bytes(W.GAMEDATA_SIZE), bytes(W.GAMEDATA_SIZE)])
        self.assertEqual(bytes(data[0]), bytes(g))

    def test_hd_round_trip_after_repair(self):
        # converted file -> GameCube layout -> HD: unchanged (HD's finished state is valid on both)
        hd, data, _ = convert([gc_slot(6), gc_slot(3), gc_slot(6, W.DELUXE_PICTO_BOX)])
        back = gc2hd.convert_gci(W.write_gci(b"GZLE01", hd2gc.hd_to_gc_slots(hd)))[1]
        self.assertEqual(W.read_hd(back)[0], data)

    def test_runtime_rule_matches(self):
        # runtime/src/hle/picto_quest.h checks the same bytes; keep the offsets in step
        self.assertEqual((REG, SLOT8, GET8), (0x6DC, 0x44, 0x59))


if __name__ == "__main__":
    unittest.main()
