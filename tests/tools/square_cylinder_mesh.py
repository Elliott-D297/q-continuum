#!/usr/bin/env python3
# tests/tools/square_cylinder_mesh.py > system/blockMeshDict: a 3-D channel round a square cylinder (side 0.1 m, 0.2 m
# span) for the turbulence journeys (t_ddes_cylinder): eight blocks round the hole; patches inlet, outlet, cylinder (wall),
# symTopBottom and symSides (plain patches the setup makes symmetry planes by their names).
xs, ys, zs = (-0.5, -0.05, 0.05, 1.5), (-0.5, -0.05, 0.05, 0.5), (0.0, 0.2)
nx, ny, nz = (18, 8, 50), (18, 8, 18), 8
def v(i, j, k): return k * 16 + j * 4 + i
pts = [(x, y, z) for z in zs for y in ys for x in xs]
blocks, faces = [], {'inlet': [], 'outlet': [], 'cylinder': [], 'symTopBottom': [], 'symSides': []}
for j in range(3):
    for i in range(3):
        if i == 1 and j == 1: continue                       # the cylinder
        c = [v(i, j, 0), v(i + 1, j, 0), v(i + 1, j + 1, 0), v(i, j + 1, 0), v(i, j, 1), v(i + 1, j, 1), v(i + 1, j + 1, 1), v(i, j + 1, 1)]
        blocks.append('hex (%s) (%d %d %d) simpleGrading (1 1 1)' % (' '.join(map(str, c)), nx[i], ny[j], nz))
        if i == 0: faces['inlet'].append((c[0], c[4], c[7], c[3]))
        if i == 2: faces['outlet'].append((c[1], c[2], c[6], c[5]))
        if j == 0: faces['symTopBottom'].append((c[0], c[1], c[5], c[4]))
        if j == 2: faces['symTopBottom'].append((c[3], c[7], c[6], c[2]))
        faces['symSides'] += [(c[0], c[3], c[2], c[1]), (c[4], c[5], c[6], c[7])]
        # the faces against the hole
        if i == 0 and j == 1: faces['cylinder'].append((c[1], c[2], c[6], c[5]))
        if i == 2 and j == 1: faces['cylinder'].append((c[0], c[4], c[7], c[3]))
        if j == 0 and i == 1: faces['cylinder'].append((c[3], c[7], c[6], c[2]))
        if j == 2 and i == 1: faces['cylinder'].append((c[0], c[1], c[5], c[4]))
print('FoamFile { version 2.0; format ascii; class dictionary; object blockMeshDict; }')
print('scale 1;')
print('vertices (\n%s\n);' % '\n'.join('    (%g %g %g)' % p for p in pts))
print('blocks (\n%s\n);' % '\n'.join('    ' + b for b in blocks))
print('boundary\n(')
for name, fl in faces.items():
    t = 'wall' if name == 'cylinder' else 'patch'
    print('    %s { type %s; faces ( %s ); }' % (name, t, ' '.join('(%d %d %d %d)' % f for f in fl)))
print(');')
