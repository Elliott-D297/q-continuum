#!/bin/bash
# salome_import.sh <bashrc> <scratch> <mesh_import.py>: Mesh > SALOME > Import a mesh's converter (assets/salome/mesh_import.py)
# on the meshes it has to cope with (tests/tools/salome_test_meshes.py), as page_salome.c runs it: a MED with a polyhedron
# (written as polyMesh), second-order MED and UNV (made first-order; the UNV only after ideasUnvToFoam refused it), a saved
# study. Each must pass checkMesh with its groups as patches. Skips (77) without SALOME.
set -u
bashrc=$1; out=$2/salome_import; script=$3
here=$(cd "$(dirname "$0")" && pwd)
salome=$(ls -d "$HOME"/salome/SALOME-*/salome 2>/dev/null | tail -1)
[ -x "$salome" ] || { echo "SALOME not installed: skipped"; exit 77; }
rm -rf "$out"; mkdir -p "$out/src"
run_salome() { env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME "$salome" -t --shutdown-servers=1 "$@"; }
(cd "$out/src" && run_salome "$here/tools/salome_test_meshes.py") > "$out/make.log" 2>&1
for f in poly.med quad.med quad.unv study.hdf; do [ -s "$out/src/$f" ] || { echo "FAIL: no $f"; tail -20 "$out/make.log"; exit 1; }; done
set +u --; source "$bashrc" > /dev/null 2>&1; set -u   # (its own arguments: none)
fails=0
check() {   # case, what its boundary must name
    local c=$1; shift
    (cd "$c" && checkMesh > log.checkMesh 2>&1)
    grep -q 'Mesh OK' "$c/log.checkMesh" || { echo "FAIL $c: checkMesh"; fails=$((fails + 1)); return; }
    for p in "$@"; do grep -q "^ *$p\$" "$c/constant/polyMesh/boundary" || { echo "FAIL $c: no patch $p"; fails=$((fails + 1)); }; done
    echo "ok   $(basename "$c"): $(grep -m1 'cells:' "$c/log.checkMesh" | awk '{print $2}') cells"
}
for f in poly.med quad.med study.hdf quad.unv; do
    k=${f#*.}; c=$out/${f%.*}_$k; mkdir -p "$c/system" "$c/salome"
    cp "$FOAM_TUTORIALS/incompressible/icoFoam/cavity/cavity/system/"{controlDict,fvSchemes,fvSolution} "$c/system/"
    cp "$out/src/$f" "$c/salome/imported.$k"; cp "$script" "$c/salome/.qc-mesh-import.py"
    conv() { (cd "$c/salome" && QC_IN="$PWD/imported.$1" QC_OUT="$PWD/imported.unv" QC_POLY="$PWD/polyMesh-import" run_salome .qc-mesh-import.py) >> "$c/log.salome" 2>&1; }
    if [ "$k" = unv ]; then
        (cd "$c" && ideasUnvToFoam salome/imported.unv > log.first 2>&1) && { echo "FAIL $c: ideasUnvToFoam read second-order elements (the fallback is untested)"; fails=$((fails + 1)); }
        mv "$c/salome/imported.unv" "$c/salome/imported-orig.unv"; k=orig.unv
        (cd "$c/salome" && QC_IN="$PWD/imported-orig.unv" QC_OUT="$PWD/imported.unv" QC_POLY="$PWD/polyMesh-import" run_salome .qc-mesh-import.py) > "$c/log.salome" 2>&1
    else conv "$k"; fi
    if [ -d "$c/salome/polyMesh-import" ]; then mkdir -p "$c/constant" && cp -r "$c/salome/polyMesh-import" "$c/constant/polyMesh"
    else (cd "$c" && ideasUnvToFoam salome/imported.unv > log.ideasUnvToFoam 2>&1) || { echo "FAIL $c: ideasUnvToFoam"; tail -5 "$c/log.ideasUnvToFoam"; fails=$((fails + 1)); continue; }; fi
    case $f in poly.med) check "$c" inlet walls defaultFaces;; *) check "$c" side_wall end_0;; esac
done
grep -q 'polyhedra, which UNV cannot hold' "$out/poly_med/log.salome" || { echo "FAIL: poly.med not written as polyMesh"; fails=$((fails + 1)); }
grep -q 'made first-order' "$out/quad_med/log.salome" || { echo "FAIL: quad.med not made first-order"; fails=$((fails + 1)); }
[ $fails = 0 ] && echo "all ok"
exit $fails
