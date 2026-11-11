#!/bin/bash
# salome_tool.sh <scratch> <salome_tool.py>: the SALOME service ("every mesher" plan, step 5) driven through every command
# as Q-Continuum runs it (salometool.c): inspect and faces_stl on a STEP part (tests/tools/flange_mm.step), quality and
# export (MED, UNV, CGNS, STL, GMF) on SALOME meshes (tests/tools/salome_test_meshes.py), UNV refused for polyhedra, and a
# bad request answered with ok false and a reason. Skips (77) without SALOME.
set -u
out=$1/salome_tool; script=$(realpath "$2"); here=$(cd "$(dirname "$0")" && pwd)
salome=$(ls -d "$HOME"/salome/SALOME-*/salome 2>/dev/null | tail -1)
[ -x "$salome" ] || { echo "SALOME not installed: skipped"; exit 77; }
rm -rf "$out"; mkdir -p "$out/src"
run_salome() { env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME "$salome" -t --shutdown-servers=1 "$@"; }
(cd "$out/src" && run_salome "$here/tools/salome_test_meshes.py" && run_salome "$here/tools/salome_test_cad.py") > "$out/make.log" 2>&1
fails=0
# ask <name> <request json> <python check on d (the answer)>
ask() {
    local n=$1 req=$2 chk=$3
    echo "$req" > "$out/$n.req.json"
    (cd "$out" && QC_CMD="$out/$n.req.json" QC_OUT="$out/$n.ans.json" run_salome "$script") > "$out/$n.log" 2>&1
    if python3 -c "import json,sys,os; d=json.load(open('$out/$n.ans.json')); sys.exit(0 if ($chk) else 1)" 2>/dev/null; then echo "ok   $n"
    else echo "FAIL $n: $(head -c 400 "$out/$n.ans.json" 2>/dev/null)"; tail -3 "$out/$n.log"; fails=$((fails + 1)); fi
}
step=$here/tools/flange_mm.step
ask inspect "{\"cmd\":\"inspect\",\"cad\":\"$step\"}" \
    "d['ok'] and d['closed'] and d['solids'] == 1 and len(d['faces']) >= 6 and any(f['kind'] == 'CYLINDER2D' for f in d['faces']) and all(f['neighbours'] for f in d['faces']) and abs(d['bbox'][3]-d['bbox'][0]) < 1"
ask faces_stl "{\"cmd\":\"faces_stl\",\"cad\":\"$step\",\"out\":\"$out/faces.stl\"}" \
    "d['ok'] and open('$out/faces.stl').read().count('\nsolid face_') + open('$out/faces.stl').read().startswith('solid face_') == d['faces']"
ask quality "{\"cmd\":\"quality\",\"mesh\":\"$out/src/quad.med\"}" \
    "d['ok'] and d['cells'] > 100 and len(d['measures']) >= 4 and sum(b['count'] for b in [m for m in d['measures'] if m['key']=='aspect_ratio'][0]['bins']) == d['cells'] and d['worst']"
for f in med unv cgns stl gmf; do
    ext=$f; [ $f = gmf ] && ext=mesh
    ask export_$f "{\"cmd\":\"export\",\"mesh\":\"$out/src/quad.med\",\"format\":\"$f\",\"out\":\"$out/x.$ext\"}" "d['ok'] and os.path.getsize('$out/x.$ext') > 1000"
done
ask export_unv_poly "{\"cmd\":\"export\",\"mesh\":\"$out/src/poly.med\",\"format\":\"unv\",\"out\":\"$out/p.unv\"}" "not d['ok'] and 'polyhedra' in d['error']"
# step 6: geometry operations, each on the part it is for, the result written as BREP + STL
c=$out/src
ask op_fluid_inside "{\"cmd\":\"ops\",\"cad\":\"$c/block_pipe.brep\",\"ops\":[{\"op\":\"fluid_inside\"}],\"out\":\"$out/fi\"}" \
    "d['ok'] and abs(d['volume'] - 3.14159265*0.03**2*0.2) < 0.02 * 5.65e-4 and d['closed'] and os.path.getsize('$out/fi.stl') > 1000 and os.path.isfile('$out/fi.brep')"
ask op_sew "{\"cmd\":\"ops\",\"cad\":\"$c/loose.brep\",\"ops\":[{\"op\":\"sew\"}],\"out\":\"$out/sew\"}" \
    "d['ok'] and d['closed'] and abs(d['volume'] - 0.0014345) < 1e-6"
ask op_holes "{\"cmd\":\"ops\",\"cad\":\"$c/plate_holes.brep\",\"ops\":[{\"op\":\"remove_holes\",\"radius\":0.01}],\"out\":\"$out/holes\"}" \
    "d['ok'] and d['faces'] == d['faces_before'] - 4 and d['volume'] > d['volume_before']"
ask op_fuse "{\"cmd\":\"ops\",\"cad\":\"$c/two_boxes.brep\",\"ops\":[{\"op\":\"fuse\"},{\"op\":\"merge_faces\"},{\"op\":\"heal\"}],\"out\":\"$out/fuse\"}" \
    "d['ok'] and abs(d['volume'] - 0.00175) < 1e-6"
ask op_refused "{\"cmd\":\"ops\",\"cad\":\"$c/two_boxes.brep\",\"ops\":[{\"op\":\"fluid_inside\"}],\"out\":\"$out/no\"}" \
    "not d['ok'] and 'no openings found' in d['error']"
ask bad_file "{\"cmd\":\"inspect\",\"cad\":\"$out/nope.step\"}" "not d['ok'] and 'does not exist' in d['error']"
ask bad_cmd "{\"cmd\":\"frobnicate\"}" "not d['ok'] and 'unknown command' in d['error']"
[ $fails = 0 ] && echo "salome_tool: all passed"
exit $fails
