# SPDX-License-Identifier: MPL-2.0
import copy
import math
import struct
import unittest
import actor_animation as animation
import actor_model
from skinned_actor_model import append_dictionary, append_string


def fixture(interpolation='LINEAR'):
    binary = struct.pack('<3f12f', 0., .5, 1., 0., 0., 0., 1., 0., math.sin(.5), 0., math.cos(.5), 0., 0., 0., -1.)
    doc = dict(buffers=[{'byteLength': len(binary)}], bufferViews=[{'byteOffset': 0, 'byteLength': 12}, {'byteOffset': 12, 'byteLength': 48}], accessors=[{'bufferView': 0, 'componentType': 5126, 'type': 'SCALAR', 'count': 3}, {'bufferView': 1, 'componentType': 5126, 'type': 'VEC4', 'count': 3}], skins=[{'joints': [0]}], nodes=[{'name': 'head'}], animations=[{'name': 'idle', 'channels': [{'sampler': 0, 'target': {'node': 0, 'path': 'rotation'}}], 'samplers': [{'input': 0, 'output': 1, 'interpolation': interpolation}]}])
    return dict(document=doc, binary=binary, bones=[{'name': 'head'}])


def fake_bfres():
    data = bytearray(0x100); data[:4] = b'FRES'; struct.pack_into('>I', data, 4, 0x03040001)
    f = actor_model.base.Bfres(data)
    fm = f.append(b'\0' * 0x30, 4); sk = f.append(b'FSKL' + b'\0' * 0x20, 4)
    f.set_rel(fm + 12, sk); f.set_rel(0x20, append_dictionary(f, [('original', fm)]))
    return bytes(f.b), sk


class AnimationTests(unittest.TestCase):
    def test_shortest_arc_sampling_and_sentinel(self):
        clip = animation.clips(fixture())[0]
        self.assertEqual((clip['frames'], clip['end']), (31, 30))
        samples = clip['bones'][0]['samples']['rotation']
        self.assertEqual(len(samples), 32)
        self.assertEqual(samples[-1], samples[-2])
        self.assertAlmostEqual(samples[-1][1], 0., places=6)
        self.assertAlmostEqual(samples[7][1], 14/30, places=6)

    def test_step_and_nonzero_start_clamp(self):
        model = fixture('STEP')
        model['binary'] = struct.pack('<3f', .2, .5, 1.) + model['binary'][12:]
        samples = animation.clips(model)[0]['bones'][0]['samples']['rotation']
        self.assertEqual(samples[0], samples[14]); self.assertNotEqual(samples[14], samples[15])
        self.assertEqual(samples[30], samples[31])

    def test_native_structure_independent_decode(self):
        original, skeleton = fake_bfres()
        data = animation.append_clips(original, fixture())
        # Independent decoder deliberately does not use Bfres.dict/rel helpers.
        def u32(p): return struct.unpack_from('>I', data, p)[0]
        def rel(p): return p + struct.unpack_from('>i', data, p)[0]
        dictionary = rel(0x28)
        self.assertEqual(struct.unpack_from('>H', data, 0x54)[0], 1)
        self.assertEqual(u32(dictionary + 4), 1)
        clip = rel(dictionary + 8 + 16 + 12)
        self.assertEqual(data[clip:clip+4], b'FSKA')
        self.assertEqual(u32(clip+0x10), 31)
        # Native wrapper027F7B00 reads low s16 of framecount; setAnm
        # subtracts1 for FSKA loopbit4, even for requested NONE mode.
        maximum = struct.unpack_from('>h', data, clip+0x12)[0]
        self.assertEqual(maximum - bool(u32(clip+0xC) & 4), 30)
        self.assertEqual(rel(clip+0x24), skeleton)
        self.assertEqual(u32(clip+0x18), 3)
        bone = rel(clip+0x20)
        self.assertEqual(tuple(data[bone+8:bone+13]), (3, 6, 3, 0, 0))
        curve = rel(bone+0x10)
        for i in range(3):
            p = curve + i*0x24
            flags, keys, target = struct.unpack_from('>HHI', data, p)
            self.assertEqual((flags, keys, target), (0x20, 32, 0x20+i*4))
            self.assertEqual(u32(p+0x1C), 0)
            samples = struct.unpack_from('>32f', data, rel(p+0x20))
            self.assertEqual(samples[-1], samples[-2])
            # Native027E9A48 endpoint reads both end and end+1.
            self.assertTrue(math.isfinite(samples[30] * 1. + samples[31] * 0.))
        self.assertEqual(u32(12), len(data))

    def test_translation_units(self):
        model = fixture()
        model['document']['animations'][0]['channels'][0]['target']['path'] = 'translation'
        model['document']['accessors'][1]['type'] = 'VEC3'
        model['binary'] = struct.pack('<3f9f', 0., .5, 1., 0., 0., 0., 0., 1., 0., 0., 2., 0.)
        model['document']['buffers'][0]['byteLength'] = len(model['binary']); model['document']['bufferViews'][1]['byteLength'] = 36
        samples = animation.clips(model)[0]['bones'][0]['samples']['translation']
        self.assertAlmostEqual(samples[15][1], 100.)
        self.assertAlmostEqual(samples[30][1], 200.)

    def test_rejections(self):
        changes = [lambda m: m['document']['animations'][0].update(name='../bad'), lambda m: m['document']['animations'][0]['samplers'][0].update(interpolation='CUBICSPLINE'), lambda m: m['document']['animations'][0]['channels'][0]['target'].update(path='scale'), lambda m: m['document']['animations'][0]['channels'][0]['target'].update(node=2), lambda m: m['document']['animations'][0]['channels'].append(copy.deepcopy(m['document']['animations'][0]['channels'][0])), lambda m: m['document']['accessors'][1].update(count=2), lambda m: m.update(binary=struct.pack('<3f', 0., 0., 1.) + m['binary'][12:]), lambda m: m.update(binary=struct.pack('<3f', 0., .5, 31.) + m['binary'][12:]), lambda m: m.update(binary=m['binary'][:12] + b'\0'*16 + m['binary'][28:]), lambda m: m['document'].update(animations=m['document']['animations']*9)]
        for change in changes:
            with self.subTest(change=change):
                model = fixture(); change(model)
                with self.assertRaises(actor_model.BuildError): animation.clips(model)

    def test_euler_equivalence_continuity_wrap_and_gimbal(self):
        def from_euler(e):
            x,y,z=(a*.5 for a in e);sx,cx=math.sin(x),math.cos(x);sy,cy=math.sin(y),math.cos(y);sz,cz=math.sin(z),math.cos(z)
            return (sx*cy*cz-cx*sy*sz,cx*sy*cz+sx*cy*sz,cx*cy*sz-sx*sy*cz,cx*cy*cz+sx*sy*sz)
        previous=None
        for i in range(121):
            authored=(.3, math.pi/2+(i-60)*.01, 2.8+i*.01)
            q=from_euler(authored); e=animation._euler(q,previous); actual=from_euler(e)
            # q and -q are the same rotation matrix.
            self.assertAlmostEqual(abs(sum(a*b for a,b in zip(q,actual))),1.,places=6)
            if previous:self.assertLess(max(abs(a-b) for a,b in zip(e,previous)),.05)
            previous=e
        for pitch in (-math.pi/2,math.pi/2):
            q=from_euler((.7,pitch,.4));e=animation._euler(q,(.7,pitch,.4))
            self.assertAlmostEqual(abs(sum(a*b for a,b in zip(q,from_euler(e)))),1.,places=6)

    def test_empty_clips_clears_template(self):
        data, _ = fake_bfres(); model = fixture(); model['document']['animations'] = []
        out = animation.append_clips(data, model)
        self.assertEqual(struct.unpack_from('>I', out, 0x28)[0], 0)
        self.assertEqual(animation.inspect_clips(model), [])


if __name__ == '__main__': unittest.main()
