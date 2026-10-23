#!/bin/bash
# f_killed.sh <outdir> <seed cavity>: kill -9 the app mid-run (phase 1), relaunch it on the same sandbox (phase 2)
# and check that it adopts the orphaned ranks, that Stop ends them, and that nothing is left running.
here=$(cd "$(dirname "$0")/.." && pwd)
out=$1; seed=$2; sb=$out/sandbox
ranks() { for p in /proc/[0-9]*; do [ "$(cat $p/comm 2>/dev/null)" = icoFoam ] || continue
          case "$(readlink $p/cwd 2>/dev/null)" in "$sb"*) echo ${p#/proc/};; esac; done; }
QC_PHASE=1 QC_NO_REAP=1 QC_SEED_CASES=$seed "$here/run_journey.sh" "$here/failures/f_killed_1.qcs" "$out" > "$out.1.txt" 2>&1 &
for i in $(seq 120); do [ -n "$(ranks)" ] && grep -q "^Time = 0.5" "$sb/ws/cavity/log.icoFoam" 2>/dev/null && break; sleep 0.5; done
app=$(for p in /proc/[0-9]*; do { tr '\0' ' ' < $p/cmdline; } 2>/dev/null | grep -q "$out/script1.qcs" || continue
      case "$(readlink $p/exe 2>/dev/null)" in *q-continuum*) echo ${p#/proc/};; esac; done | head -1)   # not timeout/env
[ -n "$app" ] || { echo "FAIL: app not found"; exit 1; }
before=$(ranks | wc -l)
kill -9 "$app"; wait
sleep 2
orphans=$(ranks | wc -l)
echo "ranks before kill: $before, orphaned after kill: $orphans"
[ "$orphans" -gt 0 ] || { echo "FAIL: no orphaned ranks to adopt (the run died with the app?)"; exit 1; }
QC_PHASE=2 QC_KEEP_SANDBOX=1 QC_SEED_CASES=$seed "$here/run_journey.sh" "$here/failures/f_killed_2.qcs" "$out" > "$out.2.txt" 2>&1
rc=$?
sleep 2
left=$(ranks | wc -l)
echo "phase 2 rc=$rc, ranks left after Stop: $left"
[ "$left" = 0 ] || { ranks | xargs -r kill -TERM; echo "FAIL: ranks still running after Stop"; exit 1; }
[ $rc = 0 ]
