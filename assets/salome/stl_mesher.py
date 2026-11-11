# Q-Continuum STL mesher for SALOME
# ------------------------------------------------------------------------------
# Written by Q-Continuum (Mesh > STL mesher) and rewritten on every Generate: change the settings there, or in
# ../mesher/mesher.ini, not here. Runs without SALOME's window from Q-Continuum, or open it in SALOME
# (File > Load Script) to look at the result. It writes salome/mesh.unv, which ideasUnvToFoam turns into
# constant/polyMesh, or for a mesh with polyhedra (UNV has none) salome/polyMesh itself; each boundary becomes an OpenFOAM
# patch with the same name.
#
# The settings follow STAR-CCM+'s automated mesher: a base size, the target and minimum surface sizes as a percentage
# of it, the curvature (points a circle), surface and volume growth rates, prism layers (number, total thickness as a
# percentage of the base size, stretching) and volumetric controls (boxes, spheres and cylinders with their own size).
#
# Meshers:
#   tet      NETGEN tetrahedra
#   poly     NETGEN tetrahedra here; Q-Continuum then turns them into polyhedra with OpenFOAM's polyDualMesh
#   bodyfit  SALOME's Body Fitting: a Cartesian (trimmed) hexahedral mesh cut by the surface
#   sgmsh    Gmsh's tetrahedra (its 3-D algorithm: gmsh_algorithm = delaunay, frontal, hxt, mmg3d or rtree), on the CAD
#   shexa    structured hexahedra (i,j,k): edges divided, faces mapped to quadrangles, solids to hexahedra; every solid
#            must be a block (6 faces)
#   sprism   prisms swept between two opposite faces of each solid (an extruded part), triangles on the source faces
# surface_remesher = netgen | mmg: what remeshes an STL whose triangles are too coarse or thin for the target size
# Surface:
#   remesh_surface = 0   NETGEN meshes on the STL's own triangles (fast, any size of STL; it must be closed). When they are
#                        much coarser than the target size or over 1 % are slivers, NETGEN's surface remesher redoes them at
#                        the target size first. Prism layers for a body in a wind tunnel that is all walls (off the ground):
#                        grown out of its surface along the normals, the tetrahedra filling the tunnel beyond them; else none
#                        (SALOME's own layers need CAD faces).
#   remesh_surface = 1   CAD is built from the STL and NETGEN remeshes the surface at the target size, with prism layers
#                        (best for CAD-made STL with flat faces; slow above a few thousand triangles). Body Fitting always
#                        builds CAD, and makes no layers.
# 2-D (two_d = 1, a profile extruded along two_d_normal): the STL is cut through the middle of its depth; the cut's loops
# become smooth CAD edges (split where the boundary changes and at corners sharper than feature_angle), the face between
# them (and the wind tunnel, or the outer loop) is meshed by NETGEN 2-D with boundary layers on the walls (Viscous Layers
# 2D), triangles (tet, poly) or mostly quadrilaterals (bodyfit), and extruded one cell through the part's depth: front2D
# and back2D, which Q-Continuum makes empty.
import configparser
import math
import os
import struct
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
CAD_ONLY = ("bodyfit", "sgmsh", "shexa", "sprism")             # meshers that work on CAD faces: the STL is made CAD first
REMESH = int(num("remesh_surface", 0)) != 0 or MESHER in CAD_ONLY
GMSH_ALGOS = {"delaunay": 0, "frontal": 1, "mmg3d": 2, "rtree": 3, "hxt": 4}   # GMSHPlugin_Hypothesis.hxx Algo3D
GMSH_ALGO = S.get("gmsh_algorithm", "delaunay").strip()
SURF_REMESHER = S.get("surface_remesher", "netgen").strip()
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
BELOW = num("below", SIDE)
GROUND = int(num("ground", 0)) != 0
# the flow's direction and up, as "+x" ... ("-z"): axis and sign; the tunnel's distances are in REF (0: the body's length
# along the flow). The same rules as stlmesh.c mesher_domain.
def axis_sign(w, d):
    w = (w or d).strip().lower()
    return "xyz".index(w[-1]), (-1 if w.startswith("-") else 1)
FA, FS = axis_sign(S.get("flow_direction"), "+x")
UA, US = axis_sign(S.get("up_direction"), "+z")
if UA == FA:
    UA, US = (1 if FA == 2 else 2), 1
SA = 3 - FA - UA
REF = num("reference_length", 0)
ZONES = []
for i in range(int(num("zones", 0))):
    w = S.get("zone%d" % i).split()
    ZONES.append(dict(kind=w[0], a=[float(v) for v in w[1:4]], b=[float(v) for v in w[4:7]], r=float(w[7]), size=BASE * float(w[8]) / 100))
BOUNDARIES = []
for i in range(int(num("boundaries", 0))):
    name, kind = S.get("boundary%d" % i).split()[:2]
    BOUNDARIES.append((name, kind.lower()))
WALLS = [n for n, k in BOUNDARIES if k == "wall"]
TWO_D = int(num("two_d", 0)) != 0
N2 = "xyz".index((S.get("two_d_normal", "z").strip() or "z")[0])
FEATURE = num("feature_angle", 30)
say("mesher %s, base %g m, target %g m, min %g m, %s flow, %d boundaries%s" %
    (MESHER, BASE, TARGET, MIN, "external" if EXTERNAL else "internal", len(BOUNDARIES),
     (", 2-D across %s" % "xyz"[N2]) if TWO_D else ", surface remeshed" if REMESH else ""))


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
L = REF if REF > 0 else (HI[FA] - LO[FA]) or max(HI[k] - LO[k] for k in range(3))


def tunnel():
    lo, hi = list(LO), list(HI)
    if FS > 0: lo[FA] -= UP * L; hi[FA] += DOWN * L
    else: lo[FA] -= DOWN * L; hi[FA] += UP * L
    lo[SA] -= SIDE * L; hi[SA] += SIDE * L
    below = 0 if GROUND else BELOW * L
    if US > 0: hi[UA] += TOP * L; lo[UA] -= below
    else: lo[UA] -= TOP * L; hi[UA] += below
    return lo, hi


def tunnel_face_name(c, lo, hi, tol):
    inlet, outlet = (lo[FA], hi[FA]) if FS > 0 else (hi[FA], lo[FA])
    if abs(c[FA] - inlet) < tol: return "inlet"
    if abs(c[FA] - outlet) < tol: return "outlet"
    if GROUND and abs(c[UA] - (lo[UA] if US > 0 else hi[UA])) < tol: return "ground"
    return "farfield"


def size_file(extra=(), plane=None):
    """NETGEN's mesh-size file: points (x y z h) filling each volumetric control, and the boxes of extra (kind box, a, b,
    size); plane (axis, coordinate): every point put in that plane (2-D: NETGEN only sizes there)"""
    pts = []
    for z in list(ZONES) + list(extra):
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
                    if plane:
                        p[plane[0]] = plane[1]
                    pts.append((p, h))
    if not pts:
        return None
    path = os.path.join(HERE, "zones.msz")
    with open(path, "w") as f:
        f.write("%d\n" % len(pts))
        for p, h in pts:
            f.write("%.9g %.9g %.9g %.9g\n" % (p[0], p[1], p[2], h))
        f.write("0\n")
    say("size controls: %d size points in %s" % (len(pts), path))
    return path


def far_field():
    """as snappyHexMesh's far field (stlmesh.c mesher_far_levels): in a wind tunnel, cells up to 8 base sizes and no larger
    than half the body, with the base size kept half a length round it and two lengths into its wake (mesher_near_box).
    Returns (the largest size, the size controls keeping the base size near the body)."""
    far = max(BASE, min(BASE * 8, L / 2)) if EXTERNAL else BASE
    if far <= BASE:
        return BASE, []
    lo, hi = tunnel()
    a = [max(lo[k], LO[k] - 0.5 * L) for k in range(3)]
    b = [min(hi[k], HI[k] + 0.5 * L) for k in range(3)]
    if FS > 0: b[FA] = min(hi[FA], HI[FA] + 2 * L)
    else: a[FA] = max(lo[FA], LO[FA] - 2 * L)
    return far, [dict(kind="box", a=a, b=b, r=0, size=BASE)]


def netgen_params(p, three_d=True):
    far, near = far_field()
    p.SetMaxSize(far)
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
    f = size_file(near)
    if f:
        try:
            p.SetMeshSizeFile(f)
        except Exception as e:
            say("this NETGEN has no mesh-size file (%s): the volumetric controls are skipped" % e)
    return far


# The outer layer is held between these fractions of the surface cell next to it (as for snappyHexMesh, stlmesh.h
# mesher_layer_rel): much thinner, the jump to the cells beyond it makes the flow solver diverge there (a NACA profile
# with its outer layer a thirteenth of its triangles blew up in 100 iterations, and ran with it half of them).
LAYER_OUTER_MIN, LAYER_OUTER_MAX = 0.3, 1.0


def layer_total():
    n, r = max(1, NLAYERS), STRETCH
    share = 1.0 / n if abs(r - 1) < 1e-9 else r ** (n - 1) * (r - 1) / (r ** n - 1)
    outer = max(LAYER_OUTER_MIN, min(LAYER_OUTER_MAX, THICK * share / TARGET))
    total = outer * TARGET / share
    if abs(total - THICK) > 0.02 * THICK:
        say("prism layers %g m thick, not %g m: the outer one must be %.0f%% to %.0f%% of the %g m surface cells next to it" %
            (total, THICK, 100 * LAYER_OUTER_MIN, 100 * LAYER_OUTER_MAX, TARGET))
    return total


def add_layers(algo3d, mesh, faces_or_ids, ignore):
    if not PRISM:
        return
    try:
        t = layer_total()
        vl = algo3d.ViscousLayers(t, NLAYERS, STRETCH, faces_or_ids, ignore)
        say("prism layers: %d, total thickness %g m, stretching %g" % (NLAYERS, t, STRETCH))
        return vl
    except Exception as e:
        say("prism layers could not be added (%s): meshing without them" % e)


# ---- MESH: the STL's triangles as the boundary (no geometry) ----------------------------------------------------------
def stl_quality():
    """the mean edge length of the STL's triangles (from the region files) and the share of slivers (an angle under 10
    degrees): NETGEN_3D keeps the boundary's triangles, and thin ones give flat tetrahedra"""
    import struct
    tot, n, thin = 0.0, 0, 0
    for name, _ in BOUNDARIES:
        with open(region_file(name), "rb") as f:
            data = f.read()
        for t in range(struct.unpack_from("<I", data, 80)[0]):
            v = struct.unpack_from("<9f", data, 84 + t * 50 + 12)
            e = [math.sqrt(sum((v[a + k] - v[b + k]) ** 2 for k in range(3))) for a, b in ((0, 3), (3, 6), (6, 0))]
            tot += sum(e)
            n += 1
            e.sort()
            if e[0] < 1e-30:                                 # degenerate; else the smallest angle is opposite the shortest edge
                thin += 1
                continue
            cosa = max(-1.0, min(1.0, (e[1] ** 2 + e[2] ** 2 - e[0] ** 2) / (2 * e[1] * e[2])))
            if math.degrees(math.acos(cosa)) < 10:
                thin += 1
    return (tot / 3 / n, thin / n) if n else (0.0, 0.0)


def stl_meshes():
    """the STL's regions as meshes, each with a face group of its name"""
    parts = []
    for name, _ in BOUNDARIES:
        m = smesh.CreateMeshesFromSTL(region_file(name))   # a Mesh (older SALOME: a (mesh, status) pair)
        if isinstance(m, (list, tuple)):
            m = m[0]
        g = m.CreateEmptyGroup(SMESH.FACE, name)
        g.AddFrom(m.GetMesh())
        parts.append(m.GetMesh())
    return parts


def region_finder(src):
    """a function giving the region (STL group of src) of the face nearest a point"""
    region = {}
    for g in src.GetGroups():
        if g.GetType() == SMESH.FACE:
            for i in g.GetIDs():
                region[i] = g.GetName()
    def find(c):
        hit = src.ProjectPoint(c[0], c[1], c[2], SMESH.FACE)
        return region.get(hit[0] if hit else -1, BOUNDARIES[0][0])
    return find


def mmg_remesh(src):
    """MMG's surface remesher (mmgs, shipped with SALOME) on the STL's triangles: exported as Gmf (.mesh), remeshed between
    the minimum and target sizes keeping the shape within a hundredth of the target, read back; each new face goes to the
    region of the source face nearest its centre. None when MMG is missing or fails."""
    import shutil
    import subprocess
    exe = shutil.which("mmgs_O3") or shutil.which("mmgs")
    if not exe:
        say("MMG's surface remesher (mmgs) is not in this SALOME: NETGEN remeshes instead")
        return None
    find = region_finder(src)
    a, b = os.path.join(HERE, "mmg_in.mesh"), os.path.join(HERE, "mmg_out.mesh")
    for f in (a, b):
        if os.path.exists(f):
            os.remove(f)
    src.ExportGMF(a)
    cmd = [exe, "-in", a, "-out", b, "-hmax", "%g" % TARGET, "-hmin", "%g" % min(MIN, TARGET), "-hausd", "%g" % (0.01 * TARGET),
           "-hgrad", "%g" % max(1.05, SGROW), "-ar", "30", "-v", "0"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(b):
        say("MMG could not remesh the surface (%s): NETGEN remeshes instead" % ((r.stderr or r.stdout).strip().splitlines() or ["no reason given"])[-1])
        return None
    got = smesh.CreateMeshesFromGMF(b)
    rm = got[0] if isinstance(got, (list, tuple)) else got
    rm = smesh.Mesh(rm.GetMesh()) if not hasattr(rm, "MakeGroupByIds") else rm
    for g in list(rm.GetGroups()):
        rm.RemoveGroup(g)
    ids = {}
    for fid in rm.GetElementsByType(SMESH.FACE):
        ids.setdefault(find(rm.BaryCenter(fid)), []).append(fid)
    for name, _ in BOUNDARIES:
        if name in ids:
            rm.MakeGroupByIds(name, SMESH.FACE, ids[name])
    say("surface remeshed by MMG at the target size: %d triangles from the STL's %d" % (rm.NbFaces(), src.NbFaces()))
    return rm


def remesh_body(src):
    """NETGEN's surface remesher on the STL's triangles at the target size, keeping the boundaries: it drops the groups,
    so each new face goes to the region of the source face nearest its centre (surface_remesher = mmg: MMG's instead)"""
    if SURF_REMESHER == "mmg":
        rm = mmg_remesh(src)
        if rm:
            return rm
    find = region_finder(src)
    rm = smesh.CopyMesh(src.GetMesh(), "body", False, False)   # a copy: the source keeps its faces for the lookup
    algo = rm.Triangle(algo=smeshBuilder.NETGEN_1D2D)      # on a mesh without a shape: NETGEN_Remesher_2D
    p = algo.Parameters()
    p.SetMaxSize(TARGET)
    p.SetMinSize(min(MIN, TARGET))
    p.SetGrowthRate(max(0.05, min(1.0, SGROW - 1.0)))
    for setter, value in (("SetRidgeAngle", 30.0), ("SetUseSurfaceCurvature", True),
                          ("SetNbSegPerRadius", max(1.0, CURV / (2 * math.pi))), ("SetQuadAllowed", False)):
        try:
            getattr(p, setter)(value)
        except Exception:
            pass
    if not rm.Compute() or rm.NbFaces() == 0:
        say("NETGEN could not remesh the STL's surface: meshing on its own triangles")
        return None
    ids = {}
    for fid in rm.GetElementsByType(SMESH.FACE):
        ids.setdefault(find(rm.BaryCenter(fid)), []).append(fid)
    for name, _ in BOUNDARIES:
        if name in ids:
            rm.MakeGroupByIds(name, SMESH.FACE, ids[name])
    say("surface remeshed at the target size: %d triangles from the STL's %d" % (rm.NbFaces(), src.NbFaces()))
    return rm


def grow_layers(body):
    """prism layers on a closed body in a wind tunnel, made without CAD: its surface extruded along its normals, out into
    the air, NLAYERS times, the layers then spaced by the stretching. Returns (the layers' prisms, their outer surface as a
    mesh of faces in the group qc_interface), or None when they cross themselves."""
    T, n, r = layer_total(), max(1, NLAYERS), STRETCH
    lay = smesh.CopyMesh(body.GetMesh(), "layers", True, False)
    probe = [(LO[k] + HI[k]) / 2 for k in range(3)]
    probe[0] = LO[0] - 0.01 * L
    lay.Reorient2D(lay, [-1, 0, 0], probe)                 # out of the body, into the air
    base = set(lay.GetNodesId())
    s0 = T / n
    lay.ExtrusionByNormal(lay.GetElementsByType(SMESH.FACE), s0, n, False, True, False, 2)
    prisms = lay.GetElementsByType(SMESH.VOLUME)
    below, tops = {}, []
    for v in prisms:
        q = lay.GetElemNodes(v)
        if len(q) != 6:
            continue
        for i in range(3):
            below[q[3 + i]] = q[i]
        tops.append(q[3:6])
    # the layers spaced by the stretching: the k-th at t_k of the total, along the line it was extruded on
    t = [T * ((r ** k - 1) / (r ** n - 1) if abs(r - 1) > 1e-9 else k / n) for k in range(n + 1)]
    depth = {}
    def walk(v):
        k, b = 0, v
        while b not in base:
            b = below[b]
            k += 1
        return k, b
    for v in below:
        k, b = walk(v)
        depth[v] = k
        pb, pv = lay.GetNodeXYZ(b), lay.GetNodeXYZ(v)
        f = t[k] / (k * s0)
        lay.MoveNode(v, *[pb[c] + f * (pv[c] - pb[c]) for c in range(3)])
    outer = smesh.Mesh()
    ids = {}
    def node(v):
        if v not in ids:
            ids[v] = outer.AddNode(*lay.GetNodeXYZ(v))
        return ids[v]
    top = [outer.AddFace([node(a) for a in q]) for q in tops if depth.get(q[0]) == n]
    outer.MakeGroupByIds("qc_interface", SMESH.FACE, top)
    # crossed layers (a concave corner deeper than they are thick) show as cells turned inside out
    bad = lay.GetIdsFromFilter(smesh.GetFilter(SMESH.VOLUME, SMESH.FT_Volume3D, SMESH.FT_LessThan, 0.0))
    if bad:
        say("the prism layers cross themselves in %d cells (a concave corner, or layers thick against its features)" % len(bad))
        return None
    say("prism layers: %d, total thickness %g m, stretching %g, grown out of the STL's surface (%d prisms)" % (n, T, r, len(prisms)))
    return lay, outer


def mesh_from_triangles():
    parts = stl_meshes()
    # NETGEN_3D keeps the boundary's triangles: an STL much coarser than the target (CAD exports: a box is 12 triangles)
    # would give a few huge tetrahedra, so its surface is remeshed at the target size first
    edge, thin = stl_quality()
    if edge > 2 * TARGET or thin > 0.01:
        say((("the STL's triangles (%.3g m on average) are much coarser than the target size" % edge) if edge > 2 * TARGET else
            ("%.0f %% of the STL's triangles are slivers (an angle under 10 degrees)" % (100 * thin))) + ": remeshing its surface")
        src = smesh.Concatenate(parts, 1, 1, 1e-7 * L, False, "stl")
        body = remesh_body(src)
        parts = [body.GetMesh() if body else src.GetMesh()]
    # prism layers without CAD: an external body that is all walls, off the ground (internal flow: they would have to end at
    # the inlets, which this doesn't do)
    layered = None
    if PRISM and EXTERNAL and not GROUND and BOUNDARIES and all(k == "wall" for _, k in BOUNDARIES):
        body = smesh.Concatenate(parts, 1, 1, 1e-7 * L, False, "body")
        layered = grow_layers(body)
        if layered:
            parts = [layered[1].GetMesh()]
        else:
            parts = [body.GetMesh()]
            say("meshing without prism layers")
    LAYERED.append(layered)
    if EXTERNAL:
        lo, hi = tunnel()
        box = geompy.MakeBoxTwoPnt(geompy.MakeVertex(*lo), geompy.MakeVertex(*hi))
        bm = smesh.Mesh(box, "tunnel")
        a = bm.Triangle(algo=smeshBuilder.NETGEN_1D2D)
        pp = a.Parameters()
        pp.SetMaxSize(far_field()[0])
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
    # NETGEN wants the boundary's normals out of the fluid: an external body's faces point into it, an internal one's out
    # of it (the STL's own orientation may be either, or mixed). The faces nearest a point just off the body are turned
    # that way and the rest of each connected surface follows.
    probe = [(LO[k] + HI[k]) / 2 for k in range(3)]
    probe[0] = LO[0] - 0.01 * L - (layer_total() if layered else 0)   # outside the layers' outer surface, when it is the boundary
    mesh.Reorient2D(mesh, [1 if EXTERNAL else -1, 0, 0], probe)
    algo = mesh.Tetrahedron(algo=smeshBuilder.NETGEN_3D)
    netgen_params(algo.Parameters(), True)
    if PRISM and WALLS and not layered and not (EXTERNAL and not GROUND):
        # SALOME builds viscous layers on CAD faces only: on the bare triangles NETGEN ignores them (and with the
        # triangles imported onto CAD made of them, it crashes)
        say("prism layers on the STL's own triangles are grown for an external body only (all walls, off the ground); here they "
            "need the surface rebuilt from CAD (Remesh surface on, best for CAD-made parts), or snappyHexMesh: this mesh has none")
    return mesh


# ---- MESH: CAD built from the STL, the surface remeshed -----------------------------------------------------------------
def cad_brep():
    """the exact CAD the STL was made from (Q-Continuum writes it beside the STL, in metres), when the part came from a
    STEP or IGES file and its unit was not changed since"""
    if not S.get("cad_source", "").strip() or abs(num("scale", 1) - 1) > 1e-12:
        return None
    p = os.path.splitext(S.get("stl", "").strip())[0] + ".brep"
    return p if os.path.isfile(p) else None


def point_tri_dist2(p, t):
    """squared distance from p to the triangle t (9 floats)"""
    a, b, c = t[0:3], t[3:6], t[6:9]
    sub = lambda u, v: (u[0] - v[0], u[1] - v[1], u[2] - v[2])
    dot = lambda u, v: u[0] * v[0] + u[1] * v[1] + u[2] * v[2]
    ab, ac, ap = sub(b, a), sub(c, a), sub(p, a)
    d1, d2 = dot(ab, ap), dot(ac, ap)
    if d1 <= 0 and d2 <= 0: q = a
    else:
        bp = sub(p, b); d3, d4 = dot(ab, bp), dot(ac, bp)
        cp = sub(p, c); d5, d6 = dot(ab, cp), dot(ac, cp)
        vc, vb, va = d1 * d4 - d3 * d2, d5 * d2 - d1 * d6, d3 * d6 - d5 * d4
        if d3 >= 0 and d4 <= d3: q = b
        elif d6 >= 0 and d5 <= d6: q = c
        elif vc <= 0 and d1 >= 0 and d3 <= 0:
            v = d1 / (d1 - d3); q = (a[0] + v * ab[0], a[1] + v * ab[1], a[2] + v * ab[2])
        elif vb <= 0 and d2 >= 0 and d6 <= 0:
            w = d2 / (d2 - d6); q = (a[0] + w * ac[0], a[1] + w * ac[1], a[2] + w * ac[2])
        elif va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
            w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); q = (b[0] + w * (c[0] - b[0]), b[1] + w * (c[1] - b[1]), b[2] + w * (c[2] - b[2]))
        else:
            den = 1 / (va + vb + vc); v, w = vb * den, vc * den
            q = (a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w, a[2] + ab[2] * v + ac[2] * w)
    d = sub(p, q)
    return dot(d, d)


def cad_parts(path):
    """the CAD's solid, and its faces sorted into the boundaries: each face goes to the region whose triangles pass
    nearest a point inside it (the regions were cut from the STL made of this CAD)"""
    shape = geompy.ImportBREP(path)
    solids = geompy.SubShapeAll(shape, geompy.ShapeType["SOLID"])
    solid = solids[0] if len(solids) == 1 else shape
    tris = []                                                # (name, triangle, centroid, radius about the centroid)
    for name, _ in BOUNDARIES:
        for t in region_tris(name):
            c = ((t[0] + t[3] + t[6]) / 3, (t[1] + t[4] + t[7]) / 3, (t[2] + t[5] + t[8]) / 3)
            r = max(math.sqrt((t[3 * k] - c[0]) ** 2 + (t[3 * k + 1] - c[1]) ** 2 + (t[3 * k + 2] - c[2]) ** 2) for k in range(3))
            tris.append((name, t, c, r))
    faces = {}
    for f in geompy.SubShapeAll(solid, geompy.ShapeType["FACE"]):
        try:
            p = geompy.PointCoordinates(geompy.MakeVertexInsideFace(f))
        except Exception:
            p = geompy.PointCoordinates(geompy.MakeCDG(f))
        best, bd = BOUNDARIES[0][0], 1e300
        for name, t, c, r in tris:
            dc = math.sqrt((p[0] - c[0]) ** 2 + (p[1] - c[1]) ** 2 + (p[2] - c[2]) ** 2) - r
            if dc > 0 and dc * dc >= bd:
                continue                                     # this triangle cannot be nearer
            d = point_tri_dist2(p, t)
            if d < bd:
                best, bd = name, d
        faces.setdefault(best, []).append(f)
    say("meshing the exact CAD (%s): %d faces in %d boundaries" % (os.path.basename(S.get("cad_source").strip()),
        sum(len(v) for v in faces.values()), len(faces)))
    return solid, {n: geompy.MakeCompound(fs) for n, fs in faces.items()}


def cad_from_stl():
    brep = cad_brep()
    if brep:
        solid, by_name = cad_parts(brep)
        return cad_domain(solid, by_name)
    # each region imported, its coplanar triangles merged into faces, the regions sewn: right for CAD-made STL with flat
    # faces (a box: 6 faces, each a boundary), where the faces must stay per region for the groups and the prism layers
    shells, by_name = [], {}
    ntri = 0
    for name, _ in BOUNDARIES:
        with open(region_file(name), "rb") as f:
            ntri += struct.unpack("<I", f.read(84)[80:84])[0]
    try:
        if ntri > 20000:                                     # a fine, curved STL: this route took 12 minutes to fail on 110 k
            raise ValueError("%d triangles" % ntri)
        for name, _ in BOUNDARIES:
            s = geompy.ImportSTL(region_file(name))
            try:
                s = geompy.UnionFaces(s)                     # coplanar triangles into one face
            except Exception:
                pass
            by_name[name] = s
            shells.append(s)
        sewn = geompy.MakeSewing(shells, 1e-6 * L) if len(shells) > 1 else None   # one shell: nothing to sew (it returns None)
        if sewn is None:
            sewn = shells[0] if len(shells) == 1 else geompy.MakeCompound(shells)
        solid = geompy.MakeSolid([geompy.MakeShell(geompy.SubShapeAll(sewn, geompy.ShapeType["FACE"]))])
        say("CAD: a solid of %d faces, %g m3" % (geompy.NumberOfFaces(solid), geompy.BasicProperties(solid)[2]))
        return cad_domain(solid, by_name)
    except Exception as e:
        say("%s: building the solid from all the triangles at once" %
            ("a fine STL (%s)" % e if ntri > 20000 else "the regions did not sew into a closed solid (%s)" % e))
    # that left an edge open on the user's intake (110 k triangles, curved), and so did merging coplanar triangles of the
    # closed shell, which no sewing or ShapeFix closed again (2026-11-10). One STL of every region's triangles, imported
    # at once, shares the edges between regions and closes without sewing: a face per triangle (slow to mesh)
    tris, by_name = [], {}
    for name, _ in BOUNDARIES:
        with open(region_file(name), "rb") as f:
            data = f.read()
        n = struct.unpack_from("<I", data, 80)[0]
        tris += [data[84 + 50 * t:84 + 50 * t + 50] for t in range(n)]
    allp = os.path.join(CASE, "salome", "regions_all.stl")
    with open(allp, "wb") as f:
        f.write(b"\0" * 80 + struct.pack("<I", len(tris)) + b"".join(tris))
    whole = geompy.ImportSTL(allp)
    try:
        solid = geompy.MakeSolid([geompy.MakeShell(geompy.SubShapeAll(whole, geompy.ShapeType["FACE"]))])
    except Exception:
        free = geompy.GetFreeBoundary(geompy.MakeShell(geompy.SubShapeAll(whole, geompy.ShapeType["FACE"])))
        raise SystemExit("Q-Continuum: the STL's surface is not closed (%d open loops of edges): SALOME cannot make a solid of it. "
                         "Prepare the geometry (Geometry step) or use snappyHexMesh or cfMesh, which take it as it is" %
                         (len(free[1]) + len(free[2])))
    for name, _ in BOUNDARIES:
        by_name[name] = geompy.ImportSTL(region_file(name))
    say("CAD: a solid of %d faces (one per triangle), %g m3" % (geompy.NumberOfFaces(solid), geompy.BasicProperties(solid)[2]))
    return cad_domain(solid, by_name)


def cad_domain(solid, by_name):
    """the fluid: the part's inside, or the wind tunnel less the part; a face group per boundary"""
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
    if not cad_brep():
        say("building CAD from the STL's %s (minutes for thousands of triangles)" % ("regions" if len(BOUNDARIES) > 1 else "surface"))
    shape, groups = cad_from_stl()
    mesh = smesh.Mesh(shape, "mesh")
    if MESHER == "bodyfit":
        algo = mesh.BodyFitted()
        h = str(min(TARGET, BASE))                           # the grid's spacing as functions of position: strings
        algo.SetGrid(h, h, h, 4.0)
        try:
            algo.hyp.SetToCreateFaces(True)                  # boundary faces on the CAD faces, so the groups get them
        except Exception as e:
            say("this SALOME cannot make Body Fitting's boundary faces (%s): the boundaries may end up in defaultFaces" % e)
        if ZONES:
            say("Body Fitting has one cell size: the volumetric controls are skipped")
        if PRISM:
            say("Body Fitting makes no prism layers here: use the tetrahedral or polyhedral mesher for them")
    elif MESHER == "sgmsh":
        algo = mesh.Tetrahedron(algo=smeshBuilder.GMSH)
        p = algo.Parameters()
        p.SetIs2d(False)
        p.Set3DAlgo(GMSH_ALGOS.get(GMSH_ALGO, 0))
        p.SetMaxSize(far_field()[0])
        p.SetMinSize(MIN)
        try:
            p.SetMeshCurvatureSize(int(max(4, CURV)))       # elements across 2 pi of curvature
        except Exception:
            pass
        say("Gmsh, 3-D algorithm %s" % GMSH_ALGO)
        if ZONES:
            say("Gmsh here has one size range (minimum to base): the volumetric controls are skipped")
        if PRISM and WALLS:
            LAYERS.append(add_layers(algo, mesh, [g for n, g in groups.items() if n in WALLS], False))
    elif MESHER in ("shexa", "sprism"):
        solids = geompy.SubShapeAll(shape, geompy.ShapeType["SOLID"]) or [shape]
        if MESHER == "shexa":
            bad = [i for i, so in enumerate(solids) if len(geompy.SubShapeAll(so, geompy.ShapeType["FACE"])) != 6]
            if bad:
                raise SystemExit("Q-Continuum: structured hexahedra need every solid to be a block (6 faces); %d of %d here are not "
                                 "(%s). Partition the part into blocks in SALOME first, or pick another mesher." %
                                 (len(bad), len(solids), "a wind tunnel round a body never is" if EXTERNAL else "a curved or holed part"))
        mesh.Segment().LocalLength(TARGET)
        if MESHER == "shexa":
            mesh.Quadrangle()
            mesh.Hexahedron()
            say("structured hexahedra (i,j,k), edges divided every %g m" % TARGET)
        else:
            mesh.Triangle(algo=smeshBuilder.NETGEN_2D).Parameters().SetMaxSize(TARGET)
            mesh.Prism()
            say("prisms swept between each solid's opposite faces, %g m apart along the sweep" % TARGET)
        if PRISM:
            say("this mesher makes no prism layers on the walls")
    else:
        algo = mesh.Tetrahedron(algo=smeshBuilder.NETGEN_1D2D3D)
        p = algo.Parameters()
        netgen_params(p, True)
        try:
            for name, g in groups.items():                   # the target size on the part's faces
                if name in dict(BOUNDARIES):
                    p.SetLocalSizeOnShape(g, TARGET)
        except Exception:
            pass
        if PRISM and WALLS:
            LAYERS.append(add_layers(algo, mesh, [g for n, g in groups.items() if n in WALLS], False))
    return mesh, groups


# ---- MESH: 2-D, a profile meshed in its plane and extruded one cell ------------------------------------------------------
def region_tris(name):
    with open(region_file(name), "rb") as f:
        data = f.read()
    return [struct.unpack_from("<9f", data, 84 + t * 50 + 12) for t in range(struct.unpack_from("<I", data, 80)[0])]


def profile_loops():
    """the STL cut through the middle of its depth: closed loops of (u, v) points, each segment with its region"""
    mid = (LO[N2] + HI[N2]) / 2
    U, V = (N2 + 1) % 3, (N2 + 2) % 3
    segs = []
    for name, _ in BOUNDARIES:
        for t in region_tris(name):
            P = [tuple(t[0:3]), tuple(t[3:6]), tuple(t[6:9])]
            pts = []
            for a, b in ((0, 1), (1, 2), (2, 0)):
                pa, pb = min(P[a], P[b]), max(P[a], P[b])   # one way round each edge: its two triangles get the same point
                da, db = pa[N2] - mid, pb[N2] - mid
                if (da < 0) != (db < 0):
                    f = da / (da - db)
                    pts.append((pa[U] + f * (pb[U] - pa[U]), pa[V] + f * (pb[V] - pa[V])))
            if len(pts) == 2 and pts[0] != pts[1]:
                segs.append((pts[0], pts[1], name))
    at = {}
    for i, (a, b, _) in enumerate(segs):
        at.setdefault(a, []).append(i)
        at.setdefault(b, []).append(i)
    used, loops = [False] * len(segs), []
    for i0 in range(len(segs)):
        if used[i0]:
            continue
        used[i0] = True
        a, b, name = segs[i0]
        pts, names = [a, b], [name]
        while True:
            nxt = [j for j in at.get(pts[-1], []) if not used[j]]
            if not nxt:
                break
            j = nxt[0]
            used[j] = True
            a, b, name = segs[j]
            pts.append(b if a == pts[-1] else a)
            names.append(name)
        if pts[-1] == pts[0] and len(pts) > 3:
            loops.append((pts[:-1], names))
        else:
            say("the cut through the part has an open line of %d points (a gap in the STL): left out" % len(pts))
    return loops


def loop_area(pts):
    return 0.5 * sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1] for i in range(len(pts)))


def mesh_2d():
    U, V = (N2 + 1) % 3, (N2 + 2) % 3
    W0, DEPTH = LO[N2], HI[N2] - LO[N2]
    tol = 1e-7 * L
    def vtx(p):
        c = [0.0, 0.0, 0.0]
        c[U], c[V], c[N2] = p[0], p[1], W0
        return geompy.MakeVertex(*c)
    loops = profile_loops()
    if not loops:
        raise SystemExit("Q-Continuum: the cut through the middle of the part's depth found no closed outline: is it a profile "
                         "extruded along %s (2-D, Across %s)?" % ("xyz"[N2], "XYZ"[N2]))
    wires, named = [], []                                    # named: (edge, region)
    for pts, names in loops:
        # points closer than a hair to the last are dropped (with their segment)
        keep_p, keep_n = [pts[0]], []
        for i in range(1, len(pts) + 1):
            p = pts[i % len(pts)]
            if math.hypot(p[0] - keep_p[-1][0], p[1] - keep_p[-1][1]) < 1e-5 * L and i < len(pts):
                continue
            keep_n.append(names[i - 1])
            if i < len(pts):
                keep_p.append(p)
        pts, names = keep_p, keep_n
        n = len(pts)
        # where an edge ends: the boundary changes, or a corner sharper than the feature angle (a trailing edge)
        breaks = []
        for i in range(n):
            a, b, c = pts[i - 1], pts[i], pts[(i + 1) % n]
            t1, t2 = (b[0] - a[0], b[1] - a[1]), (c[0] - b[0], c[1] - b[1])
            l1, l2 = math.hypot(*t1), math.hypot(*t2)
            turn = math.degrees(math.acos(max(-1.0, min(1.0, (t1[0] * t2[0] + t1[1] * t2[1]) / (l1 * l2))))) if l1 > 0 and l2 > 0 else 0
            if names[i - 1] != names[i] or turn > FEATURE:
                breaks.append(i)
        edges = []
        if not breaks:
            e = geompy.MakeInterpol([vtx(p) for p in pts], True, False)
            edges.append(e); named.append((e, names[0]))
        else:
            vs = {i: vtx(pts[i]) for i in breaks}
            for k, i in enumerate(breaks):
                j = breaks[(k + 1) % len(breaks)]
                run = list(range(i, j + 1)) if j > i else list(range(i, n)) + list(range(0, j + 1))
                if len(breaks) == 1:
                    run = list(range(i, n)) + list(range(0, i + 1))
                vv = [vs[run[0]]] + [vtx(pts[q]) for q in run[1:-1]] + [vs[run[-1]]]
                e = geompy.MakeEdge(vv[0], vv[1]) if len(vv) == 2 else geompy.MakeInterpol(vv, False, False)
                edges.append(e); named.append((e, names[i]))
        wires.append((geompy.MakeWire(edges, tol), abs(loop_area(pts))))
    if EXTERNAL:
        lo, hi = tunnel()
        corners = [(lo[U], lo[V]), (hi[U], lo[V]), (hi[U], hi[V]), (lo[U], hi[V])]
        outer = geompy.MakePolyline([vtx(p) for p in corners], True)
        holes = [w for w, _ in wires]
    else:
        wires.sort(key=lambda t: -t[1])
        outer, holes = wires[0][0], [w for w, _ in wires[1:]]
    face = geompy.MakeFaceWires([outer] + holes, 1)
    geompy.addToStudy(face, "domain2D")
    groups, walls, prof = {}, [], set()
    for e, name in named:
        try:
            subs = geompy.SubShapeAll(geompy.GetInPlace(face, e), geompy.ShapeType["EDGE"])
        except Exception:
            subs = []
        for sub in subs:
            prof.add(geompy.GetSubShapeID(face, sub))
        groups.setdefault(name, []).extend(subs)
        if name in WALLS:
            walls.extend(subs)
    if EXTERNAL:
        lo, hi = tunnel()
        for e in geompy.SubShapeAll(face, geompy.ShapeType["EDGE"]):
            if geompy.GetSubShapeID(face, e) in prof:
                continue
            c = geompy.PointCoordinates(geompy.MakeCDG(e))
            groups.setdefault(tunnel_face_name(c, lo, hi, 1e-6 * L), []).append(e)
    geo = {}
    for name, subs in groups.items():
        if not subs:
            continue
        g = geompy.CreateGroup(face, geompy.ShapeType["EDGE"])
        geompy.UnionList(g, subs)
        geompy.addToStudyInFather(face, g, name)
        geo[name] = g
    quad = MESHER == "bodyfit"
    mesh = smesh.Mesh(face, "mesh")
    algo = mesh.Triangle(algo=smeshBuilder.NETGEN_1D2D)
    p = algo.Parameters()
    # as snappyHexMesh's far field (stlmesh.c mesher_far_levels): up to 8 base sizes, no larger than half the body, with
    # the base size kept round it and two lengths of its wake (mesher_near_box)
    far, near = far_field()
    p.SetMaxSize(far)
    f = size_file(near, (N2, W0))
    if f:
        try:
            p.SetMeshSizeFile(f)
        except Exception as e:
            say("this NETGEN has no mesh-size file (%s): no refinement round the body or in the volumetric controls" % e)
    p.SetMinSize(MIN)
    p.SetGrowthRate(max(0.05, min(1.0, VGROW - 1.0)))
    for setter, value in (("SetFineness", 5), ("SetUseSurfaceCurvature", True), ("SetNbSegPerRadius", max(1.0, CURV / (2 * math.pi))),
                          ("SetSecondOrder", False), ("SetOptimize", True), ("SetQuadAllowed", quad)):
        try:
            getattr(p, setter)(value)
        except Exception:
            pass
    for name, g in geo.items():
        if name in dict(BOUNDARIES):
            try:
                p.SetLocalSizeOnShape(g, TARGET)
            except Exception as e:
                say("no local size on %s (%s)" % (name, e))
    if PRISM and walls:
        try:
            t = layer_total()
            LAYERS.append(algo.ViscousLayers2D(t, NLAYERS, STRETCH, walls, False))
            say("boundary layers: %d, total thickness %g m, stretching %g, on %d edges" % (NLAYERS, t, STRETCH, len(walls)))
        except Exception as e:
            say("boundary layers could not be added (%s): meshing without them" % e)
    say("2-D: %d loop%s cut from the part, %s cells" % (len(loops), "" if len(loops) == 1 else "s", "mostly quadrilateral" if quad else "triangular"))
    return mesh, geo, DEPTH


def extrude_2d(mesh, depth):
    """the plane mesh one cell through the part's depth: the edge groups become the side patches, the plane's faces
    front2D and their copies back2D"""
    for name, g in list(GEO.items()):
        mesh.Group(g, name)
    front = mesh.MakeGroupByIds("front2D", SMESH.FACE, mesh.GetElementsByType(SMESH.FACE))
    vec = [0.0, 0.0, 0.0]
    vec[N2] = depth
    made = mesh.ExtrusionSweepObjects([], [mesh], [mesh], vec, 1, True)
    for g in mesh.GetGroups():
        n = g.GetName()
        if g.GetType() == SMESH.FACE and n.endswith("_extruded"):
            g.SetName(n[:-len("_extruded")])
        elif g.GetType() == SMESH.FACE and n == "front2D_top":
            g.SetName("back2D")
    for g in mesh.GetGroups():                               # only boundary faces in groups: the converter makes a patch of each
        if g.GetType() != SMESH.FACE:
            mesh.RemoveGroup(g)
    say("extruded one cell, %g m deep: %d cells" % (depth, mesh.NbVolumes()))


# ---- RUN ----------------------------------------------------------------------------------------------------------------
LAYERS = []                                                  # the viscous layers hypothesis, when there is one
LAYERED = []                                                 # grow_layers' (prisms, outer surface), when there are
GEO = {}
if TWO_D:
    mesh, GEO, DEPTH2D = mesh_2d()
    geo_groups = {}
elif REMESH:
    mesh, geo_groups = mesh_remeshed()
else:
    mesh, geo_groups = mesh_from_triangles(), {}
def compute_errors():
    try:
        return "; ".join("%s: %s" % (e.algoName, e.comment) for e in mesh.GetComputeErrors())
    except Exception:
        return ""


ok = mesh.Compute()
if not ok and [h for h in LAYERS if h]:
    # NETGEN's viscous layers hold on flat CAD faces; on a curved part made of STL facets they can crash it. A mesh
    # without them beats none.
    say("meshing with prism layers failed (%s): meshing again without them. For layers on this part use "
        "snappyHexMesh (Mesher)" % (compute_errors() or "no reason given"))
    for h in LAYERS:
        if h:
            mesh.RemoveHypothesis(h)
    mesh.Clear()
    ok = mesh.Compute()
if not ok and LAYERED and LAYERED[0]:
    say("NETGEN could not fill the tunnel round the grown prism layers (%s): meshing again without them" % (compute_errors() or "no reason given"))
    PRISM = False
    LAYERED.clear()
    mesh = mesh_from_triangles()
    ok = mesh.Compute()
if not ok and MESHER in ("shexa", "sprism"):
    raise SystemExit("Q-Continuum: %s could not mesh this part (%s): it needs %s. Pick another mesher." %
                     ("Structured hexahedra" if MESHER == "shexa" else "Prism sweeping", compute_errors() or "no reason given",
                      "blocks whose opposite faces match" if MESHER == "shexa" else "solids extruded between two matching faces"))
if not ok:
    err = compute_errors()
    raise SystemExit("Q-Continuum: SALOME could not mesh the STL%s. Check that it is closed (Mesh > STL mesher says), "
                     "try a smaller base size or minimum size%s." % ((": " + err) if err else "",
                     "" if REMESH else ", or switch Remesh surface on"))
for name, g in geo_groups.items():
    mesh.Group(g, name)
if TWO_D:
    extrude_2d(mesh, DEPTH2D)
if LAYERED and LAYERED[0]:
    # the layers and the tetrahedra beyond them, one mesh: their shared surface (qc_interface) is inside it, not a boundary
    mesh = smesh.Concatenate([LAYERED[0][0].GetMesh(), mesh.GetMesh()], 1, 1, 1e-9 * L, False, "mesh")
    for g in mesh.GetGroups():
        if g.GetName() == "qc_interface":
            mesh.RemoveElements(list(g.GetIDs()))
            mesh.RemoveGroup(g)
# only boundary faces may be in the groups: the converter makes a patch of each
for g in mesh.GetGroups():
    if g.GetType() == SMESH.FACE and g.Size() == 0:
        mesh.RemoveGroup(g)
say("mesh: %d nodes, %d volumes (%d tetrahedra, %d hexahedra, %d prisms, %d polyhedra)" %
    (mesh.NbNodes(), mesh.NbVolumes(), mesh.NbTetras(), mesh.NbHexas(), mesh.NbPrisms(), mesh.NbPolyhedrons()))


CELL_FACES = {                                               # SMDS's node order: tetra, pyramid, pentahedron, hexahedron
    4: ((0, 1, 2), (0, 1, 3), (1, 2, 3), (0, 2, 3)),
    5: ((0, 1, 2, 3), (0, 1, 4), (1, 2, 4), (2, 3, 4), (3, 0, 4)),
    6: ((0, 1, 2), (3, 4, 5), (0, 1, 4, 3), (1, 2, 5, 4), (2, 0, 3, 5)),
    8: ((0, 1, 2, 3), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)),
}


def write_polymesh(mesh, out):
    """the mesh as OpenFOAM's constant/polyMesh (ascii), for meshes UNV cannot hold: Body Fitting's cut cells are
    polyhedra. Each face group is a patch."""
    nodes = mesh.GetNodesId()
    pi = {n: i for i, n in enumerate(nodes)}
    pts = [mesh.GetNodeXYZ(n) for n in nodes]
    cells = mesh.GetElementsByType(SMESH.VOLUME)
    poly = set(mesh.GetIdsFromFilter(smesh.GetFilter(SMESH.VOLUME, SMESH.FT_ElemGeomType, SMESH.Geom_POLYHEDRA)))
    faces = {}                                               # sorted point ids -> [points, owner, neighbour]
    for ci, c in enumerate(cells):
        if c in poly:
            fl = [[pi[n] for n in mesh.GetElemFaceNodes(c, k)] for k in range(mesh.ElemNbFaces(c))]
        else:                                                # one call per cell: SMDS's node order gives the faces
            en = [pi[n] for n in mesh.GetElemNodes(c)]
            fl = [[en[k] for k in f] for f in CELL_FACES[len(en)]]
        cp = set(v for f in fl for v in f)
        cc = [sum(pts[v][k] for v in cp) / len(cp) for k in range(3)]
        for f in fl:
            key = tuple(sorted(f))
            if key in faces:
                faces[key][2] = ci
                continue
            # Newell's normal, turned out of this cell (its first one: the owner)
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
                patch_of[tuple(sorted(pi[n] for n in mesh.GetElemNodes(fid)))] = g.GetName()
    # boundary faces in no group (Body Fitting's cut faces can be): the tunnel's by position, the rest by the STL
    find, lo, hi, tol = None, None, None, 1e-6 * L
    if EXTERNAL:
        lo, hi = tunnel()
    inner, bound = [], {}
    for key, (f, o, n) in faces.items():
        if n < 0:
            name = patch_of.get(key)
            if name is None:
                fc = [sum(pts[v][k] for v in f) / len(f) for k in range(3)]
                if EXTERNAL and any(abs(fc[k] - lo[k]) < tol or abs(fc[k] - hi[k]) < tol for k in range(3)):
                    name = tunnel_face_name(fc, lo, hi, tol)
                else:
                    if find is None:
                        find = region_finder(smesh.Concatenate(stl_meshes(), 1, 1, 1e-7 * L, False, "stl"))
                    name = find(fc)
            bound.setdefault(name, []).append((f, o))
        else:
            inner.append((f, o, n) if o < n else (f[::-1], n, o))
    inner.sort(key=lambda t: (t[1], t[2]))
    names = [n for n in [b[0] for b in BOUNDARIES] + ["inlet", "outlet", "ground", "farfield"] if n in bound]
    names += sorted(n for n in bound if n not in names)
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


# Q-Continuum converts mesh.unv with ideasUnvToFoam; UNV has no polyhedra, so a mesh with them is written as polyMesh/
import shutil
for old in ("mesh.unv", "polyMesh"):
    old = os.path.join(HERE, old)
    shutil.rmtree(old) if os.path.isdir(old) else (os.remove(old) if os.path.exists(old) else None)
if mesh.NbPolyhedrons():
    out = os.path.join(HERE, "polyMesh")
    write_polymesh(mesh, out)
else:
    out = os.path.join(HERE, "mesh.unv")
    mesh.ExportUNV(out)
say("wrote", out)
if salome.sg.hasDesktop():
    salome.sg.updateObjBrowser()
