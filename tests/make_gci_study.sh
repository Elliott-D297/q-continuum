#!/bin/bash
# make_gci_study.sh <workspace> <cavity case>: a mesh study of the lid-driven cavity on three meshes of Celik et al.'s (2008)
# sizes, 4500, 8000 and 18000 cells (75x60, 100x80, 150x120: refinement 1.333 then 1.5, 2-D) with a drag-like coefficient in postProcessing, Celik et al.'s (2008) worked example values
# (5.863, 5.972, 6.063: GCI 2.2 %), and the .qcstudy that ties them together. For journey t_gci (no solver run needed).
set -e
ws=$1; src=$2
. "${WM_PROJECT_DIR:-/usr/lib/openfoam/openfoam2606}/etc/bashrc" > /dev/null 2>&1 || true
vals=(5.863 5.972 6.063); fac=(1 1.333 2); nx=(75 100 150); ny=(60 80 120)
for k in 0 1 2; do
    d=$ws/gcav_${fac[$k]}
    rm -rf "$d"; cp -a "$src" "$d"; rm -rf "$d"/[1-9]* "$d"/0.[0-9]* "$d"/postProcessing "$d"/constant/polyMesh
    sed -i "s/(20 20 1)/(${nx[$k]} ${ny[$k]} 1)/" "$d/system/blockMeshDict"
    (cd "$d" && blockMesh > log.blockMesh 2>&1)
    mkdir -p "$d/postProcessing/coeffs/0"
    printf '# Time Cd\n0.1 %s\n0.2 %s\n' "${vals[$k]}" "${vals[$k]}" > "$d/postProcessing/coeffs/0/coefficient.dat"
done
{
    echo "# Q-Continuum study"
    echo "name=cavity mesh independence"
    echo "base=$src"
    echo "param=system/blockMeshDict|@refine||1,1.333,2|"
    for k in 0 1 2; do echo "case=$ws/gcav_${fac[$k]}|${fac[$k]}|"; done
    echo "output=0|coeffs/coefficient.dat|Cd"
} > "$ws/cavity_mesh.qcstudy"
