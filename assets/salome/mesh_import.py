# Q-Continuum: a mesh made in SALOME, for OpenFOAM (Mesh > SALOME > Import a mesh). Run by SALOME without its window:
#   QC_IN   a MED file (Mesh > Export > MED), a saved SALOME study (.hdf, File > Save), or a UNV ideasUnvToFoam can't read
#   QC_OUT  where the UNV goes (ideasUnvToFoam reads it); a mesh with polyhedra (UNV has none: Body Fitting's cut cells,
#           polyhedral meshes) is written as constant/polyMesh-style files in QC_POLY instead
# The mesh with the most volumes is taken, with its face groups (they become the patches). Second-order elements are made
# linear first: OpenFOAM's cells have no mid-side nodes.
import os
import sys
import salome
salome.salome_init()
import SMESH
from salome.smesh import smeshBuilder

smesh = smeshBuilder.New()
src, dst, poly_dir = os.environ["QC_IN"], os.environ["QC_OUT"], os.environ["QC_POLY"]


def say(*a):
    print("Q-Continuum:", *a)
    sys.stdout.flush()


def study_meshes(path):
    """the meshes in a saved study, as smeshBuilder meshes"""
    if not salome.myStudy.Open(path):
        say("could not open the study %s (saved by a newer SALOME?)" % path)
        sys.exit(1)
    comp = salome.myStudy.FindComponent("SMESH")
    if comp is None:
        say("the study %s has no mesh (no Mesh module in it): mesh the geometry in SALOME, then File > Save" % os.path.basename(path))
        sys.exit(1)
    builder = salome.myStudy.NewBuilder()
    try:                                                     # the mesh data is loaded on demand
        builder.LoadWith(comp, salome.lcc.FindOrLoadComponent("FactoryServer", "SMESH"))
    except Exception as e:                                   # noqa: BLE001 -- older SALOMEs load it by themselves
        print("(LoadWith: %s)" % e)
    out = []
    it = salome.myStudy.NewChildIterator(comp)
    it.InitEx(True)
    while it.More():
        so = it.Value()
        obj = so.GetObject()
        m = obj._narrow(SMESH.SMESH_Mesh) if obj is not None else None
        if m is not None:
            out.append(smesh.Mesh(m))
        it.Next()
    return out


ext = os.path.splitext(src)[1].lower()
if ext in (".hdf", ".hdf5"):
    meshes = study_meshes(src)
elif ext == ".unv":                                          # one ideasUnvToFoam could not read (second-order elements)
    meshes = [smesh.CreateMeshesFromUNV(src)]
else:
    meshes, status = smesh.CreateMeshesFromMED(src)
if not meshes:
    say("no mesh in %s" % os.path.basename(src))
    sys.exit(1)
for m in meshes:
    print("mesh %-20s %9d volumes %9d faces" % (m.GetName(), m.NbVolumes(), m.NbFaces()))
m = max(meshes, key=lambda m: m.NbVolumes())
if m.NbVolumes() == 0:
    if m.NbFaces() > 0:
        say("%s is only a surface mesh: compute a 3-D (volume) mesh in SALOME first, then save or export it again" % m.GetName())
    else:
        say("%s has no elements: compute it in SALOME (Mesh > Compute), then File > Save (or Export) again" % m.GetName())
    sys.exit(1)
if len([x for x in meshes if x.NbVolumes() > 0]) > 1:
    say("several volume meshes: took %s, the one with the most cells (export only the one you want to choose it)" % m.GetName())

if m.NbVolumesOfOrder(SMESH.ORDER_QUADRATIC) > 0:
    say("second-order elements made first-order (OpenFOAM's cells have no mid-side nodes)")
    m.ConvertFromQuadratic()

groups = [g for g in m.GetGroups() if g.GetType() == SMESH.FACE]
print("mesh %s: %d volumes (%d tetrahedra, %d hexahedra, %d prisms, %d pyramids, %d polyhedra), %d faces; face groups: %s"
      % (m.GetName(), m.NbVolumes(), m.NbTetras(), m.NbHexas(), m.NbPrisms(), m.NbPyramids(), m.NbPolyhedrons(), m.NbFaces(),
         ", ".join(g.GetName() for g in groups) or "none"))
if not groups:
    say("the mesh has no face groups, so the boundary will be one patch (defaultFaces): in SALOME, Mesh > Create Groups from Geometry "
        "(or Create Group on faces) names the inlet, outlet and walls")
# ideasUnvToFoam/our writer key patches on group names: keep them OpenFOAM words
for g in groups:
    n = g.GetName()
    ok = "".join(ch if (ch.isalnum() or ch == "_") else "_" for ch in n.strip()) or "group"
    if ok[0].isdigit():
        ok = "g_" + ok
    if ok != n:
        say("group '%s' renamed %s (an OpenFOAM patch name is letters, digits and _)" % (n, ok))
        g.SetName(ok)

CELL_FACES = {                                               # SMDS's node order: tetra, pyramid, pentahedron, hexahedron
    4: ((0, 1, 2), (0, 1, 3), (1, 2, 3), (0, 2, 3)),
    5: ((0, 1, 2, 3), (0, 1, 4), (1, 2, 4), (2, 3, 4), (3, 0, 4)),
    6: ((0, 1, 2), (3, 4, 5), (0, 1, 4, 3), (1, 2, 5, 4), (2, 0, 3, 5)),
    8: ((0, 1, 2, 3), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)),
}


def write_polymesh(mesh, out):
    """the mesh as OpenFOAM's polyMesh (ascii), for meshes UNV cannot hold. Each face group is a patch; boundary faces in
    no group go to defaultFaces."""
    nodes = mesh.GetNodesId()
    pi = {n: i for i, n in enumerate(nodes)}
    pts = [mesh.GetNodeXYZ(n) for n in nodes]
    cells = mesh.GetElementsByType(SMESH.VOLUME)
    poly = set(mesh.GetIdsFromFilter(smesh.GetFilter(SMESH.VOLUME, SMESH.FT_ElemGeomType, SMESH.Geom_POLYHEDRA)))
    faces = {}
    for ci, c in enumerate(cells):
        if c in poly:
            fl = [[pi[n] for n in mesh.GetElemFaceNodes(c, k)] for k in range(mesh.ElemNbFaces(c))]
        else:
            en = [pi[n] for n in mesh.GetElemNodes(c)]
            if len(en) not in CELL_FACES:
                say("an element with %d nodes is not a cell OpenFOAM knows" % len(en))
                sys.exit(1)
            fl = [[en[k] for k in f] for f in CELL_FACES[len(en)]]
        cp = set(v for f in fl for v in f)
        cc = [sum(pts[v][k] for v in cp) / len(cp) for k in range(3)]
        for f in fl:
            key = tuple(sorted(f))
            if key in faces:
                faces[key][2] = ci
                continue
            nrm = [0.0, 0.0, 0.0]
            for a, b in zip(f, f[1:] + f[:1]):
                pa, pb = pts[a], pts[b]
                nrm[0] += (pa[1] - pb[1]) * (pa[2] + pb[2])
                nrm[1] += (pa[2] - pb[2]) * (pa[0] + pb[0])
                nrm[2] += (pa[0] - pb[0]) * (pa[1] + pb[1])
            fc = [sum(pts[v][k] for v in f) / len(f) for k in range(3)]
            if sum(nrm[k] * (fc[k] - cc[k]) for k in range(3)) < 0:
                f = f[::-1]
            faces[key] = [f, ci, -1]
    patch_of = {}
    for g in mesh.GetGroups():
        if g.GetType() == SMESH.FACE:
            for fid in g.GetIDs():
                patch_of.setdefault(tuple(sorted(pi[n] for n in mesh.GetElemNodes(fid))), g.GetName())
    inner, bound = [], {}
    for key, (f, o, n) in faces.items():
        if n < 0:
            bound.setdefault(patch_of.get(key, "defaultFaces"), []).append((f, o))
        else:
            inner.append((f, o, n) if o < n else (f[::-1], n, o))
    inner.sort(key=lambda t: (t[1], t[2]))
    names = sorted(n for n in bound if n != "defaultFaces") + (["defaultFaces"] if "defaultFaces" in bound else [])
    allf = [t[0] for t in inner] + [f for n in names for f, _ in bound[n]]
    owner = [t[1] for t in inner] + [o for n in names for _, o in bound[n]]
    os.makedirs(out, exist_ok=True)

    def write(name, cls, items, note=""):
        with open(os.path.join(out, name), "w") as fh:
            fh.write('FoamFile\n{\n    version 2.0;\n    format ascii;\n    class %s;\n%s    location "constant/polyMesh";\n'
                     '    object %s;\n}\n\n%d\n(\n' % (cls, ('    note "%s";\n' % note) if note else "", name, len(items)))
            fh.writelines(items)
            fh.write(")\n")
    note = "nPoints:%d nCells:%d nFaces:%d nInternalFaces:%d" % (len(pts), len(cells), len(allf), len(inner))
    write("points", "vectorField", ["(%.12g %.12g %.12g)\n" % tuple(p) for p in pts])
    write("faces", "faceList", ["%d(%s)\n" % (len(f), " ".join(map(str, f))) for f in allf])
    write("owner", "labelList", ["%d\n" % o for o in owner], note)
    write("neighbour", "labelList", ["%d\n" % t[2] for t in inner], note)
    with open(os.path.join(out, "boundary"), "w") as fh:
        fh.write('FoamFile\n{\n    version 2.0;\n    format ascii;\n    class polyBoundaryMesh;\n'
                 '    location "constant/polyMesh";\n    object boundary;\n}\n\n%d\n(\n' % len(names))
        start = len(inner)
        for n in names:
            fh.write("    %s\n    {\n        type patch;\n        nFaces %d;\n        startFace %d;\n    }\n" % (n, len(bound[n]), start))
            start += len(bound[n])
        fh.write(")\n")


if m.NbPolyhedrons():
    say("the mesh has %d polyhedra, which UNV cannot hold: writing it as an OpenFOAM polyMesh" % m.NbPolyhedrons())
    write_polymesh(m, poly_dir)
    print("wrote %s" % poly_dir)
else:
    m.ExportUNV(dst)
    print("wrote %s" % dst)
