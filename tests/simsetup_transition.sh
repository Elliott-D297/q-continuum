#!/bin/bash
# tests/simsetup_transition.sh <test_simsetup> <OpenFOAM bashrc> <scratch>
# Step 5 of the setup plan (2026-11-01): k-omega SST with transition (Langtry-Menter) finds where a flat plate's boundary
# layer turns turbulent. The ERCOFTAC T3A plate (5.4 m/s, 3.3 % turbulence coming in at a viscosity ratio of 12), set up
# by sim_write and run by simpleFoam on a 2-D mesh with y+ under 1. The skin friction along the plate must start on the
# laminar Blasius line (0.664 Re_x^-1/2, within 15 % at Re_x 3e4), end on the turbulent one (0.0592 Re_x^-1/5, within 15 %
# at Re_x 6e5), and turn between them: the onset (Cf's least value) between Re_x 5e4 and 4e5 (T3A's measured onset is
# about 1.4e5).
bin=$(realpath "$1"); bashrc=$2; d=$(mktemp -d "$3/simsetup_transition.XXXXXX")
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --
source "$bashrc" > /dev/null 2>&1
set -u
mkdir -p "$d/plate/system"
cat > "$d/plate/system/blockMeshDict" <<'DICT'
FoamFile { version 2.0; format ascii; class dictionary; object blockMeshDict; }
scale 1;
// 0.1 m upstream (a symmetry plane), then the plate to 2 m; 0.3 m high, the first cell 24 µm (y+ about 0.2)
vertices ( (-0.1 0 0) (0 0 0) (2 0 0) (2 0.3 0) (0 0.3 0) (-0.1 0.3 0)
           (-0.1 0 0.01) (0 0 0.01) (2 0 0.01) (2 0.3 0.01) (0 0.3 0.01) (-0.1 0.3 0.01) );
blocks
(
    hex (0 1 4 5 6 7 10 11) (20 100 1) simpleGrading (0.2 811 1)
    hex (1 2 3 4 7 8 9 10) (300 100 1) simpleGrading (20 811 1)
);
boundary
(
    inlet        { type patch;         faces ( (0 6 11 5) ); }
    outlet       { type patch;         faces ( (2 3 9 8) ); }
    top          { type patch;         faces ( (5 11 10 4) (4 10 9 3) ); }
    front        { type symmetryPlane; faces ( (0 1 7 6) ); }
    plate        { type wall;          faces ( (1 2 8 7) ); }
    frontAndBack { type empty;         faces ( (0 5 4 1) (1 4 3 2) (6 7 10 11) (7 8 9 10) ); }
);
DICT
printf 'FoamFile { version 2.0; format ascii; class dictionary; object controlDict; }\napplication simpleFoam; startTime 0; endTime 1; deltaT 1; writeControl timeStep; writeInterval 1;\n' > "$d/plate/system/controlDict"
cd "$d/plate" && blockMesh > log.blockMesh 2>&1 || { echo "blockMesh failed"; exit 1; }
"$bin" plate "$d/plate" || exit 1
t0=$(date +%s)
timeout 1800 simpleFoam > log.simpleFoam 2>&1 || { echo "simpleFoam failed"; grep -m1 -A4 "FOAM FATAL" log.simpleFoam; tail -3 log.simpleFoam; exit 1; }
its=$(grep -o "converged in [0-9]*" log.simpleFoam | grep -o "[0-9]*$")
cat > system/cfDict <<'DICT'
FoamFile { version 2.0; format ascii; class dictionary; object cfDict; }
functions
{
    wss { type wallShearStress; libs (fieldFunctionObjects); patches (plate); }
    cf  { type surfaceFieldValue; libs (fieldFunctionObjects); regionType patch; name plate; operation none;
          fields (wallShearStress); writeFields yes; surfaceFormat raw; }
}
DICT
simpleFoam -postProcess -latestTime -dict system/cfDict > log.cf 2>&1
python3 - "$(( $(date +%s) - t0 ))" "${its:-not converged}" <<'PY'
import sys, glob, math
secs, its = sys.argv[1], sys.argv[2]
f = sorted(glob.glob('postProcessing/cf/*/*/*wallShearStress*'))[-1]
U, nu = 5.4, 1.5e-5
pts = []
for l in open(f):
    if not l.strip() or l.startswith('#'): continue
    c = list(map(float, l.split()))
    x, tx = c[0], c[3]
    if x > 1e-4: pts.append((U * x / nu, abs(tx) / (0.5 * U * U)))   # kinematic wall shear: Cf = |tau| / (U^2 / 2)
pts.sort()
def cf_at(re):
    for i in range(1, len(pts)):
        if pts[i][0] >= re:
            a, b = pts[i - 1], pts[i]; t = (re - a[0]) / (b[0] - a[0]); return a[1] + t * (b[1] - a[1])
lam, tur = cf_at(3e4), cf_at(6e5)
blas, turb = 0.664 / math.sqrt(3e4), 0.0592 * 6e5 ** -0.2
# the onset: Cf's least value past the leading edge (it falls on the laminar line, then rises through transition)
mid = [p for p in pts if 2e4 < p[0] < 6e5]
onset = min(mid, key=lambda p: p[1])[0]
el, et = abs(lam - blas) / blas, abs(tur - turb) / turb
print('T3A (%s s, converged in %s): Cf %.5f vs Blasius %.5f at Re_x 3e4 (%.0f %%), %.5f vs turbulent %.5f at 6e5 (%.0f %%); onset at Re_x %.3g (x %.3f m)'
      % (secs, its, lam, blas, 100 * el, tur, turb, 100 * et, onset, onset * nu / U))
sys.exit(0 if el < 0.15 and et < 0.15 and 5e4 < onset < 4e5 else 1)
PY
