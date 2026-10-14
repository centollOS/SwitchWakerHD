"""Read-only Android key descriptor handling (authored keys, no game files)."""
import os
import tempfile
import unittest
from unittest import mock
from ondevice_setup import main, read_key_descriptors


class KeyDescriptors(unittest.TestCase):
    def test_regular_file_and_pipe_stay_in_memory_and_original_fds_stay_open(self):
        payload = b'# authored keys\n' + b'01' * 16 + b' # synthetic\n' + b'02' * 16 + b'\n'
        with tempfile.TemporaryDirectory() as directory, tempfile.TemporaryFile() as file:
            file.write(payload)
            file.seek(0)
            reader, writer = os.pipe()
            try:
                os.write(writer, payload)
                os.close(writer)
                writer = None
                files = read_key_descriptors({'disc': file.fileno(), 'common': reader})
                self.assertEqual(files, {'disc': payload, 'common': payload})
                os.fstat(reader)
                os.fstat(file.fileno())
                self.assertEqual(os.listdir(directory), [])
            finally:
                os.close(reader)
                if writer is not None:
                    os.close(writer)

    def test_oversized_file_has_value_free_role_specific_error(self):
        with tempfile.TemporaryFile() as file:
            file.write(b'x' * (1024 * 1024 + 1))
            file.seek(0)
            with self.assertRaisesRegex(ValueError, 'selected disc key file exceeds 1 MiB'):
                read_key_descriptors({'disc': file.fileno()})

    def test_worker_cli_passes_only_memory_content_to_runner(self):
        with tempfile.TemporaryFile() as file:
            file.write(b'01' * 16)
            file.seek(0)
            with mock.patch('ondevice_setup.run') as run:
                main(['--job', '/synthetic/job.json', '--disc-key-fd', str(file.fileno())])
                self.assertEqual(run.call_args.kwargs['key_files'], {'disc': b'01' * 16})


@unittest.skipUnless(os.environ.get('WWHD_KEY_TEST_EXTRACTOR') and os.environ.get('WWHD_KEY_TEST_FIXTURES'),
                     'set paths for real native-bridge checks over the authored encrypted disc')
class NativeKeySelection(unittest.TestCase):
    def test_candidates_through_production_adapter_and_process_bridge(self):
        from pathlib import Path
        import subprocess
        from setup_adapter import Adapter
        root = Path(os.environ['WWHD_KEY_TEST_FIXTURES'])
        disc = (root / 'disc.key').read_bytes()
        common = bytes.fromhex((root / 'common_hex.txt').read_text().strip())
        entries = [bytes(16), common, disc]
        events = []

        def native(command, input=None, env=None, cancel=None, separate=False, on_output=None):
            result = subprocess.run(command, input=input, capture_output=True, env=env)
            if on_output:
                on_output('stdout', result.stdout)
                on_output('stderr', result.stderr)
            return result.returncode, result.stdout, result.stderr

        with tempfile.TemporaryDirectory() as directory:
            adapter = Adapter(Path(__file__).resolve().parents[2], Path(directory) / 'checkpoints',
                              'authored-fixture', events.append, lambda: False)
            # The fixture has authored file bytes, rather than a retail RPX checksum.
            with mock.patch.object(adapter.setup, 'check_game_version'):
                game = adapter.extract(root / 'disc.wux', 'image', os.environ['WWHD_KEY_TEST_EXTRACTOR'],
                                       'authored-extractor', native, {'common_key_file': 'keys.txt',
                                       '_entries': {'keys.txt': entries}})
            self.assertTrue((game / 'code/cking.rpx').is_file())
            notices = [event['detail'] for event in events if event.get('state') == 'keys_found']
            self.assertEqual(notices, ['Disc key: found in keys.txt (entry 3)',
                                      'Common key: found in keys.txt (entry 2)'])
            for path in Path(directory).rglob('*'):
                if path.suffix in ('.json', '.log'):
                    self.assertNotIn(disc.hex(), path.read_text())
                    self.assertNotIn(common.hex(), path.read_text())


if __name__ == '__main__':
    unittest.main()
