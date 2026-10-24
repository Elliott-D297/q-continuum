# Q-Continuum STL mesher for SALOME
# ------------------------------------------------------------------------------
# Written by Q-Continuum (Mesh > STL mesher) and rewritten on every Generate: change the settings there, or in
# ../mesher/mesher.ini, not here. Runs without SALOME's window from Q-Continuum, or open it in SALOME
# (File > Load Script) to look at the result. It writes salome/mesh.unv, which ideasUnvToFoam turns into
# constant/polyMesh; each boundary becomes an OpenFOAM patch with the same name.
#
# The settings follow STAR-CCM+'s automated mesher: a base size, the target and minimum surface sizes as a percentage
# of it, the curvature (points a circle), surface and volume growth rates, prism layers (number, total thickness as a
# percentage of the base size, stretching) and volumetric controls (boxes, spheres and cylinders with their own size).
#
# Meshers:
#   tet      NETGEN tetrahedra
#   poly     NETGEN tetrahedra here; Q-Continuum then turns them into polyhedra with OpenFOAM's polyDualMesh
#   bodyfit  SALOME's Body Fitting: a Cartesian (trimmed) hexahedral mesh cut by the surface
# Surface:
#   remesh_surface = 0   the STL's own triangles are the boundary mesh (fast, any size of STL; the STL must be good)
#   remesh_surface = 1   CAD is built from the STL and NETGEN remeshes the surface at the target size (best for CAD-made
#                        STL with flat faces; slow above ~50,000 triangles). Body Fitting always builds CAD.
import configparser
import math
import os
import sys
import salome
salome.salome_init()
import GEOM
import SMESH
from salome.geom import geomBuilder
from salome.smesh import smeshBuilder

geompy = geomBuilder.New()
smesh = smeshBuilder.New()

HERE = os.path.dirname(os.path.abspath(globals().get("__file__") or os.path.join(os.getcwd(), "stl_mesh.py")))
CASE = os.path.dirname(HERE)


def say(*a):
    print("Q-Continuum:", *a)
    sys.stdout.flush()


# ---- SETTINGS -------------------------------------------------------------------------------------------------------
ini = configparser.ConfigParser(inline_comment_prefixes=("#",))
ini.read(os.path.join(CASE, "mesher", "mesher.ini"))
S = ini["mesher"]
num = lambda k, d: float(S.get(k, d))
MESHER = S.get("mesher", "tet")
REMESH = int(num("remesh_surface", 0)) != 0 or MESHER == "bodyfit"
BASE = num("base_size", 0.01)
TARGET = BASE * num("target_size_pct", 100) / 100
MIN = BASE * num("min_size_pct", 25) / 100
CURV = num("curvature_points", 36)
SGROW = num("surface_growth", 1.3)
VGROW = num("volume_growth", 1.2)
PRISM = int(num("prism_layers_on", 1)) != 0 and int(num("prism_layers", 0)) > 0
NLAYERS = int(num("prism_layers", 3))
THICK = BASE * num("prism_thickness_pct", 33.3) / 100
STRETCH = num("prism_stretching", 1.2)
EXTERNAL = S.get("domain", "internal") == "external"
UP, DOWN, SIDE, TOP = num("upstream", 3), num("downstream", 8), num("sides", 3), num("top", 3)
GROUND = int(num("ground", 0)) != 0
ZONES = []
for i in range(int(num("zones", 0))):
    w = S.get("zone%d" % i).split()
    ZONES.append(dict(kind=w[0], a=[float(v) for v in w[1:4]], b=[float(v) for v in w[4:7]], r=float(w[7]), size=BASE * float(w[8]) / 100))
BOUNDARIES = []
for i in range(int(num("boundaries", 0))):
    name, kind = S.get("boundary%d" % i).split()[:2]
    BOUNDARIES.append((name, kind.lower()))
WALLS = [n for n, k in BOUNDARIES if k == "wall"]
say("mesher %s, base %g m, target %g m, min %g m, %s flow, %d boundaries%s" %
    (MESHER, BASE, TARGET, MIN, "external" if EXTERNAL else "internal", len(BOUNDARIES), ", surface remeshed" if REMESH else ""))


def region_file(name):
    return os.path.join(CASE, "mesher", "regions", name + ".stl")


def read_stl_bbox():
    lo, hi = [1e30] * 3, [-1e30] * 3
    import struct
    for name, _ in BOUNDARIES:
        with open(region_file(name), "rb") as f:
            data = f.read()
        n = struct.unpack_from("<I", data, 80)[0]
        for t in range(n):
            v = struct.unpack_from("<9f", data, 84 + t * 50 + 12)
            for k in range(3):
                for c in (v[k], v[k + 3], v[k + 6]):
                    lo[k] = min(lo[k], c)
                    hi[k] = max(hi[k], c)
    return lo, hi


LO, HI = read_stl_bbox()
L = max(HI[k] - LO[k] for k in range(3))


def tunnel():
    lo = [LO[0] - UP * L, LO[1] - SIDE * L, LO[2] if GROUND else LO[2] - SIDE * L]
    hi = [HI[0] + DOWN * L, HI[1] + SIDE * L, HI[2] + TOP * L]
    return lo, hi


def tunnel_face_name(c, lo, hi, tol):
    if abs(c[0] - lo[0]) < tol: return "inlet"
    if abs(c[0] - hi[0]) < tol: return "outlet"
    if GROUND and abs(c[2] - lo[2]) < tol: return "ground"
    return "farfield"


def size_file():
    """NETGEN's mesh-size file: points (x y z h) filling each volumetric control"""
    pts = []
    for z in ZONES:
        h = z["size"]
        if z["kind"] == "box":
            lo = [min(z["a"][k], z["b"][k]) for k in range(3)]
            hi = [max(z["a"][k], z["b"][k]) for k in range(3)]
        elif z["kind"] == "sphere":
            lo = [z["a"][k] - z["r"] for k in range(3)]
            hi = [z["a"][k] + z["r"] for k in range(3)]
        else:
            lo = [min(z["a"][k], z["b"][k]) - z["r"] for k in range(3)]
            hi = [max(z["a"][k], z["b"][k]) + z["r"] for k in range(3)]
        step = max(h * 2, max(hi[k] - lo[k] for k in range(3)) / 40)
        n = [max(1, int(math.ceil((hi[k] - lo[k]) / step))) for k in range(3)]
        for i in range(n[0] + 1):
            for j in range(n[1] + 1):
                for k in range(n[2] + 1):
                    p = [lo[0] + (hi[0] - lo[0]) * i / n[0], lo[1] + (hi[1] - lo[1]) * j / n[1], lo[2] + (hi[2] - lo[2]) * k / n[2]]
                    if z["kind"] == "sphere" and sum((p[q] - z["a"][q]) ** 2 for q in range(3)) > z["r"] ** 2:
                        continue
                    if z["kind"] == "cylinder":
                        a, b = z["a"], z["b"]
                        ax = [b[q] - a[q] for q in range(3)]
                        la = math.sqrt(sum(v * v for v in ax)) or 1
                        t = sum((p[q] - a[q]) * ax[q] for q in range(3)) / la / la
                        if t < 0 or t > 1: continue
                        d = [p[q] - a[q] - t * ax[q] for q in range(3)]
                        if sum(v * v for v in d) > z["r"] ** 2: continue
                    pts.append((p, h))
    if not pts:
        return None
    path = os.path.join(HERE, "zones.msz")
    with open(path, "w") as f:
        f.write("%d\n" % len(pts))
        for p, h in pts:
            f.write("%.9g %.9g %.9g %.9g\n" % (p[0], p[1], p[2], h))
        f.write("0\n")
    say("volumetric controls: %d size points in %s" % (len(pts), path))
    return path


def netgen_params(p, three_d=True):
    p.SetMaxSize(BASE)
    p.SetMinSize(MIN)
    p.SetGrowthRate(max(0.05, min(1.0, (VGROW if three_d else SGROW) - 1.0)))   # NETGEN's growth is the rate - 1
    try:
        p.SetFineness(5)                                     # custom: the values below are used
    except Exception:
        pass
    for setter, value in (("SetUseSurfaceCurvature", True), ("SetNbSegPerRadius", max(1.0, CURV / (2 * math.pi))),
                          ("SetNbSegPerEdge", max(1.0, L / TARGET / 10)), ("SetSecondOrder", False), ("SetOptimize", True),
                          ("SetQuadAllowed", False)):
        try:
            getattr(p, setter)(value)
        except Exception:
            pass
    f = size_file()
    if f:
        try:
            p.SetMeshSizeFile(f)
        except Exception as e:
            say("this NETGEN has no mesh-size file (%s): the volumetric controls are skipped" % e)


def add_layers(algo3d, mesh, faces_or_ids, ignore):
    if not PRISM:
        return
    try:
        vl = algo3d.ViscousLayers(THICK, NLAYERS, STRETCH, faces_or_ids, ignore)
        say("prism layers: %d, total thickness %g m, stretching %g" % (NLAYERS, THICK, STRETCH))
        return vl
    except Exception as e:
        say("prism layers could not be added (%s): meshing without them" % e)


# ---- MESH: the STL's triangles as the boundary (no geometry) ----------------------------------------------------------
def mesh_from_triangles():
    parts = []
    for name, _ in BOUNDARIES:
        meshes, status = smesh.CreateMeshesFromSTL(region_file(name))
        m = meshes if not isinstance(meshes, (list, tuple)) else meshes[0]
        g = m.CreateEmptyGroup(SMESH.FACE, name)
        g.AddFrom(m.GetMesh())
        parts.append(m.GetMesh())
    if EXTERNAL:
        lo, hi = tunnel()
        box = geompy.MakeBoxTwoPnt(geompy.MakeVertex(*lo), geompy.MakeVertex(*hi))
        bm = smesh.Mesh(box, "tunnel")
        a = bm.Triangle(algo=smeshBuilder.NETGEN_1D2D)
        pp = a.Parameters()
        pp.SetMaxSize(BASE * 4)
        pp.SetMinSize(BASE)
        pp.SetGrowthRate(0.3)
        bm.Compute()
        tol = 1e-6 * L
        groups = {}
        for fid in bm.GetElementsByType(SMESH.FACE):
            groups.setdefault(tunnel_face_name(bm.BaryCenter(fid), lo, hi, tol), []).append(fid)
        for name, ids in groups.items():
            bm.MakeGroupByIds(name, SMESH.FACE, ids)
        parts.append(bm.GetMesh())
    mesh = smesh.Concatenate(parts, 1, 1, 1e-7 * L, False, "mesh")   # groups kept, coincident nodes merged
    algo = mesh.Tetrahedron(algo=smeshBuilder.NETGEN_3D)
    netgen_params(algo.Parameters(), True)
    if PRISM and WALLS:
        ids = []
        for g in mesh.GetGroups():
            if g.GetName() in WALLS:
                ids += list(g.GetIDs())
        add_layers(algo, mesh, ids, False)
    return mesh


# ---- MESH: CAD built from the STL, the surface remeshed -----------------------------------------------------------------
def cad_from_stl():
    shells, by_name = [], {}
    for name, _ in BOUNDARIES:
        s = geompy.ImportSTL(region_file(name))
        try:
            s = geompy.UnionFaces(s)                         # coplanar triangles into one face
        except Exception:
            pass
        by_name[name] = s
        shells.append(s)
    sewn = geompy.MakeSewing(shells, 1e-6 * L)
    solid = geompy.MakeSolid([geompy.MakeShell(geompy.SubShapeAll(sewn, geompy.ShapeType["FACE"]))])
    shape = solid
    if EXTERNAL:
        lo, hi = tunnel()
        box = geompy.MakeBoxTwoPnt(geompy.MakeVertex(*lo), geompy.MakeVertex(*hi))
        shape = geompy.MakeCutList(box, [solid], True)
    geompy.addToStudy(shape, "domain")
    groups = {}
    for name, part in by_name.items():
        try:
            faces = geompy.SubShapeAll(geompy.GetInPlace(shape, part), geompy.ShapeType["FACE"])
        except Exception:
            faces = []
        if faces:
            g = geompy.CreateGroup(shape, geompy.ShapeType["FACE"])
            geompy.UnionList(g, faces)
            geompy.addToStudyInFather(shape, g, name)
            groups[name] = g
    if EXTERNAL:
        lo, hi = tunnel()
        tol = 1e-6 * L
        rest = {}
        for f in geompy.SubShapeAll(shape, geompy.ShapeType["FACE"]):
            c = geompy.PointCoordinates(geompy.MakeCDG(f))
            n = tunnel_face_name(c, lo, hi, tol)
            on_box = abs(c[0] - lo[0]) < tol or abs(c[0] - hi[0]) < tol or abs(c[1] - lo[1]) < tol or abs(c[1] - hi[1]) < tol \
                or abs(c[2] - lo[2]) < tol or abs(c[2] - hi[2]) < tol
            if on_box:
                rest.setdefault(n, []).append(f)
        for n, faces in rest.items():
            g = geompy.CreateGroup(shape, geompy.ShapeType["FACE"])
            geompy.UnionList(g, faces)
            geompy.addToStudyInFather(shape, g, n)
            groups[n] = g
    return shape, groups


def mesh_remeshed():
    shape, groups = cad_from_stl()
    mesh = smesh.Mesh(shape, "mesh")
    if MESHER == "bodyfit":
        algo = mesh.BodyFitted()
        algo.SetGrid(TARGET if TARGET < BASE else BASE, TARGET if TARGET < BASE else BASE, TARGET if TARGET < BASE else BASE, 4.0)
        if ZONES:
            say("Body Fitting has one cell size: the volumetric controls are skipped")
        if PRISM:
            say("Body Fitting makes no prism layers here: use the tetrahedral or polyhedral mesher for them")
    else:
        algo = mesh.Tetrahedron(algo=smeshBuilder.NETGEN_1D2D3D)
        p = algo.Parameters()
        netgen_params(p, True)
        p.SetMaxSize(BASE)
        try:
            for name, g in groups.items():                   # the target size on the part's faces
                if name in dict(BOUNDARIES):
                    p.SetLocalSizeOnShape(g, TARGET)
        except Exception:
            pass
        if PRISM and WALLS:
            add_layers(algo, mesh, [g for n, g in groups.items() if n in WALLS], False)
    return mesh, groups


# ---- RUN ----------------------------------------------------------------------------------------------------------------
if REMESH:
    mesh, geo_groups = mesh_remeshed()
else:
    mesh, geo_groups = mesh_from_triangles(), {}
if not mesh.Compute():
    err = ""
    try:
        err = "; ".join("%s: %s" % (e.algoName, e.comment) for e in mesh.GetComputeErrors())
    except Exception:
        pass
    raise SystemExit("Q-Continuum: SALOME could not mesh the STL%s. Check that it is closed (Mesh > STL mesher says), "
                     "try a smaller base size or minimum size, or switch Remesh surface on." % ((": " + err) if err else ""))
for name, g in geo_groups.items():
    mesh.Group(g, name)
# only boundary faces may be in the groups: the converter makes a patch of each
for g in mesh.GetGroups():
    if g.GetType() == SMESH.FACE and g.Size() == 0:
        mesh.RemoveGroup(g)
say("mesh: %d nodes, %d volumes (%d tetrahedra, %d hexahedra, %d prisms, %d polyhedra)" %
    (mesh.NbNodes(), mesh.NbVolumes(), mesh.NbTetras(), mesh.NbHexas(), mesh.NbPrisms(), mesh.NbPolyhedrons()))
out = os.path.join(HERE, "mesh.unv")
mesh.ExportUNV(out)
say("wrote", out)
if salome.sg.hasDesktop():
    salome.sg.updateObjBrowser()
