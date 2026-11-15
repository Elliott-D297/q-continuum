#!/bin/bash
# tests/simsetup_adopt.sh <test_simsetup> <OpenFOAM bashrc> <scratch>
# Workflow plan step W2: a tutorial read into Physics > Setup ("adopt"). pitzDaily (simpleFoam, k-epsilon) and the cavity
# (icoFoam, a moving lid): the boundaries, model and fluid read right; adopting changes no case file; a first Write case
# files leaves every value as the case had it (its own schemes, $macros, outlet types, function objects); a change made in
# Setup (the inlet's velocity) is the one thing that changes; both still run. A case Setup can't take is refused with why.
bin=$1; bashrc=$2; d=$(mktemp -d "$3/simsetup_adopt.XXXXXX")
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --
source "$bashrc" > /dev/null 2>&1
set -u
fail=0
check() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fail=1; fi; }
# every value of each file as OpenFOAM reads it, order aside
same_values() {
    local a=$1 b=$2 f
    for f in $(cd "$a" && find 0 constant system -maxdepth 1 -type f ! -name '*.orig' | sort); do
        [ -f "$b/$f" ] || { echo "  missing $f"; return 1; }
        diff <(foamDictionary -expand "$a/$f" 2>&1 | grep -v '^ *//\|location' | sort) \
             <(foamDictionary -expand "$b/$f" 2>&1 | grep -v '^ *//\|location' | sort) > /dev/null || { echo "  values differ: $f"; return 1; }
    done
}

# ---- pitzDaily
cp -a "$FOAM_TUTORIALS/incompressible/simpleFoam/pitzDaily" "$d/pitz"
(cd "$d/pitz" && blockMesh > log.blockMesh 2>&1) || { echo "blockMesh failed"; exit 1; }
cp -a "$d/pitz" "$d/pitz.orig"
out=$("$bin" adopt "$d/pitz" read); echo "$out" | sed 's/^/  | /'
check "pitzDaily adopted" 'echo "$out" | grep -q "^adopt: ok"'
check "simpleFoam, k-epsilon, steady" 'echo "$out" | grep -q "app simpleFoam, turbulence k-epsilon, flow steady, nu 1e-05"'
check "inlet at (10 0 0), 5 % intensity" 'echo "$out" | grep -q "boundary inlet: Inlet vec (10 0 0) I 5%"'
check "outlet at 0" 'echo "$out" | grep -q "boundary outlet: Outlet p 0"'
check "walls" 'echo "$out" | grep -q "boundary upperWall: Wall" && echo "$out" | grep -q "boundary lowerWall: Wall"'
check "frontAndBack empty" 'echo "$out" | grep -q "boundary frontAndBack: empty"'
check "adopting changed no case file" '[ -z "$(diff -rq "$d/pitz.orig" "$d/pitz" | grep -v "Only in $d/pitz: \(simulation.ini\|.qc-setup\)")" ]'
"$bin" adopt "$d/pitz" write > "$d/pitz-write1.txt" || { echo "write failed"; cat "$d/pitz-write1.txt"; exit 1; }
check "first write keeps every value" 'same_values "$d/pitz.orig" "$d/pitz"'
check "\$turbulence still defined before use" 'foamDictionary -entry divSchemes/div\(phi,k\) -expand "$d/pitz/system/fvSchemes" 2>&1 | grep -q limitedLinear'
"$bin" adopt "$d/pitz" vec inlet 12 0 0 > "$d/pitz-write2.txt" || { echo "write 2 failed"; cat "$d/pitz-write2.txt"; exit 1; }
check "the inlet change went in" 'foamDictionary -entry boundaryField/inlet/value "$d/pitz/0/U" -value 2>&1 | tr -d " " | grep -q "(1200)"'
check "the outlet kept its own condition" '[ "$(foamDictionary -entry boundaryField/outlet/type "$d/pitz/0/U" -value)" = zeroGradient ]'
check "the schemes kept" 'same_values_one() { diff <(foamDictionary -expand "$d/pitz.orig/system/fvSchemes" | grep -v "^ *//\|location" | sort) <(foamDictionary -expand "$d/pitz/system/fvSchemes" | grep -v "^ *//\|location" | sort) > /dev/null; }; same_values_one'
(cd "$d/pitz" && foamDictionary -entry endTime -set 3 system/controlDict > /dev/null && simpleFoam > log.simpleFoam 2>&1)
check "pitzDaily runs after the writes" '! grep -q "FOAM FATAL" "$d/pitz/log.simpleFoam" && grep -q "^End" "$d/pitz/log.simpleFoam"'

# ---- the cavity (icoFoam, the lid moving)
cp -a "$FOAM_TUTORIALS/incompressible/icoFoam/cavity/cavity" "$d/cav"
(cd "$d/cav" && blockMesh > log.blockMesh 2>&1) || { echo "blockMesh failed"; exit 1; }
cp -a "$d/cav" "$d/cav.orig"
out=$("$bin" adopt "$d/cav" read); echo "$out" | sed 's/^/  | /'
check "cavity adopted, laminar transient" 'echo "$out" | grep -q "^adopt: ok" && echo "$out" | grep -q "turbulence Laminar, flow transient"'
check "the lid moves at (1 0 0)" 'echo "$out" | grep -q "boundary movingWall: Wall moving (1 0 0)"'
check "fixed walls" 'echo "$out" | grep -q "boundary fixedWalls: Wall$"'
"$bin" adopt "$d/cav" write > "$d/cav-write1.txt" || { echo "cavity write failed"; cat "$d/cav-write1.txt"; exit 1; }
check "cavity: first write keeps every value" 'same_values "$d/cav.orig" "$d/cav"'
check "still runs icoFoam" '[ "$(foamDictionary -entry application -value "$d/cav/system/controlDict")" = icoFoam ]'
(cd "$d/cav" && foamDictionary -entry endTime -set 0.02 system/controlDict > /dev/null && icoFoam > log.icoFoam 2>&1)
check "the cavity runs" '! grep -q "FOAM FATAL" "$d/cav/log.icoFoam" && grep -q "^End" "$d/cav/log.icoFoam"'

# ---- refused: a solver Setup doesn't read yet
cp -a "$FOAM_TUTORIALS/compressible/rhoCentralFoam/forwardStep" "$d/fwd" 2>/dev/null || cp -a "$FOAM_TUTORIALS/compressible/rhoCentralFoam/obliqueShock" "$d/fwd"
(cd "$d/fwd" && blockMesh > log.blockMesh 2>&1)
out=$("$bin" adopt "$d/fwd" read); rc=$?
check "rhoCentralFoam refused, with the reason" '[ $rc -eq 3 ] && echo "$out" | grep -q "rhoCentralFoam"'
[ $fail -eq 0 ] && echo "simsetup_adopt: all passed" && rm -rf "$d"
exit $fail
