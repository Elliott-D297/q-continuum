#!/bin/bash
# simsetup_outputs.sh <test_simsetup> <bashrc> <scratch>: Physics > Setup's "Fields written with the results" (mesh plan step 10,
# E9) on pitzDaily with simpleFoam for 20 iterations: wallShearStress, Q and vorticity written at the end, and kept by a
# second write of the setup (the switches saved).
bin=$(readlink -f "$1"); bashrc=$2; d=$(mktemp -d "$3/simsetup_outputs.XXXXXX")
set --
source "$bashrc" > /dev/null 2>&1
set -u
cp -a "$FOAM_TUTORIALS/incompressible/simpleFoam/pitzDaily" "$d/pitz" && cd "$d/pitz" && rm -rf 0 && blockMesh > log.blockMesh 2>&1 || { echo "FAIL: blockMesh"; exit 1; }
QC_OUT_FIELDS=1 "$bin" pitz "$d/pitz" balanced none 10 0 20 > /dev/null || { echo "FAIL: setup"; exit 1; }
for fo in wallShearStress Q vorticity; do grep -q "type            $fo;" system/controlDict || { echo "FAIL: $fo not in controlDict"; exit 1; }; done
grep -q "write_wall_shear = 1" .qc-setup.ini 2>/dev/null || grep -rq "write_wall_shear = 1" . || { echo "FAIL: switches not saved"; exit 1; }
timeout 600 simpleFoam > log.simpleFoam 2>&1 || { tail -20 log.simpleFoam; echo "FAIL: simpleFoam"; exit 1; }
last=$(foamListTimes -latestTime 2>/dev/null | tail -1)
for f in wallShearStress Q vorticity; do [ -f "$last/$f" ] || { echo "FAIL: $last/$f not written"; exit 1; }; done
echo "simsetup_outputs: all checks passed ($last)"
