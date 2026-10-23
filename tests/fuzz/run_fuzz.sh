#!/bin/bash
# run_fuzz.sh [seconds per target] [outdir]
# Builds the libFuzzer harnesses with clang (+ ASan/UBSan) and fuzzes each target, seeded with real inputs: tutorial
# dictionaries, the solver logs in tests/logs, preCICE configurations. Crashes land in <outdir>/<target>/crashes and
# must become fixtures in tests/fuzz/fixtures/<target>/ once fixed (the fixtures are replayed by ctest).
set -eu
here=$(cd "$(dirname "$0")" && pwd); root=$here/../..
secs=${1:-60}; out=${2:-$root/build/fuzz}
CC=${QC_CLANG:-$HOME/miniforge/envs/qc-dev/bin/clang}
T=${FOAM_TUTORIALS:-$HOME/OpenFOAM/OpenFOAM-v2606/tutorials}
mkdir -p "$out"
core="$root/src/pl_util.c $root/src/foamdict.c $root/src/runner.c $root/src/xmlspan.c"
for t in foamdict logparse xml; do
    "$CC" -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -I"$root/src" \
        "$here/fuzz_$t.c" $core -lm -o "$out/fuzz_$t"
done
# seed corpora (small files: the fuzzer mutates from them)
if [ ! -d "$out/corpus_foamdict" ]; then
    mkdir -p "$out/corpus_foamdict"
    find "$T" -path '*/system/*' -size -8k -type f | head -400 | while read -r f; do cp "$f" "$out/corpus_foamdict/$(echo "$f" | md5sum | cut -c1-12)"; done
    find "$T" -path '*/0.orig/*' -size -8k -type f | head -200 | while read -r f; do cp "$f" "$out/corpus_foamdict/$(echo "$f" | md5sum | cut -c1-12)"; done
fi
if [ ! -d "$out/corpus_logparse" ]; then
    mkdir -p "$out/corpus_logparse"
    for f in "$root"/tests/logs/*.log; do head -c 60000 "$f" > "$out/corpus_logparse/$(basename "$f")"; done
fi
if [ ! -d "$out/corpus_xml" ]; then
    mkdir -p "$out/corpus_xml"
    find "$HOME/src/precice-tutorials" -name 'precice-config.xml' 2>/dev/null | head -100 | while read -r f; do cp "$f" "$out/corpus_xml/$(echo "$f" | md5sum | cut -c1-12)"; done
fi
rc=0
for t in foamdict logparse xml; do
    mkdir -p "$out/$t/crashes"
    echo "== fuzz_$t for $secs s"
    ( cd "$out/$t" && ASAN_OPTIONS=detect_leaks=1 "$out/fuzz_$t" -max_total_time="$secs" -max_len=65536 -timeout=10 \
        -artifact_prefix=crashes/ "$out/corpus_$t" 2>&1 | grep -E "^#[0-9]+ +(DONE|INITED)|ERROR|SUMMARY|Test unit written" | tail -4 ) || rc=1
    n=$(ls "$out/$t/crashes" | wc -l)
    echo "   crashes: $n"
    [ "$n" = 0 ] || rc=1
done
exit $rc
