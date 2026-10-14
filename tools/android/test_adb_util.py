"""Transport faults must recover without turning remote failures into success."""
import io
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from unittest.mock import patch

import adb_util


class AdbTests(unittest.TestCase):
    def result(self, rc=0, stderr=b'', stdout=b'ok'):
        return subprocess.CompletedProcess(['adb'], rc, stdout, stderr)

    def test_transient_errors_retry_and_discard_partial_output(self):
        faults = [self.result(1, message, b'partial') for message in adb_util.TRANSIENT]
        faults += [self.result(255), subprocess.TimeoutExpired(['adb'], 1)]
        for fault in faults:
            with self.subTest(fault=fault), patch('adb_util.subprocess.run', side_effect=[fault, self.result()]) as run, patch('adb_util.time.sleep') as sleep, redirect_stderr(io.StringIO()):
                self.assertEqual(adb_util.run(['adb', 'get-state'], check=True).stdout, b'ok')
                self.assertEqual(run.call_count, 2)
                sleep.assert_called_once_with(.5)

    def test_exit_one_is_optional_or_fatal_without_retry(self):
        for check in (False, True):
            with patch('adb_util.subprocess.run', return_value=self.result(1, b'No such file')) as run:
                if check:
                    with self.assertRaises(adb_util.AdbError) as caught:
                        adb_util.run(['adb', 'shell', 'test -e missing'], check=True)
                    self.assertIn('after 1 attempt(s)', str(caught.exception))
                else:
                    self.assertEqual(adb_util.run(['adb', 'shell', 'test -e missing']).returncode, 1)
                self.assertEqual(run.call_count, 1)

    def test_final_failure_has_diagnostics_and_bounded_backoff(self):
        with patch('adb_util.subprocess.run', return_value=self.result(255, b'error: closed')) as run, patch('adb_util.time.sleep') as sleep, redirect_stderr(io.StringIO()):
            with self.assertRaises(adb_util.AdbError) as caught:
                adb_util.run(['adb', '-s', 'emulator-test', 'get-state'])
            self.assertEqual(run.call_count, 4)
            self.assertEqual([c.args[0] for c in sleep.call_args_list], [.5, 1, 2])
            for text in ('4 attempt(s)', 'rc=255', 'error: closed', 'emulator-test'):
                self.assertIn(text, str(caught.exception))

    def test_raw_read_frame_preserves_binary_and_remote_failure(self):
        payload = bytes(range(256)) * 4096 + b'\n'
        def fake(command, **kwargs):
            self.assertIn('exec-out', command)
            script = shlex.split(command[-1])[-1]
            marker = script.split("printf '", 1)[1].split('%s', 1)[0].encode()
            return subprocess.CompletedProcess(command, 0, payload + marker + b'0\n', b'')
        with patch('adb_util.subprocess.run', side_effect=fake):
            self.assertEqual(adb_util.check_output(['adb', 'shell', "run-as pkg cat 'path with space'"]), payload)
        def failed(command, **kwargs):
            result = fake(command, **kwargs)
            result.stdout = result.stdout[:-2] + b'1\n'
            return result
        with patch('adb_util.subprocess.run', side_effect=failed):
            result = adb_util.run(['adb', 'exec-out', 'cat', 'absent'])
            self.assertEqual((result.returncode, result.stdout, result.stderr), (1, b'', b''))
            with self.assertRaises(adb_util.AdbError):
                adb_util.check_output(['adb', 'exec-out', 'cat', 'absent'])

    def test_incomplete_frame_is_retried(self):
        with patch('adb_util.subprocess.run', return_value=self.result(stdout=b'partial')), patch('adb_util.time.sleep'), redirect_stderr(io.StringIO()):
            with self.assertRaises(adb_util.AdbError) as caught:
                adb_util.check_output(['adb', 'shell', 'cat', 'file'])
            self.assertIn('incomplete exec-out read', str(caught.exception))

    def test_retry_input_and_destination_are_binary_safe(self):
        data = bytes(range(256))
        destination = io.BytesIO()
        with patch('adb_util.subprocess.run', side_effect=[self.result(255, stdout=b'partial'), self.result(stdout=data)]) as run, patch('adb_util.time.sleep'), redirect_stderr(io.StringIO()):
            adb_util.run(['adb', 'shell', "run-as pkg sh -c 'cat > file'"], input=data, stdout=destination, check=True)
            self.assertEqual(destination.getvalue(), data)
            for call in run.call_args_list:
                self.assertEqual(call.kwargs['input'], data)
                self.assertEqual(call.args[0][1:3], ['shell', '-T'])

    def test_text_and_timeout_final_failure(self):
        with patch('adb_util.subprocess.run', return_value=self.result(stdout=b'hello\r\n')):
            self.assertEqual(adb_util.check_output(['adb', 'get-state'], text=True), 'hello\n')
        with patch('adb_util.subprocess.run', side_effect=subprocess.TimeoutExpired(['adb'], 1)), patch('adb_util.time.sleep'), redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(adb_util.AdbError, 'TimeoutExpired'):
                adb_util.run(['adb', 'get-state'], attempts=2)

    def test_executable_fault_injection_and_real_missing_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            fake = root / 'adb'
            counter = root / 'calls'
            fake.write_text('#!' + sys.executable + '\n' + '''import os, pathlib, sys, time
p = pathlib.Path(os.environ['ADB_TEST_COUNTER'])
n = int(p.read_text()) if p.exists() else 0
p.write_text(str(n + 1))
if n < int(os.environ['ADB_TEST_FAILURES']):
    print('error: closed', file=sys.stderr)
    sys.exit(255)
os.execvp('sh', ['sh', '-c', sys.argv[-1]])
''')
            fake.chmod(0o700)
            env = {**os.environ, 'ADB_TEST_COUNTER': str(counter), 'ADB_TEST_FAILURES': '2'}
            with redirect_stderr(io.StringIO()):
                self.assertEqual(adb_util.check_output([str(fake), 'shell', 'printf recovered'], env=env, backoff=0), b'recovered')
            self.assertEqual(counter.read_text(), '3')
            with self.assertRaises(adb_util.AdbError):
                adb_util.check_output([str(fake), 'shell', 'cat', str(root / 'missing')], env=env, backoff=0)
            self.assertEqual(counter.read_text(), '4')
            payload = bytes(range(256)) * 4096
            binary = root / 'binary with spaces'
            counter.write_text('0')
            with redirect_stderr(io.StringIO()):
                adb_util.run([str(fake), 'shell', '-T', 'cat > ' + shlex.quote(str(binary))],
                             input=payload, check=True, env=env, backoff=0)
            self.assertEqual(counter.read_text(), '3')
            counter.write_text('0')
            with redirect_stderr(io.StringIO()):
                self.assertEqual(adb_util.check_output([str(fake), 'exec-out', 'cat', str(binary)],
                                                      env=env, backoff=0), payload)
            self.assertEqual(counter.read_text(), '3')

    def test_failed_read_never_includes_partial_payload_in_diagnostics(self):
        def failed(command, **kwargs):
            script = shlex.split(command[-1])[-1]
            marker = script.split("printf '", 1)[1].split('%s', 1)[0].encode()
            return subprocess.CompletedProcess(command, 0, b'private payload' + marker + b'1\nread failed', b'')
        with patch('adb_util.subprocess.run', side_effect=failed):
            with self.assertRaises(adb_util.AdbError) as caught:
                adb_util.check_output(['adb', 'shell', 'cat', 'fixture'])
            self.assertIn('read failed', str(caught.exception))
            self.assertNotIn('private payload', str(caught.exception))


class ReadinessTests(unittest.TestCase):
    def test_reboot_disconnect_is_not_replayed_and_requires_changed_boot(self):
        prefix = ['adb', '-s', 'emulator-test']
        failure = adb_util.AdbTransportError(prefix, subprocess.CompletedProcess(prefix, 255, b'', b'error: closed'), 1)
        with patch('adb_util.run', side_effect=failure) as run, patch('adb_util.wait_until_ready') as ready, patch('adb_util.time.monotonic', side_effect=[10, 12]), redirect_stderr(io.StringIO()):
            adb_util.reboot_and_wait(prefix, previous_boot_id=b'old-boot')
        run.assert_called_once_with(prefix + ['shell', 'svc power reboot'], attempts=1, check=True, timeout=30)
        ready.assert_called_once_with(prefix, previous_boot_id=b'old-boot', timeout=178)

    def test_reboot_does_not_hide_an_ordinary_command_error(self):
        failure = adb_util.AdbError(['adb'], subprocess.CompletedProcess(['adb'], 1, b'', b'Permission denied'), 1)
        with patch('adb_util.run', side_effect=failure), patch('adb_util.wait_until_ready') as ready:
            with self.assertRaises(adb_util.AdbError) as caught:
                adb_util.reboot_and_wait(['adb'], previous_boot_id=b'old-boot')
        self.assertIs(caught.exception, failure)
        ready.assert_not_called()

    def test_transport_loss_without_reboot_still_fails(self):
        clock = [0]
        commands = []
        def probe(command, **kwargs):
            commands.append(command[-1])
            if command[-1] == 'svc power reboot':
                raise adb_util.AdbTransportError(command, subprocess.CompletedProcess(command, 255, b'', b'error: closed'), 1)
            out = b'1' if command[-1] == 'getprop sys.boot_completed' else b'old-boot'
            return subprocess.CompletedProcess(command, 0, out, b'')
        def sleep(seconds): clock[0] += seconds
        with patch('adb_util.run', side_effect=probe), patch('adb_util.time.monotonic', side_effect=lambda: clock[0]), patch('adb_util.time.sleep', side_effect=sleep), redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(TimeoutError, 'new_boot:.*old-boot'):
                adb_util.reboot_and_wait(['adb'], previous_boot_id=b'old-boot', timeout=2)
        self.assertEqual(commands.count('svc power reboot'), 1)

    def test_reboot_requires_a_nonempty_changed_kernel_boot_id(self):
        clock = [0]
        offline = adb_util.AdbError(['adb'], subprocess.CompletedProcess(['adb'], 255, b'', b'device offline'), 1)
        ids = iter((offline, b'', b'old-boot\n', b'new-boot\n'))
        def probe(command, **kwargs):
            remote = command[-1]
            if remote == 'cat /proc/sys/kernel/random/boot_id':
                out = next(ids)
                if isinstance(out, Exception): raise out
            elif remote == 'getprop sys.boot_completed': out = b'1'
            elif remote == 'cmd package path android': out = b'package:android'
            elif remote == 'sm list-volumes': out = b'emulated;0 mounted null'
            elif remote.startswith('content query '): out = b'No result found.\n'
            else: out = b''
            return subprocess.CompletedProcess(command, 0, out, b'')
        def sleep(seconds): clock[0] += seconds
        with patch('adb_util.run', side_effect=probe), patch('adb_util.time.monotonic', side_effect=lambda: clock[0]), patch('adb_util.time.sleep', side_effect=sleep), redirect_stderr(io.StringIO()):
            adb_util.wait_until_ready(['adb', '-s', 'emulator-test'], timeout=5, previous_boot_id=b'old-boot')
        self.assertEqual(clock[0], 3)

    def test_waits_for_every_readiness_signal(self):
        clock = [0]
        rounds = [0]
        calls = []
        def probe(command, **kwargs):
            self.assertEqual(command[:3], ['adb', '-s', 'emulator-test'])
            self.assertEqual(kwargs['attempts'], 1)
            self.assertLessEqual(kwargs['timeout'], 5)
            remote = command[-1]
            calls.append(remote)
            rc, out = 0, b''
            if remote == 'getprop sys.boot_completed':
                rounds[0] += 1
                out = b'1' if rounds[0] > 1 else b'0'
            elif remote == 'cmd package path android':
                rc = 1 if rounds[0] == 2 else 0
                out = b'package:/system/framework/framework-res.apk'
            elif remote == 'sm list-volumes':
                out = b'emulated;0 checking null' if rounds[0] == 3 else b'emulated;0 mounted null'
            elif remote.startswith('content query '):
                out = b'Error while accessing provider: Volume external_primary not found' if rounds[0] == 4 else b'No result found.\n'
            else:
                self.assertIn('ls /sdcard', remote)
                rc = 1 if rounds[0] == 5 else 0
            return subprocess.CompletedProcess(command, rc, out, b'')
        def sleep(seconds): clock[0] += seconds
        with patch('adb_util.run', side_effect=probe), patch('adb_util.time.monotonic', side_effect=lambda: clock[0]), patch('adb_util.time.sleep', side_effect=sleep), redirect_stderr(io.StringIO()) as log:
            adb_util.wait_until_ready(['adb', '-s', 'emulator-test'], timeout=10)
        self.assertEqual(rounds[0], 6)
        self.assertEqual(clock[0], 5)
        for name in ('boot:', 'package_manager:', 'shared_volume:', 'media_volume:', 'sdcard:', 'Emulator ready:'):
            self.assertIn(name, log.getvalue())

    def test_offline_device_stays_bounded_and_reports_last_probe(self):
        clock = [0]
        def probe(command, **kwargs):
            self.assertLessEqual(kwargs['timeout'], 3 - clock[0])
            raise adb_util.AdbError(command, subprocess.CompletedProcess(command, 255, b'', b'device offline'), 1)
        def sleep(seconds): clock[0] += seconds
        with patch('adb_util.run', side_effect=probe) as run, patch('adb_util.time.monotonic', side_effect=lambda: clock[0]), patch('adb_util.time.sleep', side_effect=sleep), redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(TimeoutError, 'boot:.*rc=255.*device offline'):
                adb_util.wait_until_ready(['adb', '-s', 'emulator-test'], timeout=3)
        self.assertEqual(clock[0], 3)
        self.assertEqual(run.call_count, 3)

    def test_missing_volume_fails_instead_of_starting_storage_work(self):
        clock = [0]
        def probe(command, **kwargs):
            remote = command[-1]
            out = {'getprop sys.boot_completed': b'1', 'cmd package path android': b'package:android',
                   'sm list-volumes': b'private mounted null\nemulated;0 unmounted null'}[remote]
            return subprocess.CompletedProcess(command, 0, out, b'')
        def sleep(seconds): clock[0] += seconds
        with patch('adb_util.run', side_effect=probe), patch('adb_util.time.monotonic', side_effect=lambda: clock[0]), patch('adb_util.time.sleep', side_effect=sleep), redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(TimeoutError, 'shared_volume:.*unmounted'):
                adb_util.wait_until_ready(['adb', '-s', 'emulator-test'], timeout=2)


if __name__ == '__main__':
    unittest.main()
