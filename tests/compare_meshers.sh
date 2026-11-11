#!/bin/bash
# compare_meshers.sh <test_stlmesh> <OpenFOAM bashrc> <scratch> <NACA_Airfoil.STL> <flange.stl> [csv]
# "Every mesher" plan, step 9: each mesher on four parts (a box duct inside, the flange inside, a sphere in a wind tunnel, the
# NACA profile in 2-D) from the same settings, as Generate runs them; time, cells, checkMesh, worst non-orthogonality and
# skewness, and the app's own estimate, one CSV row each. Not a pass/fail test: the numbers behind the recommendations.
bin=$(realpath "$1"); bashrc=$2; d=$3/compare_meshers; naca=$(realpath "$4"); flange=$(realpath "$5"); csv=${6:-$d/compare.csv}
here=$(cd "$(dirname "$0")" && pwd); py=$here/../assets/salome/stl_mesher.py
salome=$(ls -d "$HOME"/salome/SALOME-*/salome 2>/dev/null | tail -1)
set --; source "$bashrc" > /dev/null 2>&1; set -u
rm -rf "$d"; mkdir -p "$d"
python3 - "$d" < <(sed -n '/^import sys, math$/,/^write("box", t)$/p' "$here/stlmesh_layers.sh")
set_ini() { local f=$1/mesher/mesher.ini; shift; for kv in "$@"; do sed -i "s|^${kv%%=*} = .*|${kv%%=*} = ${kv#*=}|" "$f"; done; }
echo "part,mesher,seconds,cells,checkMesh,max_non_ortho,max_skewness,estimate" > "$csv"
run() {   # part stl scale mesher settings...
    local part=$1 stl=$2 sc=$3 ms=$4; shift 4
    local c=$d/${part}_$ms
    "$bin" init "$stl" "$c" "$sc" > /dev/null && set_ini "$c" mesher=$ms "$@"
    local est; est=$("$bin" prep "$c" 2>&1 | grep -oE 'about [0-9]+ cells' | grep -oE '[0-9]+')
    local s=$(date +%s.%N) rc
    (
        cd "$c" || exit 1
        case $ms in
            snappy) QC_NO_PREP=1 timeout 1800 bash "$here/stlmesh_mesh.sh" "$bin" . > run.out 2>&1 ;;
            cf*|foamy) rm -rf constant/polyMesh; timeout 1800 bash mesher/$([ -f mesher/cf.sh ] && echo cf.sh || echo after.sh) > run.out 2>&1 ;;
            *) cp "$py" salome/stl_mesh.py
               (cd salome && rm -rf mesh.unv polyMesh && timeout 1800 env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME "$salome" -t --shutdown-servers=1 stl_mesh.py) > log.salome 2>&1
               if [ -d salome/polyMesh ]; then rm -rf constant/polyMesh; mkdir -p constant && cp -r salome/polyMesh constant/; else ideasUnvToFoam salome/mesh.unv > log.unv 2>&1; fi
               [ $ms = poly ] && grep -q '^two_d = 0' mesher/mesher.ini && polyDualMesh 30 -overwrite -concaveMultiCells > log.polyDual 2>&1
               grep -q '^two_d = 1' mesher/mesher.ini && for p in front2D back2D; do foamDictionary constant/polyMesh/boundary -entry entry0/$p/type -set empty > /dev/null 2>&1; done ;;
        esac
        checkMesh > log.checkMesh 2>&1
    )
    local t; t=$(echo "$(date +%s.%N) - $s" | bc)
    local L=$c/log.checkMesh
    local cells; cells=$(awk '/^ *cells:/ {print $2; exit}' "$L" 2>/dev/null)
    local st; st=$(grep -oE 'Mesh OK|Failed [0-9]+ mesh checks' "$L" 2>/dev/null | head -1); [ -n "$cells" ] || st="no mesh"
    local no; no=$(grep -oE 'Mesh non-orthogonality Max: [0-9.e+-]+' "$L" 2>/dev/null | awk '{print $4}')
    local sk; sk=$(grep -oE 'Max skewness = [0-9.e+-]+' "$L" 2>/dev/null | awk '{print $4}')
    printf '%s,%s,%.0f,%s,%s,%s,%s,%s\n' "$part" "$ms" "$t" "${cells:-0}" "$st" "${no:-}" "${sk:-}" "${est:-}" | tee -a "$csv"
}
box="base_size=0.01"
fl="base_size=0.002"
sph="domain=external base_size=0.02 target_size_pct=25 min_size_pct=12.5 upstream=1 downstream=2 sides=1 top=1 below=1"
na="domain=external flow_direction=+y up_direction=+z two_d=1 two_d_normal=x base_size=0.05 target_size_pct=25 min_size_pct=6.25"
for ms in snappy cfcart cfpoly cftet foamy tet poly bodyfit sgmsh shexa sprism; do run box "$d/box.stl" 1 $ms $box; done
for ms in snappy cfcart cfpoly cftet foamy tet poly bodyfit sgmsh; do run flange "$flange" 1 $ms $fl; done
for ms in snappy cfcart cfpoly cftet foamy tet poly bodyfit sgmsh; do run sphere "$d/sphere.stl" 1 $ms $sph; done
for ms in snappy cfcart foamy tet bodyfit; do run naca "$naca" 0.001 $ms $na; done
echo "wrote $csv"
