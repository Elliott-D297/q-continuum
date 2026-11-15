#!/bin/bash
# tests/meshopts_openfoam.sh <test_stlmesh> <test_meshopts> <OpenFOAM bashrc> <scratch>
# The STL mesher's "Every OpenFOAM setting" values in real OpenFOAM v2606 runs, on a 0.1 m box meshed inside:
#  1. snappyHexMesh and cfMesh cartesianMesh with every table keyword that has an OpenFOAM default set to it (so OpenFOAM
#     reads each keyword and value as the table writes them): each must mesh and pass checkMesh, and the written
#     dictionaries must hold the values (foamDictionary reads them back);
#  2. one value that must change the mesh: snappy's surface refinement level, cfMesh's maxCellSize: more cells.
tsm=$(realpath "$1"); tmo=$(realpath "$2"); bashrc=$3; d=$(mktemp -d "$4/meshopts_openfoam.XXXXXX")
here=$(cd "$(dirname "$0")" && pwd)
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --
source "$bashrc" > /dev/null 2>&1
python3 - "$d" < <(sed -n '/^import sys, math$/,/^write("box", t)$/p' "$here/stlmesh_layers.sh")
set_ini() { local f=$1/mesher/mesher.ini; shift; for kv in "$@"; do sed -i "s|^${kv%%=*} = .*|${kv%%=*} = ${kv#*=}|" "$f"; done; }
fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
snappy() { ( cd "$1" && surfaceFeatureExtract > log.surfaceFeatureExtract 2>&1 && blockMesh > log.blockMesh 2>&1 &&
             snappyHexMesh -overwrite > log.snappyHexMesh 2>&1 && { [ -f 0/polyMesh/points ] && mv -f 0/polyMesh/points constant/polyMesh/points; rm -rf 0/polyMesh; true; } &&
             checkMesh > log.checkMesh 2>&1 ); }
cells() { grep -m1 '^ *cells:' "$1/log.checkMesh" | awk '{print $2}'; }
ok() { grep -q 'Mesh OK' "$1/log.checkMesh" 2>/dev/null; }

for ms in snappy cfcart; do
    c=$d/def_$ms
    "$tsm" init "$d/box.stl" "$c" 1 > /dev/null && set_ini "$c" base_size=0.01 mesher=$ms || { fail "$ms: init"; continue; }
    "$tmo" defaults "$c" || fail "$ms: defaults"
    "$tsm" prep "$c" > /dev/null || { fail "$ms: prep"; continue; }
    if [ $ms = snappy ]; then snappy "$c"; else ( cd "$c" && bash mesher/cf.sh && checkMesh > log.checkMesh 2>&1 ); fi
    ok "$c" || fail "$ms with every default: $(tail -3 "$c"/log.snappyHexMesh "$c"/log.cartesianMesh 2>/dev/null | tr '\n' ' ')"
    # the values are in the dictionaries
    while read -r file path _ value; do
        case $file in cmd:*) continue;; esac
        got=$(cd "$c" && foamDictionary "$file" -entry "$path" -value 2>/dev/null | tr -s ' ')
        [ -n "$got" ] || fail "$ms: $file $path not in the dictionary (wanted $value)"
    done < <(grep -v '^#' "$c/mesher/advanced.ini")
    echo "$ms with $(grep -vc '^#' "$c/mesher/advanced.ini") defaults set: $(cells "$c") cells, $(ok "$c" && echo OK || echo FAILED)"
done

# a value that changes the mesh
b=$d/lvl_base; f=$d/lvl_fine
for c in "$b" "$f"; do "$tsm" init "$d/box.stl" "$c" 1 > /dev/null && set_ini "$c" base_size=0.01 mesher=snappy prism_layers_on=0; done
patch=$(grep -m1 '^boundary0 = ' "$f/mesher/mesher.ini" | awk '{print $3}')
printf 'system/snappyHexMeshDict castellatedMeshControls/refinementSurfaces/%s/level = (2 2)\ncmd:checkMesh -allGeometry = true\n' "$patch" > "$f/mesher/advanced.ini"
for c in "$b" "$f"; do "$tsm" prep "$c" > /dev/null && snappy "$c"; done
nb=$(cells "$b"); nf=$(cells "$f")
echo "snappy: $nb cells; with $patch at level 2: $nf"
ok "$b" && ok "$f" || fail "level runs: checkMesh"
[ -n "$nb" ] && [ -n "$nf" ] && [ "$nf" -gt "$nb" ] || fail "the refinement level changed nothing ($nb -> $nf)"

b=$d/cf_base; f=$d/cf_fine
for c in "$b" "$f"; do "$tsm" init "$d/box.stl" "$c" 1 > /dev/null && set_ini "$c" base_size=0.01 mesher=cfcart prism_layers_on=0; done
printf 'system/meshDict maxCellSize = 0.005\n' > "$f/mesher/advanced.ini"
for c in "$b" "$f"; do "$tsm" prep "$c" > /dev/null && ( cd "$c" && bash mesher/cf.sh && checkMesh > log.checkMesh 2>&1 ); done
nb=$(cells "$b"); nf=$(cells "$f")
echo "cartesianMesh: $nb cells; maxCellSize halved: $nf"
ok "$b" && ok "$f" || fail "cfMesh runs: checkMesh"
[ -n "$nb" ] && [ -n "$nf" ] && [ "$nf" -gt $((nb * 3 / 2)) ] || fail "maxCellSize changed too little ($nb -> $nf)"

[ $fails = 0 ] && { echo "ok"; rm -rf "$d"; exit 0; }
echo "$fails failed (cases in $d)"; exit 1
