#!/usr/bin/env python3
"""A game-free check of dksh_cache (issue #88: the Windows installer builds the shader cache too): writes a WGS1 file
with two hand-written GLSL shaders (no game code), runs `dksh_cache build` on it through build.sh and checks that both
compiled into the WDK1 file. Used by the Windows CI; runs anywhere build.sh does.

    python3 tools/switch/dksh_cache/smoke.py [WORK_DIR]
"""
import os
import struct
import subprocess
import sys
import zlib

# the shape the Latte decompiler gives a vertex shader (LatteDecompilerEmitGLSLHeader.hpp): glsl_to_deko keeps the
# #ifdef VULKAN branch and its SET_POSITION
VERTEX = """#version 450
#ifdef VULKAN
#define SET_POSITION(_v) gl_Position = _v; gl_Position.z = (gl_Position.z + gl_Position.w) / 2.0
#else
#define SET_POSITION(_v) gl_Position = _v
#endif
layout(location = 0) in vec4 attrDataSem0;
layout(location = 0) out vec4 passParameterSem0;
void main()
{
    passParameterSem0 = attrDataSem0.zyxw;
    SET_POSITION(vec4(attrDataSem0.xy, 0.5, 1.0));
}
"""
PIXEL = """#version 450
layout(location = 0) in vec4 passParameterSem0;
layout(location = 0) out vec4 passPixelColor0;
void main()
{
    passPixelColor0 = passParameterSem0 * 0.5 + 0.25;
}
"""


def record(vertex, text, h):
    """record 1 of shadercache_gl.bin (runtime/src/gfx/deko/shaders_dk.cpp cache_gl_source)"""
    data = text.encode()
    packed = zlib.compress(data, 6)
    return struct.pack("<BBQII", 1, vertex, h, len(packed), len(data)) + packed


def main():
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
    work = sys.argv[1] if len(sys.argv) > 1 else os.path.join("build", "dksh_cache-smoke")
    os.makedirs(os.path.join(root, work), exist_ok=True)
    gl, dksh = os.path.join(work, "smoke_gl.bin"), os.path.join(work, "smoke_dksh.bin")
    with open(os.path.join(root, gl), "wb") as f:
        f.write(b"WGS1" + record(1, VERTEX, 0x1111) + record(0, PIXEL, 0x2222))
    out = subprocess.run(["bash", os.path.join("tools", "switch", "dksh_cache", "build.sh"), "build", gl, dksh],
                         cwd=root, capture_output=True, text=True)
    sys.stdout.write(out.stdout)
    sys.stderr.write(out.stderr)
    if out.returncode != 0 or "compiled 2 / 2" not in out.stdout or "2 with DKSH" not in out.stdout:
        sys.exit("dksh_cache smoke test: FAILED (exit %d)" % out.returncode)
    print("dksh_cache smoke test: ok (2 shaders compiled)")


if __name__ == "__main__":
    main()
