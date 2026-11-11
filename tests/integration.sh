#!/bin/bash
# integration.sh <outdir> (ROADMAP 1.9): each tutorial in the matrix is run twice from the same shortened copy: once by
# its own Allrun (or blockMesh + the solver when it has none) as the reference, once through the app's Run page by a
# script, as a user would. The latest time's fields must agree (tests/integration_compare.py). Rows run one after
# another so they don't compete for cores. QC_ONLY="name ..." runs a subset.
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: integration.sh <outdir>}; mkdir -p "$out"
# a private copy of the binary: rebuilding build/ while a long suite runs must not change it under the later rows
src_bin=${QC_BIN:-$here/../build/q-continuum}
case "$src_bin" in *.sh) ;; *) mkdir -p "$out"; cp "$src_bin" "$out/.q-continuum.bin" && export QC_BIN=$out/.q-continuum.bin;; esac
T=${FOAM_TUTORIALS:-$HOME/OpenFOAM/OpenFOAM-v2606/tutorials}
BASHRC=${QC_BASHRC:-$HOME/OpenFOAM/OpenFOAM-v2606/etc/bashrc}

# name | tutorial | processes for the app run | endTime | writeInterval | tolerance | timeout s
# Serial runs of the same steps should match to round-off; a parallel run against the serial reference only to the
# solver tolerance (the decomposed linear solves converge differently).
MATRIX="
cavity          | incompressible/icoFoam/cavity/cavity              | 1 | 0.1  | 0.1  | 1e-6 | 120
cavityPar       | incompressible/icoFoam/cavity/cavity              | 4 | 0.1  | 0.1  | 1e-2 | 120
pitzDaily       | incompressible/simpleFoam/pitzDaily               | 1 | 100  | 100  | 1e-6 | 300
pitzDailyPar    | incompressible/simpleFoam/pitzDaily               | 4 | 100  | 100  | 5e-2 | 300
pimplePitzDaily | incompressible/pimpleFoam/RAS/pitzDaily           | 1 | 0.005 | 0.005 | 1e-6 | 300
damBreak        | multiphase/interFoam/laminar/damBreak/damBreak    | 1 | 0.05 | 0.05 | 1e-6 | 300
flange          | basic/laplacianFoam/flange                        | 1 | 0.1  | 0.1  | 1e-6 | 300
radiationFvDOM  | heatTransfer/buoyantSimpleFoam/hotRadiationRoomFvDOM | 1 | 10 | 10 | 1e-6 | 600
radiationP1     | heatTransfer/buoyantSimpleFoam/hotRadiationRoom   | 1 | 20   | 20   | 1e-6 | 600
chtSerial       | heatTransfer/chtMultiRegionFoam/multiRegionHeater | 1 | 0.02 | 0.02 | 5e-2 | 600
chtPar          | heatTransfer/chtMultiRegionFoam/multiRegionHeater | 4 | 0.02 | 0.02 | 5e-2 | 600
forwardStep     | compressible/rhoCentralFoam/forwardStep           | 1 | 0.1  | 0.1  | 1e-6 | 300
"
# chtSerial's reference is the tutorial's Allrun, which runs on its decomposeParDict's 4 ranks: a parallel reference,
# hence the solver-level tolerance there too.

pass=0; fail=0
printf '%-16s %-6s %-8s %-8s %s\n' case result ref app compare
while IFS='|' read -r name tut np endT wint tol tmo; do
    name=$(echo $name); [ -z "$name" ] && continue
    tut=$(echo $tut); np=$(echo $np); endT=$(echo $endT); wint=$(echo $wint); tol=$(echo $tol); tmo=$(echo $tmo)
    case " ${QC_ONLY:-$name} " in *" $name "*) ;; *) continue;; esac
    d=$out/$name; rm -rf "$d"; mkdir -p "$d"
    cp -a "$T/$tut" "$d/seed"
    # shorten the run and write ascii, identically for both copies
    (. "$BASHRC" >/dev/null 2>&1; cd "$d/seed" &&
        foamDictionary system/controlDict -entry endTime -set "$endT" >/dev/null &&
        { [ "$(foamDictionary system/controlDict -entry writeControl -value)" != timeStep ] ||
          foamDictionary system/controlDict -entry writeControl -set runTime >/dev/null; } &&    # writeInterval in seconds
        foamDictionary system/controlDict -entry writeInterval -set "$wint" >/dev/null &&
        foamDictionary system/controlDict -entry writeFormat -set ascii >/dev/null) || { echo "$name: could not edit controlDict"; fail=$((fail+1)); continue; }
    if [ "$np" -gt 1 ]; then
        for dpd in $(cd "$d/seed" && ls system/decomposeParDict system/*/decomposeParDict 2>/dev/null); do
            (. "$BASHRC" >/dev/null 2>&1; cd "$d/seed" && foamDictionary "$dpd" -entry numberOfSubdomains -set "$np" >/dev/null)
        done
    fi
    cp -a "$d/seed" "$d/ref"
    mkdir -p "$d/ws"; cp -a "$d/seed" "$d/ws/$name"

    # reference: the tutorial's own way
    t0=$(date +%s)
    (. "$BASHRC" >/dev/null 2>&1; cd "$d/ref" &&
        if [ -x Allrun ]; then timeout "$tmo" ./Allrun > log.Allrun 2>&1
        else . "$WM_PROJECT_DIR/bin/tools/RunFunctions"; timeout "$tmo" bash -c '. "$WM_PROJECT_DIR/bin/tools/RunFunctions"; runApplication blockMesh && runApplication $(getApplication)' > log.Allrun 2>&1; fi)
    rref=$?; tref=$(( $(date +%s) - t0 ))

    # the app: open, set processes, Run, wait
    {
        echo "open \$SB/ws/$name"
        echo "click nav:run"
        echo "wait-until page Run"
        echo "wait 10"
        # the row's count, not the app's default (an Allrun that runs serial disables the box: click? skips it then)
        echo "click? np-in"; echo "enter $np"; echo "wait 5"
        echo "click run-go"
        echo "wait-until job running timeout 30"
        echo "wait-until not job running timeout $tmo"
        echo "assert job finished"
    } > "$d/run.qcs"
    t0=$(date +%s)
    QC_TIMEOUT=$((tmo + 120)) QC_PROCS=$np QC_SEED_CASES="$d/ws/$name" "$here/run_journey.sh" "$d/run.qcs" "$d/app" > "$d/app.txt" 2>&1
    rapp=$?; tapp=$(( $(date +%s) - t0 ))
    appcase=$d/app/sandbox/ws/$name

    python3 -I "$here/integration_compare.py" "$d/ref" "$appcase" "$tol" > "$d/compare.txt" 2>&1
    rcmp=$?
    if [ $rref = 0 ] && [ $rapp = 0 ] && [ $rcmp = 0 ]; then r=PASS; pass=$((pass+1)); else r=FAIL; fail=$((fail+1)); fi
    printf '%-16s %-6s %-8s %-8s %s\n' "$name" "$r" "${tref}s/$rref" "${tapp}s/$rapp" "$(tail -1 "$d/compare.txt")"
done <<< "$MATRIX"
echo "integration: $pass passed, $fail failed  ($out)"
[ $fail = 0 ]
