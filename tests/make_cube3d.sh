#!/bin/bash
# make_cube3d.sh <dest>: a lid-driven unit cube (icoFoam cavity made 3-D, 20^3 cells, Re 100) run to 0.5 s with five times
# written: the seed for Results > Visualization's journey (t_viz)
d=$1
[ -d "$d" ] && exit 0
. "${WM_PROJECT_DIR:-/usr/lib/openfoam/openfoam2606}/etc/bashrc" 2>/dev/null || . /usr/lib/openfoam/openfoam2606/etc/bashrc
cp -a "$FOAM_TUTORIALS/incompressible/icoFoam/cavity/cavity" "$d" || exit 1
python3 -I - "$d/system/blockMeshDict" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p).read()
s = re.sub(r'scale\s+[0-9.]+;', 'scale 1;', s)
s = re.sub(r'vertices\s*\((.|\n)*?\);', 'vertices ( (0 0 0) (1 0 0) (1 1 0) (0 1 0) (0 0 1) (1 0 1) (1 1 1) (0 1 1) );', s, count=1)
s = s.replace('(20 20 1)', '(20 20 20)').replace('type empty;', 'type wall;')
open(p, 'w').write(s)
PY
sed -i 's/^endTime .*/endTime 0.5;/; s/^deltaT .*/deltaT 0.005;/; s/^writeInterval .*/writeInterval 20;/' "$d/system/controlDict"
sed -i 's/nu *\[.*\] *[0-9.e-]*;/nu [0 2 -1 0 0 0 0] 0.01;/; s/^nu .*/nu 0.01;/' "$d/constant/transportProperties"
for f in p U; do sed -i 's/frontAndBack/frontAndBack/' "$d/0/$f"; done
sed -i '/frontAndBack/,/}/ s/type *empty;/type zeroGradient;/' "$d/0/p"
sed -i '/frontAndBack/,/}/ s/type *empty;/type noSlip;/' "$d/0/U"
(blockMesh -case "$d" && icoFoam -case "$d") > "$d/log.make" 2>&1 || { echo "make_cube3d failed: $d/log.make"; exit 1; }
# "par": the same run in parallel (4 processors) and not reconstructed: its times only in processor*/
if [ "${2:-}" = par ]; then
    rm -rf "$d"/0.[1-9]* "$d"/[1-9]*
    cat > "$d/system/decomposeParDict" <<'D'
FoamFile { version 2.0; format ascii; class dictionary; object decomposeParDict; }
numberOfSubdomains 4;
method simple;
coeffs { n (2 2 1); }
D
    (decomposePar -case "$d" && mpirun -np 4 --oversubscribe icoFoam -parallel -case "$d") >> "$d/log.make" 2>&1 || { echo "make_cube3d par failed: $d/log.make"; exit 1; }
fi
