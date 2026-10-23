#!/bin/bash
# explain_fixes.sh <outdir> (ROADMAP 3.2): each mistake is made on a copy of a tutorial and run with the real solver;
# the error translator must recognise the FOAM FATAL message, point at the file, and (where it offers one) its fix
# applied must make the same error go away when the solver runs again.
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: explain_fixes.sh <outdir>}; rm -rf "$out"; mkdir -p "$out"
bin=${QC_TEST_EXPLAIN:-$here/../build/test_explain}
T=${FOAM_TUTORIALS:-$HOME/OpenFOAM/OpenFOAM-v2606/tutorials}
B2606=${QC_BASHRC:-$HOME/OpenFOAM/OpenFOAM-v2606/etc/bashrc}
B2412=/usr/lib/openfoam/openfoam2412/etc/bashrc
pass=0; fail=0
# name | tutorial | breakage | tool | expected fix kind | text the explanation must contain | bashrc
while IFS='|' read -r name tut brk app kind want rc; do
    name=$(echo $name); [ -z "$name" ] && continue
    tut=$(echo $tut); app=$(echo $app); kind=$(echo $kind); want=$(echo "$want" | sed 's/^ *//; s/ *$//'); rc=$(echo $rc)
    bashrc=$B2606; [ "$rc" = 2412 ] && bashrc=$B2412
    [ -f "$bashrc" ] || { echo "$name: SKIP (no $bashrc)"; continue; }
    d=$out/$name; cp -a "$T/$tut" "$d"
    (cd "$d" && . "$bashrc" >/dev/null 2>&1 && foamDictionary system/controlDict -entry endTime -set 5 >/dev/null 2>&1
     [ "$app" = icoFoam ] && foamDictionary system/controlDict -entry endTime -set 0.05 >/dev/null 2>&1
     blockMesh > log.blockMesh 2>&1; eval "$brk" > /dev/null 2>&1; timeout 60 $app > log.$app 2>&1)
    msg=$(grep -A1 "FOAM FATAL" "$d/log.$app" | sed -n 2p)
    "$bin" "$d" "log.$app" > "$d/explain.txt" 2>&1
    got=$(sed -n 's/^fix: \([a-z-]*\).*/\1/p' "$d/explain.txt")
    ok=1
    [ "$got" = "$kind" ] || { ok=0; why="fix '$got', want '$kind'"; }
    grep -qF -- "$want" "$d/explain.txt" || { ok=0; why="${why:+$why; }no '$want' in the explanation"; }
    if [ $ok = 1 ] && { [ "$kind" = put ] || [ "$kind" = rename ]; }; then
        (cd "$d" && . "$bashrc" >/dev/null 2>&1 && timeout 120 $app > log.$app.after 2>&1)
        if grep -qF -- "$msg" "$d/log.$app.after"; then ok=0; why="the same error after the fix: $msg"; fi
    fi
    if [ $ok = 1 ]; then pass=$((pass+1)); printf '%-16s PASS  %s\n' "$name" "$(sed -n 's/^title: //p' "$d/explain.txt")"
    else fail=$((fail+1)); printf '%-16s FAIL  %s\n' "$name" "$why"; sed 's/^/    /' "$d/explain.txt"; fi
done <<'EOF2'
missing_patch  | incompressible/icoFoam/cavity/cavity | sed -i 's/^    movingWall$/    movingWallX/' 0/U | icoFoam | rename | most likely the same name misspelt | 2606
missing_bc     | incompressible/icoFoam/cavity/cavity | foamDictionary 0/U -entry boundaryField/movingWall -remove >/dev/null | icoFoam | add-bc | has none for movingWall | 2606
bad_bc_type    | incompressible/icoFoam/cavity/cavity | sed -i '0,/type            zeroGradient;/s//type            zeroGradeint;/' 0/p | icoFoam | put | The closest is zeroGradient | 2606
missing_scheme | incompressible/icoFoam/cavity/cavity | sed -i '/div(phi,U)/d' system/fvSchemes | icoFoam | put | div(phi,U) in system/fvSchemes under divSchemes | 2606
missing_dt     | incompressible/icoFoam/cavity/cavity | sed -i '/^deltaT/d' system/controlDict | icoFoam | put | A usual value is 0.0025 | 2606
bad_dimensions | incompressible/icoFoam/cavity/cavity | sed -i 's/^nu .*/nu              [0 2 -2 0 0 0 0] 0.01;/' constant/transportProperties | icoFoam | put | is given in m²/s² [0 2 -2 0 0 0 0], but OpenFOAM expects m²/s | 2606
bad_turb_model | incompressible/simpleFoam/pitzDaily | sed -i 's/kEpsilon;/kEpsilonn;/' constant/turbulenceProperties constant/momentumTransport 2>/dev/null | simpleFoam | put | Did you mean kEpsilon? | 2606
bad_decomp     | incompressible/icoFoam/cavity/cavity | foamDictionary system/decomposeParDict -entry numberOfSubdomains -set 3 >/dev/null; foamDictionary system/decomposeParDict -entry method -set hierarchical >/dev/null; foamDictionary system/decomposeParDict -entry coeffs -set '{ n (2 2 1); }' >/dev/null | decomposePar | put | splits the mesh into 2 × 2 × 1 = 4 pieces | 2606
no_pref        | incompressible/simpleFoam/pitzDaily | sed -i 's/fixedValue;/zeroGradient;/' 0/p | simpleFoam | put | Nothing fixes the level of p | 2606
region_mesh    | heatTransfer/chtMultiRegionFoam/multiRegionHeater | true | chtMultiRegionFoam | mesh | constant/bottomWater/polyMesh doesn't exist | 2606
no_outlet      | incompressible/simpleFoam/pitzDaily | sed -i 's/fixedValue;/zeroGradient;/' 0/p; foamDictionary system/fvSolution -entry SIMPLE/pRefCell -add 0 >/dev/null; foamDictionary system/fvSolution -entry SIMPLE/pRefValue -add 0 >/dev/null | simpleFoam | put | Patch outlet has p zeroGradient | 2606
bad_solver     | incompressible/icoFoam/cavity/cavity | sed -i '0,/solver          PCG;/s//solver          PCGG;/' system/fvSolution | icoFoam | put | Did you mean PCG? | 2606
bad_U_dims     | incompressible/icoFoam/cavity/cavity | sed -i 's/^dimensions .*/dimensions      [0 2 -1 0 0 0 0];/' 0/U | icoFoam | put | but U is always m/s [0 1 -1 0 0 0 0] | 2606
geko_v2412     | incompressible/simpleFoam/pitzDaily | sed -i 's/kEpsilon;/GEKO;/' constant/turbulenceProperties | simpleFoam | none | GEKO came in OpenFOAM v2606, and this run used v2412 | 2412
EOF2
echo "explain fixes: $pass passed, $fail failed  ($out)"
[ $fail = 0 ]
