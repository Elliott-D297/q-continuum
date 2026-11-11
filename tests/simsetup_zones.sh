#!/bin/bash
# tests/simsetup_zones.sh <test_simsetup> <OpenFOAM bashrc> <scratch>
# Step 7 of the setup plan (2026-11-08): rotating and porous zones.
# 1. mixerVessel2D: the tutorial as shipped run to convergence (1500 SIMPLE iterations; at its own 500 the torque is still
#    2.5 % off its final value), a forces function object added for the rotor's torque, against the same mesh set up by
#    sim_write (its rotor cellZone turning at 104.72 rad/s, steady k-epsilon, the torque report). With the tutorial's
#    numerics (its fvSchemes and fvSolution) the torque about z and the velocity field (RMS of the difference over the RMS
#    speed) agree within 2 % (they come out the same to 1e-6): the zone, its walls, the fields and the torque are set up
#    right. With the setup's own numerics (linearUpwind, upwind turbulence, SIMPLEC) both are within 10 % on this
#    3072-cell mesh (scheme error: 6.9 % in the torque, 2026-11-08). The tutorial's limited schemes with the setup's
#    SIMPLEC relaxation stall at residuals of 1e-4, so the two are copied together.
# 2. A porous block in a slip-walled channel asked for 100 Pa at 1 m/s (one point): topoSet marks it, the pressure drop
#    report inlet to outlet within 5 % of the request.
bin=$(realpath "$1"); bashrc=$2; d=$(mktemp -d "$3/simsetup_zones.XXXXXX")
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --
source "$bashrc" > /dev/null 2>&1
set -u
T="$FOAM_TUTORIALS/incompressible/simpleFoam/mixerVessel2D"
FORCES='    rotorTorque
    {
        type            forces;
        libs            (forces);
        writeControl    timeStep;
        writeInterval   1;
        patches         (rotor);
        rho             rhoInf;
        rhoInf          1;
        CofR            (0 0 0);
    }'
# ---- 1a the tutorial as shipped
cp -r "$T" "$d/tut" && cd "$d/tut" || exit 1
cp -r 0.orig 0
sed -i 's/^endTime .*/endTime         1500;/; s/^writeInterval .*/writeInterval   1500;/' system/controlDict
m4 system/blockMeshDict.m4 > system/blockMeshDict && blockMesh > log.blockMesh 2>&1 || { echo "blockMesh failed"; exit 1; }
python3 - "$FORCES" <<'PY'
import sys
p = 'system/controlDict'; s = open(p).read()
s = s.replace('#include "relativeVelocity"', '#include "relativeVelocity"\n' + sys.argv[1], 1)
open(p, 'w').write(s)
PY
grep -q rotorTorque system/controlDict || { echo "could not add the torque to the tutorial's controlDict"; exit 1; }
timeout 900 simpleFoam > log.simpleFoam 2>&1 || { echo "tutorial simpleFoam failed"; tail -5 log.simpleFoam; exit 1; }
# ---- 1b set up by Q-Continuum
mkdir -p "$d/qc/system" "$d/qc/constant" && cd "$d/qc" || exit 1
cp "$d/tut/system/blockMeshDict" system/ && cp -r "$d/tut/constant/polyMesh" constant/
"$bin" mixer "$d/qc" || { echo "setup failed"; exit 1; }
[ -f constant/MRFProperties ] || { echo "no MRFProperties"; exit 1; }
run_qc() {   # run_qc <dir>: simpleFoam, results in ASCII to read here
    ( cd "$1" && timeout 900 simpleFoam > log.simpleFoam 2>&1 ) || { echo "simpleFoam failed in $1"; grep -m1 -A6 "FOAM FATAL" "$1/log.simpleFoam"; tail -5 "$1/log.simpleFoam"; return 1; }
    ( cd "$1" && foamDictionary system/controlDict -entry writeFormat -set ascii > /dev/null 2>&1 && foamFormatConvert -latestTime > log.convert 2>&1 )
}
cp -r "$d/qc" "$d/qcs" && cp "$d/tut/system/fvSchemes" "$d/tut/system/fvSolution" "$d/qcs/system/"
run_qc "$d/qc" || exit 1
run_qc "$d/qcs" || exit 1
python3 - "$d" <<'PY'
import sys, glob, math, re
d = sys.argv[1]
def torque(case, name):
    fs = sorted(glob.glob('%s/postProcessing/%s/*/moment.dat' % (case, name)))
    rows = [l.split() for l in open(fs[-1]) if l.strip() and not l.startswith('#')]
    return float(rows[-1][3])                      # total moment z
def U(case):
    times = sorted((t for t in glob.glob(case + '/[0-9]*') if re.search(r'/[0-9.e+-]+$', t)), key=lambda t: float(t.rsplit('/', 1)[1]))
    s = open(times[-1] + '/U').read()
    body = s[s.index('internalField'):s.index('boundaryField')]
    n = int(re.search(r'nonuniform List<vector>\s*(\d+)', body).group(1))
    vals = re.findall(r'\(([-0-9.eE+]+) ([-0-9.eE+]+) ([-0-9.eE+]+)\)', body)
    assert len(vals) == n, '%s: read %d of %d velocities' % (case, len(vals), n)
    return [tuple(map(float, v)) for v in vals]
tt, ts, tq = torque(d + '/tut', 'rotorTorque'), torque(d + '/qcs', 'rotor_torque'), torque(d + '/qc', 'rotor_torque')
ut, us, uq = U(d + '/tut'), U(d + '/qcs'), U(d + '/qc')
rms = math.sqrt(sum(a * a + b * b + c * c for a, b, c in ut) / len(ut))
rmsd = lambda A, B: math.sqrt(sum((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2 for a, b in zip(A, B)) / len(A))
diff, diffq = rmsd(ut, us), rmsd(ut, uq)
es, eq = abs(ts - tt) / abs(tt), abs(tq - tt) / abs(tt)
print('    mixer torque  tutorial %.6g N m/m; setup with the tutorial numerics %.6g (%.3f %%); with its own %.6g (%.2f %%)' % (tt, ts, 100 * es, tq, 100 * eq))
print('    mixer U       rms speed %.4g m/s; rms difference, tutorial numerics %.3f %%, own %.2f %%' % (rms, 100 * diff / rms, 100 * diffq / rms))
sys.exit(0 if es < 0.02 and diff / rms < 0.02 and eq < 0.10 and diffq / rms < 0.10 else 1)
PY
mixer=$?
# ---- 2 porous block
mkdir -p "$d/por/system" "$d/por/constant" && cd "$d/por" || exit 1
cat > system/blockMeshDict <<'BM'
FoamFile { version 2.0; format ascii; class dictionary; object blockMeshDict; }
vertices ( (0 0 0) (1 0 0) (1 0.1 0) (0 0.1 0) (0 0 0.01) (1 0 0.01) (1 0.1 0.01) (0 0.1 0.01) );
blocks ( hex (0 1 2 3 4 5 6 7) (100 10 1) simpleGrading (1 1 1) );
boundary
(
    inlet { type patch; faces ((0 4 7 3)); }
    outlet { type patch; faces ((1 2 6 5)); }
    walls { type patch; faces ((0 1 5 4) (3 7 6 2)); }
    frontAndBack { type empty; faces ((0 3 2 1) (4 5 6 7)); }
);
BM
printf 'FoamFile { version 2.0; format ascii; class dictionary; object controlDict; }\napplication simpleFoam;\nstartTime 0;\nendTime 1;\ndeltaT 1;\nwriteControl timeStep;\nwriteInterval 1;\n' > system/controlDict   # blockMesh needs one; the setup writes its own
blockMesh > log.blockMesh 2>&1 || { echo "channel blockMesh failed"; tail -5 log.blockMesh; exit 1; }
read -r -a out < <("$bin" porous "$d/por" 100 1 0.2 0.4) || { echo "porous setup failed"; exit 1; }
rep=${out[1]}
topoSet -dict system/topoSetDict.zones > log.topoSet.zones 2>&1 || { echo "topoSet failed"; tail -5 log.topoSet.zones; exit 1; }
grep -q "porous" constant/polyMesh/cellZones || { echo "no porous cellZone"; exit 1; }
timeout 900 simpleFoam > log.simpleFoam 2>&1 || { echo "porous simpleFoam failed"; grep -m1 -A6 "FOAM FATAL" log.simpleFoam; tail -5 log.simpleFoam; exit 1; }
python3 - "$rep" <<'PY'
import sys, glob
f = sorted(glob.glob('postProcessing/%s/*/*' % sys.argv[1]))[-1]
rows = [l.split() for l in open(f) if l.strip() and not l.startswith('#')]
dp = float(rows[-1][1])                              # in Pa (the report measures pPa)
print('    porous drop   %.5g (requested 100 Pa)' % dp)
sys.exit(0 if abs(dp - 100) / 100 < 0.05 else 1)
PY
por=$?
[ $mixer -eq 0 ] && [ $por -eq 0 ]
