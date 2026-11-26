#!/bin/bash
# viz_openfoam.sh <test_viz> <OpenFOAM bashrc> <scratch>: the visualization engine on a real mesh (plan V1-V2): a unit cube of
# 10 x 10 x 10 cells by blockMesh, fields with known answers written onto it, checked; then the case converted to binary by
# foamFormatConvert must load the same
tv=$1; rc=$2; out=$3/viz_openfoam
set --
. "$rc" || exit 2
rm -rf "$out"; mkdir -p "$out"
c=$out/cube
cp -a "$FOAM_TUTORIALS/incompressible/icoFoam/cavity/cavity" "$c"
python3 - "$c/system/blockMeshDict" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p).read()
s = re.sub(r'scale\s+[0-9.]+;', 'scale 1;', s)
s = re.sub(r'convertToMeters\s+[0-9.]+;', 'convertToMeters 1;', s)
s = re.sub(r'vertices\s*\((.|\n)*?\);', 'vertices ( (0 0 0) (1 0 0) (1 1 0) (0 1 0) (0 0 1) (1 0 1) (1 1 1) (0 1 1) );', s, count=1)
s = s.replace('(20 20 1)', '(10 10 10)').replace('type empty;', 'type wall;')
open(p, 'w').write(s)
PY
blockMesh -case "$c" > "$c/log.blockMesh" 2>&1 || { echo "FAIL blockMesh"; tail -5 "$c/log.blockMesh"; exit 1; }
rm -rf "$c/0"
fail=0
"$tv" write "$c" && "$tv" check "$c" || fail=1
b=$out/cube_binary
cp -a "$c" "$b"
foamDictionary "$b/system/controlDict" -entry writeFormat -set binary > /dev/null
(cd "$b" && foamFormatConvert > log.convert 2>&1) || { echo "FAIL foamFormatConvert"; fail=1; }
grep -q 'format *binary' "$b/0/T" || { echo "FAIL: 0/T not converted to binary"; fail=1; }
grep -q 'format *binary' "$b/constant/polyMesh/faces" || echo "(the mesh stayed ASCII)"
"$tv" same "$c" "$b" || fail=1
"$tv" check "$b" || fail=1
# decomposed into 4 (full precision, so the copies are exact): the processors' fields put back on the whole mesh
dp=$out/cube_procs
cp -a "$c" "$dp"
foamDictionary "$dp/system/controlDict" -entry writePrecision -set 17 > /dev/null
cat > "$dp/system/decomposeParDict" <<'D'
FoamFile { version 2.0; format ascii; class dictionary; object decomposeParDict; }
numberOfSubdomains 4;
method simple;
coeffs { n (2 2 1); }
D
decomposePar -case "$dp" > "$dp/log.decomposePar" 2>&1 || { echo "FAIL decomposePar"; tail -5 "$dp/log.decomposePar"; fail=1; }
"$tv" procs "$dp" || fail=1
[ $fail = 0 ] && echo "viz_openfoam: all passed"
exit $fail
