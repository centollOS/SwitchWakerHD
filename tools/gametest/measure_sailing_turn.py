#!/usr/bin/env python3
"""Measure HD rudder yaw in a headless isolated guest, before and after #169.

Usage: python3 tools/gametest/measure_sailing_turn.py /path/to/generated/code
Uses your locally recompiled game function, not a game-code fixture in the repo.
This is an isolated setMoveAngle test, NOT a full gameplay/weather simulation.
Two downstream control-angle callees are stubbed; neither changes shape_angle.y.
Output stays in build/sail-169 (<10 MB), with no saves, states, graphics or audio.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('gen', type=Path)
    args = parser.parse_args()
    args.gen = args.gen.resolve()
    repo = Path(__file__).resolve().parents[2]
    os.chdir(repo)
    out = repo / 'build/sail-169'
    out.mkdir(parents=True, exist_ok=True)
    function = None
    for path in args.gen.glob('code_*.c'):
        match = re.search(r'^void f_0247D4B0\(.*?^}', path.read_text(), re.M | re.S)
        if match:
            function = match.group()
            break
    if function is None:
        raise SystemExit('setMoveAngle 0247D4B0 not found; use USA v0 generated code')
    # Refuse a fixture that already has this site, to keep baseline honest.
    if 'site_0247D4E8' in function:
        raise SystemExit('Use generated code from before the #169 steering hook')
    marker = '    c->f[11].ps0 = fabs(c->f[10].ps0);'
    if function.count(marker) != 1:
        raise SystemExit('Unexpected setMoveAngle translation at 0247D4E8')
    prefix = '''#include "ppc.h"
#undef PPC_ENTER
#undef PPC_LOOP
#define PPC_ENTER(x) ((void)0)
#define PPC_LOOP() ((void)0)
void site_0247D4E8(Cpu*);
void f_024752C4(Cpu*);
void f_024751E0(Cpu*);
void f_0247D65C(Cpu*);
'''
    run(['uptime'])
    if os.getloadavg()[0] > 45:
        raise SystemExit('Load >45: retry when idle')
    run(['clang', '-Iruntime/include', '-c', 'runtime/src/guest_addr_identity.c', '-o', out / 'identity.o'])
    for variant in ('before', 'after'):
        body = function if variant == 'before' else function.replace(marker, '    site_0247D4E8(c);\n' + marker)
        source = out / f'{variant}.c'
        source.write_text(prefix + body + '\n')
        obj = out / f'{variant}.o'
        run(['clang', '-std=c11', '-Iruntime/include', '-c', source, '-o', obj])
        binary = out / f'{variant}_turn_test'
        defines = ['-DTRAVERSAL_GUEST_STEERING']
        if variant == 'before':
            defines.append('-DTRAVERSAL_BEFORE')
        run(['clang++', '-std=c++20', '-UNDEBUG', *defines, '-Iruntime/src', '-Iruntime/include',
             'runtime/tools/traversal_runtime_test.cpp', 'runtime/src/mods/traversal.cpp',
             'runtime/src/mods/traversal_settings.cpp', obj, out / 'identity.o', '-o', binary])
        with (out / f'{variant}.txt').open('w') as log:
            run([binary], stdout=log)
        print(f'{variant}:\n{(out / f"{variant}.txt").read_text()}', flush=True)


if __name__ == '__main__':
    main()
