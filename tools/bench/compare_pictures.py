#!/usr/bin/env python3
"""Compare local screenshots. Requires Pillow; defaults to exact RGB/alpha parity.

The difference PNG is amplified for inspection, not a new rendered picture.
Never publish screenshots or reports containing game imagery with releases.
"""
import argparse
import hashlib
import json
from pathlib import Path
from PIL import Image, ImageChops, ImageStat


def file_hash(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def compare(reference, candidate, diff_path=None, channel_tolerance=0, amplification=4):
    if not 0 <= channel_tolerance <= 255 or amplification < 1:
        raise ValueError('channel tolerance must be 0..255; amplification must be positive')
    with Image.open(reference) as image:
        left = image.convert('RGBA')
    with Image.open(candidate) as image:
        right = image.convert('RGBA')
    if left.size != right.size:
        raise ValueError('image dimensions differ: %s vs %s' % (left.size, right.size))
    difference = ImageChops.difference(left, right)
    red, green, blue, alpha = difference.split()
    rgb = Image.merge('RGB', (red, green, blue))
    peak = ImageChops.lighter(ImageChops.lighter(red, green), blue)
    histogram = peak.histogram()
    pixels = left.width * left.height
    changed = pixels - histogram[0]
    above = sum(histogram[channel_tolerance + 1:])
    maximum = max((i for i, n in enumerate(histogram) if n), default=0)
    alpha_changed = pixels - alpha.histogram()[0]
    result = dict(reference=str(Path(reference).resolve()), candidate=str(Path(candidate).resolve()),
                  reference_sha256=file_hash(reference), candidate_sha256=file_hash(candidate),
                  width=left.width, height=left.height, pixels=pixels,
                  changed_rgb_pixels=changed, changed_rgb_fraction=changed / pixels,
                  mean_absolute_rgb_channel_error=sum(ImageStat.Stat(rgb).mean) / 3,
                  max_rgb_channel_error=maximum, changed_alpha_pixels=alpha_changed,
                  channel_tolerance=channel_tolerance, pixels_above_tolerance=above,
                  exact_rgb_and_alpha=changed == 0 and alpha_changed == 0,
                  within_tolerance=above == 0 and alpha_changed == 0,
                  difference_bbox=peak.getbbox())
    if diff_path:
        target = Path(diff_path)
        target.parent.mkdir(parents=True, exist_ok=True)
        rgb.point([min(255, value * amplification) for value in range(256)] * 3).save(target)
        result.update(diff_path=str(target.resolve()), diff_amplification=amplification)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference')
    parser.add_argument('candidate')
    parser.add_argument('--diff', help='amplified RGB difference PNG (keep local)')
    parser.add_argument('--out', help='JSON result (keep local)')
    parser.add_argument('--channel-tolerance', type=int, default=0)
    args = parser.parse_args()
    result = compare(args.reference, args.candidate, args.diff, args.channel_tolerance)
    rendered = json.dumps(result, indent=2) + '\n'
    if args.out:
        Path(args.out).write_text(rendered)
    print(rendered, end='')
    return 0 if result['within_tolerance'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
