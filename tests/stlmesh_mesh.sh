#!/bin/bash
# tests/stlmesh_mesh.sh <test_stlmesh> <case>: what Generate runs for the snappyHexMesh route (page_stlmesh.c, app.h
# SNAPPY_CMD), then checkMesh; prints the layer table. Needs OpenFOAM's environment.
set -u
bin=$1; cd "$2" || exit 1
[ -n "${QC_NO_PREP:-}" ] || "$bin" prep . || exit 1
rm -rf constant/polyMesh 0/polyMesh
surfaceFeatureExtract > log.surfaceFeatureExtract 2>&1 || { echo "surfaceFeatureExtract failed"; exit 1; }
blockMesh > log.blockMesh 2>&1 || { echo "blockMesh failed"; exit 1; }
snappyHexMesh -overwrite > log.snappyHexMesh 2>&1 || { echo "snappyHexMesh failed"; tail -5 log.snappyHexMesh; exit 1; }
if [ -f 0/polyMesh/points ]; then mv -f 0/polyMesh/points constant/polyMesh/points && rm -rf 0/polyMesh; fi
if grep -q '^two_d *= *1' mesher/mesher.ini; then
    bash mesher/2d.sh > log.extrudeMesh 2>&1 || { echo "extrudeMesh failed"; tail -5 log.extrudeMesh; exit 1; }
fi
checkMesh > log.checkMesh 2>&1
grep -q 'Mesh OK' log.checkMesh && echo "checkMesh: Mesh OK" || { echo "checkMesh: FAILED"; grep -E '\*\*\*' log.checkMesh; }
# the last table: each layer patch, faces, target and mean layers, thickness (m, % of the wanted)
tac log.snappyHexMesh | awk '/^-----/{exit} NF' | tac | grep -v '^Layer\|^Mesh with\|^Wrote\|^Finished\|^End\|^Writing\|^Layers' 
