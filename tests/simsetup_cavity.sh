#!/bin/bash
# tests/simsetup_cavity.sh <test_simsetup> <OpenFOAM bashrc> <scratch>
# Step 3 of the setup plan: turbulent natural convection validated. The v2606 buoyantCavity tutorial's tall cavity (76 mm
# wide, 2180 mm high, 520 mm deep; hot wall 34.6 °C, cold 15 °C), set up by sim_write (RANS k-omega SST) and run by
# buoyantSimpleFoam; the mean temperature across the cavity at y/H = 0.1, 0.3 and 0.4 against Betts and Bokhari's
# measurements (the tutorial's validation/exptData): RMS within 8 % of the 19.6 K across, each point within 15 % (the
# tutorial's own setup: RMS 7.8, 12.3 and 13.6 %, worst 20 %; this one 5.7, 6.7, 4.9 %, worst 12 %, 2026-10-29).
bin=$1; bashrc=$2; d=$(mktemp -d "$3/simsetup_cavity.XXXXXX")
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --
source "$bashrc" > /dev/null 2>&1
set -u
tut=$FOAM_TUTORIALS/heatTransfer/buoyantSimpleFoam/buoyantCavity
[ -d "$tut" ] || { echo "no buoyantCavity tutorial at $tut"; exit 1; }
mkdir -p "$d/cavity/system" "$d/cavity/constant"
cp "$tut/system/blockMeshDict" "$d/cavity/system/"
cat > "$d/cavity/system/controlDict" <<'DICT'
FoamFile { version 2.0; format ascii; class dictionary; object controlDict; }
application buoyantSimpleFoam; startTime 0; endTime 1; deltaT 1; writeControl timeStep; writeInterval 1;
DICT
cd "$d/cavity" && blockMesh > log.blockMesh 2>&1 || { echo "blockMesh failed"; exit 1; }
"$bin" tallcavity "$d/cavity" "${QC_CAVITY_ITERS:-1000}" || { echo "setup failed"; exit 1; }
np=${QC_CAVITY_PROCS:-4}
if [ "$np" -gt 1 ]; then
    printf 'FoamFile { version 2.0; format ascii; class dictionary; object decomposeParDict; }\nnumberOfSubdomains %s;\nmethod scotch;\n' "$np" > system/decomposeParDict
    decomposePar > log.decomposePar 2>&1 || { echo "decomposePar failed"; exit 1; }
    timeout 1500 mpirun -np "$np" buoyantSimpleFoam -parallel > log.buoyantSimpleFoam 2>&1; rc=$?
    [ $rc -eq 0 ] && { reconstructPar -latestTime > log.reconstructPar 2>&1 || rc=1; }
else
    timeout 1500 buoyantSimpleFoam > log.buoyantSimpleFoam 2>&1; rc=$?
fi
grep -q "FOAM FATAL" log.buoyantSimpleFoam && { grep -m1 -A4 "FOAM FATAL" log.buoyantSimpleFoam; exit 1; }
[ $rc -eq 0 ] || { echo "buoyantSimpleFoam rc=$rc"; tail -5 log.buoyantSimpleFoam; exit 1; }
# the mean temperature across the middle of the depth (z = 0) at the heights measured
cat > system/sampleT <<'DICT'
type sets; libs (sampling); interpolationScheme cellPoint; setFormat raw; fields (T);
sets
{
    y0.1 { type uniform; axis x; start (0 0.218 0); end (0.076 0.218 0); nPoints 200; }
    y0.3 { type uniform; axis x; start (0 0.654 0); end (0.076 0.654 0); nPoints 200; }
    y0.4 { type uniform; axis x; start (0 0.872 0); end (0.076 0.872 0); nPoints 200; }
}
DICT
postProcess -func sampleT -latestTime > log.sample 2>&1 || { echo "sampling failed"; tail -5 log.sample; exit 1; }
its=$(grep -o "converged in [0-9]*" log.buoyantSimpleFoam | grep -o "[0-9]*$")
python3 - "$tut/validation/exptData" "${its:-not converged in ${QC_CAVITY_ITERS:-1000}}" <<'PY'
import glob, math, sys
expt, its = sys.argv[1], sys.argv[2]
DT, ok, out = 307.75 - 288.15, True, []
for h in ('1', '3', '4'):
    f = sorted(glob.glob('postProcessing/sampleT/*/y0.%s_T.xy' % h))
    if not f: print('no samples for y/H 0.%s' % h); sys.exit(1)
    sim = [tuple(map(float, l.split()[:2])) for l in open(f[-1]) if l.strip()]
    def T_at(x):                                         # linear in x along the line
        for (x0, t0), (x1, t1) in zip(sim, sim[1:]):
            if x0 <= x <= x1: return t0 + (t1 - t0) * (x - x0) / (x1 - x0) if x1 > x0 else t0
        return sim[0][1] if x < sim[0][0] else sim[-1][1]
    err = []
    for l in open('%s/mt_z0_%s0_lo.dat' % (expt, h)):
        if l.startswith('#') or not l.strip(): continue
        x_mm, t_c = map(float, l.split()[:2])
        err.append(T_at(x_mm / 1000) - (t_c + 273.15))
    rms = math.sqrt(sum(e * e for e in err) / len(err)) / DT
    worst = max(abs(e) for e in err) / DT
    ok = ok and rms < 0.08 and worst < 0.15
    out.append('y/H 0.%s: RMS %.1f %%, worst %.1f %% of %.1f K (%d points)' % (h, 100 * rms, 100 * worst, DT, len(err)))
nu = {}
for w in ('hot', 'cold'):
    for g in sorted(glob.glob('postProcessing/Nu_%s/*/surfaceFieldValue.dat' % w)):
        for l in open(g):
            if l.strip() and not l.startswith('#'): nu[w] = float(l.split()[1])
print('; '.join(out) + '; Nu (width) hot %.3g, cold %.3g; SIMPLE %s' % (nu.get('hot', float('nan')), nu.get('cold', float('nan')), its))
sys.exit(0 if ok and nu.get('hot', 0) > 0 and nu.get('cold', 0) < 0 else 1)
PY
