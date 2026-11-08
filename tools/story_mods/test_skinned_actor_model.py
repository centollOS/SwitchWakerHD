# SPDX-License-Identifier: MPL-2.0
"""Independent original synthetic skin structure and deforming-bind fixture."""
import importlib.util
import pathlib
import struct
import os
import tempfile
import unittest
import skinned_actor_model as skin
import test_actor_model as static_fixture

ROOT=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('tide_sprite',ROOT/'examples/guest-mods/sdk-v3-windfall/assets/make_tide_sprite.py')
asset=importlib.util.module_from_spec(spec);spec.loader.exec_module(asset)


def template():
    f=skin.base.Bfres(static_fixture.static_template());fm=f.dict(f.rel(0x20))[0][1]
    f.set_rel(0x20,skin.append_dictionary(f,[('fobj00',fm)]))
    fv=f.rel(fm+16);vb=f.rel(fv+24);f.b[fv+4]=5;f.b[fv+12]=2;f.put('H',vb+12,64)
    layout=[('_p0',0,0,0x811),('_n0',0,16,0x811),('_u0',0,32,0x80d),('_i0',0,40,0x10c),('_w0',0,48,0x80d)]
    aa=f.append(b'\0'*60,4)
    for i,(name,buf,off,fmt) in enumerate(layout):
        p=aa+12*i;f.set_rel(p,skin.append_string(f,name));f.put('BBHI',p+4,buf,0,off,fmt)
    f.set_rel(fv+16,aa)
    shape=f.dict(f.rel(fm+20))[0][1];f.put('H',shape+0x1a,1)
    f.set_rel(shape+0x30,f.append(struct.pack('>6H',0,0,0,0,0,1),4))
    f.set_rel(shape+0x38,f.append(b'\0\0',4))
    f.set_rel(fm+24,skin.append_dictionary(f,[('original_material',f.append(b'FMAT'+bytes(60),4))]))
    textures=f.dict(f.rel(0x24));f.set_rel(0x24,skin.append_dictionary(f,[('Fobj00',textures[0][1])]))
    return bytes(f.b)


class SkinTests(unittest.TestCase):
    def test_code_dependencies_change_provenance_without_asset_changes(self):
        from unittest.mock import patch
        import actor_animation
        with tempfile.TemporaryDirectory() as directory:
            game=pathlib.Path(directory); archive=game/skin.ARCHIVE; archive.parent.mkdir(parents=True)
            archive.write_bytes(skin.base.yaz0_stored(skin.base.sarc({'Fobj00.bfres':template(),'fobj00.sharcfb':b'original synthetic shader'})))
            glb=game/'original.glb';glb.write_bytes(asset.make());out=str(game/'model.szs')
            baseline=skin.build(str(game),str(glb),'original',out)
            for module in (skin,skin.static,skin.base,skin.base.shaderprep,actor_animation):
                with self.subTest(module=module.__name__):
                    replacement=game/'changed-source.py'
                    replacement.write_bytes(pathlib.Path(module.__file__).read_bytes()+b'\n# provenance-only source change\n')
                    with patch.object(module,'__file__',str(replacement)):
                        changed=skin.build(str(game),str(glb),'original',out)
                    self.assertNotEqual(baseline['cache_key'],changed['cache_key'])
                    self.assertEqual(baseline['archive_sha256'],changed['archive_sha256'])
                    self.assertEqual(sum(a!=changed['converter_sources'][name] for name,a in baseline['converter_sources'].items()),1)


    def parse(self,data):
        # a closed file: Windows can't open a NamedTemporaryFile a second time while it is open
        with tempfile.TemporaryDirectory() as d:
            path=os.path.join(d,'model.glb')
            with open(path,'wb') as f: f.write(data)
            return skin.read_glb(path)

    def test_original_reproducible_and_two_influences(self):
        data=asset.make();self.assertEqual(data,(ROOT/'examples/guest-mods/sdk-v3-windfall/assets/tide_sprite.glb').read_bytes())
        model,info=self.parse(data);self.assertEqual(info['bones'],3);self.assertEqual(info['vertices'],120)
        self.assertTrue(any(a[1]>0 and b[1]>0 for a,b in model['influences']))
        self.assertEqual([b['name'] for b in model['bones']],['root','body','head'])

    def test_golden_structure_inverse_bind_palette(self):
        model,info=self.parse(asset.make());result=skin.build_bfres(template(),model,'original_skin')
        f=skin.base.Bfres(result);name,fm=f.dict(f.rel(0x20))[0];self.assertEqual(name,'original_skin');sk=f.rel(fm+12)
        self.assertEqual(struct.unpack_from('>HHH',result,sk+8),(3,3,0))
        self.assertEqual(struct.unpack_from('>3H',result,f.rel(sk+24)),(0,1,2))
        ba=f.rel(sk+20);self.assertEqual([x[0] for x in f.dict(f.rel(sk+16))],['root','body','head'])
        for i,b in enumerate(model['bones']):
            self.assertEqual(f.cstr(f.rel(ba+64*i)),b['name']);self.assertEqual(struct.unpack_from('>h',result,ba+64*i+6)[0],b['parent'])
        inverse=struct.unpack_from('>36f',result,f.rel(sk+28));self.assertAlmostEqual(inverse[19],-30,places=4);self.assertAlmostEqual(inverse[31],-105,places=4)
        fv=f.rel(fm+16);self.assertEqual(f.b[fv+12],2);vb=f.rel(fv+24);self.assertEqual(f.u32(vb+4),120*64)
        shape=f.dict(f.rel(fm+20))[0][1];box=f.rel(shape+0x34)
        self.assertEqual(struct.unpack_from('>3f',result,box),(0,0,0))
        self.assertAlmostEqual(struct.unpack_from('>f',result,shape+0x1c)[0],info['animation_radius'],places=3)
        vertices=f.rel(vb+20);self.assertEqual(struct.unpack_from('>2I',result,vertices+40),(0,1))
        self.assertEqual(f.u16(0x54),4);self.assertEqual(f.u32(12),len(result))
        clips=f.dict(f.rel(0x28));self.assertEqual([x[0] for x in clips],['idle','walk','talk','wave'])
        for _,clip in clips:
            self.assertEqual(result[clip:clip+4],b'FSKA');self.assertEqual(f.rel(clip+0x24),sk)

    def test_joint_rotation_changes_blended_vertex(self):
        model,_=self.parse(asset.make());vertex=next(i for i,p in enumerate(model['influences']) if p[0][1]>0 and p[1][1]>0)
        p=model['geometry'][0][vertex];pair=model['influences'][vertex]
        # Independent CPU reference: root unchanged, body rotated90deg aboutZ
        # around its bind origin. A mixed vertex must move less than rigid100%.
        rotated=[-(p[1]-30),p[0]+30,p[2]]
        expected=[pair[0][1]*p[k]+pair[1][1]*rotated[k] for k in range(3)]
        self.assertNotEqual(expected,p);self.assertNotEqual(expected,rotated)

    def test_invalid_rig(self):
        changes=[lambda d:d['nodes'][2].update(scale=[1,2,1]),lambda d:d['nodes'][2].update(name='root'),lambda d:d['skins'][0].update(joints=[1,3,2]),lambda d:d['nodes'][0].update(translation=[1,0,0]),lambda d:d['nodes'][3].update(translation=[0,.8,0]),lambda d:d['accessors'][3].update(componentType=5126),lambda d:d['accessors'][4].update(byteOffset=999999),lambda d:d['nodes'][3].update(children=[1])]
        for change in changes:
            with self.subTest(change=change),self.assertRaises(skin.BuildError):self.parse(static_fixture.mutate(asset.make(),change))

    def test_reject_three_weights_nan_and_invalid_joint(self):
        original=asset.make();length=struct.unpack_from('<I',original,12)[0]
        import json
        doc=json.loads(original[20:20+length]);binary=original[28+length:]
        attrs=doc['meshes'][0]['primitives'][0]['attributes']
        def location(name):
            a=doc['accessors'][attrs[name]];v=doc['bufferViews'][a['bufferView']]
            return v.get('byteOffset',0)+a.get('byteOffset',0)
        for mode in ('three','nan','negative','sum','joint'):
            changed=bytearray(binary)
            if mode=='three':
                struct.pack_into('<4B',changed,location('JOINTS_0'),0,1,2,0)
                struct.pack_into('<4f',changed,location('WEIGHTS_0'),.4,.3,.3,0)
            elif mode=='joint':struct.pack_into('<4B',changed,location('JOINTS_0'),0,99,0,0)
            else:struct.pack_into('<4f',changed,location('WEIGHTS_0'),*(dict(nan=(float('nan'),.5,0,0),negative=(-.5,1.5,0,0),sum=(.2,.2,0,0))[mode]))
            with self.subTest(mode=mode),self.assertRaises(skin.BuildError):self.parse(skin.pack_glb(doc,bytes(changed)))

    def test_envelope_includes_subframe_translation_peak(self):
        import json
        original=asset.make();length=struct.unpack_from('<I',original,12)[0]
        doc=json.loads(original[20:20+length]);binary=bytearray(original[28+length:])
        def add(rows,width,shape):
            binary.extend(b'\0'*(-len(binary)%4));body=struct.pack('<'+str(len(rows)*width)+'f',*(x for row in rows for x in row))
            view=len(doc['bufferViews']);doc['bufferViews'].append(dict(buffer=0,byteOffset=len(binary),byteLength=len(body)));binary.extend(body)
            a=len(doc['accessors']);doc['accessors'].append(dict(bufferView=view,componentType=5126,count=len(rows),type=shape));return a
        times=add([(0.,),(.01,),(1.,)],1,'SCALAR');values=add([(0.,.75,0.),(0.,20.,0.),(0.,.75,0.)],3,'VEC3')
        clip=doc['animations'][0];sampler=len(clip['samplers']);clip['samplers'].append(dict(input=times,output=values,interpolation='LINEAR'));clip['channels'].append(dict(sampler=sampler,target=dict(node=3,path='translation')))
        doc['buffers'][0]['byteLength']=len(binary)
        _,info=self.parse(skin.pack_glb(doc,bytes(binary)))
        self.assertGreater(info['animation_radius'],2000)
        self.assertEqual(info['bounds_min'],[-info['animation_radius']]*3)
        self.assertLess(info['bind_bounds_max'][1],200)

    def test_nonidentity_bind_rotations_and_native_matrix_rows(self):
        import json,math
        original=asset.make();length=struct.unpack_from('<I',original,12)[0]
        doc=json.loads(original[20:20+length]);binary=bytearray(original[28+length:])
        doc['nodes'][1]['rotation']=[0,math.sin(.15),0,math.cos(.15)]
        doc['nodes'][2]['rotation']=[math.sin(.2),0,0,math.cos(.2)]
        cy,sy,cx,sx=math.cos(.3),math.sin(.3),math.cos(.4),math.sin(.4)
        root=[[cy,0,sy,0],[0,1,0,0],[-sy,0,cy,0],[0,0,0,1]]
        body_local=[[1,0,0,0],[0,cx,-sx,.3],[0,sx,cx,0],[0,0,0,1]]
        head_local=[[1,0,0,0],[0,1,0,.75],[0,0,1,0],[0,0,0,1]]
        def multiply(a,b):return [[sum(a[r][k]*b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]
        body=multiply(root,body_local);world=[root,body,multiply(body,head_local)]
        inverses=[[[w[c][r] for c in range(3)]+[-sum(w[k][r]*w[k][3] for k in range(3))] for r in range(3)]+[[0,0,0,1]] for w in world]
        a=doc['accessors'][doc['skins'][0]['inverseBindMatrices']];view=doc['bufferViews'][a['bufferView']];start=view.get('byteOffset',0)+a.get('byteOffset',0)
        for i,inv in enumerate(inverses):struct.pack_into('<16f',binary,start+i*64,*(inv[r][c] for c in range(4) for r in range(4)))
        model,_=self.parse(skin.pack_glb(doc,bytes(binary)));result=skin.build_bfres(template(),model,'rotated_bind')
        f=skin.base.Bfres(result);fm=f.dict(f.rel(0x20))[0][1];sk=f.rel(fm+12);bones=f.rel(sk+20);inv=f.rel(sk+28)
        rotations=[struct.unpack_from('>3f',result,bones+i*64+32) for i in range(3)]
        for actual,expected in zip(rotations,[(0,.3,0),(.4,0,0),(0,0,0)]):
            for x,y in zip(actual,expected):self.assertAlmostEqual(x,y,places=5)
        for i,w in enumerate(world):
            native=struct.unpack_from('>12f',result,inv+48*i)
            # Output inverse translation is cm, as are native bind translations.
            inverse=[[native[4*r+c] for c in range(4)] for r in range(3)]+[[0,0,0,1]]
            centimetres=[row[:] for row in w]
            for r in range(3):centimetres[r][3]*=100
            identity=multiply(centimetres,inverse)
            for r in range(4):
                for c in range(4):self.assertAlmostEqual(identity[r][c],float(r==c),places=4)
            self.assertEqual(f.u32(bones+i*64+16),0x1001) # no stale identity optimization bits

    def test_patricia_native_lookup(self):
        f=skin.base.Bfres(template());names=['head','body','root','a','aa','ab','Z','z','bone_31'];at=skin.append_dictionary(f,[(n,0x200+i) for i,n in enumerate(names)])
        def lookup(key):
            prev,cur=0,f.u16(at+12)
            def ref(i):return f.u32(at+8+16*i)
            while ref(prev)>ref(cur):
                prev=cur;bit=ref(cur);body=key.encode();side=((body[bit//8]>>(bit%8))&1) if bit//8<len(body) else 0;cur=f.u16(at+8+16*cur+4+side*2)
            p=at+8+16*cur
            return f.rel(p+12) if f.cstr(f.rel(p+8))==key else None
        for i,name in enumerate(names):self.assertEqual(lookup(name),0x200+i)
        self.assertIsNone(lookup('missing'))

if __name__=='__main__':unittest.main()
