import tempfile
import unittest
from pathlib import Path
from PIL import Image
from compare_pictures import compare


class PictureComparisonTest(unittest.TestCase):
    def test_rgb_metrics_alpha_and_explicit_tolerance(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            left = Image.new('RGBA', (2, 1), (0, 0, 0, 255))
            right = left.copy()
            right.putpixel((1, 0), (6, 0, 0, 255))
            left.save(root / 'a.png'); right.save(root / 'b.png')
            result = compare(root / 'a.png', root / 'b.png', root / 'diff.png')
            self.assertEqual(result['changed_rgb_pixels'], 1)
            self.assertEqual(result['mean_absolute_rgb_channel_error'], 1)
            self.assertEqual(result['max_rgb_channel_error'], 6)
            self.assertFalse(result['within_tolerance'])
            self.assertTrue((root / 'diff.png').is_file())
            self.assertTrue(compare(root / 'a.png', root / 'b.png', channel_tolerance=6)['within_tolerance'])
            right.putpixel((0, 0), (0, 0, 0, 254)); right.save(root / 'b.png')
            result = compare(root / 'a.png', root / 'b.png', channel_tolerance=6)
            self.assertEqual(result['changed_alpha_pixels'], 1)
            self.assertFalse(result['within_tolerance'])

    def test_exact_and_dimensions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            Image.new('RGB', (2, 1), 'black').save(root / 'a.png')
            self.assertTrue(compare(root / 'a.png', root / 'a.png')['exact_rgb_and_alpha'])
            Image.new('RGB', (1, 1), 'black').save(root / 'b.png')
            with self.assertRaises(ValueError):
                compare(root / 'a.png', root / 'b.png')


if __name__ == '__main__':
    unittest.main()
