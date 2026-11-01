#!/bin/bash
# f_two.sh <outdir> <seed cavity>: two app instances on one config and one case. B must see A's run (and not start
# a second solver in the same folder); A stops it afterwards; settings.ini must still parse.
here=$(cd "$(dirname "$0")/.." && pwd)
out=$1; seed=$2; sb=$out/sandbox
solvers() { for p in /proc/[0-9]*; do [ "$(cat $p/comm 2>/dev/null)" = icoFoam ] || continue
            case "$(readlink $p/cwd 2>/dev/null)" in "$sb"*) echo ${p#/proc/};; esac; done; }
QC_PHASE=1 QC_SEED_CASES=$seed "$here/run_journey.sh" "$here/failures/f_two_1.qcs" "$out" > "$out.1.txt" 2>&1 &
a=$!
for i in $(seq 120); do [ -n "$(solvers)" ] && break; sleep 0.5; done
[ -n "$(solvers)" ] || { echo "FAIL: A's run never started"; wait; exit 1; }
QC_PHASE=2 QC_KEEP_SANDBOX=1 QC_NO_REAP=1 "$here/run_journey.sh" "$here/failures/f_two_2.qcs" "$out" > "$out.2.txt" 2>&1 &
b=$!
n=0                                      # the most solvers seen in the case while B is open
while kill -0 $b 2>/dev/null && [ ! -e "$sb/b-done" ]; do k=$(solvers | wc -l); [ $k -gt $n ] && n=$k; sleep 0.2; done
wait $b; rb=$?
wait $a; ra=$?
echo "A rc=$ra, B rc=$rb, solvers while both open: $n"
grep -q '^workspace=' "$sb/cfg/q-continuum/settings.ini" || { echo "FAIL: settings.ini damaged"; exit 1; }
[ "$n" = 1 ] && [ $ra = 0 ] && [ $rb = 0 ]
