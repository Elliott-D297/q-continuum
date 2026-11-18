# Q-Continuum: a part meshed for CalculiX (docs/FEA_PLAN.md), run by SALOME without its window
# ------------------------------------------------------------------------------------------------
#   QC_IN     the part: STEP (.step .stp), IGES (.iges .igs), BREP (.brep), STL (.stl), or a mesh made in SALOME
#             (.med, .unv, or a saved study .hdf)
#   QC_OUT    where the mesh goes (.qcmesh, plain text, read by femesh.c)
#   QC_SIZE   the target element size in millimetres (0: a twentieth of the part's largest side)
#   QC_SCALE  millimetres per unit of a file without a unit (STL, BREP, MED, UNV): 1 for mm, 1000 for metres, 25.4 inches
#   QC_ORDER  2 (quadratic tetrahedra, C3D10: the default, accurate in bending) or 1 (linear)
# Everything is written in millimetres. STEP and IGES carry their unit: SALOME reads them into metres, scaled here by 1000.
# The .qcmesh:
#   QCMESH 1
#   nodes N             then N lines: id x y z
#   volumes M           then M lines: n k id1 ... idk   (k = 4, 10 tetrahedra; 8, 20 hexahedra; 6, 15 prisms; 5, 13 pyramids)
#   faces F             then F lines: g k id1 ... idk   (k = 3, 6 triangles; 4, 8 quadrangles; g the group, 0-based)
#   groups G            then G lines: g name
# For CAD a group is a CAD face; for a SALOME mesh its face groups; an STL has none (Q-Continuum groups its faces by angle).
import math
import os
import sys
import salome
salome.salome_init()
import GEOM
from salome.geom import geomBuilder
import SMESH
from salome.smesh import smeshBuilder

geompy = geomBuilder.New()
smesh = smeshBuilder.New()
src, out = os.environ["QC_IN"], os.environ["QC_OUT"]
SIZE = float(os.environ.get("QC_SIZE", "0") or 0)
SCALE = float(os.environ.get("QC_SCALE", "1") or 1)
ORDER = int(os.environ.get("QC_ORDER", "2") or 2)
ext = os.path.splitext(src)[1].lower()
HERE = os.path.dirname(os.path.abspath(out))


def say(*a):
    print("Q-Continuum:", *a)
    sys.stdout.flush()


def fail(msg):
    say(msg)
    sys.exit(1)


def bbox_mesh(m):
    b = m.GetBoundingBox()
    return b.minX, b.maxX, b.minY, b.maxY, b.minZ, b.maxZ


# ---------------------------------------------------------------- CAD: STEP, IGES, BREP
def cad_shape():
    try:
        if ext in (".step", ".stp"):
            shape, k = geompy.ImportSTEP(src), 1000.0
        elif ext in (".iges", ".igs"):
            shape, k = geompy.ImportIGES(src), 1000.0
        else:
            shape, k = geompy.ImportBREP(src), SCALE
    except Exception:
        shape = None
    if shape is None:
        fail("%s is not a %s file SALOME can read (is it whole? saved from CAD as %s?)" % (os.path.basename(src), ext[1:].upper(), ext[1:].upper()))
    if k != 1.0:
        shape = geompy.MakeScaleTransform(shape, None, k)
    solids = geompy.SubShapeAll(shape, geompy.ShapeType["SOLID"])
    faces = geompy.SubShapeAll(shape, geompy.ShapeType["FACE"])
    if not faces:
        fail("the file has no surfaces (only points or curves?)")
    if not solids:                                   # loose faces (IGES): sewn into closed shells, each made a solid
        x0, x1, y0, y1, z0, z1 = geompy.BoundingBox(shape)
        size = max(x1 - x0, y1 - y0, z1 - z0)
        for rel in (1e-5, 1e-4, 1e-3):
            try:
                sewn = geompy.MakeSewing(faces, rel * size) if len(faces) > 1 else shape
                made = [geompy.MakeSolid([sh]) for sh in geompy.SubShapeAll(sewn, geompy.ShapeType["SHELL"])]
                if made and all(geompy.BasicProperties(m)[2] > 0 for m in made):
                    solids = made
                    shape = made[0] if len(made) == 1 else geompy.MakeCompound(made)
                    say("sewed the file's %d faces into %d solid(s)" % (len(faces), len(made)))
                    break
            except Exception:
                pass
        if not solids:
            fail("the file has surfaces but no closed solid: a part to simulate must be closed (watertight)")
    if len(solids) > 1:                              # an assembly: bonded, the touching faces shared (conformal mesh)
        try:
            shape = geompy.MakePartition(solids, [], [], [], geompy.ShapeType["SOLID"], 0, [], 0)
            say("the file's %d solids are bonded where they touch (one mesh, shared nodes)" % len(solids))
        except Exception as e:
            say("could not bond the file's %d solids (%s): fused into one" % (len(solids), e))
            shape = geompy.MakeFuseList(solids, True, True)
    return shape


def mesh_cad():
    shape = cad_shape()
    x0, x1, y0, y1, z0, z1 = geompy.BoundingBox(shape)
    L = max(x1 - x0, y1 - y0, z1 - z0)
    h = SIZE if SIZE > 0 else L / 20
    say("part %.4g x %.4g x %.4g mm, %d faces; element size %.3g mm" % (x1 - x0, y1 - y0, z1 - z0,
        len(geompy.SubShapeAll(shape, geompy.ShapeType["FACE"])), h))
    mesh = smesh.Mesh(shape, "part")
    algo = mesh.Tetrahedron(algo=smeshBuilder.NETGEN_1D2D3D)
    p = algo.Parameters()
    p.SetMaxSize(h)
    p.SetMinSize(h / 20)
    p.SetSecondOrder(ORDER == 2)
    p.SetOptimize(True)
    p.SetFineness(2)                                 # moderate
    p.SetGrowthRate(0.3)
    for setter, value in (("SetUseSurfaceCurvature", True), ("SetNbSegPerEdge", 1.0), ("SetNbSegPerRadius", 2.0)):
        try:
            getattr(p, setter)(value)
        except Exception:
            pass
    if not mesh.Compute() or mesh.NbVolumes() == 0:
        fail("NETGEN could not mesh the part at %.3g mm: try another element size (a smaller one for thin walls)" % h)
    faces = geompy.SubShapeAllSortedCentres(shape, geompy.ShapeType["FACE"])
    groups = []
    for i, f in enumerate(faces):
        g = mesh.GroupOnGeom(f, "Face %d" % (i + 1), SMESH.FACE)
        groups.append(("Face %d" % (i + 1), g))
    return mesh, groups


# ---------------------------------------------------------------- STL: its triangles, remeshed, filled with tetrahedra
def mmg_remesh(m, h):
    import shutil
    import subprocess
    exe = shutil.which("mmgs_O3") or shutil.which("mmgs")
    if not exe:
        return None
    a, b = os.path.join(HERE, "mmg_in.mesh"), os.path.join(HERE, "mmg_out.mesh")
    for f in (a, b):
        if os.path.exists(f):
            os.remove(f)
    m.ExportGMF(a)
    r = subprocess.run([exe, "-in", a, "-out", b, "-hmax", "%g" % h, "-hmin", "%g" % (h / 10), "-hausd", "%g" % (0.01 * h),
                        "-hgrad", "1.3", "-ar", "30", "-v", "0"], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(b):
        return None
    got = smesh.CreateMeshesFromGMF(b)
    rm = got[0] if isinstance(got, (list, tuple)) else got
    return smesh.Mesh(rm.GetMesh()) if not hasattr(rm, "MakeGroupByIds") else rm


def netgen_remesh(m, h):
    rm = smesh.CopyMesh(m.GetMesh(), "surface", False, False)
    algo = rm.Triangle(algo=smeshBuilder.NETGEN_1D2D)    # on a mesh without a shape: NETGEN's remesher
    p = algo.Parameters()
    p.SetMaxSize(h)
    p.SetMinSize(h / 10)
    p.SetGrowthRate(0.3)
    for setter, value in (("SetRidgeAngle", 30.0), ("SetUseSurfaceCurvature", True), ("SetQuadAllowed", False)):
        try:
            getattr(p, setter)(value)
        except Exception:
            pass
    if not rm.Compute() or rm.NbFaces() == 0:
        return None
    return rm


def mesh_stl():
    got = smesh.CreateMeshesFromSTL(src)
    m = got[0] if isinstance(got, (list, tuple)) else got
    if m is None or m.NbFaces() == 0:
        fail("%s has no triangles SALOME can read" % os.path.basename(src))
    if SCALE != 1.0:
        m.Scale(m, SMESH.PointStruct(0, 0, 0), [SCALE, SCALE, SCALE], False)
    m.MergeNodes(m.FindCoincidentNodes(1e-7 * max(1e-9, max(bbox_mesh(m)[1] - bbox_mesh(m)[0], 1e-9))))
    x0, x1, y0, y1, z0, z1 = bbox_mesh(m)
    L = max(x1 - x0, y1 - y0, z1 - z0)
    h = SIZE if SIZE > 0 else L / 20
    say("part %.4g x %.4g x %.4g mm, %d triangles; element size %.3g mm" % (x1 - x0, y1 - y0, z1 - z0, m.NbFaces(), h))
    free = m.GetFreeBorders() if hasattr(m, "GetFreeBorders") else []
    if free:
        say("the STL has %d open edge loop(s): a part to simulate must be closed (watertight); trying anyway" % len(free))
    s = mmg_remesh(m, h)
    how = "MMG"
    if s is None:
        s, how = netgen_remesh(m, h), "NETGEN"
    if s is None:
        s, how = m, None
        say("could not remesh the surface: meshing on the STL's own triangles (slivers make poor tetrahedra)")
    else:
        say("surface remeshed by %s: %d triangles" % (how, s.NbFaces()))
    probe = [(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2]
    probe[0] = x0 - 0.01 * L
    s.Reorient2D(s, [-1, 0, 0], probe)               # NETGEN wants the surface's normals out of the solid
    algo = s.Tetrahedron(algo=smeshBuilder.NETGEN_3D)
    p = algo.Parameters()
    p.SetMaxSize(h)
    p.SetMinSize(h / 20)
    p.SetOptimize(True)
    p.SetGrowthRate(0.3)
    if not s.Compute() or s.NbVolumes() == 0:
        fail("NETGEN could not fill the STL with tetrahedra: is it closed, without overlapping or self-crossing triangles?")
    if ORDER == 2:
        s.ConvertToQuadratic(True)
    return s, []


# ---------------------------------------------------------------- a mesh made in SALOME
def study_meshes(path):
    if not salome.myStudy.Open(path):
        fail("could not open the study %s (saved by a newer SALOME?)" % path)
    comp = salome.myStudy.FindComponent("SMESH")
    if comp is None:
        fail("the study %s has no mesh: mesh the part in SALOME, then File > Save" % os.path.basename(path))
    builder = salome.myStudy.NewBuilder()
    try:
        builder.LoadWith(comp, salome.lcc.FindOrLoadComponent("FactoryServer", "SMESH"))
    except Exception:
        pass
    found = []
    it = salome.myStudy.NewChildIterator(comp)
    it.InitEx(True)
    while it.More():
        o = it.Value().GetObject()
        try:
            found.append(smesh.Mesh(o.GetMesh()) if hasattr(o, "GetMesh") else None)
        except Exception:
            pass
        it.Next()
    return [f for f in found if f is not None]


def mesh_salome():
    if ext == ".med":
        got, _ = smesh.CreateMeshesFromMED(src)
    elif ext == ".unv":
        got = smesh.CreateMeshesFromUNV(src)
        got = got if isinstance(got, (list, tuple)) else [got]
    else:
        got = study_meshes(src)
    got = [m for m in got if m is not None]
    if not got:
        fail("%s has no mesh SALOME can read" % os.path.basename(src))
    m = max(got, key=lambda x: x.NbVolumes())
    if m.NbVolumes() == 0:
        fail("the mesh in %s has no volumes: a solid part needs a 3-D mesh (Mesh > Compute with a 3-D algorithm)" % os.path.basename(src))
    if SCALE != 1.0:
        m.Scale(m, SMESH.PointStruct(0, 0, 0), [SCALE, SCALE, SCALE], False)
    if ORDER == 2 and m.NbTetras() > 0 and m.NbTetrasOfOrder(SMESH.ORDER_QUADRATIC) == 0:
        m.ConvertToQuadratic(True)
        say("its linear tetrahedra made quadratic (C3D10): linear ones are far too stiff in bending")
    groups = [(g.GetName(), g) for g in m.GetGroups() if g.GetType() == SMESH.FACE]
    if not groups:
        say("the mesh has no face groups: Q-Continuum groups its faces by angle (name groups in SALOME to pick them by name)")
    return m, groups


# ---------------------------------------------------------------- the mesh written
if ext in (".step", ".stp", ".iges", ".igs", ".brep", ".brp"):
    mesh, groups = mesh_cad()
elif ext == ".stl":
    mesh, groups = mesh_stl()
elif ext in (".med", ".unv", ".hdf"):
    mesh, groups = mesh_salome()
else:
    fail("cannot read %s files: STEP, IGES, BREP, STL, or a SALOME mesh (MED, UNV, a saved study .hdf)" % ext)

nodes = mesh.GetNodesId()
vols = mesh.GetElementsByType(SMESH.VOLUME)
with open(out + ".tmp", "w") as f:
    f.write("QCMESH 1\nnodes %d\n" % len(nodes))
    for n in nodes:
        x, y, z = mesh.GetNodeXYZ(n)
        f.write("%d %.9g %.9g %.9g\n" % (n, x, y, z))
    f.write("volumes %d\n" % len(vols))
    for e in vols:
        ns = mesh.GetElemNodes(e)
        f.write("%d %s\n" % (len(ns), " ".join(str(i) for i in ns)))
    rows = []
    for gi, (name, g) in enumerate(groups):
        for fid in g.GetIDs():
            ns = mesh.GetElemNodes(fid)
            if len(ns) in (3, 4, 6, 8):
                rows.append("%d %d %s\n" % (gi, len(ns), " ".join(str(i) for i in ns)))
    f.write("faces %d\n" % len(rows))
    f.writelines(rows)
    f.write("groups %d\n" % len(groups))
    for gi, (name, g) in enumerate(groups):
        f.write("%d %s\n" % (gi, name.replace("\n", " ")))
os.replace(out + ".tmp", out)
say("wrote %s: %d nodes, %d elements, %d face groups" % (os.path.basename(out), len(nodes), len(vols), len(groups)))
