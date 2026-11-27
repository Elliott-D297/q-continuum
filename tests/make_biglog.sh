#!/bin/bash
# make_biglog.sh <dest> [MB]: the cavity run once for real, its solver log then grown to about MB megabytes (100) by
# repeating its time steps with the times counting on, ending as the real one does: the seed for perf_bigLog (a huge
# log on the Run page). Added 2026-11-26: the journey existed, its seed was never made by the test scripts.
d=$1; mb=${2:-100}
[ -d "$d" ] && exit 0
. "${WM_PROJECT_DIR:-/usr/lib/openfoam/openfoam2606}/etc/bashrc" 2>/dev/null || . /usr/lib/openfoam/openfoam2606/etc/bashrc
cp -a "$FOAM_TUTORIALS/incompressible/icoFoam/cavity/cavity" "$d" || exit 1
(cd "$d" && blockMesh > log.blockMesh 2>&1 && icoFoam > log.icoFoam 2>&1) || { echo "make_biglog: the cavity did not run"; exit 1; }
last=$(python3 -I - "$d/log.icoFoam" "$mb" <<'PY'
import re, sys
p, mb = sys.argv[1], float(sys.argv[2])
s = open(p).read()
i = s.find('\nTime = '); j = s.rfind('\nEnd')
head, body, tail = s[:i + 1], s[i + 1:j + 1], s[j + 1:]
steps = re.split(r'(?m)^(?=Time = )', body)
steps = [x for x in steps if x.startswith('Time = ')]
dt = 0.005
out = [head]; size = len(head); t = 0.0; k = 0
while size < mb * 1e6:
    st = steps[k % len(steps)]; k += 1; t += dt
    st = re.sub(r'^Time = \S+', 'Time = %g' % t, st, count=1)
    out.append(st); size += len(st)
out.append(tail)
open(p, 'w').write(''.join(out))
print('%g' % t)
PY
)
sed -i "s/^endTime .*/endTime         $last;/" "$d/system/controlDict"   # the log reached its end time: a finished run
ls -la "$d/log.icoFoam" | awk '{print "make_biglog:", $5, "bytes"}'
