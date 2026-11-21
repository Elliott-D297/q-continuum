#!/bin/bash
# preflight_shape.sh <test_sweep> <OpenFOAM bashrc> <scratch>: the sweep's mesh-shape checks (mesh overhaul plan step 3) on
# real blockMesh meshes: a 2-D cavity one cell thick (quiet) and two cells thick (warned), a wedge of 5 degrees (quiet) and
# of 10 (warned), and the misaligned-edge error from checkMesh's log.
ts=$1; rc=$2; out=$3/preflight_shape
set --
. "$rc" || exit 2
here=$(cd "$(dirname "$0")" && pwd)
rm -rf "$out"; mkdir -p "$out"
fail=0
check() {   # check <case> <grep pattern> <want: yes|no> <what>
    local got; got=$("$ts" "$1" --case 2>&1)
    if echo "$got" | grep -q "$2"; then [ "$3" = yes ] || { echo "FAIL $4: found '$2'"; echo "$got"; fail=1; }
    else [ "$3" = no ] || { echo "FAIL $4: no '$2'"; echo "$got"; fail=1; }; fi
}
for nz in 1 2; do
    d=$out/cavity$nz; cp -a "$FOAM_TUTORIALS/incompressible/icoFoam/cavity/cavity" "$d"
    sed -i "s/(20 20 1)/(20 20 $nz)/" "$d/system/blockMeshDict"
    blockMesh -case "$d" > "$d/log.blockMesh" 2>&1 && checkMesh -case "$d" > "$d/log.checkMesh" 2>&1 || { echo "FAIL mesh cavity$nz"; fail=1; }
done
check "$out/cavity1" "cells thick" no "2-D one cell thick"
check "$out/cavity2" "The 2-D mesh is 2 cells thick" yes "2-D two cells thick"
for a in 5 10; do
    d=$out/wedge$a; bash "$here/tools/wedge_mesh.sh" "$d" $a
    blockMesh -case "$d" > "$d/log.blockMesh" 2>&1 && checkMesh -case "$d" > "$d/log.checkMesh" 2>&1 || { echo "FAIL mesh wedge$a"; fail=1; }
done
check "$out/wedge5" "wedge is" no "5 degree wedge"
check "$out/wedge5" "cells thick" no "wedge one cell thick"
check "$out/wedge10" "The wedge is 10 degrees" yes "10 degree wedge"
# a side split into two wedge patches is still one side: 5 degrees, no warning
cp -a "$out/wedge5" "$out/wedge5split"
sed -i 's/^    Wedge back with angle 2.5 degrees$/&\n    Wedge back2 with angle 2.5 degrees\n    Wedge front2 with angle 2.5 degrees/' "$out/wedge5split/log.checkMesh"
grep -q 'front2' "$out/wedge5split/log.checkMesh" || { echo "FAIL: wedge5split log not made"; fail=1; }
check "$out/wedge5split" "wedge is" no "5 degree wedge split in four patches"
# checkMesh's 2-D alignment error, as it prints it, makes a problem
cp -a "$out/cavity1" "$out/misaligned"
printf '    ***Number of edges not aligned with or perpendicular to non-empty directions: 12\n' >> "$out/misaligned/log.checkMesh"
check "$out/misaligned" "aren't square to its empty direction" yes "misaligned 2-D edges"
[ $fail = 0 ] && echo "preflight_shape: all passed"
exit $fail
