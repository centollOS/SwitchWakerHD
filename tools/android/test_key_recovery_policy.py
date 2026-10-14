"""Run Android's production key recovery decisions on the host JVM, without Android or keys."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'host JDK required')
class KeyRecoveryPolicy(unittest.TestCase):
    def test_shared_access_loss_and_multikey_reselection(self):
        with tempfile.TemporaryDirectory() as directory:
            compiled = subprocess.run(['javac', '-d', directory,
                            str(ROOT / 'android/app/src/main/java/org/wwhdrecomp/wwhd/SetupKeyRecoveryPolicy.java'),
                            str(ROOT / 'tools/android/SetupKeyRecoveryPolicyTest.java')], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run(['java', '-cp', directory, 'SetupKeyRecoveryPolicyTest'],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
