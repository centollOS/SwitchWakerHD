#!/usr/bin/env python3
"""tools/switch/shader_manifest.py, the code-free template parts (issue #88): the registers a state keeps, a state back
into a manifest record, and the game-bytes check. No game files: a made-up dump with a made-up shader file."""
import os
import shutil
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shader_manifest as sm  # noqa: E402

TF = sm.template_format()


class TemplateTest(unittest.TestCase):
    def test_registers(self):
        regs = {0xA210: 0x1234, 0xA211: 9, 0xA216: 1, 0xA225: 2, 0x2300: 7, 0xC010: 5, 0xD000: 6, 0xE000: 8, 0x1000: 3,
                0x5000: 4}
        self.assertEqual(sm.template_registers(regs), [[0x2300, 7], [0xA211, 9], [0xE000, 8]])

    def test_state_round_trip(self):
        attr = dict(buffer=0, semantic=1, format=48, fetch_type=0, numeric=2, signed=0, endian=2, x=0, y=1, z=2, w=5,
                    offset=24, divisor=0)
        fetch = {"hash": 1, "metal_hash": 1, "compact": 1, "attributes": [attr]}
        states = [{"hash": 0x55, "vertex": 1, "fetch": fetch, "registers": [[0xA211, 4], [0xA000, 7]]},
                  {"hash": 0x66, "vertex": 0, "fetch": None, "registers": [[0xA217, 3]]},
                  {"hash": 0x77, "vertex": 0, "fetch": None, "registers": []}]
        doc = TF.decode(TF.encode({"version": 3, "states": states, "pipelines": []}), compact=True)
        programs = {(0x55, 32): b"v" * 32, (0x66, 16): b"p" * 16}
        recs, used, missing = sm.states_to_records(doc["states"], programs, TF)
        self.assertEqual(missing, 1)  # 0x77: not in the dump
        self.assertEqual(sorted(used), [(0x55, 32), (0x66, 16)])
        tmp = tempfile.mkdtemp()
        try:
            path = os.path.join(tmp, "m.bin")
            sm.write_manifest(path, recs)
            back = sm.read_manifest_full(path)
        finally:
            shutil.rmtree(tmp)
        v, p = back
        self.assertEqual((v[0], v[1], v[2], v[6]), (True, 0x55, 32, 2))
        self.assertEqual(TF.parse_fetch(v[4], v[5], None)["attributes"], [attr])
        self.assertEqual(v[5], {0xA211: 4, 0xA000: 7})
        self.assertEqual((p[0], p[1], p[4], p[6], p[5]), (False, 0x66, b"", 0, {0xA217: 3}))

    def test_same_hash_two_sizes(self):
        with self.assertRaises(SystemExit):
            sm.states_to_records([], {(1, 8): b"a" * 8, (1, 16): b"b" * 16}, TF)


class GameBytesTest(unittest.TestCase):
    def setUp(self):
        self.game = tempfile.mkdtemp()
        path = os.path.join(self.game, "content", "Common", "Shaders", "x.sharcfb")
        os.makedirs(os.path.dirname(path))
        self.binary = bytes(range(200, 256)) + bytes(range(0, 40))
        self.ascii = b'"hash":12345,"vertex":1'  # game bytes that happen to be JSON characters
        with open(path, "wb") as f:
            f.write(b"\x00" * 64 + self.binary + b"\x01" * 8 + self.ascii + b"\x02" * 8)
        self.archive = sm.archive_programs
        sm.archive_programs = lambda game: {}

    def tearDown(self):
        sm.archive_programs = self.archive
        shutil.rmtree(self.game)

    def test_clean(self):
        self.assertIsNone(sm.game_bytes_found([b'{"states":[{"hash":1}]}'], [os.urandom(4096)], self.game))

    def test_text(self):
        self.assertIsNotNone(sm.game_bytes_found([b"[" + self.ascii[2:20] + b"]"], [], self.game))
        self.assertIsNone(sm.game_bytes_found([b"[" + self.ascii[2:17] + b"]"], [], self.game))  # 15 bytes only

    def test_binary(self):
        for start in range(0, 9):  # the 16 bytes at every alignment of the blob
            blob = os.urandom(100 + start) + self.binary[10:26] + os.urandom(50)
            self.assertIsNotNone(sm.game_bytes_found([], [blob], self.game), start)
        self.assertIsNone(sm.game_bytes_found([], [os.urandom(64) + self.binary[10:25] + os.urandom(64)], self.game))


if __name__ == "__main__":
    unittest.main()
