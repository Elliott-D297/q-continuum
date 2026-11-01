#!/usr/bin/env python3
# tests/tools/nozzle_mesh.py conv|cd > system/blockMeshDict: a 2-D half nozzle for the compressible setup's tests
# (simsetup_gas.sh, journey t_gas_nozzle): patches inlet, outlet, wall (a spline), axis (symmetryPlane), frontAndBack.
import sys, math
kind = sys.argv[1]
if kind == 'cd':      # inlet 0.1, throat 0.05 at x = 0.4, exit 0.075 at x = 1: cosine blends, flat ends
    L, xt = 1.0, 0.4
    def y(x):
        if x <= xt: return 0.05 + 0.025 * (1 + math.cos(math.pi * x / xt))
        return 0.05 + 0.0125 * (1 - math.cos(math.pi * (x - xt) / (L - xt)))
    nx, ny = 300, 40
else:                 # converging: 0.1 to 0.05 over the middle third of 0.6 m
    L = 0.6
    def y(x):
        if x <= 0.2: return 0.1
        if x >= 0.4: return 0.05
        return 0.075 + 0.025 * math.cos(math.pi * (x - 0.2) / 0.2)
    nx, ny = 240, 40
pts = lambda z: ' '.join('(%.6f %.6f %g)' % (L * i / 60, y(L * i / 60), z) for i in range(1, 60))
print('FoamFile { version 2.0; format ascii; class dictionary; object blockMeshDict; }')
print('scale 1;')
print('vertices ( (0 0 0) (%g 0 0) (%g %g 0) (0 %g 0) (0 0 0.01) (%g 0 0.01) (%g %g 0.01) (0 %g 0.01) );' % (L, L, y(L), y(0), L, L, y(L), y(0)))
print('blocks ( hex (0 1 2 3 4 5 6 7) (%d %d 1) simpleGrading (1 1 1) );' % (nx, ny))
print('edges ( spline 3 2 ( %s ) spline 7 6 ( %s ) );' % (pts(0), pts(0.01)))
print('''boundary
(
    inlet        { type patch;         faces ( (0 4 7 3) ); }
    outlet       { type patch;         faces ( (1 2 6 5) ); }
    wall         { type wall;          faces ( (3 7 6 2) ); }
    axis         { type symmetryPlane; faces ( (0 1 5 4) ); }
    frontAndBack { type empty;         faces ( (0 3 2 1) (4 5 6 7) ); }
);''')
