"""pai1: animation content. frame count u16, loop u8, pad u8, texture count u16, content count u16,
content offsets offset u32 (relative to the section start); then the texture list (string table, only
when texture patterns are animated), the content offsets (relative to the section start) and the contents.

A content: target name[28] (pane or material), tag count u8, content type u8 (0 pane, 1 material), pad[2],
u32 tag offsets (relative to the content start), then the tag blocks (tracks.tag_block)."""
import struct

from .. import binio
from . import tracks


def content(target_name, ctype, tags):
    """tags: [(tag magic, [encoded entries])] in order."""
    blocks = [tracks.tag_block(t, es) for t, es in tags]
    head = binio.name(target_name, 28) + bytes([len(blocks), ctype, 0, 0])
    first = len(head) + 4 * len(blocks)
    offs, blob = [], b""
    for b in blocks:
        offs.append(first + len(blob))
        blob += binio.pad4(b)
    return head + b"".join(struct.pack(">I", o) for o in offs) + blob


def write(frames, loop, textures, contents):
    """contents: [encoded content bytes]; textures: [.bflim file names] for texture pattern tracks."""
    texlist = binio.string_table(textures) if textures else b""
    table_off = 8 + 12 + len(texlist)
    head = struct.pack(">HBBHHI", frames, 1 if loop else 0, 0, len(textures), len(contents), table_off)
    first = table_off + 4 * len(contents)
    offs, blob = [], b""
    for c in contents:
        offs.append(first + len(blob))
        blob += binio.pad4(c)
    return binio.section(b"pai1", head + texlist + b"".join(struct.pack(">I", o) for o in offs) + blob)
