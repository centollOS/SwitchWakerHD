"""Checks for thread_profile.py: the sampler's file format, pointer-authentication stripping, module
lookup and the frame list (no symbolizer needed)."""
import os
import struct
import tempfile
import unittest

import thread_profile as tp


def write_samples(path, tags, samples):
    with open(path, "wb") as f:
        f.write(b"WWHDSAMP1 %d\n" % len(tags))
        for t in tags:
            f.write(t.encode() + b"\n")
        for thread, frames in samples:
            f.write(struct.pack("<II", thread, len(frames)))
            f.write(struct.pack("<%dQ" % len(frames), *frames))


class ThreadProfileTest(unittest.TestCase):
    def test_reads_samples_and_strips_pointer_authentication(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "p.samples")
            signed = 0xF4247FFC6D693C6C  # a return address with a PAC in its upper bits
            write_samples(path, ["render", "game"], [(0, [0x7FF600001000, signed]), (1, [0x7FF600002000])])
            with open(path, "ab") as f:
                f.write(struct.pack("<II", 0, 5))  # a record cut short by the process exiting
            tags, samples = tp.read_samples(path)
        self.assertEqual(tags, ["render", "game"])
        self.assertEqual(samples, [(0, (0x7FF600001000, 0x7FFC6D693C6C)), (1, (0x7FF600002000,))])

    def test_rejects_other_files(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "x.samples")
            with open(path, "wb") as f:
                f.write(b"OTHER 1\nrender\n")
            with self.assertRaises(SystemExit):
                tp.read_samples(path)

    def test_modules_and_frames(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "p.modules")
            with open(path, "w") as f:
                f.write("7ffc6d000000 800000 C:\\Windows\\System32\\drv.dll\n")
                f.write("7ff600000000 100000 C:\\game\\wwhd.exe\n")
            mods = tp.Modules(tp.read_modules(path))
        self.assertTrue(mods.find(0x7FF600000010)[2].endswith("wwhd.exe"))
        self.assertTrue(mods.find(0x7FFC6D7FFFFF)[2].endswith("drv.dll"))
        self.assertIsNone(mods.find(0x7FFC6D800000))  # one past the module's end
        # pc, then lr (a duplicate of the first frame-pointer frame outside leaf functions), then callers
        self.assertEqual(tp.frames_of((1, 2, 2, 3, 0, 4)), [1, 2, 3, 4])


if __name__ == "__main__":
    unittest.main()
