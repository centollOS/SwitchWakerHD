#!/usr/bin/env python3
"""Game-free Android handheld input regression test; disposable emulator only.

Install a placeholder-code debug APK first. Registers a Linux uinput Odin pad
through Android's real input stack, not SDL's virtual-controller API. Requires
an emulator image whose shell can use uinput and Python Pillow. No game files are used.
"""
import argparse
import io
from PIL import Image
import json
from pathlib import Path
import re
import shlex
import subprocess
import adb_util
import time

PACKAGE = 'org.wwhdrecomp.wwhd'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prefix = [args.adb, '-s', args.serial]
    adb_util.wait_until_ready(prefix)
    args.output.mkdir(parents=True, exist_ok=True)

    def adb(*command, check=True, data=None):
        if command[0] == 'shell':
            command = ('shell', shlex.join(command[1:]))
        return adb_util.run(prefix + list(command), input=data, check=check,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30).stdout

    if adb('shell', 'getprop', 'ro.kernel.qemu').strip() != b'1':
        parser.error('requires a disposable emulator, never a physical device')
    original_size = adb('shell', 'wm', 'size').decode()
    override = re.search(r'Override size: (\d+x\d+)', original_size)
    settings_path = f'/sdcard/Android/data/{PACKAGE}/files/config/wwhd/settings.ini'
    settings = adb('shell', 'cat', settings_path, check=False)
    prefs = adb('shell', 'run-as', PACKAGE, 'cat', 'shared_prefs/controls.xml', check=False)
    observations = {}
    process = None
    log = (args.output / 'uinput-log.txt').open('wb')

    def metrics():
        return json.loads(adb('shell', 'run-as', PACKAGE, 'cat', 'files/display-smoke.json'))

    def until(predicate, timeout=20):
        deadline = time.monotonic() + timeout
        latest = None
        while time.monotonic() < deadline:
            try:
                latest = metrics()
                if predicate(latest):
                    return latest
            except (ValueError, subprocess.CalledProcessError):
                pass
            time.sleep(.04)
        raise AssertionError('timed out; last metrics=' + repr(latest))

    def start():
        adb('shell', 'am', 'force-stop', PACKAGE)
        adb('shell', 'run-as', PACKAGE, 'rm', '-f', 'files/display-smoke.json')
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/.DisplaySmokeActivity')
        until(lambda m: m['primary_presented'] > 10)

    def command(value):
        script = "printf '%s' " + shlex.quote(value) + ' > files/display-smoke-command.tmp && mv files/display-smoke-command.tmp files/display-smoke-command'
        adb('shell', 'run-as', PACKAGE, 'sh', '-c', script)

    def send(command, **fields):
        process.stdin.write((json.dumps(dict(id=1, command=command, **fields)) + '\n').encode())
        process.stdin.flush()

    def key(code, value):
        send('updateTimeBase')
        send('inject', events=[1, code, value, 0, 0, 0])

    def screenshot(name):
        data = adb('exec-out', 'screencap', '-p')
        (args.output / (name + '.png')).write_bytes(data)
        return Image.open(io.BytesIO(data)).convert('RGB')

    def hint_visible(image):
        # The authored red fixture is behind a blue notice. Sample its empty left
        # margin, away from text; accessibility idle waits outlast the brief hint.
        r, g, b = image.getpixel((int(image.width * .03), int(image.height * .9)))
        return r < 100 and g > r + 5 and b > r + 10

    try:
        adb('shell', 'am', 'force-stop', PACKAGE)
        adb('shell', 'run-as', PACKAGE, 'rm', '-f', 'shared_prefs/controls.xml')
        adb('shell', 'wm', 'size', '1080x1920')
        adb('logcat', '-c')
        start()
        command('touchauto')
        until(lambda m: m['touch_buttons'])  # keyboard/D-pad alone is not a physical gamepad
        process = adb_util.open_stream(prefix + ['shell', 'uinput', '-'], stdin=subprocess.PIPE,
                                   stdout=log, stderr=log)
        registration_started = time.monotonic()
        send('register', name='Odin Controller', vid=0x2020, pid=0x0111, bus='usb',
             configuration=[{'type': 100, 'data': [1, 3]},
                            {'type': 101, 'data': [304, 305, 307, 308, 310, 311, 314, 315, 316]},
                            {'type': 103, 'data': [0, 1, 3, 4, 2, 5]}],
             abs_info=[{'code': axis, 'info': {'value': 0, 'minimum': -32768 if axis in [0, 1, 3, 4] else 0,
                                              'maximum': 32767, 'fuzz': 0, 'flat': 0, 'resolution': 0}}
                       for axis in [0, 1, 3, 4, 2, 5]])
        until(lambda m: not m['touch_buttons'])
        # uinput/Android may take seconds to create and classify a Linux device.
        # Time SDL's refresh from Android's listener-driven physical presence,
        # rather than including that unrelated emulator registration delay.
        android_recognized = time.monotonic()
        deadline = android_recognized + .5
        while b'applied Odin 2020:0111 Nintendo face-position mapping' not in adb('logcat', '-d', '-s', 'wwhd'):
            assert time.monotonic() < deadline, 'native controller enumeration still delayed'
            time.sleep(.04)
        observations['controller_registration_seconds_observed'] = time.monotonic() - registration_started
        observations['android_to_sdl_seconds_observed'] = time.monotonic() - android_recognized
        assert observations['android_to_sdl_seconds_observed'] < .5
        assert process.poll() is None, 'uinput device registration failed'
        assert hint_visible(screenshot('controller-hint')), 'startup hint was not drawn'
        adb('shell', 'input', 'tap', '960', '980')  # dismiss the notice
        assert not hint_visible(screenshot('hint-dismissed'))
        # SDL's Android enumeration can lag Java's listener. Wait for the actual
        # native connection before timing a hold, instead of timing registration.
        deadline = time.monotonic() + 15
        while b'applied Odin 2020:0111 Nintendo face-position mapping' not in adb('logcat', '-d', '-s', 'wwhd'):
            assert time.monotonic() < deadline, 'SDL did not enumerate the uinput pad'
            time.sleep(.1)
        key(314, 1)
        until(lambda m: m['pad_buttons'] & 4)
        time.sleep(.15)
        key(314, 0)
        until(lambda m: not m['pad_buttons'])
        assert not metrics()['settings_open'], 'short Select opened Settings'
        started = time.monotonic()
        key(314, 1)
        until(lambda m: m['settings_open'], 3)
        elapsed = time.monotonic() - started
        assert .45 <= elapsed < 1.2, elapsed  # includes adb + 100 ms metrics publication latency
        observations['select_open_seconds_observed'] = elapsed
        time.sleep(.6)
        assert metrics()['settings_open'], 'held Select toggled the overlay again'
        key(314, 0)
        time.sleep(.2)
        key(314, 1)
        until(lambda m: not m['settings_open'])
        key(314, 0)
        time.sleep(.3)
        for preset in ('faceposition', 'faceauto'):
            command(preset)
            time.sleep(.2)
            assert metrics()['pad_family'] == 3, 'keyboard/D-pad stole Nintendo label detection'
            for code, expected in [(304, 0x8000), (305, 0x4000), (307, 0x2000), (308, 0x1000)]:
                key(code, 1)
                current = until(lambda m: m['pad_buttons'] != 0)
                assert current['pad_buttons'] == expected, (preset, code, current['pad_buttons'])
                key(code, 0)
                until(lambda m: m['pad_buttons'] == 0)
        command('touchshow')
        until(lambda m: m['touch_buttons'])
        command('touchhide')
        until(lambda m: not m['touch_buttons'])
        adb('shell', 'input', 'motionevent', 'DOWN', '50', '150')
        adb('shell', 'input', 'motionevent', 'UP', '50', '150')
        until(lambda m: m['touch_buttons'])
        assert metrics()['pad_buttons'] == 0, 'reveal gesture reached game controls'
        screenshot('temporary-reveal')
        until(lambda m: not m['touch_buttons'], 7)
        command('touchshow')
        until(lambda m: m['touch_buttons'])
        start()
        assert metrics()['touch_buttons'], 'visibility setting was not saved'
        assert not hint_visible(screenshot('hint-after-restart')), 'startup hint repeated after restart'
        command('touchauto')
        until(lambda m: not m['touch_buttons'])
        process.stdin.close()
        process.wait(timeout=10)
        until(lambda m: m['touch_buttons'])
        observations.update(faces='PASS', short_select='PASS', visibility='PASS',
                            reveal_isolated='PASS', saved_visibility='PASS', hint_once='PASS')
        (args.output / 'observations.json').write_text(json.dumps(observations, indent=2) + '\n')
        print('PASS: Android Odin faces; short Select; %.3fs observed hold; visibility, reveal, saved setting, one-time hint' % elapsed)
    finally:
        if process is not None and process.poll() is None:
            process.stdin.close()
            process.wait(timeout=10)
        log.close()
        (args.output / 'logcat.txt').write_bytes(adb('logcat', '-d', '-s', 'wwhd'))
        adb('shell', 'am', 'force-stop', PACKAGE)
        for content, path, private in [(settings, settings_path, False), (prefs, 'shared_prefs/controls.xml', True)]:
            if content:
                cmd = ('run-as', PACKAGE, 'sh', '-c', 'cat > ' + shlex.quote(path)) if private else ('sh', '-c', 'cat > ' + shlex.quote(path))
                adb('shell', *cmd, data=content)
            else:
                cmd = ('run-as', PACKAGE, 'rm', '-f', path) if private else ('rm', '-f', path)
                adb('shell', *cmd)
        adb('shell', 'wm', 'size', override[1] if override else 'reset')


if __name__ == '__main__':
    main()
