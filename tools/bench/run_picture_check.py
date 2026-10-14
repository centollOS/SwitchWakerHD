#!/usr/bin/env python3
"""Capture baseline twice and candidate twice at one post-load full-pass checkpoint.

Correctness only: no frame-time or performance summary. Requires Pillow and local
user-supplied game/save/state files. Outputs, screenshots and diffs must stay local.
"""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import time
from compare_pictures import compare, file_hash
from run_bench import SCENES


def prepare_candidate_generation(source, cache):
    """Copy a prepared setup generation privately; never modify its cache seed.

    Headstart belongs to the generation; current caches belong to its sibling
    live/ directory. Legacy flat generation caches remain supported.
    A supplied generation does not itself prove that the route is fully warm.
    """
    if Path(source).is_symlink():
        raise ValueError('prepared generation must not be a symbolic link')
    source, cache = Path(source).resolve(), Path(cache).resolve()
    if source == cache or cache.is_relative_to(source) or source.is_relative_to(cache):
        raise ValueError('prepared generation and run cache must not overlap')
    if not source.is_dir() or not (source / 'headstart.bin').is_file():
        raise ValueError('prepared generation needs headstart.bin')
    entries = sorted(source.rglob('*'))
    if source.is_symlink() or any(path.is_symlink() for path in entries):
        raise ValueError('prepared generation must not contain symbolic links')
    seeds = [source]
    live = source.parent / 'live'
    if live.exists():
        if live.is_symlink() or not live.is_dir() or any(p.is_symlink() for p in live.rglob('*')):
            raise ValueError('live cache seed must be a directory without symbolic links')
        if cache == live or cache.is_relative_to(live) or live.is_relative_to(cache):
            raise ValueError('live seed and run cache must not overlap')
        seeds.append(live)
    def inventory(seed):
        return {p.relative_to(seed).as_posix(): file_hash(p)
                for p in sorted(seed.rglob('*')) if p.is_file()}
    inventories = [inventory(seed) for seed in seeds]
    hashes = {}
    for files in inventories:
        hashes.update(files)  # Stable live entries supersede legacy seed entries.
    target = cache / 'prebuilt'
    for seed in seeds:
        shutil.copytree(seed, target, dirs_exist_ok=seed != source)
    for path in [target, *target.rglob('*')]:
        path.chmod(0o755 if path.is_dir() else 0o644)
    copied = {path.relative_to(target).as_posix(): file_hash(path)
              for path in sorted(target.rglob('*')) if path.is_file()}
    after = [inventory(seed) for seed in seeds]
    if copied != hashes or after != inventories:
        raise RuntimeError('prepared cache seed changed while copying')
    environment = dict(WWHD_HEADSTART=str(target / 'headstart.bin'),
        WWHD_SHADER_CACHE=str(target / 'shaders.bin'),
        WWHD_VK_SHADER_CACHE=str(target / 'vulkan-shaders'),
        WWHD_VK_PIPELINE_CACHE=str(target / 'vulkan-pipelines.bin'),
        WWHD_VK_PIPELINE_WARMUP='1')
    return environment, dict(source=str(source), live_source=str(live) if len(seeds) > 1 else None, files_sha256=hashes,
        copied_to=str(target), route_warmth='not established by this flag')


def capture(args, name, binary, revision):
    run = args.out / name
    run.mkdir()  # Never overwrite a prior run or its evidence.
    shutil.copytree(args.save, run / 'save')
    for path in [run / 'save', *(run / 'save').rglob('*')]:
        path.chmod(0o755 if path.is_dir() else 0o644)
    cache = run / 'cache'
    cache.mkdir()
    env = {key: value for key, value in os.environ.items() if not key.startswith('WWHD_')}
    slot, stick, rstick = SCENES[args.scene]
    env.update(WWHD_NO_AUDIO='1', WWHD_NO_GAMEPAD='1', WWHD_NO_HOST_INPUT='1', WWHD_HIDDEN_WINDOWS='1',
        WWHD_RENDERER_RUNTIME='vulkan', WWHD_INTERP='0', WWHD_HEADSTART='0',
        WWHD_VK_PIPELINE_WARMUP='0', WWHD_SHADER_CACHE=str(cache / 'shaders.bin'),
        WWHD_VK_SHADER_CACHE=str(cache / 'vkshaders'), WWHD_VK_PIPELINE_CACHE=str(cache / 'pipelines.bin'),
        WWHD_SETTINGS=str(run / 'settings.ini'), WWHD_DISPLAY_SETTINGS=str(run / 'display.plist'),
        XDG_CONFIG_HOME=str(run / 'config'), WWHD_STATE_DIR=str(args.state_dir),
        WWHD_STATE_LOAD_AT='%d:%d' % (args.load_frame, args.slot or slot),
        WWHD_PRESS=','.join('%d-%d:8000' % (frame, frame + 8) for frame in range(120, args.load_frame, 30)),
        WWHD_TEST_ORIGIN_LOAD=str(args.origin_load_steps), WWHD_TEST_END=str(args.seconds),
        WWHD_TEST_CAPTURE_LOAD_STEP=str(args.capture_load_step))
    preparation = None
    generation = getattr(args, 'candidate_prebuild_generation', None)
    if name.startswith('candidate') and generation:
        prepared_env, preparation = prepare_candidate_generation(generation, cache)
        env.update(prepared_env)
    if stick: env['WWHD_TEST_STICK'] = stick
    if rstick: env['WWHD_TEST_RSTICK'] = rstick
    if args.validation: env['WWHD_VK_VALIDATION'] = '1'
    if name.startswith('candidate'): env['WWHD_VK_STUTTER_TRACE'] = str(run / 'events.csv')
    command = [str(binary), '--game', str(args.game), '--save', 'save']
    invocation = dict(command=command, cwd=str(run), revision=revision, binary_sha256=file_hash(binary),
        state_sha256=file_hash(args.state_dir / ('slot%d.bin' % (args.slot or slot))),
        scene=args.scene, capture_load_step=args.capture_load_step, origin_load_steps=args.origin_load_steps,
        caches=('private copy of prepared candidate generation; driver caches uncontrolled' if preparation
                else 'isolated cold application caches; driver caches uncontrolled'),
        cache_preparation=preparation,
        env={key: value for key, value in env.items() if key.startswith(('WWHD_', 'VK_', 'MVK_', 'DYLD_'))
             or key == 'XDG_CONFIG_HOME'})
    (run / 'invocation.json').write_text(json.dumps(invocation, indent=2) + '\n')
    print('Capturing ' + name, flush=True)
    with (run / 'log').open('w') as log:
        process = subprocess.Popen(command, cwd=run, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL)
        deadline = time.monotonic() + args.timeout
        try:
            while process.poll() is None and not (run / 'test_done').is_file() and time.monotonic() < deadline:
                time.sleep(.25)
        finally:
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=10)
    log = (run / 'log').read_text(errors='replace')
    pictures = sorted((run / 'captures').glob('*/tv.png'))
    expected = 'capture at loaded logic step +%d ' % args.capture_load_step
    if not (run / 'test_done').is_file() or expected not in log or len(pictures) != 1:
        raise RuntimeError(name + ': endpoint/checkpoint/capture missing; inspect ' + str(run / 'log'))
    if '[vulkan validation error]' in log:
        raise RuntimeError(name + ': Vulkan validation error; inspect ' + str(run / 'log'))
    checkpoint = [line for line in log.splitlines() if '[test] capture at loaded' in line]
    counter = re.search(r'game counter ([0-9A-Fa-f]+)', checkpoint[0])
    if not counter:
        raise RuntimeError(name + ': checkpoint game counter missing')
    print(name + ': checkpoint and capture complete', flush=True)
    return dict(name=name, tv=str(pictures[0]),
                present=str(pictures[0].with_name('present.png')),
                gamepad=str(pictures[0].with_name('gamepad.png')),
                checkpoint_log=checkpoint, game_counter=int(counter[1], 16),
                validation_warnings=log.count('[vulkan validation warning]'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'candidate', 'game', 'save', 'state-dir', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--baseline-revision', required=True)
    parser.add_argument('--candidate-revision', required=True)
    parser.add_argument('--scene', choices=SCENES, default='outset')
    parser.add_argument('--slot', type=int, default=0)
    parser.add_argument('--load-frame', type=int, default=450)
    parser.add_argument('--origin-load-steps', type=int, default=180)
    parser.add_argument('--capture-load-step', type=int, default=200)
    parser.add_argument('--seconds', type=float, default=2)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--candidate-prebuild-generation', type=Path,
        help='copy a local setup generation into each candidate run only; baseline remains headstart-disabled')
    args = parser.parse_args()
    if min(args.load_frame, args.origin_load_steps, args.capture_load_step) < 0 or args.seconds <= 0 or args.timeout <= 0:
        parser.error('frame/step offsets must be nonnegative; seconds and timeout must be positive')
    if args.capture_load_step > args.origin_load_steps + args.seconds * 30:
        parser.error('capture checkpoint is beyond the scenario endpoint')
    for name in ('baseline', 'candidate', 'game', 'save', 'state_dir', 'out'):
        setattr(args, name, getattr(args, name).resolve())
    if args.candidate_prebuild_generation:
        args.candidate_prebuild_generation = args.candidate_prebuild_generation.resolve()
        seed = args.candidate_prebuild_generation
        if args.out == seed or args.out.is_relative_to(seed) or seed.is_relative_to(args.out):
            parser.error('output and prepared generation directories must not overlap')
        if not seed.is_dir() or not (seed / 'headstart.bin').is_file():
            parser.error('prepared generation needs headstart.bin')
    args.out.mkdir(parents=True, exist_ok=True)
    captures = {}
    for name, binary, revision in [('baseline_1', args.baseline, args.baseline_revision),
            ('baseline_2', args.baseline, args.baseline_revision),
            ('candidate_1', args.candidate, args.candidate_revision),
            ('candidate_2', args.candidate, args.candidate_revision)]:
        captures[name] = capture(args, name, binary, revision)
        (args.out / 'captures.json').write_text(json.dumps(captures, indent=2) + '\n')
    comparisons, missing = {}, {}
    pairs = [('baseline_1', 'baseline_2'), ('candidate_1', 'candidate_2'),
             ('baseline_1', 'candidate_1'), ('baseline_2', 'candidate_2')]
    for kind in ('tv', 'gamepad', 'present'):
        absent = [name for name, item in captures.items() if not Path(item[kind]).is_file()]
        if absent:
            missing[kind] = absent
            continue
        for left, right in pairs:
            name = kind + '_' + left + '_vs_' + right
            comparisons[name] = compare(captures[left][kind], captures[right][kind], args.out / (name + '_diff.png'))
    counters_match = len({item['game_counter'] for item in captures.values()}) == 1
    report = dict(correctness_only=True, checkpoint_game_counters_match=counters_match, missing_captures=missing, tolerance='exact RGB and alpha; baseline variation is not assumed harmless',
                  captures=captures, comparisons=comparisons)
    (args.out / 'pictures.json').write_text(json.dumps(report, indent=2) + '\n')
    for name, result in comparisons.items():
        print(name + ': %d changed RGB pixels, max channel error %d' %
              (result['changed_rgb_pixels'], result['max_rgb_channel_error']), flush=True)
    return 0 if not missing and counters_match and all(item['exact_rgb_and_alpha'] for item in comparisons.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
