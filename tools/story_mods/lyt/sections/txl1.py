"""txl1: texture list. count u16, pad u16, then a string table (u32 offsets relative to the table start,
NUL-terminated .bflim file names). Materials refer to textures by their index in this list."""
import struct

from .. import binio


def write(names):
    return binio.section(b"txl1", struct.pack(">HH", len(names), 0) + binio.string_table(names))
