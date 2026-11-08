"""fnl1: font list, the same layout as txl1 (count u16, pad u16, string table of font file names).
Text panes (txt1) refer to fonts by their index here. Owned by the text pane work (lyt-text): the layout of
this section is taken over from txl1 and still has to be compared with the game's own fnl1 sections."""
import struct

from .. import binio


def write(names):
    return binio.section(b"fnl1", struct.pack(">HH", len(names), 0) + binio.string_table(names))
