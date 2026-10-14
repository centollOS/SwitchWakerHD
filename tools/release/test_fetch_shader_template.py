"""Private-template CI behavior using synthetic payloads and mocked git only."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import fetch_shader_template as fetcher
from test_guard_template import encode, fixture


class FetchTemplateTests(unittest.TestCase):
    def test_upstream_release_events_require_secret_but_forks_and_prs_skip(self):
        upstream = 'ZeldaWWHDRecomp/ZeldaWWHDRecomp'
        cases = [
            (upstream, 'push', 'refs/tags/v1.0', '', True),
            (upstream, 'schedule', 'refs/heads/devel', '', True),
            (upstream, 'workflow_dispatch', 'refs/heads/devel', 'true', True),
            (upstream, 'workflow_dispatch', 'refs/heads/devel', 'false', False),
            (upstream, 'pull_request', 'refs/pull/1/merge', 'true', False),
            ('someone/fork', 'push', 'refs/tags/v1.0', '', False),
            ('someone/fork', 'schedule', 'refs/heads/devel', '', False),
        ]
        for repository, event, ref, devel, required in cases:
            with self.subTest(repository=repository, event=event, devel=devel), tempfile.TemporaryDirectory() as tmp:
                env = dict(GITHUB_REPOSITORY=repository, GITHUB_EVENT_NAME=event,
                           GITHUB_REF=ref, SHADER_TEMPLATE_DEVEL_BUILD=devel)
                with patch.object(fetcher, 'ROOT', Path(tmp)), patch.dict(os.environ, env, clear=True), patch.object(subprocess, 'run') as run:
                    if required:
                        with self.assertRaisesRegex(RuntimeError, 'required'):
                            fetcher.fetch()
                    else:
                        fetcher.fetch()
                    run.assert_not_called()

    def test_fork_or_pr_never_fetches_even_if_a_key_is_present(self):
        for repository, event in [('someone/fork', 'schedule'),
                                  ('ZeldaWWHDRecomp/ZeldaWWHDRecomp', 'pull_request'),
                                  ('ZeldaWWHDRecomp/ZeldaWWHDRecomp', 'pull_request_target')]:
            with self.subTest(repository=repository, event=event), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                target = root / 'tools/shadercache/template.bin'
                target.parent.mkdir(parents=True)
                target.write_bytes(b'synthetic stale payload')
                env = dict(GITHUB_REPOSITORY=repository, GITHUB_EVENT_NAME=event,
                           SHADER_TEMPLATE_DEPLOY_KEY='synthetic test key')
                with patch.object(fetcher, 'ROOT', root), patch.dict(os.environ, env, clear=True), patch.object(subprocess, 'run') as run:
                    fetcher.fetch()
                    run.assert_not_called()
                self.assertFalse(target.exists())
                self.assertFalse((root / 'shader-template-deploy-key').exists())

    def test_no_secret_skips_network_and_removes_stale_payload(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            target = root / 'tools/shadercache/template.bin'
            target.parent.mkdir(parents=True)
            target.write_bytes(b'synthetic stale payload')
            with patch.object(fetcher, 'ROOT', root), patch.dict(os.environ, {}, clear=True), patch.object(subprocess, 'run') as run:
                fetcher.fetch()
                run.assert_not_called()
            self.assertFalse(target.exists())

    def test_git_failure_and_guard_rejection_never_publish_and_remove_key(self):
        for failure in ('fetch', 'guard'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                folder = root / 'tools/shadercache'
                folder.mkdir(parents=True)
                commit = 'a' * 40
                # Correct hash alone must never bypass the code-free validator.
                data = b'synthetic invalid template'
                (folder / 'template.ref').write_text(json.dumps(dict(commit=commit,
                    sha256=hashlib.sha256(data).hexdigest())))
                def git(command, **kwargs):
                    if failure == 'fetch' and 'fetch' in command:
                        raise subprocess.CalledProcessError(128, command)
                    output = (commit + '\n').encode() if 'rev-parse' in command else data if 'show' in command else b''
                    return subprocess.CompletedProcess(command, 0, stdout=output)
                with patch.object(fetcher, 'ROOT', root), patch.dict(os.environ,
                    {'RUNNER_TEMP': tmp, 'SHADER_TEMPLATE_DEPLOY_KEY': 'synthetic test key'}, clear=True), patch.object(subprocess, 'run', side_effect=git):
                    with self.assertRaises(subprocess.CalledProcessError if failure == 'fetch' else ValueError):
                        fetcher.fetch()
                self.assertFalse((folder / 'template.bin').exists())
                self.assertFalse((root / 'shader-template-deploy-key').exists())
                self.assertFalse((root / 'shader-template-known-hosts').exists())

    def test_verified_payload_and_failed_hash_both_remove_key(self):
        for valid in (True, False):
            with self.subTest(valid=valid), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                folder = root / 'tools/shadercache'
                folder.mkdir(parents=True)
                data = encode(fixture())
                commit = 'a' * 40
                (folder / 'template.ref').write_text(json.dumps(dict(commit=commit,
                    sha256=hashlib.sha256(data).hexdigest() if valid else 'b' * 64)))
                def git(command, **kwargs):
                    self.assertNotIn('SHADER_TEMPLATE_DEPLOY_KEY', kwargs['env'])
                    self.assertIn('IdentitiesOnly=yes', kwargs['env']['GIT_SSH_COMMAND'])
                    self.assertIn('StrictHostKeyChecking=yes', kwargs['env']['GIT_SSH_COMMAND'])
                    hosts = root / 'shader-template-known-hosts'
                    self.assertEqual(hosts.read_text(), fetcher.GITHUB_HOST_KEY)
                    self.assertIn('UserKnownHostsFile=' + hosts.as_posix(), kwargs['env']['GIT_SSH_COMMAND'])
                    key = root / 'shader-template-deploy-key'
                    self.assertTrue(key.is_file())
                    # OpenSSH rejects a key with CRLF line endings ("invalid format"), which text-mode
                    # writes produced on Windows: both files must be LF only, even for a CRLF secret
                    self.assertEqual(key.read_bytes(), b'-----BEGIN SYNTHETIC KEY-----\nline\n-----END SYNTHETIC KEY-----\n')
                    self.assertNotIn(b'\r', hosts.read_bytes())
                    if os.name != 'nt':
                        self.assertEqual(key.stat().st_mode & 0o777, 0o600)
                    output = (commit + '\n').encode() if 'rev-parse' in command else data if 'show' in command else b''
                    return subprocess.CompletedProcess(command, 0, stdout=output)
                with patch.object(fetcher, 'ROOT', root), patch.dict(os.environ,
                    {'RUNNER_TEMP': tmp, 'SHADER_TEMPLATE_DEPLOY_KEY': '-----BEGIN SYNTHETIC KEY-----\r\nline\r\n-----END SYNTHETIC KEY-----\r\n'},
                    clear=True), patch.object(subprocess, 'run', side_effect=git):
                    if valid:
                        fetcher.fetch()
                        self.assertEqual((folder / 'template.bin').read_bytes(), data)
                    else:
                        with self.assertRaisesRegex(ValueError, 'checksum'):
                            fetcher.fetch()
                        self.assertFalse((folder / 'template.bin').exists())
                self.assertFalse((root / 'shader-template-deploy-key').exists())
                self.assertFalse((root / 'shader-template-known-hosts').exists())


if __name__ == '__main__':
    unittest.main()
