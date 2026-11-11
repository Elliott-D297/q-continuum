#!/bin/bash
# tests/stlmesh_openfoam.sh <test_stlmesh> <OpenFOAM bashrc> <scratch> <NACA_Airfoil.STL> [meshers]
# The STL mesher's OpenFOAM meshers besides snappyHexMesh ("every mesher" plan, steps 2-3): a box meshed inside, a sphere
# in a wind tunnel and the NACA profile in 2-D, with cfMesh's cartesianMesh, pMesh and tetMesh (2-D: cartesian2DMesh),
# foamyHexMesh (2-D: foamyQuadMesh) and a cartesianMesh turned into polyhedra, as Generate runs them (test_stlmesh prep
# writes the commands to mesher/cf.sh or mesher/after.sh). Each must pass checkMesh (foamy: at most 2 failed checks, it is
# experimental) with the part's patches and types, the tunnel's inlet/outlet/farfield as patches, in 2-D one cell deep
# with front2D/back2D empty, and the case's controlDict as it was.
bin=$(realpath "$1"); bashrc=$2; d=$(mktemp -d "$3/stlmesh_openfoam.XXXXXX"); naca=$(realpath "$4"); meshers=${5:-cfcart cfpoly cftet foamy}
here=$(cd "$(dirname "$0")" && pwd)
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --
source "$bashrc" > /dev/null 2>&1
set -u
command -v cartesianMesh > /dev/null || { echo "SKIP: no cfMesh in this OpenFOAM"; exit 77; }
python3 - "$d" < <(sed -n '/^import sys, math$/,/^write("box", t)$/p' "$here/stlmesh_layers.sh")
set_ini() { local f=$1/mesher/mesher.ini; shift; for kv in "$@"; do sed -i "s|^${kv%%=*} = .*|${kv%%=*} = ${kv#*=}|" "$f"; done; }
cases=""
for ms in $meshers; do
    "$bin" init "$d/box.stl" "$d/box_$ms" 1 > /dev/null && set_ini "$d/box_$ms" base_size=0.01 mesher=$ms && cases="$cases box_$ms"
    "$bin" init "$d/sphere.stl" "$d/sphere_$ms" 1 > /dev/null && set_ini "$d/sphere_$ms" domain=external base_size=0.02 target_size_pct=25 \
        min_size_pct=12.5 upstream=1 downstream=2 sides=1 top=1 below=1 mesher=$ms && cases="$cases sphere_$ms"
done
for ms in cfcart foamy; do
    case " $meshers " in *" $ms "*)
        "$bin" init "$naca" "$d/naca_$ms" 0.001 > /dev/null && set_ini "$d/naca_$ms" domain=external flow_direction=+y up_direction=+z two_d=1 \
            two_d_normal=x base_size=0.05 target_size_pct=25 min_size_pct=6.25 mesher=$ms && cases="$cases naca_$ms";;
    esac
done
case " $meshers " in *" cfcart "*)                       # any mesh into polyhedra afterwards
    "$bin" init "$d/box.stl" "$d/box_cfpolyconv" 1 > /dev/null && set_ini "$d/box_cfpolyconv" base_size=0.01 mesher=cfcart poly_convert=1 && cases="$cases box_cfpolyconv";;
esac
for c in $cases; do
    ( cd "$d/$c" && cp system/controlDict system/controlDict.before 2>/dev/null; "$bin" prep . > prep.out 2>&1 && s=$(date +%s) &&
      { [ -f mesher/cf.sh ] && bash mesher/cf.sh || bash mesher/after.sh; } > cf.out 2>&1; echo $? > rc; echo $(( $(date +%s) - ${s:-0} )) > secs
      checkMesh > log.checkMesh 2>&1 ) &
    case $c in *foamy*) wait;; esac                        # (foamy runs alone: it is slow and fragile under load)
done
wait
fails=0
for c in $cases; do
    f=$d/$c; ok=$(grep -c 'Mesh OK' "$f/log.checkMesh" 2>/dev/null); cells=$(grep -m1 'cells:' "$f/log.checkMesh" 2>/dev/null | awk '{print $2}')
    bad=""
    [ "$(cat "$f/rc" 2>/dev/null)" = 0 ] || bad="the mesher failed ($(tail -1 "$f/cf.out" 2>/dev/null))"
    nfail=$(grep -oE 'Failed [0-9]+ mesh checks' "$f/log.checkMesh" 2>/dev/null | grep -oE '[0-9]+')
    case $c in *foamy*) [ "${ok:-0}" = 1 ] || [ "${nfail:-9}" -le 2 ] && ok=1;; esac
    [ -z "$bad" ] && [ "${ok:-0}" = 1 ] || bad="${bad:-checkMesh: $(grep -E '\*\*\*' "$f/log.checkMesh" | head -2 | tr '\n' ' ')}"
    [ -z "$bad" ] && ! cmp -s "$f/system/controlDict" "$f/system/controlDict.before" && [ -f "$f/system/controlDict.before" ] && bad="controlDict changed"
    case $c in *polyconv) [ -z "$bad" ] && ! grep -q 'End' "$f/log.polyDualMesh" 2>/dev/null && bad="polyDualMesh did not run";; esac
    b=$f/constant/polyMesh/boundary
    if [ -z "$bad" ]; then
        case $c in
        sphere_*|naca_*) for p in inlet outlet farfield; do foamDictionary "$b" -entry "entry0/$p/type" -value 2>/dev/null | grep -qx patch || bad="$p is not a patch"; done
                         grep -q 'type *wall' "$b" || bad="no wall patch for the part";;
        box_*) grep -q 'type *wall' "$b" || bad="no wall patch";;
        esac
        case $c in naca_*)
            for p in front2D back2D; do foamDictionary "$b" -entry "entry0/$p/type" -value 2>/dev/null | grep -qx empty || bad="$p is not empty"; done
            grep -q 'Mesh has 2 geometric (non-empty/wedge) directions' "$f/log.checkMesh" || bad="not a 2-D mesh";;
        esac
    fi
    printf '%-16s %6s cells  %4ss  %s\n' "$c" "${cells:--}" "$(cat "$f/secs" 2>/dev/null)" "${bad:-ok}"
    [ -z "$bad" ] || fails=$((fails + 1))
done
[ $fails = 0 ] && rm -rf "$d" && echo "stlmesh_openfoam: all passed"
exit $fails
