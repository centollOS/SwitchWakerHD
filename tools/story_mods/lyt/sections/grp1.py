"""grp1 / grs1 / gre1: groups. A group: name[24], pane count u16, pad u16, pane names[24].
The layout has one RootGroup; the description's groups are its children (between grs1 and gre1)."""
import struct

from .. import binio


def group(name, panes):
    return binio.section(b"grp1", binio.name(name, 24) + struct.pack(">HH", len(panes), 0) +
                         b"".join(binio.name(p, 24) for p in panes))


def write(groups):
    out = [group("RootGroup", [])]
    if groups:
        out.append(binio.section(b"grs1", b""))
        out += [group(n, members) for n, members in groups.items()]
        out.append(binio.section(b"gre1", b""))
    return out
