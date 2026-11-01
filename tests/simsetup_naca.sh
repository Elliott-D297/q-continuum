#!/bin/bash
# tests/simsetup_naca.sh <test_simsetup> <OpenFOAM bashrc> <scratch>
# Step 4 of the setup plan (2026-11-01): the setup against OpenFOAM's own compressible reference. The v2606 tutorial
# rhoSimpleFoam/aerofoilNACA0012 (Mach 0.73, k-omega SST, freestream far field) is run as it ships; its mesh is then set
# up by sim_write from the High-speed gas template with the tutorial's free stream and run again. The drag and lift on the
# wall, and the wall's pressure, from both are compared: drag within 5 %, the wall pressure RMS within 1 % of the free
# stream's dynamic pressure.
# Step 5 (2026-11-01): the y+ helper's estimate for the mesh's own first cell on the wall, against the yPlus function
# object's mean over it, within 30 %. On a third run with 1 % turbulence coming in: the tutorial's own free stream (k 0.01,
# 0.03 %) leaves k-omega SST's boundary layer laminar (wall shear 12 Pa, Cf a laminar 3.4e-4), and the helper, like any
# y+ calculator, assumes a turbulent one (88 Pa from Cf = 0.026 Re^-1/7; the 1 % run gives 82).
# (Before the schemes followed checkMesh's non-orthogonality the drag was 6.8 % off; with full correction 1.4 %.)
bin=$(realpath "$1"); bashrc=$2; d=$(mktemp -d "$3/simsetup_naca.XXXXXX")
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --
source "$bashrc" > /dev/null 2>&1
set -u
cp -r "$FOAM_TUTORIALS/compressible/rhoSimpleFoam/aerofoilNACA0012" "$d/tut"
(cd "$d/tut" && ./Allrun.pre > log.pre 2>&1) || { echo "tutorial mesh failed"; tail "$d/tut/log.pre"; exit 1; }
mkdir -p "$d/qc/constant" "$d/qc/system"
cp -r "$d/tut/constant/polyMesh" "$d/qc/constant/"
cp "$d/tut/system/"{controlDict,fvSchemes,fvSolution} "$d/qc/system/"   # checkMesh reads them; sim_write replaces them
(cd "$d/qc" && checkMesh > log.checkMesh 2>&1)                # as the STL mesher's Generate leaves it: the schemes follow it
"$bin" naca "$d/qc" || exit 1
# the same forces from each: rho of the free stream, 1 bar as the reference pressure
rhoInf=$(python3 -c "print(1e5/(8314.462618/28.96*298))")
for c in tut qc; do
    cat > "$d/$c/system/fcDict" <<DICT
FoamFile { version 2.0; format ascii; class dictionary; object fcDict; }
functions
{
    fc { type forceCoeffs; libs (forces); patches (wall); rho rhoInf; rhoInf $rhoInf; pRef 1e5; CofR (0.25 0 0);
         dragDir (1 0 0); liftDir (0 0 1); pitchAxis (0 1 0); magUInf 250; lRef 1; Aref 1; writeControl timeStep; }
    pw { type surfaceFieldValue; libs (fieldFunctionObjects); regionType patch; name wall; operation none;
         fields (p); writeFields yes; surfaceFormat raw; writeControl timeStep; }
}
DICT
done
for c in tut qc; do
    cd "$d/$c" || exit 1
    t0=$(date +%s)
    timeout 1800 rhoSimpleFoam > log.run 2>&1 || { echo "$c: rhoSimpleFoam failed"; grep -m1 -A4 "FOAM FATAL" log.run; tail -3 log.run; exit 1; }
    echo "$c: $(( $(date +%s) - t0 )) s, $(grep -c '^Time =' log.run) iterations $(grep -o 'converged in [0-9]*' log.run)"
    rhoSimpleFoam -postProcess -latestTime -dict system/fcDict > log.fc 2>&1 || { echo "$c: forces failed"; tail log.fc; exit 1; }
done
cd "$d"
python3 - <<'PY'
import glob, math, sys
def coeffs(c):
    f = sorted(glob.glob(c + '/postProcessing/fc/*/coefficient.dat'))[-1]
    hdr, last = None, None
    for line in open(f):
        if line.startswith('#') and 'Cd' in line: hdr = line[1:].split()
        elif line.strip() and not line.startswith('#'): last = line.split()
    return { k: float(v) for k, v in zip(hdr, last) }
def wallp(c):
    f = sorted(glob.glob(c + '/postProcessing/pw/*/*/*p*'))[-1]   # raw: x y z p per face
    return [list(map(float, l.split())) for l in open(f) if l.strip() and not l.startswith('#')]
a, b = coeffs('tut'), coeffs('qc')
pa, pb = wallp('tut'), wallp('qc')
qInf = 0.5 * 1e5 / (8314.462618 / 28.96 * 298) * 250 ** 2
n = min(len(pa), len(pb))
rms = math.sqrt(sum((pa[i][3] - pb[i][3]) ** 2 for i in range(n)) / n) / qInf
dCd = abs(b['Cd'] - a['Cd']) / abs(a['Cd'])
print('tutorial Cd %.5f Cl %.5f | setup Cd %.5f Cl %.5f | Cd %.2f %% off, Cl %.4f apart; wall p RMS %.3f %% of q (%d faces)' %
      (a['Cd'], a['Cl'], b['Cd'], b['Cl'], 100 * dCd, abs(b['Cl'] - a['Cl']), 100 * rms, n))
sys.exit(0 if dCd < 0.05 and rms < 0.01 and len(pa) == len(pb) else 1)
PY
rc=$?
# the first cell on the wall: twice the mean distance from a wall face's centre to its cell's (the tutorial's ascii mesh)
(cd "$d/tut" && postProcess -func writeCellCentres -time 0 > log.C 2>&1)
h=$(python3 - "$d/tut" <<'PY'
import sys, re, math
d = sys.argv[1]
def body(path):
    t = open(path).read()
    return t
def vlist(t):
    return [tuple(map(float, v.split())) for v in re.findall(r'\(([-0-9.eE+]+ [-0-9.eE+]+ [-0-9.eE+]+)\)', t)]
C = body(d + '/0/C')
i = C.index('internalField'); b = C.index('boundaryField')
cells = vlist(C[i:b])
w = re.search(r'\n\s*aerofoil\s*\{', C[b:]).start() + b; w2 = C.index('}', w)
faces = vlist(C[w:w2])
bnd = open(d + '/constant/polyMesh/boundary').read()
m = re.search(r'aerofoil\s*\{[^}]*nFaces\s+(\d+);[^}]*startFace\s+(\d+);', bnd)
n, start = int(m.group(1)), int(m.group(2))
own = open(d + '/constant/polyMesh/owner').read()
m2 = re.search(r'\n(\d+)\s*\n\(', own)
own = list(map(int, own[m2.end():].split(')')[0].split()))
ds = [math.dist(faces[k], cells[own[start + k]]) for k in range(n)]
print(2 * sum(ds) / len(ds))
PY
)
mkdir -p "$d/qc1/constant" && cp -r "$d/qc/constant/polyMesh" "$d/qc1/constant/" && cp "$d/qc/log.checkMesh" "$d/qc1/"
"$bin" naca "$d/qc1" 1 > /dev/null || exit 1
(cd "$d/qc1" && timeout 1800 rhoSimpleFoam > log.run 2>&1) || { echo "qc1: rhoSimpleFoam failed"; tail -3 "$d/qc1/log.run"; exit 1; }
read -r est hfor delta < <("$bin" yplus "$d/qc1" 250 1 "$h")
python3 - "$d/qc1" "$est" "$h" <<'PY' || rc=1
import sys, glob
d, est, h = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
f = sorted(glob.glob(d + '/postProcessing/yPlus/*/yPlus.dat'))[-1]
avg = [float(l.split()[4]) for l in open(f) if l.strip() and not l.startswith('#') and l.split()[1] == 'aerofoil'][-1]   # the patch (wall is its group)
e = abs(est - avg) / avg
print('first cell %.3g m: helper y+ %.3g, yPlus mean %.3g: %.0f %% apart' % (h, est, avg, 100 * e))
sys.exit(0 if e < 0.3 else 1)
PY
exit $rc
