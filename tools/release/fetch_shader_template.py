"""Fetch only the pinned private template; no-secret CI skips without network access."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
REPOSITORY = 'git@github.com:ZeldaWWHDRecomp/shader-template.git'
GITHUB_HOST_KEY = 'github.com ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl\n'

def template_required(env):
    if env.get('GITHUB_REPOSITORY') != 'ZeldaWWHDRecomp/ZeldaWWHDRecomp':
        return False
    event = env.get('GITHUB_EVENT_NAME')
    return (event == 'schedule' or
            event == 'push' and env.get('GITHUB_REF', '').startswith('refs/tags/') or
            event == 'workflow_dispatch' and env.get('SHADER_TEMPLATE_DEVEL_BUILD', '').lower() == 'true')


def read_pin(path):
    pin = json.loads(Path(path).read_text())
    if (set(pin) != {'commit', 'sha256'} or
            not re.fullmatch('[0-9a-f]{40}', pin['commit']) or
            not re.fullmatch('[0-9a-f]{64}', pin['sha256'])):
        raise ValueError('Invalid shader template pin')
    return pin


def fetch():
    key = os.environ.get('SHADER_TEMPLATE_DEPLOY_KEY', '')
    destination = ROOT / 'tools/shadercache/template.bin'
    repository = os.environ.get('GITHUB_REPOSITORY')
    event = os.environ.get('GITHUB_EVENT_NAME', '')
    if (repository and repository != 'ZeldaWWHDRecomp/ZeldaWWHDRecomp' or
            event in ('pull_request', 'pull_request_target')):
        destination.unlink(missing_ok=True)
        print('::notice::Fork/PR build; packaging without optional shader preparation')
        return
    if not key:
        # A reused checkout must not accidentally package an unverified local file.
        destination.unlink(missing_ok=True)
        if template_required(os.environ):
            raise RuntimeError('SHADER_TEMPLATE_DEPLOY_KEY is required for upstream release/devel builds')
        print('::notice::No shader template deploy key; packaging without optional shader preparation')
        return
    pin = read_pin(ROOT / 'tools/shadercache/template.ref')
    key_path = Path(os.environ['RUNNER_TEMP']) / 'shader-template-deploy-key'
    known_hosts = key_path.with_name('shader-template-known-hosts')
    try:
        known_hosts.write_bytes(GITHUB_HOST_KEY.encode('ascii'))  # binary: no CRLF on Windows
        os.chmod(known_hosts, 0o600)
        descriptor = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        # binary: text mode on Windows writes CRLF, and OpenSSH rejects such a key ("invalid format");
        # a secret pasted with CRLF is normalised too
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write((key.replace('\r\n', '\n').replace('\r', '\n').rstrip() + '\n').encode('ascii'))
        os.chmod(key_path, 0o600)
        env = os.environ.copy()
        env.pop('SHADER_TEMPLATE_DEPLOY_KEY', None)
        env['GIT_TERMINAL_PROMPT'] = '0'
        env['GIT_SSH_COMMAND'] = shlex.join(['ssh', '-i', key_path.as_posix(),
            '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes',
            '-o', 'UserKnownHostsFile=' + known_hosts.as_posix(),
            '-o', 'StrictHostKeyChecking=yes'])
        with tempfile.TemporaryDirectory(dir=os.environ['RUNNER_TEMP']) as temporary:
            def git(*args):
                return subprocess.run(['git', '-C', temporary, *args], env=env,
                    check=True, stdout=subprocess.PIPE).stdout
            git('init', '--quiet')
            git('fetch', '--quiet', '--depth=1', REPOSITORY, pin['commit'])
            if git('rev-parse', 'FETCH_HEAD').decode().strip() != pin['commit']:
                raise ValueError('Shader template commit mismatch')
            data = git('show', 'FETCH_HEAD:template.bin')
            if hashlib.sha256(data).hexdigest() != pin['sha256']:
                raise ValueError('Shader template checksum mismatch')
            import guard
            problems = []
            guard.check_entry('tools/shadercache/template.bin', data, problems)
            if problems:
                raise ValueError('Shader template guard rejected payload: ' + '; '.join(problems))
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
    finally:
        key_path.unlink(missing_ok=True)
        known_hosts.unlink(missing_ok=True)


if __name__ == '__main__':
    fetch()
