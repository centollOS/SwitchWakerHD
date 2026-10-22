import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch
from PIL import Image
import run_picture_check as pictures


class CaptureEvidenceTest(unittest.TestCase):
    def run_fake(self, complete=True, correct_step=True, name='baseline_1', prepared=False, fallback=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'save').mkdir(); (root / 'states').mkdir(); (root / 'out').mkdir()
            (root / 'binary').write_bytes(b'synthetic executable fixture')
            (root / 'states/slot3.bin').write_bytes(b'synthetic state fixture')
            args = types.SimpleNamespace(out=root / 'out', save=root / 'save', state_dir=root / 'states',
                game=root / 'game', scene='outset', slot=3, load_frame=450, origin_load_steps=180,
                seconds=2, capture_load_step=200, validation=True, timeout=1,
                candidate_vertex_fallback=fallback)
            if prepared:
                seed = root / 'generation'
                (seed / 'vulkan-shaders').mkdir(parents=True)
                for relative in ('headstart.bin', 'shaders.bin', 'vulkan-pipelines.bin',
                                 'vulkan-pipelines.bin.recipes', 'vulkan-shaders/spirv.bin'):
                    (seed / relative).write_bytes(('synthetic ' + relative).encode())
                args.candidate_prebuild_generation = seed
                original = {str(p.relative_to(seed)): p.read_bytes() for p in seed.rglob('*') if p.is_file()}
            launched = []

            class Process:
                def __init__(self, command, **kwargs):
                    launched.append(kwargs['env'])
                    run = Path(kwargs['cwd'])
                    if name.startswith('candidate') and prepared:
                        copy = run / 'cache/prebuilt'
                        self_copy = {str(p.relative_to(copy)): p.read_bytes() for p in copy.rglob('*') if p.is_file()}
                        if self_copy != original:
                            raise AssertionError('Prepared files were not copied before launch')
                        (copy / 'vulkan-pipelines.bin').write_bytes(b'synthetic runtime mutation')
                    target = run / 'captures/fixture'; target.mkdir(parents=True)
                    Image.new('RGB', (2, 1), 'black').save(target / 'tv.png')
                    offset = 200 if correct_step else 201
                    kwargs['stdout'].write('[test] capture at loaded logic step +%d (game counter 00000100), full pass\n' % offset)
                    if complete: (run / 'test_done').touch()
                def poll(self): return 0

            with patch.object(pictures.subprocess, 'Popen', Process):
                result = pictures.capture(args, name, root / 'binary', 'fixture-revision')
            if prepared:
                self.assertEqual({str(p.relative_to(seed)): p.read_bytes() for p in seed.rglob('*') if p.is_file()}, original)
            return result, launched[0]

    def test_vertex_fallback_only_affects_candidate(self):
        _, baseline = self.run_fake(fallback=True)
        self.assertNotIn('WWHD_VK_VERTEX_FALLBACK', baseline)
        _, candidate = self.run_fake(name='candidate_1', fallback=True)
        self.assertEqual(candidate['WWHD_VK_VERTEX_FALLBACK'], '1')
        _, normal = self.run_fake(name='candidate_1')
        self.assertNotIn('WWHD_VK_VERTEX_FALLBACK', normal)

    def test_requires_completed_endpoint_and_exact_checkpoint(self):
        with self.assertRaises(RuntimeError): self.run_fake(complete=False)
        with self.assertRaises(RuntimeError): self.run_fake(correct_step=False)

    def test_records_cold_local_capture_without_performance_mode(self):
        result, env = self.run_fake()
        self.assertEqual(result['name'], 'baseline_1')
        self.assertEqual(env['WWHD_TEST_ORIGIN_LOAD'], '180')
        self.assertEqual(env['WWHD_TEST_CAPTURE_LOAD_STEP'], '200')
        self.assertEqual(env['WWHD_HEADSTART'], '0')
        self.assertEqual(env['WWHD_VK_PIPELINE_WARMUP'], '0')
        self.assertNotIn('WWHD_PROFILE', env)

    def test_prepared_generation_only_affects_candidate_and_seed_is_immutable(self):
        _, baseline = self.run_fake(prepared=True)
        self.assertEqual(baseline['WWHD_HEADSTART'], '0')
        self.assertEqual(baseline['WWHD_VK_PIPELINE_WARMUP'], '0')
        _, candidate = self.run_fake(name='candidate_1', prepared=True)
        self.assertTrue(candidate['WWHD_HEADSTART'].endswith('cache/prebuilt/headstart.bin'))
        self.assertTrue(candidate['WWHD_VK_SHADER_CACHE'].endswith('cache/prebuilt/vulkan-shaders'))
        self.assertTrue(candidate['WWHD_VK_PIPELINE_CACHE'].endswith('cache/prebuilt/vulkan-pipelines.bin'))
        self.assertTrue(candidate['WWHD_SHADER_CACHE'].endswith('cache/prebuilt/shaders.bin'))
        self.assertEqual(candidate['WWHD_VK_PIPELINE_WARMUP'], '1')

    def test_generation_uses_stable_live_cache_without_mutating_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generation = root / 'generation-test'
            generation.mkdir()
            (generation / 'headstart.bin').write_bytes(b'synthetic headstart')
            (generation / 'shaders.bin').write_bytes(b'old seed')
            live = root / 'live'
            live.mkdir()
            (live / 'shaders.bin').write_bytes(b'player accumulated cache')
            env, evidence = pictures.prepare_candidate_generation(generation, root / 'output')
            copied = Path(env['WWHD_SHADER_CACHE'])
            self.assertEqual(copied.read_bytes(), b'player accumulated cache')
            copied.write_bytes(b'synthetic new entries')
            self.assertEqual((live / 'shaders.bin').read_bytes(), b'player accumulated cache')
            self.assertEqual(evidence['live_source'], str(live.resolve()))

    def test_prepared_generation_rejects_overlapping_destination_and_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            seed = Path(directory) / 'generation'
            seed.mkdir()
            (seed / 'headstart.bin').write_bytes(b'synthetic headstart')
            with self.assertRaises(ValueError):
                pictures.prepare_candidate_generation(seed, seed / 'output')
            try:
                (seed / 'outside-link').symlink_to(Path(directory) / 'outside')
            except (OSError, NotImplementedError):
                self.skipTest('symbolic links unavailable')
            with self.assertRaises(ValueError):
                pictures.prepare_candidate_generation(seed, Path(directory) / 'cache')


if __name__ == '__main__':
    unittest.main()
