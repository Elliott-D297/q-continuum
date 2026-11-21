#!/bin/bash
# bmesh_openfoam.sh <test_bmesh> <OpenFOAM bashrc> <scratch>: the block builder against OpenFOAM v2606 (mesh plan step 5).
#  1. every template (defaults, and the 3-D channel) meshed by blockMesh and passed by checkMesh ("Mesh OK")
#  2. every tutorial blockMeshDict the builder can read, put back through it (bm_apply): blockMesh on the original and on
#     ours must give the same points, faces and boundary
tb=$1; rc=$2; out=$3/bmesh_openfoam
set --
. "$rc" || exit 2
rm -rf "$out"; mkdir -p "$out"
fail=0
"$tb" templates "$out/t" || fail=1
for d in "$out"/t/*/; do
    n=$(basename "$d")
    cp "$FOAM_TUTORIALS"/incompressible/icoFoam/cavity/cavity/system/{controlDict,fvSchemes,fvSolution} "$d/system/"
    if ! blockMesh -case "$d" > "$d/log.blockMesh" 2>&1; then echo "FAIL $n: blockMesh"; tail -5 "$d/log.blockMesh"; fail=1; continue; fi
    checkMesh -case "$d" > "$d/log.checkMesh" 2>&1
    if grep -q '^Mesh OK' "$d/log.checkMesh"; then printf "%-10s Mesh OK  %s cells, max non-orth %s, max skew %s\n" "$n" \
        "$(awk '/^ *cells:/{print $2; exit}' "$d/log.checkMesh")" "$(grep -o 'non-orthogonality Max: [0-9.e+-]*' "$d/log.checkMesh" | awk '{print $3}')" \
        "$(grep -o 'Max skewness = [0-9.e+-]*' "$d/log.checkMesh" | awk '{print $4}')"
    else echo "FAIL $n: checkMesh"; grep -E '\*\*\*|Failed' "$d/log.checkMesh"; fail=1; fi
done
"$tb" roundtrip "$FOAM_TUTORIALS" "$out/rt" || fail=1
same=0; diff_=0; skip=0
for d in "$out"/rt/*/; do
    ok=1
    for k in orig ours; do
        cp "$FOAM_TUTORIALS"/incompressible/icoFoam/cavity/cavity/system/{controlDict,fvSchemes,fvSolution} "$d/$k/system/"
        blockMesh -case "$d/$k" > "$d/$k/log.blockMesh" 2>&1 || ok=0
    done
    if [ $ok = 0 ]; then
        # the original itself needs what isn't here (an #include'd file, a geometry from the case): not ours to judge
        if blockMesh -case "$d/orig" > /dev/null 2>&1; then echo "FAIL roundtrip $(cat "$d/source"): ours does not mesh"; tail -3 "$d/ours/log.blockMesh"; fail=1; diff_=$((diff_ + 1))
        else skip=$((skip + 1)); fi
        continue
    fi
    if cmp -s "$d/orig/constant/polyMesh/points" "$d/ours/constant/polyMesh/points" 2>/dev/null || \
       diff <(sed '1,/^(/d' "$d/orig/constant/polyMesh/points") <(sed '1,/^(/d' "$d/ours/constant/polyMesh/points") > /dev/null; then
        if diff <(sed '1,/^(/d' "$d/orig/constant/polyMesh/faces") <(sed '1,/^(/d' "$d/ours/constant/polyMesh/faces") > /dev/null &&
           diff <(sed '1,/^FoamFile/d' "$d/orig/constant/polyMesh/boundary" | grep -v '^//') <(sed '1,/^FoamFile/d' "$d/ours/constant/polyMesh/boundary" | grep -v '^//') > /dev/null; then
            same=$((same + 1))
        else echo "FAIL roundtrip $(cat "$d/source"): faces or boundary differ"; fail=1; diff_=$((diff_ + 1)); fi
    else echo "FAIL roundtrip $(cat "$d/source"): points differ"; fail=1; diff_=$((diff_ + 1)); fi
done
echo "roundtrip: $same identical meshes, $diff_ different, $skip skipped (the original doesn't mesh on its own)"
[ $fail = 0 ] && echo "bmesh_openfoam: all passed"
exit $fail
