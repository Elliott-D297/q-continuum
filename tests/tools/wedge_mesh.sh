#!/bin/bash
# wedge_mesh.sh <case dir> <wedge angle in degrees>: a pipe 1 m long, radius 0.1 m, as an axisymmetric wedge (axis x)
d=$1; a=$2
mkdir -p $d/system $d/constant $d/0
python3 - "$d" "$a" <<'PY'
import sys, math
d, a = sys.argv[1], float(sys.argv[2])
h = math.radians(a / 2); R = 0.1; c, s = R * math.cos(h), R * math.sin(h)
open(d + "/system/blockMeshDict", "w").write(f"""FoamFile {{ version 2.0; format ascii; class dictionary; object blockMeshDict; }}
scale 1;
vertices ( (0 0 0) (1 0 0) (1 {c} {-s}) (0 {c} {-s}) (1 {c} {s}) (0 {c} {s}) );
blocks ( hex (0 1 2 3 0 1 4 5) (20 10 1) simpleGrading (1 1 1) );
boundary (
  inlet {{ type patch; faces ( (0 3 5 0) ); }}
  outlet {{ type patch; faces ( (1 4 2 1) ); }}
  wall {{ type wall; faces ( (3 2 4 5) ); }}
  front {{ type wedge; faces ( (0 1 4 5) ); }}
  back {{ type wedge; faces ( (0 3 2 1) ); }}
  axis {{ type empty; faces ( (0 1 1 0) ); }}
);
""")
open(d + "/system/controlDict", "w").write("FoamFile { version 2.0; format ascii; class dictionary; object controlDict; }\napplication icoFoam;\nstartTime 0;\nendTime 1;\ndeltaT 1;\nwriteControl timeStep;\nwriteInterval 1;\n")
PY
cp "$FOAM_TUTORIALS"/incompressible/icoFoam/cavity/cavity/system/fv* "$d/system/"
