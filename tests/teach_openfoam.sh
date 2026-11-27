#!/bin/bash
# teach_openfoam.sh <test_teach> <bashrc> <scratch>: the two starters with exact answers run in OpenFOAM; the probe's last
# value against the answer: the bar's middle at 10 s (Fourier series, 352.55 K) within 0.2 K; the flow's value at
# x = 0.09 m when steady ((e^10 - e^9) / (e^10 - 1) = 0.6322) within 0.005.
t=$(readlink -f "$1"); rc=$2; scratch=$(readlink -f "$3")
[ -f "$rc" ] || { echo "SKIP: no OpenFOAM at $rc"; exit 77; }
set --
. "$rc" > /dev/null 2>&1
d=$(mktemp -d "$scratch/teach.XXXXXX")
fails=0
for k in bar flow; do
    want=$("$t" $k "$d/$k") || { echo "FAIL  $k not written: $want"; exit 1; }
    ( cd "$d/$k" && blockMesh > log.blockMesh 2>&1 && $(foamDictionary -entry application -value system/controlDict) > log.solver 2>&1 ) ||
        { echo "FAIL  $k did not run"; tail -20 "$d/$k/log.solver"; exit 1; }
    col=2; [ $k = flow ] && col=3
    got=$(grep -v '^#' "$d/$k/postProcessing/probes/0/T" | tail -1 | awk -v c=$col '{print $c}')
    tol=0.2; [ $k = flow ] && tol=0.005
    python3 -I -c "
g=float('$got'); w=float('$want'); ok=abs(g-w)<=$tol
print('%s: probe %.5f, exact %.5f (%s)'%('$k',g,w,'ok' if ok else 'FAIL')); raise SystemExit(0 if ok else 1)" || fails=$((fails + 1))
done
[ $fails = 0 ] && rm -rf "$d" && echo "all ok"
exit $fails
