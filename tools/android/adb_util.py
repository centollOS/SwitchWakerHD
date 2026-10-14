"""Bounded adb transport retries for emulator smoke tooling.

Exit 1 remains a command result when check=False. Only transport errors are
retried; application assertions and ordinary remote command failures are not.
"""
import shlex
import re
import subprocess
import sys
import time
import uuid

TRANSIENT = (b'error: closed', b'device offline', b'no devices/emulators found',
             b'protocol fault', b'unable to connect to adb daemon',
             b'cannot connect to daemon', b'device not found',
             b'connection reset by peer', b'failed to read response from daemon')


class AdbError(subprocess.CalledProcessError):
    def __init__(self, command, result, attempts):
        super().__init__(result.returncode, command, result.stdout, result.stderr)
        self.attempts = attempts

    def __str__(self):
        stderr = self.stderr or b''
        if isinstance(stderr, bytes): stderr = stderr.decode(errors='replace')
        return (f'adb failed after {self.attempts} attempt(s): {shlex.join(self.cmd)}; '
                f'rc={self.returncode}; stderr={stderr.strip()}')


class AdbTransportError(AdbError):
    """All attempts ended in a transient transport failure or timeout."""


def _binary_command(command, has_input):
    """Frame raw cat output because exec-out itself drops remote exit status."""
    command = list(command)
    index = next((i for i, arg in enumerate(command) if arg in ('shell', 'exec-out')), None)
    if index is None: return command, None
    remote = command[index + 1:]
    if remote and remote[0] == '-T': remote = remote[1:]
    raw_remote = remote
    if len(remote) == 1:
        try: remote = shlex.split(remote[0])
        except ValueError: return command, None
    plain_cat = (len(remote) == 2 and remote[0] == 'cat') or (
        len(remote) == 4 and remote[0] == 'run-as' and remote[2] == 'cat')
    if plain_cat and not has_input:
        marker = ('\nWWHD_ADB_' + uuid.uuid4().hex + ':').encode()
        # Run the envelope outside run-as, so its failure is framed too.
        script = ('exec 3>&1; err=$(' + shlex.join(remote) + ' 2>&1 1>&3); rc=$?; printf '
                  + shlex.quote(marker.decode() + '%s\n') + " \"$rc\"; printf '%s' \"$err\"")
        return command[:index] + ['exec-out', shlex.join(['sh', '-c', script])], marker
    if has_input and command[index] == 'shell':
        # shell v2 with no PTY preserves binary stdin AND remote exit status.
        return command[:index] + ['shell', '-T'] + raw_remote, None
    return command, None


def _unframe(result, marker):
    if marker is None or result.returncode: return result
    payload, found, status = result.stdout.rpartition(marker)
    status, separator, stderr = status.partition(b'\n')
    if not found or not separator or not status.isdigit():
        return subprocess.CompletedProcess(result.args, 255, b'', b'protocol fault: incomplete exec-out read')
    rc = int(status)
    return subprocess.CompletedProcess(result.args, rc, payload if rc == 0 else b'',
                                       stderr + result.stderr)


def run(command, *, attempts=4, backoff=.5, check=False, **kwargs):
    """Run a finite adb command, buffering output until an attempt completes.

    shell -T uses adb's non-PTY shell protocol for binary stdin, including
    run-as sh -c 'cat > path'. All attempts receive the same input bytes.
    No stdin or stdout contents are included in retry diagnostics.
    check=False accepts status 1 for optional metadata/probes; unexpected status
    codes still raise with diagnostics. No ordinary remote error is retried.
    """
    if attempts < 1: raise ValueError('attempts must be positive')
    original_command = list(command)
    command, marker = _binary_command(command, kwargs.get('input') is not None)
    text_mode = kwargs.pop('text', False) or kwargs.pop('universal_newlines', False)
    encoding = kwargs.pop('encoding', None)
    errors = kwargs.pop('errors', None)
    text_mode = text_mode or encoding is not None or errors is not None
    if isinstance(kwargs.get('input'), str):
        kwargs['input'] = kwargs['input'].encode(encoding or 'utf-8', errors or 'strict')
    destination = kwargs.pop('stdout', None)
    capture = kwargs.pop('capture_output', False)
    if capture and (destination is not None or 'stderr' in kwargs):
        raise ValueError('capture_output cannot be combined with stdout/stderr')
    kwargs['stdout'] = subprocess.PIPE
    kwargs['stderr'] = subprocess.PIPE
    kwargs.setdefault('timeout', 30)
    for attempt in range(1, attempts + 1):
        try:
            result = subprocess.run(command, check=False, **kwargs)
            result = _unframe(result, marker)
            stderr = result.stderr or b''
            if isinstance(stderr, str): stderr = stderr.encode()
            transient = result.returncode != 0 and (
                result.returncode == 255 or any(pattern in stderr.lower() for pattern in TRANSIENT)
                or (b'device ' in stderr.lower() and b' not found' in stderr.lower()))
        except subprocess.TimeoutExpired as error:
            result = subprocess.CompletedProcess(command, 124, error.output,
                                                 b'TimeoutExpired')
            transient = True
        if not transient:
            if check and result.returncode: raise AdbError(original_command, result, attempt)
            if result.returncode not in (0, 1): raise AdbError(original_command, result, attempt)
            if text_mode:
                result.stdout = result.stdout.decode(encoding or 'utf-8', errors or 'strict').replace('\r\n', '\n').replace('\r', '\n')
                result.stderr = result.stderr.decode(encoding or 'utf-8', errors or 'strict').replace('\r\n', '\n').replace('\r', '\n')
            if destination not in (None, subprocess.PIPE, subprocess.DEVNULL):
                destination.write(result.stdout)
            return result
        if attempt == attempts:
            failure = AdbTransportError(original_command, result, attempt)
            print(str(failure), file=sys.stderr, flush=True)
            raise failure
        print(f'adb transport retry {attempt}/{attempts}: {shlex.join(original_command)}; '
              f'rc={result.returncode}; stderr={str(result.stderr).strip()}', file=sys.stderr, flush=True)
        time.sleep(backoff * 2 ** (attempt - 1))


def check_output(command, **kwargs):
    return run(command, check=True, **kwargs).stdout


def wait_until_ready(prefix, *, timeout=180, interval=1, previous_boot_id=None):
    """Wait for framework boot, package manager and user-0 shared storage.

    boot_completed alone can precede external_primary being mounted. Each
    read-only readiness probe is bounded by the remaining overall deadline;
    ordinary smoke commands keep their existing exit-code semantics.
    """
    deadline = time.monotonic() + timeout
    probes = (
        ('boot', ['getprop', 'sys.boot_completed'], lambda r: r.stdout.strip() == b'1'),
        ('package_manager', ['cmd', 'package', 'path', 'android'],
         lambda r: r.stdout.startswith(b'package:')),
        ('shared_volume', ['sm', 'list-volumes'],
         lambda r: re.search(rb'^emulated(?:;0)? mounted(?: |$)', r.stdout, re.MULTILINE) is not None),
        # Query no rows: confirm MediaProvider registered external_primary
        # without inspecting any user's media. content can report errors at rc 0.
        ('media_volume', ['content', 'query', '--uri', 'content://media/external_primary/file',
                          '--projection', '_id', '--where', '0'],
         lambda r: r.stdout.strip() == b'No result found.' and not r.stderr.strip()),
        ('sdcard', ['sh', '-c', 'test -d /sdcard && test -r /sdcard && test -w /sdcard && ls /sdcard >/dev/null'],
         lambda r: True),
    )
    if previous_boot_id is not None:
        probes = probes[:1] + (
            ('new_boot', ['cat', '/proc/sys/kernel/random/boot_id'],
             lambda r: bool(r.stdout.strip()) and r.stdout.strip() != previous_boot_id),
        ) + probes[1:]
    last = 'no probe completed'
    previous = None
    while time.monotonic() < deadline:
        for label, command, ready in probes:
            remaining = deadline - time.monotonic()
            if remaining <= 0: break
            try:
                result = run(list(prefix) + ['shell', shlex.join(command)],
                             attempts=1, check=False, timeout=min(5, remaining))
                if result.returncode == 0 and ready(result): continue
                last = (f'{label}: command={shlex.join(command)}; rc={result.returncode}; '
                        f'stdout={result.stdout!r}; stderr={result.stderr!r}')
            except AdbError as error:
                last = f'{label}: {error}'
            break
        else:
            print('Emulator ready: boot completed, package manager available, shared storage mounted',
                  file=sys.stderr, flush=True)
            return
        if last != previous:
            print('Waiting for emulator readiness: ' + last, file=sys.stderr, flush=True)
            previous = last
        remaining = deadline - time.monotonic()
        if remaining > 0: time.sleep(min(interval, remaining))
    raise TimeoutError(f'Emulator readiness timed out after {timeout}s; last probe={last}')


def reboot_and_wait(prefix, *, previous_boot_id, timeout=180):
    """Request one framework reboot, then verify its effect and storage readiness.

    A successful reboot may disconnect adb before the request returns. Never
    replay that request: a changed, nonempty kernel boot ID is still mandatory,
    so a transport failure that prevented the reboot cannot turn into success.
    """
    started = time.monotonic()
    try:
        run(list(prefix) + ['shell', 'svc power reboot'], attempts=1, check=True,
            timeout=min(30, timeout))
    except AdbTransportError as error:
        print('Reboot request disconnected adb; verifying changed kernel boot: ' + str(error),
              file=sys.stderr, flush=True)
    wait_until_ready(prefix, previous_boot_id=previous_boot_id,
                     timeout=max(0, timeout - (time.monotonic() - started)))


def open_stream(command, **kwargs):
    """Open a stateful uinput stream; never replay partially delivered events.

    The caller owns its lifetime and checks registration/command effects. Unlike
    finite commands, a failed live input session must fail the smoke test.
    """
    command, _ = _binary_command(command, True)
    return subprocess.Popen(command, **kwargs)
