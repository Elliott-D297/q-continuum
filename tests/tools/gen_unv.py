# a 0.4 x 0.1 x 0.1 m duct of hex cells, as SALOME exports UNV: nodes (2411), hexahedra 115 and boundary quads 44 (2412),
# face groups inlet (x = 0), outlet (x = 0.4) and walls (2467)
import sys
nx, ny, nz = 16, 4, 4
L = (0.4, 0.1, 0.1)
nid = lambda i, j, k: 1 + i + (nx + 1) * (j + (ny + 1) * k)
out = []
out += ["    -1", "  2411"]
for k in range(nz + 1):
    for j in range(ny + 1):
        for i in range(nx + 1):
            out.append("%10d%10d%10d%10d" % (nid(i, j, k), 1, 1, 11))
            out.append("%25.16E%25.16E%25.16E" % (L[0] * i / nx, L[1] * j / ny, L[2] * k / nz))
out += ["    -1", "    -1", "  2412"]
eid = 0
groups = {"inlet": [], "outlet": [], "walls": []}
def elem(fe, nodes):
    global eid
    eid += 1
    out.append("%10d%10d%10d%10d%10d%10d" % (eid, fe, 2, 1, 7, len(nodes)))
    out.append("".join("%10d" % n for n in nodes))
    return eid
for k in range(nz):
    for j in range(ny):
        for i in range(nx):
            elem(115, [nid(i, j, k), nid(i + 1, j, k), nid(i + 1, j + 1, k), nid(i, j + 1, k),
                       nid(i, j, k + 1), nid(i + 1, j, k + 1), nid(i + 1, j + 1, k + 1), nid(i, j + 1, k + 1)])
for k in range(nz):
    for j in range(ny):
        groups["inlet"].append(elem(44, [nid(0, j, k), nid(0, j, k + 1), nid(0, j + 1, k + 1), nid(0, j + 1, k)]))
        groups["outlet"].append(elem(44, [nid(nx, j, k), nid(nx, j + 1, k), nid(nx, j + 1, k + 1), nid(nx, j, k + 1)]))
for i in range(nx):
    for k in range(nz):
        groups["walls"].append(elem(44, [nid(i, 0, k), nid(i + 1, 0, k), nid(i + 1, 0, k + 1), nid(i, 0, k + 1)]))
        groups["walls"].append(elem(44, [nid(i, ny, k), nid(i, ny, k + 1), nid(i + 1, ny, k + 1), nid(i + 1, ny, k)]))
    for j in range(ny):
        groups["walls"].append(elem(44, [nid(i, j, 0), nid(i, j + 1, 0), nid(i + 1, j + 1, 0), nid(i + 1, j, 0)]))
        groups["walls"].append(elem(44, [nid(i, j, nz), nid(i + 1, j, nz), nid(i + 1, j + 1, nz), nid(i, j + 1, nz)]))
out += ["    -1", "    -1", "  2467"]
for g, (name, ids) in enumerate(groups.items(), 1):
    out.append("%10d%10d%10d%10d%10d%10d%10d%10d" % (g, 0, 0, 0, 0, 0, 0, len(ids)))
    out.append(name)
    for a in range(0, len(ids), 2):
        out.append("".join("%10d%10d%10d%10d" % (8, e, 0, 0) for e in ids[a:a + 2]))
out.append("    -1")
open(sys.argv[1], "w").write("\n".join(out) + "\n")
