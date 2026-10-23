#!/usr/bin/env python3
"""tools/switch/perf_baseline.py and perf_test.py's verdict: which commit a baseline is, whether it needs the
bench patch, the generated-code cache key, and how a comparison is read. No console, no build: a made-up git
repository and made-up results."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import perf_baseline as pb  # noqa: E402
import perf_test as pt  # noqa: E402


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, text=True).stdout.strip()


class BaselineTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name) / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q", "-b", "main")
        git(self.repo, "config", "user.email", "t@example.com")
        git(self.repo, "config", "user.name", "t")
        self.commit("tools/recomp/recomp.py", "v1", "first")
        git(self.repo, "tag", "v0.4.7")
        self.old = git(self.repo, "rev-parse", "HEAD")
        self.commit(pb.BENCH_SOURCE, "bench", "bench")
        self.commit("tools/recomp/recomp.py", "v2", "recomp changed")
        self.new = git(self.repo, "rev-parse", "HEAD")
        # origin/main as the remote-tracking ref the real checkout has
        git(self.repo, "update-ref", "refs/remotes/origin/main", self.new)

    def tearDown(self):
        self.tmp.cleanup()

    def commit(self, rel, text, msg):
        p = self.repo / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        git(self.repo, "add", rel)
        git(self.repo, "commit", "-q", "-m", msg)

    def test_main_is_origin_main(self):
        self.assertEqual(pb.resolve(self.repo, "main"), ("main", self.new))

    def test_last_tag_is_the_newest_tag_on_origin_main(self):
        self.assertEqual(pb.resolve(self.repo, "last-tag"), ("v0.4.7", self.old))
        self.commit("a.txt", "x", "after")
        git(self.repo, "tag", "v0.4.8")
        git(self.repo, "update-ref", "refs/remotes/origin/main", "HEAD")
        self.assertEqual(pb.resolve(self.repo, "last-tag")[0], "v0.4.8")

    def test_unknown_baseline_refused(self):
        with self.assertRaises(SystemExit):
            pb.resolve(self.repo, "no-such-tag")

    def test_has_bench(self):
        self.assertFalse(pb.has_bench(self.repo, self.old))
        self.assertTrue(pb.has_bench(self.repo, self.new))

    def test_patch_needed_only_without_bench(self):
        patches = Path(self.tmp.name) / "patches"
        patches.mkdir()
        (patches / "v0.4.7.patch").write_text("")
        self.assertEqual(pb.bench_patch(self.repo, "v0.4.7", self.old, patches), patches / "v0.4.7.patch")
        self.assertIsNone(pb.bench_patch(self.repo, "main", self.new, patches))
        with self.assertRaises(SystemExit):  # no bench and no patch: it cannot be measured
            pb.bench_patch(self.repo, "v0.4.6", self.old, patches)

    def test_recomp_key_follows_tools_recomp(self):
        self.assertNotEqual(pb.recomp_key(self.repo, self.old), pb.recomp_key(self.repo, self.new))
        self.commit("other.txt", "y", "unrelated")
        self.assertEqual(pb.recomp_key(self.repo, "HEAD"), pb.recomp_key(self.repo, self.new))


class RpxTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.repo, pb.REPO = pb.REPO, self.root
        self.env = os.environ.pop("WWHD_RPX", None)

    def tearDown(self):
        pb.REPO = self.repo
        if self.env is not None:
            os.environ["WWHD_RPX"] = self.env
        self.tmp.cleanup()

    def rpx(self, rel):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"rpx")
        return p

    def test_one_dump_is_found(self):
        p = self.rpx("Rom/Decrypted/WWHD/code/cking.rpx")
        self.assertEqual(pb.find_rpx(), p.resolve())

    def test_two_dumps_need_rpx(self):  # a USA and an EUR dump: never pick one silently
        self.rpx("Rom/Decrypted/WWHD/code/cking.rpx")
        self.rpx("Rom/Decrypted/WWHD-EU/code/cking.rpx")
        with self.assertRaises(SystemExit) as e:
            pb.find_rpx()
        self.assertIn("--rpx", str(e.exception))

    def test_given_path_wins(self):
        self.rpx("Rom/Decrypted/WWHD/code/cking.rpx")
        mine = self.rpx("elsewhere/cking.rpx")
        self.assertEqual(pb.find_rpx(str(mine)), mine.resolve())


def result(build, scene, fps, main_ms):
    return {"build": build, "scene": scene, "fps": fps, "main_ms": main_ms}


class IntervalTest(unittest.TestCase):
    def test_welch_t_not_normal(self):  # 4 boots each: t with ~6 degrees of freedom (2.447), not 1.96
        a, b = [28.4, 28.5, 28.5, 28.6], [29.0, 29.1, 29.1, 29.2]
        se = (0.08165 ** 2 / 4 * 2) ** 0.5
        self.assertAlmostEqual(pt.ci95(a, b), 2.447 * se, places=2)

    def test_one_run_has_no_interval(self):
        self.assertEqual(pt.ci95([28.0], [29.0, 29.1]), 0.0)


class VerdictTest(unittest.TestCase):
    def test_within_the_noise(self):
        r = [result("main", "lava", f, 34.0) for f in (28.4, 28.6, 28.5, 28.7)] + \
            [result("mine", "lava", f, 34.0) for f in (28.5, 28.7, 28.4, 28.6)]
        (line,) = pt.verdict(r, "main", "mine")
        self.assertIn("lava", line)
        self.assertIn("no measurable change", line)

    def test_faster(self):
        r = [result("main", "lava", f, 34.0) for f in (28.4, 28.5, 28.5, 28.6)] + \
            [result("mine", "lava", f, 33.0) for f in (29.0, 29.1, 29.1, 29.2)]
        (line,) = pt.verdict(r, "main", "mine")
        self.assertIn("faster", line)
        self.assertIn("+0.60 fps", line)

    def test_slower(self):
        r = [result("main", "windfall", f, 34.0) for f in (28.4, 28.5, 28.5, 28.6)] + \
            [result("mine", "windfall", f, 35.0) for f in (27.8, 27.9, 27.9, 28.0)]
        (line,) = pt.verdict(r, "main", "mine")
        self.assertIn("slower", line)

    def test_at_the_cap_reads_the_main_thread(self):
        r = [result("main", "outset", 29.98, m) for m in (26.3, 26.4, 26.3, 26.4)] + \
            [result("mine", "outset", 29.98, m) for m in (25.3, 25.4, 25.3, 25.4)]
        (line,) = pt.verdict(r, "main", "mine")
        self.assertIn("30 fps cap", line)
        self.assertIn("main thread", line)
        self.assertIn("-1.00 ms", line)

    def test_one_run_each_is_not_a_comparison(self):
        r = [result("main", "lava", 28.0, 34.0), result("mine", "lava", 29.0, 33.0)]
        (line,) = pt.verdict(r, "main", "mine")
        self.assertIn("not enough runs", line)

    def test_errors_and_missing_scenes_are_said(self):
        r = [result("main", "lava", f, 34.0) for f in (28.4, 28.6)] + [{"build": "mine", "scene": "lava", "error": "x"}]
        (line,) = pt.verdict(r, "main", "mine")
        self.assertIn("not enough runs", line)


class GpuProfileTest(unittest.TestCase):
    INI = "# Wind Waker HD settings\nswitchBloom=0\n\n[dev]\nWWHD_X=1\nWWHD_TEST_GPU_PROFILE=460\n"

    def test_profile_line(self):
        self.assertEqual(pt.settings_with_profile(self.INI, "384"),
                         "# Wind Waker HD settings\nswitchBloom=0\n\n[dev]\nWWHD_TEST_GPU_PROFILE=384\nWWHD_X=1\n")

    def test_stock_is_explicit(self):
        out = pt.settings_with_profile(self.INI, "307")
        self.assertIn("[dev]\nWWHD_TEST_GPU_PROFILE=307\n", out)
        self.assertEqual(out.count("WWHD_TEST_GPU_PROFILE"), 1)

    def test_without_dev_section(self):
        self.assertEqual(pt.settings_with_profile("switchBloom=0\n", "1600"),
                         "switchBloom=0\n\n[dev]\nWWHD_TEST_GPU_PROFILE=1600\n")


if __name__ == "__main__":
    unittest.main()
