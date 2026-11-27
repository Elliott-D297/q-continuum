#!/bin/bash
# cht_openfoam.sh <test_cht> <bashrc> <scratch>: conjugate heat transfer written by chtsetup.c and run in OpenFOAM, the
# Run page's steps (blockMesh, topoSet, splitMeshRegions -cellZones, restore0Dir, the solver), against hand-worked answers:
#  1. channel: a 0.2 m aluminium plate (5 mm) making 5 W under air (20 mm gap, 10 mm deep) at 0.5 m/s and 300 K, every
#     outer wall adiabatic. All 5 W leave with the air: dT = Q / (mdot cp), mdot = rho U A = (1e5 / (287.1 x 300)) x 0.5 x
#     2e-4 = 1.161e-4 kg/s, cp 1007 -> dT = 42.77 K, the flow-weighted outlet temperature 342.8 K (2 %).
#  2. wall: aluminium (k 167) and carbon steel (k 60.5), 10 mm each, between 400 K and 300 K: q = dT / (L1/k1 + L2/k2) =
#     100 / (5.988e-5 + 1.653e-4) = 4.441e5 W/m2, on the 10 x 1 mm hot face 4.441 W (1 %).
t=$(readlink -f "$1"); rc=$2; scratch=$(readlink -f "$3")
[ -f "$rc" ] || { echo "SKIP: no OpenFOAM at $rc"; exit 77; }
set --                                   # etc/bashrc reads the caller's arguments as its own options
. "$rc" > /dev/null 2>&1
d=$(mktemp -d "$scratch/chtof.XXXXXX")
base() {   # $1 case, $2 blocks+boundary text
    mkdir -p "$1/system" "$1/constant"
    cat > "$1/system/blockMeshDict" <<EOF
FoamFile { version 2.0; format ascii; class dictionary; object blockMeshDict; }
scale 1;
$2
EOF
    cat > "$1/system/controlDict" <<'EOF'
FoamFile { version 2.0; format ascii; class dictionary; object controlDict; }
application blockMesh; startTime 0; endTime 1; deltaT 1; writeControl timeStep; writeInterval 1;
EOF
    printf 'FoamFile { version 2.0; format ascii; class dictionary; object fvSchemes; }\n' > "$1/system/fvSchemes"
    printf 'FoamFile { version 2.0; format ascii; class dictionary; object fvSolution; }\n' > "$1/system/fvSolution"
}
run() {    # $1 case: the Run page's steps for a split case with per-region 0.orig
    ( cd "$1" && blockMesh > log.blockMesh 2>&1 || exit 1
      . "$WM_PROJECT_DIR/bin/tools/RunFunctions"
      [ -f system/topoSetDict ] && { topoSet > log.topoSet 2>&1 || exit 1; }
      "$2" "$3" "$1" || exit 1
      splitMeshRegions -cellZones -overwrite > log.splitMeshRegions 2>&1 || exit 1
      restore0Dir > log.restore0 2>&1
      $(foamDictionary -entry application -value system/controlDict) > log.solver 2>&1 ) || { echo "FAIL  $1 did not run:"; tail -30 "$1"/log.* ; return 1; }
}

base "$d/channel" '
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
);'
run "$d/channel" "$t" channel || exit 1
To=$(tail -1 "$d/channel/postProcessing/air/outletT_air/0/surfaceFieldValue.dat" | awk '{print $2}')
python3 -I -c "
T=float('$To'); want=300+5/(1e5/(8314.462618/28.96*300)*0.5*2e-4*1007)
err=abs(T-want)/(want-300)*100
print('channel: outlet %.2f K, by hand %.2f K (%.1f %% of the rise)'%(T,want,err)); raise SystemExit(0 if err<2 else 1)" || { echo "FAIL  channel energy balance"; exit 1; }

base "$d/wall" '
vertices ( (0 0 0) (0.01 0 0) (0.02 0 0) (0.02 0.01 0) (0.01 0.01 0) (0 0.01 0)
           (0 0 0.001) (0.01 0 0.001) (0.02 0 0.001) (0.02 0.01 0.001) (0.01 0.01 0.001) (0 0.01 0.001) );
blocks ( hex (0 1 4 5 6 7 10 11) alu (20 2 1) simpleGrading (1 1 1)
         hex (1 2 3 4 7 8 9 10) steel (20 2 1) simpleGrading (1 1 1) );
boundary (
    hot   { type wall; faces ((0 6 11 5)); }
    cold  { type wall; faces ((2 3 9 8)); }
    sides { type wall; faces ((0 1 7 6) (1 2 8 7) (5 11 10 4) (4 10 9 3)); }
    frontAndBack { type empty; faces ((0 5 4 1) (1 4 3 2) (6 7 10 11) (7 8 9 10)); }
);'
run "$d/wall" "$t" wall || exit 1
Q=$(grep -h "hot" "$d"/wall/postProcessing/alu/wallHeat_alu/*/wallHeatFlux.dat | tail -1 | awk '{print $NF}')
python3 -I -c "
Q=abs(float('$Q')); want=100/(0.01/167+0.01/60.5)*0.01*0.001
err=abs(Q-want)/want*100
print('wall: heat through the hot face %.4f W, by hand %.4f W (%.2f %%)'%(Q,want,err)); raise SystemExit(0 if err<1 else 1)" || { echo "FAIL  composite wall"; exit 1; }
rm -rf "$d"
echo "all ok"
