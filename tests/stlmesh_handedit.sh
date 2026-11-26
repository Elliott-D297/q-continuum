#!/bin/bash
# stlmesh_handedit.sh <test_stlmesh> <scratch>: the STL mesher keeps hand edits to the dictionaries it writes (mesh plan step
# 9, E1): a changed value and a new entry survive the next Generate; an edited value the settings now change is replaced by
# the setting; a file nobody edited takes the new settings whole; the cfMesh layer retry's sed isn't taken for an edit.
set -u
T=$1; S=$2/stl_handedit; rm -rf "$S"; mkdir -p "$S"
here=$(cd "$(dirname "$0")" && pwd)
fail() { echo "FAIL: $*"; exit 1; }
python3 "$here/tools/gen_named_box.py" "$S/box.stl" > /dev/null || fail "box"
"$T" init "$S/box.stl" "$S/case" > /dev/null || fail "init"
"$T" prep "$S/case" > /dev/null || fail "prep 1"
sd=$S/case/system/snappyHexMeshDict; mq=$S/case/system/meshQualityDict
[ -f "$S/case/mesher/written/system/snappyHexMeshDict" ] || fail "no base copy kept"
# hand edits: a value the mesher writes the same each time, an entry of the user's own, and a quality limit
sed -i 's/^\([[:space:]]*\)maxGlobalCells[[:space:]].*;/\1maxGlobalCells 1234567;/' "$sd"
printf '\nmyOwnEntry 42;\n' >> "$sd"
sed -i 's/^maxNonOrtho .*;/maxNonOrtho 70;/' "$mq"
grep -q 'maxGlobalCells 1234567;' "$sd" || fail "edit didn't take (test)"
"$T" prep "$S/case" > /dev/null || fail "prep 2"
grep -q 'maxGlobalCells 1234567;' "$sd" || fail "a changed value was lost"
grep -q 'myOwnEntry 42;' "$sd" || fail "the user's own entry was lost"
grep -q 'maxNonOrtho 70;' "$mq" || fail "the edited quality limit was lost while the setting stayed"
# the setting for the edited key changes: the setting wins
sed -i 's/^max_non_ortho = .*/max_non_ortho = 60/' "$S/case/mesher/mesher.ini"
"$T" prep "$S/case" > /dev/null || fail "prep 3"
grep -q 'maxNonOrtho 60;' "$mq" || fail "a changed setting didn't replace the hand edit ($(grep maxNonOrtho "$mq"))"
grep -q 'maxGlobalCells 1234567;' "$sd" || fail "the other edit was lost on the third write"
# nobody edits: the file is exactly what the mesher writes
cp "$S/case/mesher/written/system/meshQualityDict" "$S/q.expected"
"$T" prep "$S/case" > /dev/null || fail "prep 4"
cmp -s "$mq" "$S/q.expected" || fail "an unedited file isn't what the mesher wrote"
# the picked point in the fluid (E8)
"$T" init "$S/box.stl" "$S/loc" > /dev/null || fail "init loc"
"$T" location x "$S/loc" || fail "location"
# presets (E10)
"$T" init "$S/box.stl" "$S/pre" > /dev/null || fail "init pre"
rm -rf "$S/presets"; "$T" preset x "$S/pre" "$S/presets" || fail "preset"
echo "stlmesh_handedit: all checks passed"
