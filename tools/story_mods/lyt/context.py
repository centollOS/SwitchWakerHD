"""Build context: the resource tables (textures, fonts, materials) shared by the section writers.

Pane writers ask the context for indices; the bflyt builder writes txl1 / fnl1 / mat1 from the tables
afterwards, so a pane can add the material it needs while it is written (pictures, windows, text panes).
"""
from . import binio


class Context:
    def __init__(self, desc):
        self.desc = desc
        self.textures = []   # file names in txl1, e.g. "StoryPlate_00^l.bflim"
        self.fonts = []      # file names in fnl1, e.g. a game font (txt1 / fnl1 writers)
        self.materials = []  # (name, encoded material bytes)
        self._material_index = {}

    def texture(self, tex):
        """txl1 index of a texture named in the description's "textures" (or a full .bflim file name)."""
        fname = tex if tex.endswith(".bflim") else self.desc.texture_file(tex)
        if fname not in self.textures:
            self.textures.append(fname)
        return self.textures.index(fname)

    def font(self, fname):
        """fnl1 index of a font file name (used by the text pane writer)."""
        if fname not in self.fonts:
            self.fonts.append(fname)
        return self.fonts.index(fname)

    def material(self, mat_name):
        """mat1 index of a material named in the description's "materials"; encoded on first use."""
        if mat_name not in self._material_index:
            from .sections import mat1
            spec = self.desc.materials.get(mat_name)
            if spec is None:
                raise KeyError("unknown material %r" % mat_name)
            self._material_index[mat_name] = len(self.materials)
            self.materials.append((mat_name, mat1.encode(mat_name, spec, self)))
        return self._material_index[mat_name]

    def add_material(self, mat_name, encoded):
        """Register an already encoded material (window frames and text panes build their own)."""
        if mat_name in self._material_index:
            return self._material_index[mat_name]
        binio.name(mat_name, 28)  # validates the length
        self._material_index[mat_name] = len(self.materials)
        self.materials.append((mat_name, encoded))
        return self._material_index[mat_name]
