#!/bin/bash
# tests/simsetup_start.sh <test_simsetup> <OpenFOAM bashrc> <scratch>
# Step 9 of the setup plan (2026-11-09): starting values and the numerics presets.
# 1. pitzDaily (steady, k-omega SST, 10 m/s) converges on all three presets: Robust, Balanced, Accurate.
# 2. It starts from potential flow: the Starting values step's command (potentialFoam -writephi -writep) runs and simpleFoam
#    converges from its fields.
# 3. It starts from another case's results: Balanced from the Robust run's. mapFields (the step's command) fills the cells and
#    leaves the inlet's condition as the setup wrote it (10 m/s, not mapped), and the run converges in fewer iterations than
#    from rest.
# 4. The aerofoilNACA0012 tutorial's mesh (rhoSimpleFoam, Mach 0.73 as the tutorial) converges on all three presets.
# 5. The bad case: the same aerofoil at Mach 0.87 (300 m/s). Accurate never converges (its residuals stall near 1e-2 for all
#    2000 iterations: Balanced does the same, 2026-11-09); Robust converges. That is the Run page's "Use Robust" offer.
bin=$(realpath "$1"); bashrc=$2; d=$(mktemp -d "$3/simsetup_start.XXXXXX")
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --
source "$bashrc" > /dev/null 2>&1
set -u
fail=0
its() { grep -c '^Time =' "$1"; }
conv() { grep -o 'converged in [0-9]*' "$1" | grep -o '[0-9]*$'; }
# ---- the pitzDaily mesh, and its checkMesh (the schemes follow it, as after the STL mesher's Generate)
mkdir -p "$d/mesh" && cp -r "$FOAM_TUTORIALS/incompressible/simpleFoam/pitzDaily/system" "$d/mesh/"
(cd "$d/mesh" && blockMesh > log.blockMesh 2>&1 && checkMesh > log.checkMesh 2>&1) || { echo "pitzDaily mesh failed"; exit 1; }
pitz() {   # pitz <name> <preset> [start] -> runs the Starting values step (if any) and simpleFoam in $d/<name>
    local c="$d/$1"
    mkdir -p "$c/constant" "$c/system" && cp -r "$d/mesh/constant/polyMesh" "$c/constant/" && cp "$d/mesh/log.checkMesh" "$c/"
    local cmd; cmd=$("$bin" pitz "$c" "$2" "${3:-uniform}") || { echo "$1: setup failed: $cmd"; return 1; }
    if [ -n "$cmd" ]; then
        ( cd "$c" && bash -c "$cmd" > log.start 2>&1 ) || { echo "$1: the Starting values step failed"; tail -5 "$c/log.start"; return 1; }
    fi
    ( cd "$c" && timeout 900 simpleFoam > log.simpleFoam 2>&1 ) || { echo "$1: simpleFoam failed"; grep -m1 -A4 "FOAM FATAL" "$c/log.simpleFoam"; tail -3 "$c/log.simpleFoam"; return 1; }
}
for nm in robust balanced accurate; do pitz "p_$nm" $nm & done; wait
for nm in robust balanced accurate; do
    n=$(conv "$d/p_$nm/log.simpleFoam")
    if [ -n "$n" ]; then echo "    pitzDaily $nm: converged in $n iterations"; else echo "    pitzDaily $nm: did not converge ($(its "$d/p_$nm/log.simpleFoam") iterations)"; fail=1; fi
done
# ---- 2 potential flow, 3 another case's results
pitz p_potential balanced potential & pitz p_map balanced "map:$d/p_robust" & wait
n=$(conv "$d/p_potential/log.simpleFoam")
grep -q '^End' "$d/p_potential/log.start" && [ -f "$d/p_potential/0/phi" ] && [ -n "$n" ] \
    && echo "    potential flow: potentialFoam ran, then converged in $n iterations" \
    || { echo "    potential flow: failed (potentialFoam $(tail -1 "$d/p_potential/log.start"), converged '$n')"; fail=1; }
nm=$(conv "$d/p_map/log.simpleFoam"); nb=$(conv "$d/p_balanced/log.simpleFoam")
( cd "$d/p_map" && foamDictionary 0/U -entry boundaryField/inlet -value 2>/dev/null ) > "$d/inlet.txt"
grep -q "surfaceNormalFixedValue" "$d/inlet.txt" && grep -q "refValue *uniform -10" "$d/inlet.txt" && grep -q "nonuniform" <(head -c 2000 "$d/p_map/0/U" | tr -d '\0') \
    && [ -n "$nm" ] && [ -n "$nb" ] && [ "$nm" -lt "$nb" ] \
    && echo "    another case: mapped cells, inlet kept at 10 m/s; converged in $nm iterations (from rest $nb)" \
    || { echo "    another case: failed (converged '$nm' vs '$nb' from rest; inlet: $(tr '\n' ' ' < "$d/inlet.txt"))"; fail=1; }
# ---- 4, 5 the aerofoil
cp -r "$FOAM_TUTORIALS/compressible/rhoSimpleFoam/aerofoilNACA0012" "$d/tut"
(cd "$d/tut" && ./Allrun.pre > log.pre 2>&1) || { echo "aerofoil mesh failed"; exit 1; }
naca() {   # naca <name> <preset> <speed>
    local c="$d/$1"
    mkdir -p "$c/constant" "$c/system" && cp -r "$d/tut/constant/polyMesh" "$c/constant/" && cp "$d/tut/system/"{controlDict,fvSchemes,fvSolution} "$c/system/"
    ( cd "$c" && checkMesh > log.checkMesh 2>&1 )
    QC_NUMERICS=$2 QC_SPEED=$3 QC_ITERATIONS=2000 "$bin" naca "$c" > /dev/null || { echo "$1: setup failed"; return 1; }
    ( cd "$c" && timeout 1500 rhoSimpleFoam > log.run 2>&1 ) || { echo "$1: rhoSimpleFoam failed"; grep -m1 -A4 "FOAM FATAL" "$c/log.run"; return 1; }
}
for nm in robust balanced accurate; do naca "n_$nm" $nm 250 & done; wait
for nm in robust balanced accurate; do
    n=$(conv "$d/n_$nm/log.run")
    if [ -n "$n" ]; then echo "    aerofoil Mach 0.73 $nm: converged in $n iterations"; else echo "    aerofoil Mach 0.73 $nm: did not converge"; fail=1; fi
done
naca bad_accurate accurate 300 & naca bad_robust robust 300 & wait
na=$(conv "$d/bad_accurate/log.run"); nr=$(conv "$d/bad_robust/log.run")
pa=$(grep 'Solving for p,' "$d/bad_accurate/log.run" | tail -1 | awk '{print $8}' | tr -d ,)
if [ -z "$na" ] && [ -n "$nr" ]; then echo "    aerofoil Mach 0.87: Accurate stalls (p residual $pa after $(its "$d/bad_accurate/log.run") iterations), Robust converges in $nr"
else echo "    aerofoil Mach 0.87: expected Accurate to stall and Robust to converge (Accurate '$na', Robust '$nr')"; fail=1; fi
exit $fail
