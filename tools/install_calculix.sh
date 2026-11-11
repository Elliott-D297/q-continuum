#!/bin/bash
# install_calculix.sh -- installs CalculiX from Ubuntu's packages: the solver ccx and the pre/post-processor cgx, which
# Q-Continuum runs CalculiX projects with and opens .frd results in. (ccx_preCICE, the coupled build, comes with
# tools/install_precice.sh.)
set -euo pipefail
SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO=sudo
$SUDO apt-get update -qq
$SUDO apt-get install -y calculix-ccx calculix-cgx

# does it solve? one C3D8 brick pulled at one face
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
cat > "$t/check.inp" <<'EOF'
*NODE
1,0,0,0
2,1,0,0
3,1,1,0
4,0,1,0
5,0,0,1
6,1,0,1
7,1,1,1
8,0,1,1
*ELEMENT, TYPE=C3D8, ELSET=E
1,1,2,3,4,5,6,7,8
*MATERIAL, NAME=STEEL
*ELASTIC
210000, 0.3
*SOLID SECTION, ELSET=E, MATERIAL=STEEL
*BOUNDARY
1,1,3
4,1,3
5,1,3
8,1,3
*STEP
*STATIC
*CLOAD
2,1,100
3,1,100
6,1,100
7,1,100
*NODE PRINT, NSET=NALL
U
*END STEP
EOF
if (cd "$t" && OMP_NUM_THREADS=1 ccx -i check > log 2>&1) && grep -q 'displacements' "$t/check.dat"; then
    echo "CalculiX works: $(command -v ccx), cgx: $(command -v cgx || echo 'not found')"
else
    tail -20 "$t/log"
    echo "ccx did not solve the test brick."
    exit 1
fi
