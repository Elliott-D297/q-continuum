#!/bin/bash
# tests/simsetup_solvers.sh <test_simsetup> <OpenFOAM bashrc> <scratch>
# Step 1 of the setup plan (2026-10-27): every time x turbulence model x algorithm the setup can choose, written by
# sim_write and run by the solver it picks for five iterations or time steps, on a 2-D mesh (pitzDaily) and a 3-D duct.
# Heat (step 3) and compressible gas (step 4) are swept the same way. Each must end without a FOAM FATAL; the table lists any that don't with the error's first line.
bin=$1; bashrc=$2; d=$(mktemp -d "$3/simsetup_solvers.XXXXXX")
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --                                                     # its bashrc reads our arguments as its own
source "$bashrc" > /dev/null 2>&1
set -u
# the meshes: pitzDaily's, and a 3-D duct 0.4 x 0.1 x 0.1 m with a wall on four sides
cp -r "$FOAM_TUTORIALS/incompressible/simpleFoam/pitzDaily" "$d/pitz"
(cd "$d/pitz" && blockMesh > log.blockMesh 2>&1) || { echo "pitzDaily blockMesh failed"; exit 1; }
mkdir -p "$d/duct/system"
cat > "$d/duct/system/blockMeshDict" <<'DICT'
FoamFile { version 2.0; format ascii; class dictionary; object blockMeshDict; }
scale 1;
vertices ( (0 0 0) (0.4 0 0) (0.4 0.1 0) (0 0.1 0) (0 0 0.1) (0.4 0 0.1) (0.4 0.1 0.1) (0 0.1 0.1) );
blocks ( hex (0 1 2 3 4 5 6 7) (24 8 8) simpleGrading (1 1 1) );
boundary
(
    inlet  { type patch; faces ( (0 4 7 3) ); }
    outlet { type patch; faces ( (1 2 6 5) ); }
    walls  { type wall;  faces ( (0 1 5 4) (3 7 6 2) (0 3 2 1) (4 5 6 7) ); }
);
DICT
cp "$d/pitz/system/controlDict" "$d/duct/system/" && (cd "$d/duct" && blockMesh > log.blockMesh 2>&1) || { echo "duct blockMesh failed"; exit 1; }
"$bin" matrix "$d/pitzcases" "$d/pitz/constant/polyMesh" 1e-5 inlet:inlet:patch outlet:outlet:patch upperWall:wall:wall lowerWall:wall:wall frontAndBack:wall:empty > "$d/pitz.list" || exit 1
"$bin" matrix "$d/ductcases" "$d/duct/constant/polyMesh" 1e-4 inlet:inlet:patch outlet:outlet:patch walls:wall:wall > "$d/duct.list" || exit 1
fails=0; runs=0
for set in pitz duct; do
    while read -r c; do
        cd "$d/${set}cases/$c" || continue
        app=$(foamDictionary -entry application -value system/controlDict 2>/dev/null)
        timeout 300 "$app" > log.run 2>&1; rc=$?
        runs=$((runs + 1))
        if [ $rc -ne 0 ] || grep -q "FOAM FATAL" log.run || ! grep -q "^End" log.run; then
            fails=$((fails + 1))
            printf '%-5s %-45s FAIL rc=%s  %s\n' "$set" "$c" "$rc" "$(grep -m1 -A3 'FOAM FATAL' log.run | tr '\n' ' ' | cut -c1-160)"
        else
            printf '%-5s %-45s ok   %s\n' "$set" "$c" "$app"
        fi
    done < <(grep -E "${QC_SWEEP_ONLY:-.}" "$d/$set.list")      # QC_SWEEP_ONLY=gas_: a subset
done
echo "$runs runs, $fails failed (cases in $d)"
[ $fails -eq 0 ]
