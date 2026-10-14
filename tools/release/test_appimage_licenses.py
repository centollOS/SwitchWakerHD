#!/usr/bin/env python3
"""The AppImage runtime's license texts: appimage.py puts them into the AppImage's
third-party-licenses/, and guard.py rejects an AppImage tree without them (or with changed ones)."""
import os
import re
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import appimage  # noqa: E402
import guard  # noqa: E402


class RuntimeLicenses(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        pkg = os.path.join(self.tmp.name, "WindWakerHD-test-linux-x86_64")
        os.makedirs(os.path.join(pkg, "third-party-licenses"))
        with open(os.path.join(pkg, "third-party-licenses", "README.txt"), "w") as f:
            f.write("Third-party software in this package and its license files:\n")
        with open(os.path.join(pkg, "wind-waker-hd"), "w") as f:
            f.write("#!/bin/sh\n")
        self.appdir = os.path.join(self.tmp.name, "AppDir")
        appimage.make_appdir(pkg, self.appdir)
        self.lic = os.path.join(self.appdir, "third-party-licenses")

    def tearDown(self):
        self.tmp.cleanup()

    def license_problems(self, path):
        problems, _ = guard.scan(path)
        return [p for p in problems if "license" in p]

    def test_the_build_ships_every_text_unchanged(self):
        names = appimage.runtime_license_files()
        self.assertIn("AppImage-runtime-README.txt", names)
        for name in names:
            with open(os.path.join(self.lic, name), "rb") as a, \
                    open(os.path.join(appimage.RUNTIME_LICENSES, name), "rb") as b:
                self.assertEqual(a.read(), b.read(), name)
        with open(os.path.join(self.lic, "README.txt")) as f:
            index = f.read()
        self.assertIn("AppImage-runtime-README.txt", index)
        self.assertIn("AppImage-runtime-libfuse-LGPL-2.1.txt", index)
        self.assertEqual(self.license_problems(self.appdir), [])

    def test_guard_rejects_a_missing_text(self):
        os.remove(os.path.join(self.lic, "AppImage-runtime-libfuse-LGPL-2.1.txt"))
        problems = self.license_problems(self.appdir)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("AppImage-runtime-libfuse-LGPL-2.1.txt", problems[0])

    def test_guard_rejects_a_changed_text(self):
        with open(os.path.join(self.lic, "AppImage-runtime-zlib-Zlib.txt"), "a") as f:
            f.write("changed\n")
        problems = self.license_problems(self.appdir)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("differs", problems[0])

    def test_guard_rejects_an_appimage_without_any(self):
        shutil.rmtree(self.lic)
        self.assertEqual(len(self.license_problems(self.appdir)), len(appimage.runtime_license_files()))

    def test_a_tree_without_apprun_needs_none(self):
        # the zip release has no AppImage runtime
        shutil.rmtree(self.lic)
        os.remove(os.path.join(self.appdir, "AppRun"))
        self.assertEqual(self.license_problems(self.appdir), [])

    def test_the_readme_lists_every_text(self):
        with open(os.path.join(appimage.RUNTIME_LICENSES, "AppImage-runtime-README.txt")) as f:
            readme = f.read()
        listed = set(re.findall(r"AppImage-runtime-[A-Za-z0-9.-]+\.txt", readme))
        self.assertEqual(listed, set(appimage.runtime_license_files()) - {"AppImage-runtime-README.txt"})
        self.assertIn(appimage.RUNTIME_COMMIT, readme)


if __name__ == "__main__":
    unittest.main()
