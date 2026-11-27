#!/bin/bash
# static_analysis.sh: cppcheck and clang-tidy over the project sources (not raylib). Exit status = number of findings.
# Tools: $QC_DEV_BIN, else ~/.local/qc-dev/bin (Ubuntu 24.04's cppcheck 2.13 and clang-tidy 18, the versions CI
# installs), else PATH. Missing tools skip the test (exit 77) instead of passing it: until 2026-11-26 a missing conda env
# made this report "0 findings" without running anything. Suppressions, with reasons: tests/cppcheck.supp and .clang-tidy.
here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here" || exit 1
tool() { for d in "${QC_DEV_BIN:-}" "$HOME/.local/qc-dev/bin"; do [ -n "$d" ] && [ -x "$d/$1" ] && { echo "$d/$1"; return; }; done
         for n in "$@"; do command -v "$n" && return; done; }
CPPCHECK=$(tool cppcheck); TIDY=$(tool clang-tidy clang-tidy-18)
[ -n "$CPPCHECK" ] && [ -n "$TIDY" ] || { echo "SKIP: cppcheck or clang-tidy not found (tests/static_analysis.sh says where it looks)"; exit 77; }
"$CPPCHECK" --enable=warning,performance,portability --std=c11 -j8 --quiet --error-exitcode=0 \
    --suppressions-list=tests/cppcheck.supp --suppress=missingIncludeSystem -DQC_VERSION='"test"' \
    -I src -I deps/raylib/src src/*.c 2> build/cppcheck.txt
n1=$(grep -c "^src/" build/cppcheck.txt)
echo "cppcheck: $n1 findings"; grep "^src/" build/cppcheck.txt
n2=0
if [ -f build/compile_commands.json ]; then
    "$TIDY" -p build --quiet src/*.c > build/clang-tidy.txt 2>/dev/null
    n2=$(grep -cE "^/.*/src/.*(warning|error):" build/clang-tidy.txt)
    echo "clang-tidy: $n2 findings"; grep -E "^/.*/src/.*(warning|error):" build/clang-tidy.txt
else echo "clang-tidy: skipped (configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)"; fi
[ $((n1 + n2)) = 0 ]   # exit 1 on findings (a count could read as 77, the skip code)
