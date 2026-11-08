#!/usr/bin/env python3
"""Original geometric carved post and painted wood texture. MPL-2.0."""
import json
import pathlib
import struct
import zlib


def stored_zlib(data):
    """zlib stream with uncompressed (stored) deflate blocks: the same bytes on every platform, unlike
    zlib.compress, whose output depends on the zlib build (Windows Python uses zlib-ng)."""
    out = bytearray(b'\x78\x01')
    for start in range(0, len(data), 65535):
        block = data[start:start + 65535]
        out += bytes([1 if start + 65535 >= len(data) else 0]) + struct.pack('<HH', len(block), len(block) ^ 0xffff) + block
    return bytes(out + struct.pack('>I', zlib.adler32(data) & 0xffffffff))


def make():
    positions, normals, uv, indices = [], [], [], []
    # Six independently textured faces, post rooted at local ground origin.
    faces = [([( -.3,0,.3),(.3,0,.3),(.3,1.6,.3),(-.3,1.6,.3)],(0,0,1)),
             ([(.3,0,-.3),(-.3,0,-.3),(-.3,1.6,-.3),(.3,1.6,-.3)],(0,0,-1)),
             ([(.3,0,.3),(.3,0,-.3),(.3,1.6,-.3),(.3,1.6,.3)],(1,0,0)),
             ([(-.3,0,-.3),(-.3,0,.3),(-.3,1.6,.3),(-.3,1.6,-.3)],(-1,0,0)),
             ([(-.3,1.6,.3),(.3,1.6,.3),(.3,1.6,-.3),(-.3,1.6,-.3)],(0,1,0)),
             ([(-.3,0,-.3),(.3,0,-.3),(.3,0,.3),(-.3,0,.3)],(0,-1,0))]
    for vertices, normal in faces:
        start=len(positions); positions.extend(vertices); normals.extend([normal]*4); uv.extend([(0,1),(1,1),(1,0),(0,0)]); indices.extend(start+i for i in (0,1,2,0,2,3))
    rgba=bytearray()
    for y in range(128):
        for x in range(128):
            grain=((x//7+y//19)%3)*8
            mark=abs(x-64)+abs(y-48)<22 or (45<x<83 and 79<y<85)
            rgba.extend((52,85,100,255) if mark else (145+grain,93+grain,48+grain,255))
    def chunk(kind,body): return struct.pack('>I',len(body))+kind+body+struct.pack('>I',zlib.crc32(kind+body)&0xffffffff)
    rows=b''.join(b'\0'+rgba[y*512:(y+1)*512] for y in range(128))
    image=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',128,128,8,6,0,0,0))+chunk(b'IDAT',stored_zlib(rows))+chunk(b'IEND',b'')
    parts=[struct.pack('<%df'%(len(positions)*3),*(v for p in positions for v in p)),struct.pack('<%df'%(len(normals)*3),*(v for p in normals for v in p)),struct.pack('<%df'%(len(uv)*2),*(v for p in uv for v in p)),struct.pack('<%dH'%len(indices),*indices),image]
    binary=bytearray(); views=[]
    for part in parts:
        binary.extend(b'\0'*(-len(binary)%4)); views.append({'buffer':0,'byteOffset':len(binary),'byteLength':len(part)}); binary.extend(part)
    doc={'asset':{'version':'2.0','generator':'Original MPL-2.0 carved post'},'scene':0,'scenes':[{'nodes':[0]}],'nodes':[{'mesh':0}], 'buffers':[{'byteLength':len(binary)}],'bufferViews':views,'accessors':[{'bufferView':i,'componentType':5123 if i==3 else 5126,'count':len(indices) if i==3 else len(positions),'type':('VEC3','VEC3','VEC2','SCALAR')[i]} for i in range(4)],'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1,'TEXCOORD_0':2},'indices':3,'material':0}]}],'materials':[{'pbrMetallicRoughness':{'baseColorTexture':{'index':0},'metallicFactor':0,'roughnessFactor':1}}],'textures':[{'source':0}],'images':[{'bufferView':4,'mimeType':'image/png'}]}
    text=json.dumps(doc,separators=(',',':')).encode(); text+=b' '*(-len(text)%4); binary.extend(b'\0'*(-len(binary)%4))
    body=struct.pack('<I4s',len(text),b'JSON')+text+struct.pack('<I4s',len(binary),b'BIN\0')+binary
    return b'glTF'+struct.pack('<II',2,len(body)+12)+body


if __name__=='__main__': pathlib.Path(__file__).with_name('carved_post.glb').write_bytes(make())
