#!/bin/bash
# tests/stlmesh_salome.sh <test_stlmesh> <OpenFOAM bashrc> <scratch> <stl_mesher.py>
# The STL mesher's SALOME route (assets/salome/stl_mesher.py), run as Generate runs it (page_stlmesh.c): SALOME without
# its window, then ideasUnvToFoam (or salome/polyMesh for Body Fitting's polyhedra), the boundary types and checkMesh.
#   box_tri    a 12-triangle box on its own triangles: the surface must be remeshed, each side its own flat patch
#   sphere_tri a UV sphere (slivers at the poles) on its own triangles: remeshed, no flat tetrahedra
#   box_cad    the box remeshed from CAD with prism layers inside it
#   box_fit    Body Fitting in a wind tunnel, off the grid (polyhedra: salome/polyMesh), with its inlet/outlet/farfield
#   sphere_poly the sphere's tetrahedra turned into polyhedra by polyDualMesh
#   sphere_lay the sphere in a wind tunnel on its own triangles: prism layers grown out of its surface
#   naca_2d    the user's NACA profile, 2-D: cut out as CAD, NETGEN triangles with boundary layers, one cell (front2D, back2D)
#   naca_quad  the same, mostly quadrilaterals (bodyfit in 2-D)
# Skipped (77) without SALOME: $QC_SALOME, else ~/salome/SALOME-*/binsalome.
bin=$1; bashrc=$2; d=$(mktemp -d "$3/stlmesh_salome.XXXXXX"); py=$(realpath "$4")
here=$(cd "$(dirname "$0")" && pwd)
salome=${QC_SALOME:-$(ls -d "$HOME"/salome/SALOME-*/binsalome 2>/dev/null | tail -1)}
[ -x "$salome" ] || { echo "SKIP: no SALOME (set QC_SALOME to its binsalome)"; exit 77; }
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
# the box and sphere STLs of stlmesh_layers.sh
python3 - "$d" < <(sed -n '/^import sys, math$/,/^write("box", t)$/p' "$here/stlmesh_layers.sh")
set --
source "$bashrc" > /dev/null 2>&1
set -u
set_ini() { local f=$1/mesher/mesher.ini; shift; for kv in "$@"; do sed -i "s|^${kv%%=*} = .*|${kv%%=*} = ${kv#*=}|" "$f"; done; }
tunnel="domain=external upstream=1 downstream=2 sides=1 top=1 below=1"
mk() { local c=$d/$1 stl=$d/$2.stl sc=1; shift 2; [ -f "$stl" ] || { stl=$here/tools/$(basename "$stl" .stl).STL; sc=0.001; }
       "$bin" init "$stl" "$c" $sc > /dev/null && set_ini "$c" "$@" && "$bin" prep "$c" > /dev/null && cp "$py" "$c/salome/stl_mesh.py"; }
mk box_tri box mesher=tet base_size=0.01
mk sphere_tri sphere mesher=tet base_size=0.01
mk box_cad box mesher=tet remesh_surface=1 base_size=0.01 prism_layers=3 prism_thickness_pct=30
mk box_fit box mesher=bodyfit base_size=0.03 $tunnel     # off the grid: cut cells (polyhedra, salome/polyMesh)
mk sphere_poly sphere mesher=poly base_size=0.01
mk sphere_lay sphere mesher=tet base_size=0.02 prism_layers=4 $tunnel
naca="NACA_Airfoil two_d=1 two_d_normal=x flow_direction=+y base_size=0.05 target_size_pct=25 min_size_pct=6.25 prism_layers=5 $tunnel"
mk naca_2d $naca mesher=tet
mk naca_quad $naca mesher=bodyfit
# "every mesher" step 4: Gmsh (two of its 3-D algorithms), structured hexahedra and prism sweeping on a block, MMG's surface
# remesher, and structured hexahedra refused on a sphere with the reason
mk box_gmsh box mesher=sgmsh base_size=0.01 gmsh_algorithm=delaunay
mk sphere_gmsh sphere mesher=sgmsh base_size=0.02 gmsh_algorithm=hxt $tunnel
mk box_hexa box mesher=shexa base_size=0.01
mk box_prism box mesher=sprism base_size=0.01
mk box_mmg box mesher=tet base_size=0.01 surface_remesher=mmg
mk sphere_hexa sphere mesher=shexa base_size=0.01
fails=0
for c in box_tri sphere_tri box_cad box_fit sphere_poly sphere_lay naca_2d naca_quad box_gmsh sphere_gmsh box_hexa box_prism box_mmg sphere_hexa; do
    (
        cd "$d/$c" || exit 1
        ( cd salome && rm -rf mesh.unv polyMesh && env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME "$salome" -t --shutdown-servers=1 stl_mesh.py ) > log.salome 2>&1
        if [ -d salome/polyMesh ]; then mkdir -p constant && cp -r salome/polyMesh constant/
        else ideasUnvToFoam salome/mesh.unv > log.ideasUnvToFoam 2>&1 || exit 1; fi
        [ "$c" != sphere_poly ] || polyDualMesh 30 -overwrite -concaveMultiCells > log.polyDualMesh 2>&1 || exit 1
        case $c in naca_*) for p in front2D back2D; do foamDictionary constant/polyMesh/boundary -entry entry0/$p/type -set empty > /dev/null 2>&1; done;; esac
        checkMesh > log.checkMesh 2>&1
    )
    log=$d/$c/log.checkMesh
    patches=$(awk '/Checking patch topology/,/Checking faceZone/' "$log" 2>/dev/null | awk 'NF > 3 && $1 != "Patch" && $1 != "Checking" && $1 != "\".*\"" {printf "%s(%s) ", $1, $2}')
    cells=$(awk '/^ *cells:/ {print $2; exit}' "$log" 2>/dev/null)
    echo "$c: $(grep -m1 -oE 'Mesh OK|Failed [0-9]+ mesh checks' "$log" 2>/dev/null) ${cells:-0} cells; $patches"
    grep '^Q-Continuum:' "$d/$c/log.salome" | sed 's/^/    /'
    bad=""
    if [ $c = sphere_hexa ]; then                          # refused, with the reason, before anything is meshed
        grep -q 'need every solid to be a block' "$d/$c/log.salome" || { echo "FAIL $c: not refused with the reason"; tail -5 "$d/$c/log.salome"; fails=$((fails + 1)); }
        continue
    fi
    grep -q 'Mesh OK' "$log" 2>/dev/null || bad="checkMesh"
    case $c in
        box_tri)    [ "$(grep -o 'box_[0-9]([0-9]*)' <<< "$patches" | wc -l)" = 6 ] && [ "${cells:-0}" -gt 5000 ] || bad="$bad patches/cells" ;;
        sphere_tri) grep -q 'remeshing its surface' "$d/$c/log.salome" || bad="$bad not remeshed" ;;
        box_cad)    grep -qE ' [1-9][0-9]* prisms' "$d/$c/log.salome" || bad="$bad no prisms" ;;
        box_fit)    for p in inlet outlet farfield box_1; do grep -q "$p(" <<< "$patches" || bad="$bad no $p"; done
                    [ -d "$d/$c/salome/polyMesh" ] || bad="$bad no polyMesh" ;;
        sphere_poly) grep -q 'polyhedra' "$d/$c/log.checkMesh" || bad="$bad no polyhedra" ;;
        sphere_lay) grep -q 'grown out of' "$d/$c/log.salome" || bad="$bad no grown layers"
                    grep -q 'sphere(' <<< "$patches" || bad="$bad no sphere patch" ;;
        box_gmsh|sphere_gmsh) grep -q 'Gmsh, 3-D algorithm' "$d/$c/log.salome" || bad="$bad not Gmsh"
                    [ "$(awk '/tetrahedra:/ {print $2; exit}' "$log")" -gt 500 ] || bad="$bad few tetrahedra" ;;
        box_hexa)   [ "$(awk '/hexahedra:/ {print $2; exit}' "$log")" -gt 1000 ] && [ "$(awk '/tetrahedra:/ {print $2; exit}' "$log")" = 0 ] || bad="$bad not all hexahedra" ;;
        box_prism)  [ "$(awk '/prisms:/ {print $2; exit}' "$log")" -gt 1000 ] || bad="$bad few prisms" ;;
        box_mmg)    grep -q 'surface remeshed by MMG' "$d/$c/log.salome" || bad="$bad not remeshed by MMG" ;;
        naca_*)     grep -q 'boundary layers:' "$d/$c/log.salome" || bad="$bad no boundary layers"
                    grep -q '2 solution (non-empty) directions' "$d/$c/log.checkMesh" || bad="$bad not 2-D"
                    for p in front2D back2D inlet outlet farfield wall_3; do grep -q "$p(" <<< "$patches" || bad="$bad no $p"; done
                    [ $c != naca_quad ] || [ "$(awk '/hexahedra:/ {print $2; exit}' "$d/$c/log.checkMesh")" -gt 2000 ] || bad="$bad few hexahedra" ;;
    esac
    [ -z "$bad" ] || { echo "FAIL $c:$bad"; tail -20 "$d/$c/log.salome"; fails=$((fails + 1)); }
done
[ $fails = 0 ] && rm -rf "$d"
exit $fails
