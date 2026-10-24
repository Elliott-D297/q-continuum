#!/bin/bash
# valgrind.sh: the golden-page journey (every case page, a few hundred frames) under Valgrind memcheck, on a
# RelWithDebInfo build in build-vg. Fails on invalid reads/writes, uninitialised use, or bytes definitely lost in
# our own code (src/); leaks inside Mesa, X11 and the dynamic loader are reported but not counted.
here=$(cd "$(dirname "$0")" && pwd)
vg=$here/../build-vg/q-continuum
[ -x "$vg" ] || { echo "build first: cmake -S . -B build-vg -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-vg --target q-continuum"; exit 77; }
out=${1:-$here/../build-vg/valgrind}; mkdir -p "$out"
T=${FOAM_TUTORIALS:-$HOME/OpenFOAM/OpenFOAM-v2606/tutorials}
rm -rf "$out/seed" "$out"/memcheck.*; mkdir -p "$out/seed"; cp -a "$T/incompressible/icoFoam/cavity/cavity" "$out/seed/cavity"
wrap=$out/vg-wrap.sh
cat > "$wrap" <<W
#!/bin/bash
exec valgrind --tool=memcheck --leak-check=full --show-leak-kinds=definite --errors-for-leak-kinds=definite \
     --track-origins=yes --num-callers=30 --log-file=$out/memcheck.%p.txt "$vg" "\$@"
W
chmod +x "$wrap"
QC_TIMEOUT=1500 QC_BIN=$wrap QC_SEED_CASES=$out/seed/cavity "$here/run_journey.sh" "$here/journeys/g_pages.qcs" "$out/run" >/dev/null 2>&1
rc=$?
python3 -I "$here/valgrind_parse.py" "$here/../src" "$out"/memcheck.*.txt
vrc=$?
[ $rc = 0 ] || echo "journey failed under valgrind: $out/run/journey.log"
exit $((vrc + (rc != 0)))
