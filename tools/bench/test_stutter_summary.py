"""Regression checks for attribution of worker work and repeated pictures."""
import csv
import tempfile
import unittest
from pathlib import Path
from stutter_summary import summarize


class StutterSummaryTest(unittest.TestCase):
    def test_translation_and_prerequisite_execution_are_separate(self):
        rows = [
            ('frame', 10, '0', 100000000, 200000000, 1, 0, -1),
            ('frame', 11, '0', 200000000, 300000000, 1, 0, -1),
            ('shader_translation', 0, 'a', 110000000, 120000000, 3, 0, 8000000),
            ('shader_translation', 11, 'b', 210000000, 212000000, 1, 0, 1000000),
            ('translation_wait', 0, 'a', 110000000, 121000000, 1, 0, -1),
            ('shader_pending', 0, 'a', 122000000, 125000000, 1, 0, -1),
            ('pipeline_prerequisites', 0, 'a', 120000000, 150000000, 2, 0, 2000000),
            ('pipeline_prerequisites', 11, 'b', 220000000, 225000000, 2, 0, -1),
            ('guest_pipeline', 0, 'a', 150000000, 160000000, 2, 0, 3000000),
        ]
        result = self.summary(rows, with_cpu=True)
        self.assertEqual(result['render_thread_shader_translations'], 1)
        self.assertEqual(result['other_thread_shader_translations'], 1)
        self.assertEqual(result['translation_pending_execution']['total_ms'], 11)
        self.assertEqual(result['shader_pending_execution']['total_ms'], 3)
        self.assertEqual(result['pipeline_prerequisites'], dict(count=2, total_ms=35, max_ms=30,
                         sampled_calls=1, unavailable_calls=1, calling_thread_cpu_ms=2))
        self.assertEqual(result['pipeline_creation_calls'], 1)
        self.assertEqual(result['pipeline_creation_ms'], 10)
        self.assertNotIn('pipeline_prerequisites', result['compilation_cpu_by_kind'])
        selected = self.summary(rows, with_cpu=True, frame_from=10, frame_to=11)
        self.assertEqual(selected['other_thread_shader_translations'], 1) # Request frame 0, execution frame 10.
        self.assertEqual(selected['render_thread_shader_translations'], 0)
        self.assertEqual(selected['pipeline_prerequisites']['count'], 1)
        self.assertEqual(selected['pipeline_prerequisites']['calling_thread_cpu_ms'], 2)
        empty = self.summary(rows, with_cpu=True, frame_from=20)
        self.assertEqual(empty['translation_pending_execution']['count'], 0)
        self.assertEqual(empty['pipeline_prerequisites']['count'], 0)
        self.assertEqual(empty['render_thread_shader_translations'], 0)

    def summary(self, rows, with_cpu=False, **kwargs):
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / 'events.csv'
            with trace.open('w', newline='') as target:
                writer = csv.writer(target)
                writer.writerow(('kind', 'frame', 'key', 'start_ns', 'end_ns', 'thread', 'result') + (('cpu_ns',) if with_cpu else ()))
                writer.writerows(rows)
            return summarize(trace, **kwargs)

    def test_cpu_is_separate_from_wall_and_missing_is_not_zero(self):
        result = self.summary([
            ('frame', 10, '0', 100000000, 200000000, 1, 0, -1),
            ('guest_pipeline', 10, 'a', 110000000, 180000000, 2, 0, 5000000),
            ('guest_pipeline', 10, 'b', 181000000, 190000000, 2, 0, -1),
            ('spirv_compile', 10, 'c', 120000000, 150000000, 3, 0, 0),
            ('startup_warmup', 0, 'd', 100000000, 190000000, 4, 2, 8000000),
        ], with_cpu=True)
        self.assertEqual(result['pipeline_creation_ms'], 79)
        self.assertEqual(result['compilation_cpu_by_kind']['guest_pipeline'],
                         dict(sampled_calls=1, unavailable_calls=1, calling_thread_cpu_ms=5))
        self.assertEqual(result['compilation_cpu_by_kind']['spirv_compile']['sampled_calls'], 1)
        self.assertEqual(result['startup_warmup'][0]['published_ready'], 13)
        self.assertEqual(result['startup_warmup'][0]['failures'], 2)
        self.assertEqual(result['startup_warmup'][0]['elapsed_ms'], 90)
        self.assertEqual(result['startup_warmup'][0]['calling_thread_cpu_ms'], 8)

    def test_legacy_trace_reports_cpu_unavailable(self):
        result = self.summary([('guest_pipeline', 1, 'a', 100, 200, 2, 0)])
        self.assertEqual(result['compilation_cpu_by_kind']['guest_pipeline'],
                         dict(sampled_calls=0, unavailable_calls=1, calling_thread_cpu_ms=0))

    def test_repeats_preserve_guest_stall_and_requests_are_not_compiles(self):
        result = self.summary([
            ('frame', 10, '0', 100000000, 200000000, 1, 0),
            ('pipeline_request', 10, 'abc', 110000000, 110000000, 1, 1),
            ('guest_pipeline', 10, 'abc', 111000000, 180000000, 2, 0),
            ('pipeline_pending', 10, 'abc', 110000000, 190000000, 1, 0),
            ('repeated_frame', 10, '0', 150000000, 160000000, 1, 0),
            ('repeated_frame_age', 10, '0', 100000000, 160000000, 1, 0),
            ('startup_pipeline_hit', 10, 'def', 195000000, 195000000, 1, 0),
            ('shader_request', 10, 'a', 110000000, 110000000, 1, 1),
            ('shader_pending', 10, 'a', 110000000, 115000000, 1, 0),
            ('spirv_compile', 10, 'a', 111000000, 114000000, 3, 0),
        ])
        self.assertEqual(result['max_ms'], 100)
        self.assertEqual(result['pipeline_creation_calls'], 1)
        self.assertEqual(result['render_thread_pipeline_creation_calls'], 0)
        self.assertEqual(result['other_thread_pipeline_creation_calls'], 1)
        self.assertEqual(result['demand_requests'], 1)
        self.assertEqual(result['shader_requests'], 1)
        self.assertEqual(result['shader_pending_execution']['total_ms'], 5)
        self.assertEqual(result['render_thread_spirv_compiles'], 0)
        self.assertEqual(result['other_thread_spirv_compiles'], 1)
        self.assertEqual(result['startup_pipeline_hits'], 1)
        self.assertEqual(result['pending_execution']['total_ms'], 80)
        self.assertEqual(result['repeated_presentations'], 1)
        self.assertEqual(result['max_repeated_picture_age_ms'], 60)
        self.assertEqual(result['worst_frames'][0]['render_thread_compilations'], 0)

    def test_worker_attribution_uses_execution_time_after_frame_reset(self):
        result = self.summary([
            ('frame', 10, '0', 100000000, 200000000, 1, 0),
            ('frame', 2, '0', 200000000, 300000000, 1, 0),
            ('startup_pipeline', 0, 'a', 110000000, 120000000, 2, 0),
            ('startup_pipeline', 10, 'b', 210000000, 220000000, 2, 0),
        ], frame_from=10, frame_to=11)
        self.assertEqual(result['frame_count'], 1)
        self.assertEqual(result['pipeline_creation_calls'], 1)
        self.assertEqual(result['worst_frames'][0]['pipeline_compilations'][0]['key'], 'a')


if __name__ == '__main__':
    unittest.main()
