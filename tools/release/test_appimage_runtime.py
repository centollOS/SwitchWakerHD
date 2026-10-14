#!/usr/bin/env python3
"""The pinned AppImage runtime (appimage.py RUNTIME): a file or download with another SHA-256 is
refused, the pinned one is used, and --extract-runtime gets the runtime back out of an AppImage.
No network: downloads come from file:// URLs."""
import hashlib
import os
import pathlib
import re
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import appimage  # noqa: E402

GOOD = b"\x7fELF pinned runtime bytes"
BAD = b"\x7fELF some other runtime"


class Runtime(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        pin = {"x86_64": ("runtime-x86_64", hashlib.sha256(GOOD).hexdigest())}
        self.patches = [mock.patch.object(appimage, "RUNTIME", pin),
                        mock.patch.object(appimage.tempfile, "gettempdir", lambda: os.path.join(self.dir, "cache"))]
        for p in self.patches:
            p.start()
        os.makedirs(os.path.join(self.dir, "cache"))

    def tearDown(self):
        for p in self.patches:
            p.stop()
        self.tmp.cleanup()

    def write(self, rel, data):
        path = os.path.join(self.dir, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def source(self, name, data):
        self.write(os.path.join(name, "runtime-x86_64"), data)
        # as_uri(): a valid file URL on Windows too ("file:///C:/..."; "file://C:\\..." is not one)
        return pathlib.Path(self.dir, name).resolve().as_uri() + "/"

    def test_the_real_pin(self):
        for p in self.patches:
            p.stop()
        try:
            self.assertEqual(set(appimage.RUNTIME), {"x86_64", "aarch64"})
            for arch, (name, sha) in appimage.RUNTIME.items():
                self.assertEqual(name, "runtime-" + arch)
                self.assertRegex(sha, r"^[0-9a-f]{64}$")
            self.assertTrue(re.fullmatch(r"[0-9a-f]{7,40}", appimage.RUNTIME_COMMIT))
        finally:
            for p in self.patches:
                p.start()

    def test_a_given_file_with_the_wrong_checksum_is_refused(self):
        with self.assertRaises(SystemExit) as e:
            appimage.ensure_runtime(self.write("wrong/runtime", BAD), "x86_64")
        self.assertIn("SHA-256", str(e.exception.code))

    def test_a_given_file_with_the_pinned_checksum_is_used(self):
        path = self.write("right/runtime", GOOD)
        self.assertEqual(appimage.ensure_runtime(path, "x86_64"), path)

    def test_a_download_with_the_wrong_checksum_fails_the_build(self):
        with self.assertRaises(SystemExit) as e:
            appimage.ensure_runtime(None, "x86_64", urls=[self.source("moved", BAD)])
        self.assertIn("no source has the pinned AppImage runtime", str(e.exception.code))
        self.assertEqual(os.listdir(os.path.join(self.dir, "cache")), [])  # nothing kept

    def test_the_first_matching_source_wins_and_is_cached(self):
        path = appimage.ensure_runtime(None, "x86_64", urls=[self.source("moved", BAD), self.source("pinned", GOOD)])
        with open(path, "rb") as f:
            self.assertEqual(f.read(), GOOD)
        self.assertEqual(appimage.ensure_runtime(None, "x86_64", urls=[]), path)  # cached, no download

    def test_an_unknown_architecture(self):
        with self.assertRaises(SystemExit):
            appimage.ensure_runtime(None, "riscv64", urls=[])

    def test_extract_runtime_zeroes_the_digest(self):
        # a minimal ELF64 (little endian): null, .shstrtab and .digest_md5 sections, then a squashfs
        names = b"\0.shstrtab\0.digest_md5\0"
        digest_off, names_off, shoff = 0x40, 0x50, 0x70
        body = bytearray(shoff + 3 * 64)
        body[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<Q", body, 0x28, shoff)
        struct.pack_into("<HHH", body, 0x3A, 64, 3, 1)
        body[digest_off:digest_off + 16] = bytes(range(1, 17))  # what appimagetool writes per image
        body[names_off:names_off + len(names)] = names
        struct.pack_into("<IIQQQQIIQQ", body, shoff + 64, 1, 3, 0, 0, names_off, len(names), 0, 0, 1, 0)
        struct.pack_into("<IIQQQQIIQQ", body, shoff + 128, 11, 1, 0, 0, digest_off, 16, 0, 0, 1, 0)
        image = self.write("x.AppImage", bytes(body) + b"hsqs" + b"\0" * 64)
        out = os.path.join(self.dir, "runtime-out")
        appimage.extract_runtime(image, out)
        with open(out, "rb") as f:
            got = f.read()
        want = bytearray(body)
        want[digest_off:digest_off + 16] = bytes(16)
        self.assertEqual(got, bytes(want))
        with self.assertRaises(SystemExit):  # no squashfs where the runtime ends
            appimage.extract_runtime(self.write("y.AppImage", bytes(body) + b"nope"), out)


if __name__ == "__main__":
    unittest.main()
