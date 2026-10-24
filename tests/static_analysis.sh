#!/bin/bash
# static_analysis.sh: cppcheck and clang-tidy over the project sources (not raylib). Exit status = number of findings.
# Tools come from the conda env qc-dev. Suppressions, with reasons: tests/cppcheck.supp and .clang-tidy.
here=$(cd "$(dirname "$0")/.." && pwd)
B=${QC_DEV_BIN:-$HOME/miniforge/envs/qc-dev/bin}
cd "$here" || exit 1
"$B/cppcheck" --enable=warning,performance,portability --std=c11 -j8 --quiet --error-exitcode=0 \
    --suppressions-list=tests/cppcheck.supp --suppress=missingIncludeSystem -DQC_VERSION='"test"' \
    -I src -I deps/raylib/src src/*.c 2> build/cppcheck.txt
n1=$(grep -c "^src/" build/cppcheck.txt)
echo "cppcheck: $n1 findings"; grep "^src/" build/cppcheck.txt
n2=0
if [ -f build/compile_commands.json ]; then
    "$B/clang-tidy" -p build --quiet src/*.c > build/clang-tidy.txt 2>/dev/null
    n2=$(grep -cE "^/.*/src/.*(warning|error):" build/clang-tidy.txt)
    echo "clang-tidy: $n2 findings"; grep -E "^/.*/src/.*(warning|error):" build/clang-tidy.txt
else echo "clang-tidy: skipped (configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)"; fi
exit $((n1 + n2))
