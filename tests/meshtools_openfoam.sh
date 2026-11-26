#!/bin/bash
# meshtools_openfoam.sh <test_meshtools> <bashrc> <scratch> <tutorials>: each Mesh > Tools utility, as meshtools.c writes it, run on
# the cavity with v2606; the mesh checked after each (checkMesh) and the effect seen (cells, bounding box, zones, patches).
T=$(readlink -f "$1"); BR=$2; S=$3/meshtools_of; TU=$4; set --; . "$BR" > /dev/null 2>&1
set -u
fail() { echo "FAIL: $*"; exit 1; }
mt_fresh() { rm -rf "$S"; cp -a "$TU/incompressible/icoFoam/cavity/cavity" "$S"; (cd "$S" && blockMesh > log.blockMesh 2>&1) || fail "blockMesh"; }
mt_run() { cmd=$("$T" write "$S" "$1") || fail "write $1"; (cd "$S" && bash -c "$cmd" > "log.$1" 2>&1) || { tail -20 "$S/log.$1"; fail "$1 ran: $cmd"; }
        (cd "$S" && checkMesh > log.check.$1 2>&1); grep -q "Mesh OK" "$S/log.check.$1" || fail "$1: checkMesh not OK"; }
mt_cells() { grep -m1 "cells:" "$S/log.check.$1" | awk '{print $2}'; }
mt_fresh; mt_run transform; grep -qE "bounding box \(-0.2 0 1\) \(\S+ 0.2 1.02\)" "$S/log.check.transform" || fail "transform: box $(grep 'bounding box' $S/log.check.transform)"
mt_fresh; mt_run mirror;    [ "$(mt_cells mirror)" = 800 ] || fail "mirror: $(mt_cells mirror) cells, expected 800"
mt_fresh; mt_run refine;    [ "$(mt_cells refine)" = 1600 ] || fail "refine: $(mt_cells refine) cells (refineMesh -all on a one-cell-deep mesh splits in 2-D: 1600)"
mt_fresh; mt_run renumber;  [ "$(mt_cells renumber)" = 400 ] || fail "renumber: cell count changed"
mt_fresh; mt_run zone;      grep -q "porous" "$S/constant/polyMesh/cellZones" || fail "zone: no cellZone porous"
mt_fresh; mt_run merge;     grep -q "porous" "$S/constant/polyMesh/boundary" && ! grep -q "movingWall" "$S/constant/polyMesh/boundary" || fail "merge: patches not merged into porous"
echo "meshtools_openfoam: all checks passed"
