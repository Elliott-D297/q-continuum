#!/bin/bash
# make_cht_channel.sh <dest>: a 2-D channel meshed by blockMesh with two cellZones, an aluminium plate (5 mm) under an air
# gap (20 mm), 0.2 m long, 10 mm deep: the seed for t_cht (Physics > Setup, heat through solids). No physics files yet.
d=$1
[ -d "$d" ] && exit 0
set --
. "${WM_PROJECT_DIR:-/usr/lib/openfoam/openfoam2606}/etc/bashrc" 2>/dev/null || . /usr/lib/openfoam/openfoam2606/etc/bashrc
mkdir -p "$d/system" "$d/constant"
cat > "$d/system/blockMeshDict" <<'BM'
FoamFile { version 2.0; format ascii; class dictionary; object blockMeshDict; }
scale 1;
vertices ( (0 0 0) (0.2 0 0) (0.2 0.005 0) (0 0.005 0) (0.2 0.025 0) (0 0.025 0)
           (0 0 0.01) (0.2 0 0.01) (0.2 0.005 0.01) (0 0.005 0.01) (0.2 0.025 0.01) (0 0.025 0.01) );
blocks ( hex (0 1 2 3 6 7 8 9) plate (80 4 1) simpleGrading (1 1 1)
         hex (3 2 4 5 9 8 10 11) air (80 16 1) simpleGrading (1 1 1) );
boundary (
    inlet  { type patch; faces ((3 9 11 5)); }
    outlet { type patch; faces ((2 4 10 8)); }
    top    { type wall;  faces ((5 11 10 4)); }
    bottom { type wall;  faces ((0 1 7 6)); }
    plateEnds { type wall; faces ((0 6 9 3) (1 2 8 7)); }
    frontAndBack { type empty; faces ((0 3 2 1) (3 5 4 2) (6 7 8 9) (9 8 10 11)); }
);
BM
cat > "$d/system/controlDict" <<'CD'
FoamFile { version 2.0; format ascii; class dictionary; object controlDict; }
application     chtMultiRegionSimpleFoam;
startTime 0; endTime 100; deltaT 1; writeControl timeStep; writeInterval 100;
CD
printf 'FoamFile { version 2.0; format ascii; class dictionary; object fvSchemes; }\n' > "$d/system/fvSchemes"
printf 'FoamFile { version 2.0; format ascii; class dictionary; object fvSolution; }\n' > "$d/system/fvSolution"
(cd "$d" && blockMesh > log.blockMesh 2>&1)
