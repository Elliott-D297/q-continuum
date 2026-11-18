#!/bin/bash
# fea_routes.sh <test_femesh> <scratch>: a part study from each kind of file (docs/FEA_PLAN.md F1). SALOME makes a
# 100 x 10 x 10 mm beam as BREP, STL, and a meshed MED and UNV (linear tetrahedra, face groups "fixed" and "load");
# fea_mesh.py meshes each; the study (fixed at x = 0, 100 N down at x = 100) runs in ccx against F L³ / 3 E I.
set -u
tf=$1; d=$2/fea_routes; rm -rf "$d"; mkdir -p "$d"
here=$(cd "$(dirname "$0")" && pwd)
command -v salome > /dev/null || { echo "skip: no SALOME"; exit 0; }
command -v ccx > /dev/null || { echo "skip: no ccx"; exit 0; }
cat > "$d/mk.py" <<PY
import salome; salome.salome_init()
import SMESH
from salome.geom import geomBuilder
from salome.smesh import smeshBuilder
g = geomBuilder.New(); sm = smeshBuilder.New()
b = g.MakeBoxDXDYDZ(100, 10, 10)
g.ExportBREP(b, "$d/beam.brep"); g.ExportSTL(b, "$d/beam.stl", True, 0.01, False)
m = sm.Mesh(b, "beam")
a = m.Tetrahedron(algo=smeshBuilder.NETGEN_1D2D3D); p = a.Parameters(); p.SetMaxSize(4); p.SetMinSize(0.5); p.SetSecondOrder(False)
m.Compute()
for f in g.SubShapeAllSortedCentres(b, g.ShapeType["FACE"]):
    x = g.PointCoordinates(g.MakeCDG(f))
    if abs(x[0]) < 1e-6: m.GroupOnGeom(f, "fixed", SMESH.FACE)
    if abs(x[0] - 100) < 1e-6: m.GroupOnGeom(f, "load", SMESH.FACE)
m.ExportMED("$d/beam.med"); m.ExportUNV("$d/beam.unv")
PY
S="env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME salome -t --shutdown-servers=1"
(cd "$d" && $S --port=2861 "$d/mk.py" > mk.log 2>&1)
fails=0
for f in brep stl med unv; do
    (cd "$d" && QC_IN="$d/beam.$f" QC_OUT="$d/beam_$f.qcmesh" QC_SIZE=4 $S --port=2862 "$here/../assets/salome/fea_mesh.py" > "mesh_$f.log" 2>&1)
    grep "Q-Continuum:" "$d/mesh_$f.log" | sed 's/^/      /'
    if [ -s "$d/beam_$f.qcmesh" ] && "$tf" part "$d/beam_$f.qcmesh" "$d/run_$f"; then echo "ok    $f"; else echo "FAIL  $f"; fails=$((fails + 1)); fi
done
[ $fails = 0 ] && echo "all passed" || echo "$fails FAILED"
exit $fails
