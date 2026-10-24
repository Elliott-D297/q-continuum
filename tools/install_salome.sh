#!/bin/bash
# install_salome.sh [archive.tar.gz] -- installs a SALOME binary archive into ~/salome, where Q-Continuum finds it
# (~/salome/SALOME-*/salome), with the Ubuntu packages it needs, and checks that it starts.
#
# salome-platform.org only hands its archives to a browser: download "SALOME 9.14.0 for Ubuntu 24.04" (or the Linux
# Universal one) from https://www.salome-platform.org/?page_id=2430 , then run this. Without an argument it takes the
# newest SALOME-*.tar.gz in your Windows Downloads folder (WSL) or ~/Downloads.
set -euo pipefail
dest=$HOME/salome
arch=${1:-}
if [ -z "$arch" ]; then
    arch=$(ls -t /mnt/c/Users/*/Downloads/SALOME-*.tar.gz "$HOME"/Downloads/SALOME-*.tar.gz 2>/dev/null | head -1 || true)
fi
if [ -z "$arch" ] || [ ! -f "$arch" ]; then
    echo "No SALOME archive found. Download SALOME 9.14 for Ubuntu 24.04 from"
    echo "  https://www.salome-platform.org/?page_id=2430"
    echo "into your Downloads folder, then run: bash $0 [path/to/SALOME-....tar.gz]"
    exit 1
fi
echo "Archive: $arch ($(du -h "$arch" | cut -f1))"
mkdir -p "$dest"
# copy off /mnt/c first: tar over 9p is slow
case "$arch" in /mnt/*) echo "Copying it into Linux..."; cp "$arch" "$dest/"; arch=$dest/$(basename "$arch");; esac
top=$(tar tzf "$arch" | head -1 | cut -d/ -f1)
if [ -d "$dest/$top" ]; then
    echo "$dest/$top is there already: not extracting again (delete it to reinstall)"
else
    echo "Extracting into $dest (a few minutes, ~10 GB)..."
    tar xzf "$arch" -C "$dest"
fi
dir=$dest/$top
[ -x "$dir/salome" ] || { echo "No 'salome' launcher in $dir: is this a SALOME binary archive?"; exit 1; }
case "$arch" in "$dest"/*) rm -f "$arch";; esac

# the system packages SALOME's binaries link against: SAT says which are missing
echo "Checking the system packages SALOME needs..."
missing=""
if [ -x "$dir/sat/sat" ]; then
    app=$(ls "$dir/sat/../" 2>/dev/null | grep -o 'SALOME-[0-9.]*' | head -1 || true)
    out=$("$dir/sat/sat" config "${app:-SALOME}" --check_system 2>&1 || true)
    missing=$(echo "$out" | grep -E '\bKO\b|not installed|missing' | grep -oE '\b(lib|python3-|qt|tcl|tk)[A-Za-z0-9.+_-]*' | sort -u | tr '\n' ' ' || true)
fi
# what the 9.14 / Ubuntu 24.04 reports on the SALOME forum needed, in case SAT says nothing
base="python3-pyqt5 python3-pyqt5.qtsvg libgdal34t64 libtbb12 libopenblas0 libxcb-xinerama0 libxcb-cursor0 libxkbcommon-x11-0
      libqt5svg5 libqt5x11extras5 libqt5xml5 libqt5opengl5 libqt5printsupport5 libqt5help5 libqt5sql5-sqlite libqt5test5
      libglu1-mesa libxmu6 libxi6 libxft2 libfreeimage3 libtcl8.6 libtk8.6 libgl2ps1.4 libcminpack1 libmetis5 libscotch-7.0
      libboost-filesystem1.83.0 libboost-regex1.83.0 libboost-thread1.83.0 libboost-date-time1.83.0 libboost-serialization1.83.0
      libboost-chrono1.83.0 libboost-system1.83.0 libpython3.12 libgfortran5 libopenmpi3t64 libhdf5-103-1t64 libcgns4.4t64
      libsuperlu6 libtiff6 libpng16-16t64 libjpeg8"
want=""
for p in $missing $base; do
    dpkg -s "$p" > /dev/null 2>&1 || { apt-cache show "$p" > /dev/null 2>&1 && want="$want $p"; }
done
if [ -n "$want" ]; then
    echo "Installing:$want"
    sudo apt-get update -qq
    sudo apt-get install -y $want
else
    echo "All the packages are there."
fi

# does it start? A tiny mesh without the window
echo "Checking that SALOME starts and meshes (takes up to a minute)..."
t=$(mktemp -d)
cat > "$t/check.py" <<'EOF'
import salome
salome.salome_init()
from salome.geom import geomBuilder
from salome.smesh import smeshBuilder
geompy = geomBuilder.New(); smesh = smeshBuilder.New()
box = geompy.MakeBoxDXDYDZ(1, 1, 1)
m = smesh.Mesh(box); m.Tetrahedron(algo=smeshBuilder.NETGEN_1D2D3D).Parameters().SetMaxSize(0.3)
print("QC-SALOME-OK" if m.Compute() and m.NbTetras() > 0 else "QC-SALOME-FAIL")
EOF
if (cd "$t" && env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME timeout 300 "$dir/salome" -t --shutdown-servers=1 check.py 2>&1) | tee "$t/log" | grep -q QC-SALOME-OK; then
    echo "SALOME works: $dir/salome"
    echo "Restart Q-Continuum; Mesh > STL mesher and Mesh > SALOME script use it."
else
    echo "SALOME did not mesh the test box. The end of its output:"
    tail -25 "$t/log"
    echo "Missing libraries show as 'cannot open shared object file': install the package that has each one (apt-file search <lib>)."
    exit 1
fi
rm -rf "$t"
