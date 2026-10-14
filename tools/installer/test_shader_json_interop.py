"""C++/Python JSON agreement, including Windows paths, on every host."""
import json
import subprocess
import sys
import unittest


EXECUTABLE = None


class ShaderJSONInterop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not EXECUTABLE:
            raise unittest.SkipTest("requires the compiled C++ fixture argument")

    def test_paths_keys_and_control_bytes(self):
        path = r'D:\a\_temp\msys64\tmp\retry\vulkan\failed\Jürgen-雪\100%shader'
        value = {
            'launcher': {'package': path, 'template': path + r'\template.bin'},
            'active': {'generation': 'generation-test', 'path': path},
            'failed': {'message': '"quoted"\n\r\t\b\f\\'},
            'skip': {path: path + '\\'},
            'controls': ''.join(map(chr, range(32))),
        }
        for ascii_only in (False, True):
            with self.subTest(ensure_ascii=ascii_only):
                result = subprocess.run([EXECUTABLE, '--json-roundtrip'],
                    input=json.dumps(value, ensure_ascii=ascii_only).encode('utf-8'),
                    capture_output=True, check=True)
                self.assertEqual(json.loads(result.stdout), value)
                # Feed the C++ writer's output back to its own reader as well.
                again = subprocess.run([EXECUTABLE, '--json-roundtrip'],
                    input=result.stdout, capture_output=True, check=True)
                self.assertEqual(json.loads(again.stdout), value)


if __name__ == '__main__':
    EXECUTABLE = sys.argv.pop(1)
    unittest.main()
