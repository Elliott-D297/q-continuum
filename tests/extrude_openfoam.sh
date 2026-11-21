#!/bin/bash
# extrude_openfoam.sh <test_extrude> <test_bmesh> <OpenFOAM bashrc> <scratch>: extrudeMesh as the app runs it (mesh plan step 6)
#  - a 2-D channel above the x axis, below it, and beside the y axis turned into 5-degree wedges: checkMesh "Mesh OK", the
#    wedge patches at 2.5 degrees, no "not planar" warnings (writePrecision raised while writing), prisms on the axis
#  - a channel's outlet extruded 0.5 m in 10 layers onto the mesh
te=$1; tb=$2; rc=$3; out=$4/extrude_openfoam
set --
. "$rc" || exit 2
rm -rf "$out"; mkdir -p "$out"
fail=0
"$te" unit || fail=1
mk() {  # mk <dir> <blockMeshDict vertices line>
    mkdir -p "$1/system"; cp "$FOAM_TUTORIALS"/incompressible/icoFoam/cavity/cavity/system/{controlDict,fvSchemes,fvSolution} "$1/system/"
    cat > "$1/system/blockMeshDict" <<B
FoamFile { version 2.0; format ascii; class dictionary; object blockMeshDict; }
vertices ( $2 );
blocks ( hex (0 1 2 3 4 5 6 7) (40 10 1) simpleGrading (1 1 1) );
boundary (
 inlet { type patch; faces ((0 4 7 3)); } outlet { type patch; faces ((1 2 6 5)); }
 wall { type wall; faces ((3 7 6 2)); } axis { type patch; faces ((0 1 5 4)); }
 frontAndBack { type empty; faces ((0 3 2 1) (4 5 6 7)); }
);
B
    blockMesh -case "$1" > "$1/log.blockMesh" 2>&1
}
axi() {  # axi <name> <vertices> <axisDir> <axisAt> <prisms: the cells along the axis>
    d=$out/$1; mk "$d" "$2"
    "$te" axi "$d" $3 $4 5 || { echo "FAIL $1: dictionaries"; fail=1; return; }
    (cd "$d" && foamDictionary -entry writePrecision -set 15 system/controlDict > /dev/null &&
     topoSet -dict system/topoSetDict.axi > log.topoSet 2>&1 && createPatch -dict system/createPatchDict.axi -overwrite > log.createPatch 2>&1 &&
     extrudeMesh > log.extrudeMesh 2>&1) || { echo "FAIL $1: tools"; tail -3 "$d"/log.*; fail=1; return; }
    checkMesh -case "$d" > "$d/log.checkMesh" 2>&1
    if grep -q '^Mesh OK' "$d/log.checkMesh" && ! grep -q 'not planar' "$d/log.checkMesh" && grep -q 'Wedge front with angle 2.4999' "$d/log.checkMesh" &&
       grep -q "prisms: *$5\$" "$d/log.checkMesh"; then echo "$1: Mesh OK, wedge 2 x 2.5 degrees, $5 prisms on the axis"
    else echo "FAIL $1"; grep -E 'Wedge|prisms|\*\*\*|planar' "$d/log.checkMesh" | sort | uniq -c | head; fail=1; fi
}
# the axis at y = 0 along x, the mesh above it: the 40 cells along the axis become prisms
axi above "(0 0 0) (1 0 0) (1 0.05 0) (0 0.05 0) (0 0 0.01) (1 0 0.01) (1 0.05 0.01) (0 0.05 0.01)" 0 0 40
axi below "(0 -0.05 0) (1 -0.05 0) (1 0 0) (0 0 0) (0 -0.05 0.01) (1 -0.05 0.01) (1 0 0.01) (0 0 0.01)" 0 0 40
# the axis along y at x = 0: the mesh to its right (the 'axis' patch is then the inlet side)
axi beside "(0 0 0) (0.05 0 0) (0.05 1 0) (0 1 0) (0 0 0.01) (0.05 0 0.01) (0.05 1 0.01) (0 1 0.01)" 1 0 10
# crossing the axis is refused
d=$out/across; mk "$d" "(0 -0.05 0) (1 -0.05 0) (1 0.05 0) (0 0.05 0) (0 -0.05 0.01) (1 -0.05 0.01) (1 0.05 0.01) (0 0.05 0.01)"
"$te" axi "$d" 0 0 5 | grep -q 'crosses the axis' || { echo "FAIL across: not refused"; fail=1; }
# a patch extruded onto the mesh
d=$out/outlet; mk "$d" "(0 0 0) (1 0 0) (1 0.05 0) (0 0.05 0) (0 0 0.01) (1 0 0.01) (1 0.05 0.01) (0 0.05 0.01)"
"$te" patch "$d" outlet 0 10 0.5 && (cd "$d" && extrudeMesh > log.extrudeMesh 2>&1) && checkMesh -case "$d" > "$d/log.checkMesh" 2>&1
if grep -q '^Mesh OK' "$d/log.checkMesh" && grep -q 'cells: *500$' "$d/log.checkMesh" && grep -q 'bounding box (0 0 0) (1.5 ' "$d/log.checkMesh"; then
    echo "outlet: 10 layers, 0.5 m, Mesh OK"
else echo "FAIL outlet"; grep -E 'cells:|bounding|\*\*\*' "$d/log.checkMesh"; fail=1; fi
[ $fail = 0 ] && echo "extrude_openfoam: all passed"
exit $fail
