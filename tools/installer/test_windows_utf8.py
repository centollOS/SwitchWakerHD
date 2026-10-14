"""Runtime/Python user-directory agreement on Windows; synthetic paths only."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import shader_prebuild


@unittest.skipUnless(sys.platform == 'win32' and os.environ.get('WWHD_WINDOWS_UTF8_TEST'),
                     'requires the manifested Windows test executable')
class WindowsUTF8(unittest.TestCase):
    def test_python_and_runtime_share_unicode_cache_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            user = Path(temporary) / 'Jürgen-雪'
            user.mkdir()
            env = os.environ.copy()
            env['WWHD_USER_DIR'] = str(user)
            result = subprocess.run([env['WWHD_WINDOWS_UTF8_TEST'], '--print-root'], env=env,
                capture_output=True, text=True, encoding='utf-8', check=True)
            with patch.dict(os.environ, env):
                expected = shader_prebuild.default_cache_root()
            self.assertEqual(Path(result.stdout.strip()), expected)


if __name__ == '__main__':
    unittest.main()
