#!/bin/bash
# tests/simsetup_heat.sh <test_simsetup> <OpenFOAM bashrc> <scratch>
# Step 3 of the setup plan (2026-10-28): heat validated. De Vahl Davis's differentially heated square cavity (air, Pr 0.71)
# at Ra = 1e5, set up by sim_write and run by buoyantSimpleFoam on a 2-D mesh graded toward the walls; the mean Nusselt
# number on the hot wall, from the wallHeatFlux report, against the benchmark's 4.519 (de Vahl Davis 1983), within 3 %;
# the setup's own Nusselt report (Nu_hot) must agree with it.
bin=$1; bashrc=$2; d=$(mktemp -d "$3/simsetup_heat.XXXXXX")
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --
source "$bashrc" > /dev/null 2>&1
set -u
L=0.1; Ra=1e5; NU_REF=4.519; N=${QC_CAVITY_CELLS:-64}
mkdir -p "$d/cavity/system" "$d/cavity/constant"
cat > "$d/cavity/system/blockMeshDict" <<DICT
FoamFile { version 2.0; format ascii; class dictionary; object blockMeshDict; }
scale $L;
vertices ( (0 0 0) (1 0 0) (1 1 0) (0 1 0) (0 0 0.1) (1 0 0.1) (1 1 0.1) (0 1 0.1) );
// cells packed toward every wall (5:1 from the middle out), where the thermal boundary layers are
blocks ( hex (0 1 2 3 4 5 6 7) ($N $N 1) simpleGrading (((0.5 0.5 5) (0.5 0.5 0.2)) ((0.5 0.5 5) (0.5 0.5 0.2)) 1) );
boundary
(
    hot          { type wall;  faces ( (0 4 7 3) ); }
    cold         { type wall;  faces ( (1 2 6 5) ); }
    adiabatic    { type wall;  faces ( (0 1 5 4) (3 7 6 2) ); }
    frontAndBack { type empty; faces ( (0 3 2 1) (4 5 6 7) ); }
);
DICT
cat > "$d/cavity/system/controlDict" <<'DICT'
FoamFile { version 2.0; format ascii; class dictionary; object controlDict; }
application buoyantSimpleFoam; startTime 0; endTime 1; deltaT 1; writeControl timeStep; writeInterval 1;
DICT
cd "$d/cavity" && blockMesh > log.blockMesh 2>&1 || { echo "blockMesh failed"; exit 1; }
read -r dT kappa < <("$bin" cavity "$d/cavity" $L $Ra) || { echo "setup failed"; exit 1; }
timeout 900 buoyantSimpleFoam > log.buoyantSimpleFoam 2>&1; rc=$?
grep -q "FOAM FATAL" log.buoyantSimpleFoam && { grep -m1 -A4 "FOAM FATAL" log.buoyantSimpleFoam; exit 1; }
[ $rc -eq 0 ] || { echo "buoyantSimpleFoam rc=$rc"; tail -5 log.buoyantSimpleFoam; exit 1; }
f=$(ls postProcessing/wallHeatFlux/*/wallHeatFlux.dat 2>/dev/null | tail -1)
[ -f "$f" ] || { echo "no wallHeatFlux.dat"; ls -R postProcessing; exit 1; }
its=$(grep -o "converged in [0-9]*" log.buoyantSimpleFoam | grep -o "[0-9]*$")
# the last row for each wall: time patch min max integral (W over the mesh's depth, 0.1 L = 0.01 m)
python3 - "$f" "$dT" "$kappa" "$L" "$NU_REF" "${its:-not converged}" <<'PY'
import sys
f, dT, k, L, ref, its = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5]), sys.argv[6]
last = {}
for line in open(f):
    if line.startswith('#') or not line.strip(): continue
    c = line.split()
    last[c[1]] = float(c[4])
depth = 0.1 * L
Qh, Qc = last.get('hot', 0.0), last.get('cold', 0.0)
Nu = abs(Qh) / depth / (k * dT)               # q L / (k dT) with q = Q / (L depth)
err = abs(Nu - ref) / ref
# the setup's own Nusselt report (Nu_hot: the hot wall's mean flux scaled by L / (kappa dT)) must say the same
import glob
rep = float('nan')
for g in sorted(glob.glob('postProcessing/Nu_hot/*/surfaceFieldValue.dat')):
    for line in open(g):
        if line.strip() and not line.startswith('#'): rep = float(line.split()[1])
print("dT %.4g K, Q hot %.5g W, cold %.5g W (balance %.2g %%), Nu %.4f (report %.4f) vs %.3f: %.2f %% off, SIMPLE converged in %s" %
      (dT, Qh, Qc, 100 * abs(Qh + Qc) / max(abs(Qh), 1e-30), Nu, rep, ref, 100 * err, its))
sys.exit(0 if err < 0.03 and Qh > 0 and abs(rep - Nu) < 1e-3 * Nu else 1)
PY
