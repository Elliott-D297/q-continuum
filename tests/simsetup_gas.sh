#!/bin/bash
# tests/simsetup_gas.sh <test_simsetup> <OpenFOAM bashrc> <scratch> [sub|super|transient ...]
# Step 4 of the setup plan (2026-11-01): compressible gas validated against isentropic flow. A 2-D half nozzle (slip wall,
# symmetry axis), set up by sim_write from the High-speed gas template as an inviscid nozzle with a total pressure and
# temperature at the inlet and a static pressure at the outlet, run by the solver the setup picks:
#   sub        converging nozzle, area ratio 2, pe/p0 0.9 (rhoSimpleFoam): exit Mach from the pressure ratio, inlet Mach
#              from the area ratio, exit density and temperature, all isentropic, within 2 %
#   super      converging-diverging, throat 1, inlet 2, exit 1.5, pe/p0 at the design 0.1617 (rhoSimpleFoam, transonic):
#              the inlet Mach the choked throat allows, the supersonic exit Mach (1.854) and temperature, within 2 %
#   transient  the converging nozzle again with rhoPimpleFoam, run until it settles
bin=$(realpath "$1"); bashrc=$2; d=$(mktemp -d "$3/simsetup_gas.XXXXXX")
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
shift 3; cases=${*:-sub super transient}
set --
source "$bashrc" > /dev/null 2>&1
set -u
here=$(cd "$(dirname "$0")" && pwd)

# mesh <dir> <kind>: blockMesh with a spline wall y(x); the area ratios are those of the half-heights
mesh() {
    mkdir -p "$1/system"
    python3 "$here/tools/nozzle_mesh.py" "$2" > "$1/system/blockMeshDict"
    cat > "$1/system/controlDict" <<'DICT'
FoamFile { version 2.0; format ascii; class dictionary; object controlDict; }
application rhoSimpleFoam; startTime 0; endTime 1; deltaT 1; writeControl timeStep; writeInterval 1;
DICT
    (cd "$1" && blockMesh > log.blockMesh 2>&1) || { echo "$1: blockMesh failed"; return 1; }
}

fails=0
for c in $cases; do
    case $c in
        sub)       kind=conv; ratio=0.9;    mode=steady ;;
        super)     kind=cd;   ratio=0.1617; mode=steady ;;
        transient) kind=conv; ratio=0.9;    mode=transient ;;
        *) echo "unknown case $c"; fails=$((fails + 1)); continue ;;
    esac
    cd_=$d/$c
    mesh "$cd_" $kind || { fails=$((fails + 1)); continue; }
    read -r p0 T0 gam R app Ma < <("$bin" nozzle "$cd_" $ratio $mode 2> "$cd_/log.setup") || { echo "$c: setup failed"; cat "$cd_/log.setup"; fails=$((fails + 1)); continue; }
    cd "$cd_" || continue
    t0=$(date +%s)
    timeout 1200 $app > log.$app 2>&1; rc=$?
    secs=$(( $(date +%s) - t0 ))
    if grep -q "FOAM FATAL" log.$app || [ $rc -ne 0 ]; then
        echo "$c: $app rc=$rc"; grep -m1 -A4 "FOAM FATAL" log.$app; tail -3 log.$app; fails=$((fails + 1)); continue
    fi
    its=$(grep -o "converged in [0-9]*" log.$app | grep -o "[0-9]*$")
    # the patch averages of p, T and Ma at the inlet and the outlet, at the last time
    postProcess -latestTime -func "patchAverage(name=inlet,p,T,Ma)" > log.inAvg 2>&1
    postProcess -latestTime -func "patchAverage(name=outlet,p,T,Ma)" > log.outAvg 2>&1
    python3 - "$c" "$p0" "$T0" "$gam" "$R" "$app" "${its:-n/a}" "$secs" "$(grep -c '^Time =' log.$app)" <<'PY' || fails=$((fails + 1))
import sys, glob, math
c, p0, T0, g, R, app, its, secs, nsteps = sys.argv[1], *map(float, sys.argv[2:6]), sys.argv[6], sys.argv[7], sys.argv[8], sys.argv[9]
def avg(log):
    v = {}
    for line in open(log):
        line = line.strip()
        # "areaAverage(inlet) of p = 109876"
        if line.startswith('areaAverage(') and ' of ' in line and '=' in line:
            k = line.split(' of ')[1].split('=')[0].strip()
            v[k] = float(line.split('=')[1].split()[0])
    return v
i, o = avg('log.inAvg'), avg('log.outAvg')
if not i or not o or 'Ma' not in o:
    print(c, 'no patch averages'); sys.exit(1)
def M_of_p(r): return math.sqrt(2 / (g - 1) * (r ** (-(g - 1) / g) - 1))
def A_over_Astar(M): return (1 / M) * ((2 / (g + 1)) * (1 + (g - 1) / 2 * M * M)) ** ((g + 1) / (2 * (g - 1)))
def M_of_A(A, sup):          # A/A* to M on the subsonic or supersonic branch, by bisection
    lo, hi = (1.0, 10.0) if sup else (1e-6, 1.0)
    for _ in range(200):
        m = (lo + hi) / 2
        if (A_over_Astar(m) > A) == (not sup): lo = m
        else: hi = m
    return (lo + hi) / 2
rows = []
if c in ('sub', 'transient'):
    Me = M_of_p(o['p'] / p0)                    # isentropic, from the outlet's own pressure ratio
    Astar_e = A_over_Astar(Me)                  # the exit's A/A*; the inlet's is twice it
    Mi = M_of_A(2 * Astar_e, False)
    rows = [('exit Mach', o['Ma'], Me), ('inlet Mach', i['Ma'], Mi),
            ('exit T/T0', o['T'] / T0, 1 / (1 + (g - 1) / 2 * Me * Me)),
            ('exit rho/rho0', (o['p'] / (R * o['T'])) / (p0 / (R * T0)), (o['p'] / p0) ** (1 / g))]
else:
    Me, Mi = M_of_A(1.5, True), M_of_A(2.0, False)
    rows = [('exit Mach', o['Ma'], Me), ('inlet Mach', i['Ma'], Mi),
            ('exit T/T0', o['T'] / T0, 1 / (1 + (g - 1) / 2 * Me * Me)),
            ('exit p/p0', o['p'] / p0, (1 + (g - 1) / 2 * Me * Me) ** (-g / (g - 1)))]
worst = 0
print('%s (%s, %s s, %s): converged in %s' % (c, app, secs, nsteps + ' steps' if app == 'rhoPimpleFoam' else nsteps + ' iterations', its))
for name, got, want in rows:
    e = abs(got - want) / want
    worst = max(worst, e)
    print('    %-14s %.5f  theory %.5f  %5.2f %%' % (name, got, want, 100 * e))
sys.exit(0 if worst < 0.02 else 1)
PY
done
echo "$fails failed (cases in $d)"
[ $fails -eq 0 ]
