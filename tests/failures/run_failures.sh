#!/bin/bash
# run_failures.sh <outdir> (ROADMAP 1.10): the failure-injection scenarios, one row each. QC_ONLY="name ..." for a subset.
# /mnt/c runs only on WSL with a Windows Temp folder; its case lives there in a throw-away folder that is removed after.
here=$(cd "$(dirname "$0")" && pwd); t=$here/..
out=${1:?usage: run_failures.sh <outdir>}; mkdir -p "$out"
T=${FOAM_TUTORIALS:-$HOME/OpenFOAM/OpenFOAM-v2606/tutorials}
seed=$out/seed; mkdir -p "$seed"; [ -d "$seed/cavity" ] || cp -a "$T/incompressible/icoFoam/cavity/cavity" "$seed/cavity"
src_bin=${QC_BIN:-$t/../build/q-continuum}
cp "$src_bin" "$out/.q-continuum.bin" && export QC_BIN=$out/.q-continuum.bin
cc -shared -fPIC -O2 -o "$out/enospc.so" "$here/enospc.c" -ldl || exit 1
pass=0; fail=0
row() { local name=$1; shift
        case " ${QC_ONLY:-$name} " in *" $name "*) ;; *) return;; esac
        "$@" > "$out/$name.txt" 2>&1; local rc=$?
        if [ $rc = 0 ]; then pass=$((pass+1)); r=PASS; elif [ $rc = 77 ]; then r=SKIP; else fail=$((fail+1)); r=FAIL; fi
        printf '%-12s %s  %s\n' "$name" "$r" "$(grep -E 'SUMMARY|rc=|FAIL' "$out/$name.txt" | tail -1 | cut -c1-110)"; }
journey() { local name=$1; shift; env "$@" QC_SEED_CASES=$seed/cavity "$t/run_journey.sh" "$here/$name.qcs" "$out/$name"; }
mntc() {
    local base=/mnt/c/Users/${QC_WIN_USER:-$(cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\r')}/AppData/Local/Temp
    [ -d "$base" ] || { echo "no Windows Temp folder: skipped"; return 77; }
    local w=$base/qc-test-$$; mkdir -p "$w" && cp -a "$seed/cavity" "$w/" || return 1
    sed "s|\$QC_MNTC|$w|" "$here/f_mntc.qcs" > "$out/f_mntc.qcs"
    "$t/run_journey.sh" "$out/f_mntc.qcs" "$out/f_mntc"; local rc=$?
    for p in /proc/[0-9]*; do case "$(readlink $p/cwd 2>/dev/null)" in "$w"*) kill "${p#/proc/}" 2>/dev/null;; esac; done
    rm -rf "$w"; return $rc; }
row readonly  journey f_readonly
row spaces    journey f_spaces
row nofoam    journey f_nofoam QC_APP_ENV=QC_TEST_FOAM_ROOTS=
# the shim is inherited by every OpenFOAM child; with an ASan build libasan would have to be preloaded into them too,
# and blockMesh then hangs. So the disk-full row runs on normal builds only.
diskfull() {
    ldd "$QC_BIN" 2>/dev/null | grep -q libasan && { echo "sanitizer build: skipped"; return 77; }
    journey f_diskfull QC_APP_ENV="LD_PRELOAD=$out/enospc.so QC_FULL_DIR=$out/f_diskfull/sandbox/ws/cavity/constant/polyMesh QC_FULL_BYTES=20000"; }
row diskfull  diskfull
row manytimes journey f_manytimes QC_TIMEOUT=900
row killed    "$here/f_killed.sh" "$out/f_killed" "$seed/cavity"
row two       "$here/f_two.sh" "$out/f_two" "$seed/cavity"
row mntc      mntc
echo "failure injection: $pass passed, $fail failed  ($out)"
[ $fail = 0 ]
