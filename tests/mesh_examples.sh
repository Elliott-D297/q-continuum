#!/bin/bash
# tests/mesh_examples.sh <test_stlmesh> <OpenFOAM bashrc> <scratch>
# The meshing tutorial's three example cases (meshexamples.c), made as its "Make the example case" button makes them and
# meshed as Generate meshes them (snappyHexMesh; the airfoil in 2-D with extrudeMesh): each must pass checkMesh, keep its
# boundaries (duct: inlet, outlet, walls; sphere: sphere + the tunnel; airfoil: airfoil, front2D/back2D empty, one cell
# deep) and stay small enough for a tutorial (under 200 000 cells, under 5 minutes).
tsm=$(realpath "$1"); bashrc=$2; d=$(mktemp -d "$3/mesh_examples.XXXXXX")
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --
source "$bashrc" > /dev/null 2>&1
fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
for e in duct sphere airfoil; do
    c=$d/$e
    "$tsm" example $e "$c" || { fail "$e: setup"; continue; }
    "$tsm" prep "$c" > /dev/null || { fail "$e: prep"; continue; }
    s=$(date +%s)
    ( cd "$c" && surfaceFeatureExtract > log.surfaceFeatureExtract 2>&1 && blockMesh > log.blockMesh 2>&1 &&
      snappyHexMesh -overwrite > log.snappyHexMesh 2>&1 && { [ -f 0/polyMesh/points ] && mv -f 0/polyMesh/points constant/polyMesh/points; rm -rf 0/polyMesh; true; } &&
      { [ ! -f mesher/2d.sh ] || bash mesher/2d.sh > log.extrudeMesh 2>&1; } && checkMesh > log.checkMesh 2>&1 ) || fail "$e: meshing ($(tail -2 "$c"/log.* 2>/dev/null | tail -2 | tr '\n' ' '))"
    t=$(( $(date +%s) - s ))
    cells=$(grep -m1 '^ *cells:' "$c/log.checkMesh" 2>/dev/null | awk '{print $2}')
    grep -q 'Mesh OK' "$c/log.checkMesh" 2>/dev/null || fail "$e: checkMesh $(grep -E '\*\*\*' "$c/log.checkMesh" | head -2 | tr '\n' ' ')"
    [ -n "$cells" ] && [ "$cells" -lt 200000 ] || fail "$e: $cells cells"
    [ $t -lt 300 ] || fail "$e: $t s"
    b=$c/constant/polyMesh/boundary
    case $e in
        duct) for p in inlet outlet walls; do grep -qw "$p" "$b" || fail "duct: no patch $p"; done;;
        sphere) for p in sphere inlet outlet farfield; do grep -qw "$p" "$b" || fail "sphere: no patch $p"; done;;
        airfoil) grep -qw airfoil "$b" || fail "airfoil: no patch airfoil"
                 for p in front2D back2D; do grep -A2 -w "$p" "$b" | grep -q 'type *empty' || fail "airfoil: $p is not empty"; done
                 grep -qE '^ *(front|back);?$' "$b" && fail "airfoil: the end caps became patches";;
    esac
    echo "$e: $cells cells in $t s, $(grep -q 'Mesh OK' "$c/log.checkMesh" 2>/dev/null && echo OK || echo FAILED)"
done
[ $fails = 0 ] && { echo ok; rm -rf "$d"; exit 0; }
echo "$fails failed (cases in $d)"; exit 1
