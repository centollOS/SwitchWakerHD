#!/usr/bin/env python3
"""Synthetic key selection tests; optional real extractor over extract_test's authored disc.

WWHD_KEY_TEST_EXTRACTOR=... WWHD_KEY_TEST_FIXTURES=... python3 test_setup_keys.py
"""
import os
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest import mock
import setup

DISC = bytes(range(16))
COMMON = bytes(range(16, 32))
OTHER = bytes(range(32, 48))


class KeySelection(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.file = self.root / 'keys.txt'
        self.file.write_text('# Cemu keys\nnot a key\n' + OTHER.hex() + ' # other game\n' +
                             COMMON.hex() + ' common\n# blank\n' + DISC.hex() + '# our disc\n')
        self.calls = []
        self.patch = mock.patch.object(setup, 'find_common_key', return_value=(None, None))
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def info(self, image, keys):
        self.calls.append((keys.disc, keys.common))
        if keys.disc != DISC:
            return 'disc_key_wrong', '', None
        if keys.common != COMMON:
            return 'common_key_wrong', '', None
        return None, '', {'title_id': '0005000010143500'}

    def resolve(self, **req):
        return setup.resolve_disc_keys(str(self.root / 'disc.wud'), req, info_reader=self.info)

    def test_either_selector_supplies_both_and_reports_valid_entry_numbers(self):
        for field in ('disc_key_file', 'common_key_file'):
            keys, info, why, notices = self.resolve(**{field: str(self.file)})
            self.assertIsNone(why)
            self.assertEqual((keys.disc, keys.common), (DISC, COMMON))
            self.assertEqual(notices, ['Disc key: found in keys.txt (entry 3)',
                                       'Common key: found in keys.txt (entry 2)'])
            self.assertNotIn(DISC.hex(), str(notices))
            self.assertNotIn(COMMON.hex(), str(notices))
            self.assertLessEqual(len(self.calls), 6)
            self.calls.clear()

    def test_cli_accepts_one_cemu_file_and_uses_hidden_paste_prompts(self):
        from types import SimpleNamespace
        for field in ('disc_key', 'common_key'):
            args = SimpleNamespace(disc_key=None, common_key=None)
            setattr(args, field, str(self.file))
            with mock.patch.object(setup, 'disc_info', side_effect=self.info), mock.patch.object(setup, 'say') as say:
                keys, _ = setup.get_disc_keys(str(self.root / 'disc.wud'), SimpleNamespace(interactive=False), args)
                self.assertEqual((keys.disc, keys.common), (DISC, COMMON))
                self.assertNotIn(DISC.hex(), str(say.call_args_list))
        ui = mock.Mock(interactive=True)
        ui.choose.return_value = 1
        ui.secret.side_effect = (DISC.hex(), COMMON.hex())
        with mock.patch.object(setup, 'disc_info', side_effect=self.info), mock.patch.object(setup, 'say'):
            keys, _ = setup.get_disc_keys(str(self.root / 'disc.wud'), ui,
                                         SimpleNamespace(disc_key=None, common_key=None))
        self.assertEqual(ui.secret.call_count, 2)
        self.assertEqual((keys.disc, keys.common), (DISC, COMMON))

    def test_paste(self):
        keys, _, why, _ = self.resolve(disc_key_hex=DISC.hex(), common_key_hex=COMMON.hex())
        self.assertIsNone(why)
        self.assertEqual(keys.disc, DISC)

    def test_common_key_entered_as_disc(self):
        _, _, why, _ = self.resolve(disc_key_hex=COMMON.hex(), common_key_hex=COMMON.hex())
        self.assertEqual(why[0], 'disc_key_is_common')
        self.assertEqual(why[1], setup.KEY_MESSAGES['disc_key_is_common'])
        self.assertNotIn(COMMON.hex(), why[1])

    def test_errors_identify_which_key_failed(self):
        for disc, common, expected in ((OTHER, COMMON, 'disc_key_wrong'),
                                      (DISC, OTHER, 'common_key_wrong'),
                                      (OTHER, OTHER, 'disc_key_is_common')):
            self.assertEqual(self.resolve(disc_key_hex=disc.hex(), common_key_hex=common.hex())[2][0], expected)
        self.assertEqual(self.resolve(disc_key_hex=DISC.hex())[2][0], 'common_key_missing')
        self.assertEqual(self.resolve(common_key_hex=COMMON.hex())[2][0], 'disc_key_missing')

    def test_bad_files_and_bad_paste(self):
        for content in ('', '# no keys\ngarbage', 'f' * 33, 'f' * (setup.KEY_FILE_LIMIT + 1)):
            self.file.write_text(content)
            for field, expected in (('disc_key_file', 'disc_key_bad'), ('common_key_file', 'common_key_bad')):
                self.assertEqual(self.resolve(**{field: str(self.file)})[2][0], expected)
        self.assertEqual(self.resolve(disc_key_hex='invalid')[2][0], 'disc_key_bad')
        self.assertEqual(self.resolve(common_key_hex='invalid')[2][0], 'common_key_bad')

    def test_bom_crlf_comments_and_large_cemu_library(self):
        self.file.write_bytes(b'\xef\xbb\xbf# Cemu\r\n' + (OTHER.hex() + ' // comment\r\n').encode() * 130 +
                              (DISC.hex() + '; comment\r\n').encode())
        self.assertEqual(len(setup.read_key_entries(self.file)), 131)
        self.assertEqual(setup.read_key_entries(self.file)[-1], DISC)

    def test_image_errors_are_not_misreported_as_key_errors(self):
        for code in ('image_bad', 'image_damaged'):
            with mock.patch.object(self, 'info', return_value=(code, '', None)):
                self.assertEqual(self.resolve(disc_key_file=str(self.file))[2][0], code)


@unittest.skipUnless(os.environ.get('WWHD_KEY_TEST_EXTRACTOR') and (os.environ.get('WWHD_KEY_TEST_FIXTURES') or
                     os.environ.get('WWHD_KEY_TEST_FIXTURE_BUILDER')),
                     'set extractor and synthetic fixture paths for integration checks')
class SyntheticDisc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixtures = os.environ.get('WWHD_KEY_TEST_FIXTURES')
        if not cls.fixtures:
            temporary = tempfile.TemporaryDirectory()
            cls.addClassCleanup(temporary.cleanup)
            cls.fixtures = temporary.name + '/disc'
            result = subprocess.run([os.environ['WWHD_KEY_TEST_FIXTURE_BUILDER'],
                                     os.environ['WWHD_KEY_TEST_EXTRACTOR'], cls.fixtures],
                                    env={**os.environ, 'WWHD_KEEP_TEST': '1'}, capture_output=True)
            if result.returncode:
                raise RuntimeError('Synthetic extractor fixture failed')

    def test_resolve_against_real_partition_checks(self):
        root = Path(self.fixtures)
        disc = (root / 'disc.key').read_bytes()
        common = bytes.fromhex((root / 'common_hex.txt').read_text().strip())
        with tempfile.TemporaryDirectory() as directory:
            keys_file = Path(directory) / 'keys.txt'
            keys_file.write_text('# synthetic Cemu list\njunk\n' + OTHER.hex() + ' # wrong\n' +
                                 common.hex() + ' # common\n' + disc.hex() + ' # disc\n')
            with mock.patch.object(setup, 'extractor', return_value=os.environ['WWHD_KEY_TEST_EXTRACTOR']), \
                    mock.patch.object(setup, 'find_common_key', return_value=(None, None)):
                for image in ('disc.wud', 'disc.wux'):
                    for field in ('disc_key_file', 'common_key_file'):
                        keys, info, why, notices = setup.resolve_disc_keys(str(root / image), {field: str(keys_file)})
                        self.assertIsNone(why)
                        self.assertEqual((keys.disc, keys.common), (disc, common))
                        self.assertIn('Disc key: found in keys.txt (entry 3)', notices)
                    self.assertIsNone(setup.resolve_disc_keys(str(root / image), {
                        'disc_key_hex': disc.hex(), 'common_key_hex': common.hex()})[2])
                    self.assertEqual(setup.resolve_disc_keys(str(root / image), {
                        'disc_key_hex': common.hex(), 'common_key_hex': common.hex()})[2][0], 'disc_key_is_common')
                    self.assertEqual(setup.resolve_disc_keys(str(root / image), {
                        'disc_key_hex': disc.hex(), 'common_key_hex': OTHER.hex()})[2][0], 'common_key_wrong')


if __name__ == '__main__':
    unittest.main()
