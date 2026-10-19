#!/usr/bin/env python3
"""tools/switch/game_files.py: a list made from a folder checks it OK; missing, cut-off and damaged files are
found; files the list does not know do not count. No game files: a folder made up here."""
import os
import shutil
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_files  # noqa: E402


class GameFilesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.game = os.path.join(self.tmp, "game")
        for rel, data in (("content/Common/Stage/sea_Room38.szs", b"room" * 100), ("content/Common/a.pack", b"pack" * 50),
                          ("content/Cafe/US/b.sarc", b"sarc" * 25)):
            path = os.path.join(self.game, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        self.lists = game_files.LISTS
        game_files.LISTS = self.tmp
        game_files.make(self.game, "TEST")

    def tearDown(self):
        game_files.LISTS = self.lists
        shutil.rmtree(self.tmp)

    def test_list(self):
        listed = game_files.read_list("TEST")
        self.assertEqual(sorted(listed), ["content/Cafe/US/b.sarc", "content/Common/Stage/sea_Room38.szs",
                                          "content/Common/a.pack"])
        self.assertEqual(listed["content/Common/a.pack"][0], 200)

    def test_good(self):
        with open(os.path.join(self.game, "content", "extra.bin"), "wb") as f:
            f.write(b"left by a dump tool")
        self.assertEqual(game_files.check(self.game, "TEST"), ([], [], []))

    def test_bad(self):
        os.remove(os.path.join(self.game, "content", "Common", "Stage", "sea_Room38.szs"))
        with open(os.path.join(self.game, "content", "Common", "a.pack"), "r+b") as f:
            f.truncate(10)
        with open(os.path.join(self.game, "content", "Cafe", "US", "b.sarc"), "r+b") as f:
            f.seek(5)
            f.write(b"X")
        missing, size, content = game_files.check(self.game, "TEST")
        self.assertEqual((missing, size, content), (["content/Common/Stage/sea_Room38.szs"], ["content/Common/a.pack"],
                                                    ["content/Cafe/US/b.sarc"]))
        self.assertEqual(len(game_files.report((missing, size, content))), 3)

    def test_no_list(self):
        self.assertIsNone(game_files.check(self.game, "NONE"))

    def test_shipped_lists(self):
        game_files.LISTS = self.lists
        for build in ("USA", "EU"):
            listed = game_files.read_list(build)
            self.assertGreater(len(listed), 1000, build)
            self.assertTrue(all(p.startswith("content/") and len(d) == 64 for p, (_, d) in listed.items()), build)

if __name__ == "__main__":
    unittest.main()
