#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.
"""Generate the RSX shader set used by the PS3 hardware renderer and self-test.

PSL1GHT has no runtime shader compiler, so every program the renderer can use is written here as
NV40 assembly, assembled offline with cgcomp -a (which needs no Cg toolkit), and embedded in
RSXShaders.inc. Run again after changing this file:

    python3 PS3/rsx/make-shaders.py $PS3DEV/bin/cgcomp PS3/rsx/RSXShaders.inc

NV40 fragment-program rule this generator respects: one instruction may read at most ONE
interpolated input (f[...]) and at most one inline constant. Inputs are copied to temporaries first.

Vertex layout (see RSXCore.h, RSXVertex): attribute 0 = position (x,y,z,w, already clip space),
3 = color0 (RGBA8 normalized), 4 = color1 (RGBA8), 8 = texcoord (s, t, 0, q), 9 = fog (x).
Vertex constants c[0] = texture env color, c[1] = fog color, c[2] = source blend multiplier.
They reach the fragment program as constant varyings TEX2, TEX3, TEX4, which avoids patching
fragment-program constants in RSX memory between draws.
"""
import os, subprocess, sys, tempfile

VP = """!!ARBvp1.0
#var float4 envColor : : c[0] : 1 : 1
#var float4 fogColor : : c[1] : 1 : 1
#var float4 blendMul : : c[2] : 1 : 1
MOV o[HPOS], v[OPOS];
MOV o[COL0], v[COL0];
MOV o[COL1], v[COL1];
MOV o[TEX0], v[TEX0];
MOV o[TEX1], v[TEX1];
MOV o[TEX2], c[0];
MOV o[TEX3], c[1];
MOV o[TEX4], c[2];
END
"""

# Texture functions, in GE order.
MODULATE, DECAL, BLEND, REPLACE, ADD = range(5)

def fragment(tex, func, rgba, doubling, fog, blendmul, clear=False):
    """Body for one variant. R0 ends up as the output color."""
    a = ['!!ARBfp1.0', 'OPTION NV_fragment_program2;']
    if doubling:
        a.append('#const c[0] = 2 2 2 1')
    a.append('MOVR R1, f[COL0];')
    if clear or not tex:
        a.append('MOVR R0, R1;')
    else:
        a.append('TXP R0, f[TEX0], texture[0], 2D;')
        if func == MODULATE:
            a.append('MULR R0.xyz, R0, R1;')
            a.append('MULR R0.w, R0, R1;' if rgba else 'MOVR R0.w, R1;')
        elif func == DECAL:
            if rgba:
                a.append('LRPR R0.xyz, R0.w, R0, R1;')
            a.append('MOVR R0.w, R1;')
        elif func == BLEND:
            a.append('MOVR R2, f[TEX2];')
            a.append('LRPR R2.xyz, R0, R2, R1;')
            a.append('MULR R2.w, R0, R1;' if rgba else 'MOVR R2.w, R1;')
            a.append('MOVR R0, R2;')
        elif func == REPLACE:
            if not rgba:
                a.append('MOVR R0.w, R1;')
        elif func == ADD:
            a.append('ADDR_SAT R0.xyz, R0, R1;')
            a.append('MULR R0.w, R0, R1;' if rgba else 'MOVR R0.w, R1;')
    if not clear:
        # Secondary (specular) color; zero when the GE doesn't use it.
        a.append('MOVR R3, f[COL1];')
        a.append('ADDR_SAT R0.xyz, R0, R3;')
        if doubling:
            a.append('MULR_SAT R0.xyz, R0, c[0];')
        if fog:
            a.append('MOVR R2, f[TEX1];')
            a.append('MOVR R3, f[TEX3];')
            a.append('LRPR R0.xyz, R2.x, R0, R3;')
        if blendmul:
            a.append('MOVR R2, f[TEX4];')
            a.append('MULR R0.xyz, R0, R2;')
    a.append('MOVR o[COLR], R0;')
    a.append('END')
    return '\n'.join(a) + '\n'

def burn():
    """Stress-test load: a texture fetch and ~200 ALU instructions per pixel (sin, cos, rsq, log,
    exp, dot, mad), so the shader units run flat out. Deterministic for the same inputs."""
    a = ['!!ARBfp1.0', 'OPTION NV_fragment_program2;', 'MOVR R1, f[COL0];',
         'TXP R0, f[TEX0], texture[0], 2D;', 'MOVR R2, f[TEX0];']
    for i in range(24):
        a += ['MADR R0, R0, R1, R2;', 'SINR R3.x, R0.x;', 'COSR R3.y, R0.y;', 'RSQR R3.z, |R1.z|;',
              'LG2R R3.w, |R0.w|;', 'EX2R R4.x, R3.x;', 'DP4R R4.y, R3, R0;', 'MADR R0, R3, R1, R0;', 'FRCR R0, R0;']
    a += ['MOVR o[COLR], R0;', 'END']
    return '\n'.join(a) + '\n'

def variants():
    """Yield (index, name, source). Index layout must match RSXCore::FragmentIndex()."""
    yield 0, 'clear', fragment(False, 0, False, False, False, False, clear=True)
    for tex in (0, 1):
        for func in range(5):
            if not tex and func:
                continue
            for rgba in (0, 1):
                if not tex and rgba:
                    continue
                for doubling in (0, 1):
                    for fog in (0, 1):
                        for blendmul in (0, 1):
                            index = 1 + (((tex * 5 + func) * 2 + rgba) * 2 + doubling) * 4 + fog * 2 + blendmul
                            name = 'tex%d_f%d_rgba%d_dbl%d_fog%d_mul%d' % (tex, func, rgba, doubling, fog, blendmul)
                            yield index, name, fragment(tex, func, rgba, doubling, fog, blendmul)
    yield 161, 'burn', burn()

def assemble(cgcomp, kind, source):
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, 'in.asm')
        out = os.path.join(d, 'out.bin')
        open(src, 'w').write(source)
        r = subprocess.run([cgcomp, '-' + kind, '-a', src, out], capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            raise SystemExit('cgcomp failed for:\n%s\n%s%s' % (source, r.stdout, r.stderr))
        return open(out, 'rb').read()

def emit(name, data):
    lines = ['alignas(16) static const unsigned char %s[%d] = {' % (name, len(data))]
    for i in range(0, len(data), 16):
        lines.append('\t' + ', '.join('0x%02x' % b for b in data[i:i + 16]) + ',')
    lines.append('};')
    return '\n'.join(lines)

def main():
    cgcomp, out = sys.argv[1], sys.argv[2]
    parts = ['// Generated by PS3/rsx/make-shaders.py with cgcomp -a. Do not edit.', '#pragma once', '']
    parts.append(emit('rsx_vp_main', assemble(cgcomp, 'v', VP)))
    table = {}
    for index, name, source in variants():
        sym = 'rsx_fp_' + name
        parts.append('// %d: %s' % (index, name))
        parts.append(emit(sym, assemble(cgcomp, 'f', source)))
        table[index] = sym
    count = max(table) + 1
    parts.append('static const int RSX_FRAGMENT_PROGRAM_COUNT = %d;' % count)
    parts.append('static const int RSX_FRAGMENT_BURN = 161;')
    parts.append('static const unsigned char *const rsx_fp_table[%d] = {' % count)
    for i in range(count):
        parts.append('\t%s,' % table.get(i, 'nullptr'))
    parts.append('};')
    open(out, 'w').write('\n'.join(parts) + '\n')
    print('%d fragment programs written to %s' % (len(table), out))

if __name__ == '__main__':
    main()
