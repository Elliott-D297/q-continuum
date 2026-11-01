#!/bin/bash
# tests/stlmesh_layers.sh <test_stlmesh> <OpenFOAM bashrc> <scratch> <NACA_Airfoil.STL>
# The STL mesher's snappyHexMesh route with prism layers (user report 2026-10-26: "the boundary layers do not work at
# all"): a sphere in a wind tunnel, a box meshed inside and the NACA profile in 2-D, each meshed as Generate does
# (tests/stlmesh_mesh.sh). Each must pass checkMesh and get at least 85 % of the layers asked for; the 2-D mesh must be
# one cell deep, as deep as the part.
bin=$1; bashrc=$2; d=$(mktemp -d "$3/stlmesh_layers.XXXXXX"); naca=$(realpath "$4")
here=$(cd "$(dirname "$0")" && pwd)
[ -f "$bashrc" ] || { echo "no OpenFOAM at $bashrc"; exit 1; }
set --                                                     # its bashrc reads our arguments as its own (and sources itself)
source "$bashrc" > /dev/null 2>&1                         # (before set -u: it reads unset variables)
set -u
python3 - "$d" <<'PY'
import sys, math
d = sys.argv[1]
def write(name, tris):
    with open("%s/%s.stl" % (d, name), "w") as f:
        f.write("solid %s\n" % name)
        for t in tris:
            f.write(" facet normal 0 0 0\n  outer loop\n" + "".join("   vertex %r %r %r\n" % p for p in t) + "  endloop\n endfacet\n")
        f.write("endsolid %s\n" % name)
R, n, m, t = 0.05, 48, 24, []
P = lambda i, j: (R * math.sin(math.pi * j / m) * math.cos(2 * math.pi * i / n), R * math.sin(math.pi * j / m) * math.sin(2 * math.pi * i / n), R * math.cos(math.pi * j / m))
for i in range(n):
    for j in range(m):
        a, b, c, e = P(i, j), P(i + 1, j), P(i + 1, j + 1), P(i, j + 1)
        if j > 0: t.append((a, e, b))
        if j < m - 1: t.append((b, e, c))
write("sphere", t)
L, t = [0.2, 0.1, 0.1], []
for ax in range(3):
    for s in (0, 1):
        u, v = [k for k in range(3) if k != ax]
        def p(a, b):
            q = [0.0] * 3; q[ax] = s * L[ax]; q[u] = a * L[u]; q[v] = b * L[v]; return tuple(q)
        quad = [p(0, 0), p(1, 0), p(1, 1), p(0, 1)]
        if (s == 0) != (ax == 1): quad = quad[::-1]
        t += [(quad[0], quad[1], quad[2]), (quad[0], quad[2], quad[3])]
write("box", t)
PY
set_ini() { local f=$1/mesher/mesher.ini; shift; for kv in "$@"; do sed -i "s|^${kv%%=*} = .*|${kv%%=*} = ${kv#*=}|" "$f"; done; }
"$bin" init "$d/sphere.stl" "$d/sphere" 1 && set_ini "$d/sphere" domain=external base_size=0.01 target_size_pct=25 min_size_pct=12.5 \
    prism_thickness_pct=10 upstream=1 downstream=2 sides=1 top=1 below=1
"$bin" init "$d/box.stl" "$d/box" 1 && set_ini "$d/box" base_size=0.01
"$bin" init "$naca" "$d/naca" 0.001 && set_ini "$d/naca" domain=external flow_direction=+y up_direction=+z two_d=1 two_d_normal=x \
    base_size=0.05 target_size_pct=25 min_size_pct=6.25 prism_thickness_pct=10
fails=0
for c in sphere box naca; do
    ( bash "$here/stlmesh_mesh.sh" "$bin" "$d/$c" > "$d/$c.out" 2>&1 ) &
done
wait
for c in sphere box naca; do
    cov=$(tac "$d/$c/log.snappyHexMesh" 2>/dev/null | awk '/^-----/{exit} NF' | grep -E '^[A-Za-z_0-9]+ +[0-9]+ +[0-9]+ +[0-9.]+ ' |
          awk '{f += $2; l += $2 * $4 / $3} END {if (f) printf "%.0f", 100 * l / f; else print 0}')
    ok=$(grep -c 'Mesh OK' "$d/$c/log.checkMesh" 2>/dev/null)
    echo "$c: checkMesh ${ok:-0}, layers ${cov:-0} % of those asked for"
    if [ "${ok:-0}" != 1 ] || [ "${cov:-0}" -lt 85 ]; then echo "FAIL $c"; cat "$d/$c.out"; fails=$((fails + 1)); fi
done
# 2-D: one cell deep (front2D and back2D empty), from x = 0 to the part's 10 mm
if ! grep -q 'Mesh has 2 geometric (non-empty/wedge) directions' "$d/naca/log.checkMesh" ||
   ! grep -Eq 'bounding box \((-?[0-9.e-]+) .*\) \(0\.01' "$d/naca/log.checkMesh"; then
    echo "FAIL naca: not a 2-D mesh over the part's depth"; grep -E 'bounding box|directions' "$d/naca/log.checkMesh"; fails=$((fails + 1))
fi
[ $fails = 0 ] && rm -rf "$d" && echo "stlmesh_layers: all passed"
exit $fails
