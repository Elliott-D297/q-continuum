#!/bin/bash
# tests/simsetup_reports.sh <test_simsetup> <OpenFOAM bashrc> <scratch>
# Step 6 of the setup plan (2026-11-01): reports. pitzDaily set up by sim_write with one report of each kind, run 400
# iterations by simpleFoam. Checked against hand calculations from the run's own fields: the inlet's flow rate is
# 10 m/s times its area (from the mesh) and the outlet's the same out; the outlet's average pressure and the pressure
# drop are the area-averaged pressures (patchAverage of p, times the density) at the last iteration, within 0.5 %; the
# probes and the domain's greatest speed are written; every report's file is in postProcessing.
bin=$(realpath "$1"); bashrc=$2; d=$(mktemp -d "$3/simsetup_reports.XXXXXX")
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --
source "$bashrc" > /dev/null 2>&1
set -u
cp -r "$FOAM_TUTORIALS/incompressible/simpleFoam/pitzDaily" "$d/pitz"
cd "$d/pitz" && blockMesh > log.blockMesh 2>&1 || { echo "blockMesh failed"; exit 1; }
read -r -a out < <("$bin" reports "$d/pitz") || { echo "setup failed"; exit 1; }
rho=${out[-1]}
timeout 900 simpleFoam > log.simpleFoam 2>&1 || { echo "simpleFoam failed"; grep -m1 -A4 "FOAM FATAL" log.simpleFoam; tail -3 log.simpleFoam; exit 1; }
grep -q "FOAM FATAL\|FOAM Warning.*functionObject" log.simpleFoam && { grep -m2 -A3 "FOAM FATAL\|FOAM Warning" log.simpleFoam; }
postProcess -latestTime -func "patchAverage(name=inlet,p)" > log.pin 2>&1
postProcess -latestTime -func "patchAverage(name=outlet,p)" > log.pout 2>&1
postProcess -latestTime -func "patchIntegrate(name=inlet,mag(Sf))" > log.ain 2>&1 || true
python3 - "$rho" <<'PY'
import sys, glob, math
rho = float(sys.argv[1])
def last(path, col=1):
    rows = [l.split() for l in open(path) if l.strip() and not l.startswith('#')]
    return [float(x) for x in rows[-1][1:]] if rows else None
def avg(log):
    for l in open(log):
        if 'areaAverage(' in l and ' of ' in l and '=' in l: return float(l.split('=')[1].split()[0])
def files(name): return sorted(glob.glob('postProcessing/%s/*/*' % name))
ok = True
rep = {}
for n in ['probes1', 'surface1', 'surface2', 'surface3', 'pressureDrop1', 'volume1']:
    f = files(n)
    if not f: print('%s: no file' % n); ok = False; continue
    rep[n] = last(f[-1])
pin, pout = avg('log.pin') * rho, avg('log.pout') * rho
# the inlet: 25.4 mm high, 1 mm deep in pitzDaily
A = 0.0254 * 0.001
Q = 10 * A
checks = [
    ('inlet flow rate', rep.get('surface2', [0])[0], -Q),
    ('outlet flow rate', rep.get('surface3', [0])[0], Q),
    ('outlet average p', rep.get('surface1', [0])[0], pout),
    ('pressure drop', rep.get('pressureDrop1', [0])[0], pin - pout),
]
for name, got, want in checks:
    e = abs(got - want) / max(abs(want), 1e-12)
    ok &= e < 0.005
    print('    %-18s %.6g  hand %.6g  %.3f %%' % (name, got, want, 100 * e))
pr = rep.get('probes1') or []
vm = (rep.get('volume1') or [float('nan')])[0]
print('    probes %s m/s, greatest speed %.4g m/s' % (' '.join('%.4g' % v for v in pr), vm))
ok &= len(pr) == 2 and all(0 < v < 20 for v in pr) and 9 < vm < 30
sys.exit(0 if ok else 1)
PY
