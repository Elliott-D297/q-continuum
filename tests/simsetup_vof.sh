#!/bin/bash
# tests/simsetup_vof.sh <test_simsetup> <OpenFOAM bashrc> <scratch>
# Step 8 of the setup plan (2026-11-08): free surface (interFoam), set up by sim_write from the Free surface template.
# 1. The v2606 interFoam/laminar/damBreak mesh, its own physics files taken away, set up with the water column as a box:
#    run to 0.15 s beside the tutorial as shipped; the water front along the floor (the bottom row's water fraction times
#    the cell width) agrees within 1 % at every write (it came out the same, 2026-11-08).
# 2. A finer dam break with no obstacle (260 x 140 cells, the same column, a = 0.1461 m) against Martin and Moyce (1952,
#    n^2 = 2): the front's speed dZ/dT over T = 2.2 to 3.11 within 5 % of theirs, the front within 12 % at T 2.2, 2.51,
#    2.83 and 3.11 (simulations lead the experiment by its gate's opening; 6 to 10 % here), Z = x / a, T = t sqrt(2 g / a).
# 3. A tank filled through a water inlet below the surface: the water's volume (the water fraction's volume average report)
#    gains the inflow times the time within 1 %.
bin=$(realpath "$1"); bashrc=$2; d=$(mktemp -d "$3/simsetup_vof.XXXXXX"); here=$(cd "$(dirname "$0")" && pwd)
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --
source "$bashrc" > /dev/null 2>&1
set -u
T="$FOAM_TUTORIALS/multiphase/interFoam/laminar/damBreak/damBreak"
CD='FoamFile { version 2.0; format ascii; class dictionary; object controlDict; }\napplication interFoam;\nstartTime 0;\nendTime 1;\ndeltaT 1;\nwriteControl timeStep;\nwriteInterval 1;\n'
ascii() { ( cd "$1" && foamDictionary system/controlDict -entry writeFormat -set ascii > /dev/null 2>&1; foamFormatConvert > /dev/null 2>&1; postProcess -func writeCellCentres -time 0 > /dev/null 2>&1 ); }
# ---- 1 the tutorial and the setup on its mesh
cp -r "$T" "$d/tut" && cd "$d/tut" || exit 1
cp -r 0.orig 0 && blockMesh > log.blockMesh 2>&1 && setFields > log.setFields 2>&1 || { echo "tutorial mesh/setFields failed"; exit 1; }
sed -i 's/^endTime .*/endTime         0.15;/; s/^writeInterval .*/writeInterval   0.005;/; s/#sinclude.*//' system/controlDict
timeout 900 interFoam > log.interFoam 2>&1 || { echo "tutorial interFoam failed"; tail -5 log.interFoam; exit 1; }
cp -r "$T" "$d/qc" && cd "$d/qc" || exit 1
rm -rf 0.orig Allrun Allclean constant/transportProperties constant/turbulenceProperties constant/g system/fvSchemes system/fvSolution system/setFieldsDict system/sampling
blockMesh > log.blockMesh 2>&1 || exit 1
"$bin" dambreak "$d/qc" 0.15 0.005 || { echo "setup failed"; exit 1; }
setFields > log.setFields 2>&1 || { echo "setFields failed"; tail -5 log.setFields; exit 1; }
timeout 900 interFoam > log.interFoam 2>&1 || { echo "Q-Continuum's interFoam failed"; grep -m1 -A6 "FOAM FATAL" log.interFoam; tail -5 log.interFoam; exit 1; }
ascii "$d/tut"; ascii "$d/qc"
# ---- 2 the finer dam break, no obstacle
mkdir -p "$d/fine/system" "$d/fine/constant" && cd "$d/fine" || exit 1
cat > system/blockMeshDict <<'BM'
FoamFile { version 2.0; format ascii; class dictionary; object blockMeshDict; }
vertices ( (0 0 0) (0.65 0 0) (0.65 0.35 0) (0 0.35 0) (0 0 0.005) (0.65 0 0.005) (0.65 0.35 0.005) (0 0.35 0.005) );
blocks ( hex (0 1 2 3 4 5 6 7) (260 140 1) simpleGrading (1 1 1) );
boundary
(
    leftWall { type wall; faces ((0 4 7 3)); }
    rightWall { type wall; faces ((1 2 6 5)); }
    lowerWall { type wall; faces ((0 1 5 4)); }
    atmosphere { type patch; faces ((3 7 6 2)); }
    defaultFaces { type empty; faces ((0 3 2 1) (4 5 6 7)); }
);
BM
printf "$CD" > system/controlDict
blockMesh > log.blockMesh 2>&1 && "$bin" dambreak "$d/fine" 0.28 0.01 > /dev/null && setFields > log.setFields 2>&1 || { echo "fine dam break setup failed"; exit 1; }
timeout 1200 interFoam > log.interFoam 2>&1 || { echo "fine interFoam failed"; tail -5 log.interFoam; exit 1; }
ascii "$d/fine"
# ---- 3 the tank
mkdir -p "$d/tank/system" "$d/tank/constant" && cd "$d/tank" || exit 1
cat > system/blockMeshDict <<'BM'
FoamFile { version 2.0; format ascii; class dictionary; object blockMeshDict; }
vertices ( (0 0 0) (0.2 0 0) (0.2 0.05 0) (0 0.05 0) (0 0 0.01) (0.2 0 0.01) (0.2 0.05 0.01) (0 0.05 0.01)
           (0.2 0.4 0) (0 0.4 0) (0.2 0.4 0.01) (0 0.4 0.01) );
blocks ( hex (0 1 2 3 4 5 6 7) (40 10 1) simpleGrading (1 1 1) hex (3 2 8 9 7 6 10 11) (40 70 1) simpleGrading (1 1 1) );
boundary
(
    inlet { type patch; faces ((0 4 7 3)); }
    walls { type wall; faces ((1 2 6 5) (2 8 10 6) (3 7 11 9) (0 1 5 4)); }
    top { type patch; faces ((9 11 10 8)); }
    frontAndBack { type empty; faces ((0 3 2 1) (4 5 6 7) (3 9 8 2) (7 6 10 11)); }
);
BM
printf "$CD" > system/controlDict
blockMesh > log.blockMesh 2>&1 && "$bin" tank "$d/tank" 0.2 > /dev/null && setFields > log.setFields 2>&1 || { echo "tank setup failed"; exit 1; }
timeout 900 interFoam > log.interFoam 2>&1 || { echo "tank interFoam failed"; tail -5 log.interFoam; exit 1; }
python3 - "$d" <<'PY'
import sys, re, glob, math
d = sys.argv[1]
a, g = 0.1461, 9.81
k = math.sqrt(2 * g / a)
def field(path):
    s = open(path, errors='replace').read()
    b = s[s.index('internalField'):s.index('boundaryField')]
    if 'nonuniform' not in b: return None
    vec = re.findall(r'\(([-0-9.eE+]+) ([-0-9.eE+]+) ([-0-9.eE+]+)\)', b)
    if vec: return [tuple(map(float, x)) for x in vec]
    return [float(x) for x in b[b.index('(') + 1:b.rindex(')')].split()]
def fronts(case, xmax, n):
    """t -> the water's length along the floor: the bottom row's water fraction times the cell width"""
    C = field(case + '/0/C'); ymin = min(c[1] for c in C); dx = xmax / n
    dy = min(c[1] for c in C if c[1] > ymin + 1e-5) - ymin             # the bottom row only: half a cell up
    out = {}
    for t in sorted((p.rsplit('/', 1)[1] for p in glob.glob(case + '/[0-9]*') if re.match(r'^[0-9.e+-]+$', p.rsplit('/', 1)[1])), key=float):
        if float(t) == 0: continue
        al = field('%s/%s/alpha.water' % (case, t))
        if al: out[float(t)] = sum(x * dx for c, x in zip(C, al) if c[1] < ymin + 0.5 * dy and c[0] < xmax)
    return out
ok = True
ft, fq = fronts(d + '/tut', 0.292, 23), fronts(d + '/qc', 0.292, 23)
worst = max(abs(fq[t] - ft[t]) / ft[t] for t in ft if t in fq)
print('    damBreak front, setup vs tutorial: largest difference %.3f %% over %d writes' % (100 * worst, len(ft)))
ok &= worst < 0.01 and len(fq) >= 30
ff = fronts(d + '/fine', 0.65, 260)
ts = sorted(ff)
def Z(T):
    t = T / k
    for i in range(1, len(ts)):
        if ts[i] >= t:
            w = (t - ts[i - 1]) / (ts[i] - ts[i - 1]); return (ff[ts[i - 1]] * (1 - w) + ff[ts[i]] * w) / a
    return float('nan')
MM = [(2.20, 2.56), (2.51, 3.00), (2.83, 3.44), (3.11, 3.89)]     # Martin and Moyce 1952, n^2 = 2
for T, z in MM:
    e = (Z(T) - z) / z
    print('    Martin-Moyce  T %.2f  Z %.3f  theirs %.2f  %+.1f %%' % (T, Z(T), z, 100 * e))
    ok &= abs(e) < 0.12
sp, sm = (Z(3.11) - Z(2.20)) / 0.91, (3.89 - 2.56) / 0.91
print('    front speed dZ/dT  %.3f  theirs %.3f  %+.1f %%' % (sp, sm, 100 * (sp - sm) / sm))
ok &= abs(sp - sm) / sm < 0.05
f = glob.glob(d + '/tank/postProcessing/volume1/0/*')[0]
r = [l.split() for l in open(f) if l.strip() and l[0] != '#']
V, Q = 8e-4, 0.2 * 0.05 * 0.01
t, al = float(r[-1][0]), float(r[-1][1])
e = (al * V - (2e-4 + Q * t)) / (Q * t)
print('    tank: water %.6g m3 after %.4g s, start 2e-4 + inflow %.6g: %+.3f %% of the inflow' % (al * V, t, Q * t, 100 * e))
ok &= abs(e) < 0.01
sys.exit(0 if ok else 1)
PY
