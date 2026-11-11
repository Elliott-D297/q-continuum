# Q-Continuum's SALOME service: SALOME's geometry and mesh features without its window ("every mesher" plan, step 5)
# ------------------------------------------------------------------------------
# Run by Q-Continuum with SALOME's Python (salome -t): QC_CMD is a JSON file with the request, QC_OUT the JSON answer it
# writes. Every answer has "ok" (true/false) and, when false, "error" (a sentence for the user); "log" lists what was done.
#   {"cmd": "inspect", "cad": path}               the CAD's solids and faces: id, kind, area, centre, normal, neighbours,
#                                                  the neighbours it meets smoothly (under smooth_angle degrees, default 30)
#   {"cmd": "ops", "cad": path, "ops": [...], "out": stem}   geometry operations in order, the result as stem.brep + stem.stl
#   {"cmd": "faces_stl", "cad": path, "out": stl, "defl": rel}   the CAD's surface as an ASCII STL, a solid a face (face_<id>)
#   {"cmd": "quality", "mesh": path}               SALOME's quality controls on a mesh file (UNV, MED): histograms, the worst
#                                                  cells and where they are
#   {"cmd": "export", "mesh": path, "format": f, "out": path}   a mesh file to MED, UNV, CGNS, STL (its surface) or GMF
import json
import math
import os
import sys
import traceback

LOG = []


def say(*a):
    LOG.append(" ".join(str(x) for x in a))
    print("Q-Continuum:", *a)
    sys.stdout.flush()


def answer(d):
    d.setdefault("ok", True)
    d["log"] = LOG
    with open(os.environ["QC_OUT"], "w") as f:
        json.dump(d, f)


try:
    with open(os.environ["QC_CMD"]) as f:
        REQ = json.load(f)
except Exception as e:  # noqa: BLE001
    answer({"ok": False, "error": "the request could not be read (%s)" % e})
    sys.exit(1)

import salome
salome.salome_init()
import GEOM
import SMESH
from salome.geom import geomBuilder
from salome.smesh import smeshBuilder
geompy = geomBuilder.New()
smesh = smeshBuilder.New()


class Refused(Exception):
    """a request that can't be done, with the reason in words"""


def read_cad(path):
    ext = os.path.splitext(path)[1].lower()
    if not os.path.isfile(path):
        raise Refused("%s does not exist" % path)
    if ext in (".step", ".stp"):
        shape = geompy.ImportSTEP(path)
    elif ext in (".iges", ".igs"):
        shape = geompy.ImportIGES(path)
    elif ext in (".brep", ".brp"):
        shape = geompy.ImportBREP(path)
    else:
        raise Refused("%s is not a CAD file SALOME reads (STEP, IGES, BREP)" % os.path.basename(path))
    if shape is None:
        raise Refused("SALOME could not read %s" % os.path.basename(path))
    return shape


def vec(v):
    try:
        return list(geompy.VectorCoordinates(v))
    except Exception:  # noqa: BLE001
        a, b = geompy.SubShapeAll(v, geompy.ShapeType["VERTEX"])[:2]
        pa, pb = geompy.PointCoordinates(a), geompy.PointCoordinates(b)
        return [pb[k] - pa[k] for k in range(3)]


def unit(v):
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


def write_faces_stl(g, shape, out, defl):
    """the surface as an ASCII STL, a solid a CAD face (face_<id>, ids 1.. in SubShapeAll's order): Q-Continuum picks faces
    by these and groups them into boundaries (mesher/faces.ini). The triangles are the whole shape's tessellation (exported
    face by face, the faces don't meet exactly at their edges: 23 open edges on a 62-face intake); each is labelled with the
    face whose own tessellation has a triangle with the same centre, else the nearest such centre."""
    import re
    faces = g.SubShapeAll(shape, g.ShapeType["FACE"])
    tmp = out + ".tmp.stl"
    num = r"([-+0-9.eE]+)"
    pat = re.compile(r"vertex\s+" + num + r"\s+" + num + r"\s+" + num)

    def tris(path):
        with open(path) as t:
            v = [(float(a), float(b), float(c)) for a, b, c in pat.findall(t.read())]
        return [v[i:i + 3] for i in range(0, len(v) - 2, 3)]

    x0, x1, y0, y1, z0, z1 = g.BoundingBox(shape)
    q = max(x1 - x0, y1 - y0, z1 - z0) * 1e-6
    cen = lambda t: tuple(round(sum(p[k] for p in t) / 3 / q) for k in range(3))
    lookup, cents = {}, []
    for i, fc in enumerate(faces):
        g.ExportSTL(fc, tmp, True, defl, False)
        for t in tris(tmp):
            c = cen(t)
            lookup[c] = i + 1
            cents.append((c, i + 1))
    g.ExportSTL(shape, tmp, True, defl, False)           # the whole shape: one watertight tessellation
    whole = tris(tmp)
    if os.path.exists(tmp):
        os.remove(tmp)
    by_face = {}
    for t in whole:
        c = cen(t)
        f = lookup.get(c)
        if f is None:                                      # (a sliver the face's own export left out): the nearest centre
            f = min(cents, key=lambda e: (e[0][0] - c[0]) ** 2 + (e[0][1] - c[1]) ** 2 + (e[0][2] - c[2]) ** 2)[1]
        by_face.setdefault(f, []).append(t)
    with open(out, "w") as f:
        for fid in sorted(by_face):
            f.write("solid face_%d\n" % fid)
            for t in by_face[fid]:
                f.write(" facet normal 0 0 0\n  outer loop\n")
                for p in t:
                    f.write("   vertex %.9g %.9g %.9g\n" % p)
                f.write("  endloop\n endfacet\n")
            f.write("endsolid face_%d\n" % fid)
    return len(faces)


def faces_of(shape):
    return geompy.SubShapeAll(shape, geompy.ShapeType["FACE"])


def face_info(shape, smooth_angle=30.0):
    """each face: its id (1-based, SubShapeAll's order), kind, area, centre, normal; who it borders and how sharply"""
    faces = faces_of(shape)
    solids = geompy.SubShapeAll(shape, geompy.ShapeType["SOLID"]) or [shape]
    solid_of = {}
    for si, so in enumerate(solids):
        for f in faces_of(so):
            solid_of[geompy.GetSubShapeID(shape, f)] = si
    edge_faces = {}
    seam = set()                                              # a cylinder's seam: one face on both sides, not a free edge
    out = []
    for i, f in enumerate(faces):
        sid = geompy.GetSubShapeID(shape, f)
        props = geompy.BasicProperties(f)
        c = geompy.MakeCDG(f)
        cc = geompy.PointCoordinates(c)
        try:
            n = unit(vec(geompy.GetNormal(f, c)))
        except Exception:  # noqa: BLE001
            n = [0.0, 0.0, 0.0]
        kind = str(geompy.KindOfShape(f)[0]).split(".")[-1]
        for e in geompy.SubShapeAll(f, geompy.ShapeType["EDGE"]):
            eid = geompy.GetSubShapeID(shape, e)
            if i in edge_faces.get(eid, []):
                seam.add(eid)
            edge_faces.setdefault(eid, []).append(i)
        out.append({"id": i + 1, "sub": sid, "kind": kind, "area": props[1], "centre": list(cc), "normal": n,
                    "solid": solid_of.get(sid, 0), "neighbours": [], "smooth": []})
    edges = {geompy.GetSubShapeID(shape, e): e for e in geompy.SubShapeAll(shape, geompy.ShapeType["EDGE"])}
    cosA = math.cos(math.radians(smooth_angle))
    free = 0
    for eid, fl in edge_faces.items():
        fl = sorted(set(fl))
        if len(fl) < 2:
            if eid not in seam:
                free += 1
            continue
        a, b = fl[0], fl[1]
        if b + 1 not in out[a]["neighbours"]:
            out[a]["neighbours"].append(b + 1)
            out[b]["neighbours"].append(a + 1)
        try:                                                  # the bend across the edge, at its middle
            m = geompy.MakeVertexOnCurve(edges[eid], 0.5)
            na, nb = unit(vec(geompy.GetNormal(faces[a], m))), unit(vec(geompy.GetNormal(faces[b], m)))
            if sum(na[k] * nb[k] for k in range(3)) >= cosA and b + 1 not in out[a]["smooth"]:
                out[a]["smooth"].append(b + 1)
                out[b]["smooth"].append(a + 1)
        except Exception:  # noqa: BLE001
            pass
    return out, free


def cmd_inspect(r):
    shape = read_cad(r["cad"])
    faces, free = face_info(shape, float(r.get("smooth_angle", 30)))
    try:                                                      # SALOME's own word on gaps: the open wires of the free boundary
        ok, closed_w, open_w = geompy.GetFreeBoundary(shape)
        free = len(open_w) if ok else free
    except Exception:  # noqa: BLE001
        pass
    x0, x1, y0, y1, z0, z1 = geompy.BoundingBox(shape)
    props = geompy.BasicProperties(shape)
    solids = geompy.SubShapeAll(shape, geompy.ShapeType["SOLID"])
    say("%d solid(s), %d faces, %s" % (len(solids), len(faces), "closed" if free == 0 else "%d open boundaries (gaps)" % free))
    return {"solids": len(solids), "faces": faces, "free_edges": free, "closed": free == 0 and len(solids) > 0,
            "bbox": [x0, y0, z0, x1, y1, z1], "area": props[1], "volume": props[2]}


def cmd_faces_stl(r):
    """the CAD's surface as an ASCII STL with a solid a face, named face_<id>: Q-Continuum picks faces by these"""
    shape = read_cad(r["cad"])
    n = write_faces_stl(geompy, shape, r["out"], float(r.get("defl", 0.0003)) * size_of(shape))
    say("wrote %d faces, a solid each, to %s" % (n, os.path.basename(r["out"])))
    return {"faces": n}


def read_mesh(path):
    ext = os.path.splitext(path)[1].lower()
    if not os.path.isfile(path):
        raise Refused("%s does not exist" % path)
    if ext == ".unv":
        return smesh.CreateMeshesFromUNV(path)
    if ext in (".med", ".rmed"):
        ms, _ = smesh.CreateMeshesFromMED(path)
        if not ms:
            raise Refused("no mesh in %s" % os.path.basename(path))
        return max(ms, key=lambda m: m.NbVolumes())
    raise Refused("%s: a UNV or MED mesh is needed here" % os.path.basename(path))


QUALITY = [("aspect_ratio", "FT_AspectRatio3D", "aspect ratio (1 is a perfect cell)"),
           ("volume", "FT_Volume3D", "cell volume, m^3"),
           ("max_element_length", "FT_MaxElementLength3D", "longest edge or diagonal, m"),
           ("skew", "FT_Skew", "skewness of the boundary faces, degrees"),
           ("warping", "FT_Warping", "warping of the boundary faces, degrees")]


def cmd_quality(r):
    m = read_mesh(r["mesh"])
    out = {"cells": m.NbVolumes(), "faces": m.NbFaces(), "nodes": m.NbNodes(),
           "types": {"tetrahedra": m.NbTetras(), "hexahedra": m.NbHexas(), "prisms": m.NbPrisms(), "pyramids": m.NbPyramids(),
                     "polyhedra": m.NbPolyhedrons()}, "measures": []}
    for key, ft, words in QUALITY:
        try:
            f = smesh.GetFunctor(getattr(SMESH, ft))
            f.SetMesh(m.GetMesh())
            hist = f.GetHistogram(int(r.get("bins", 10)), False)
            lo, hi = m.GetMinMax(getattr(SMESH, ft))
            bins = [{"min": h.min, "max": h.max, "count": h.nbEvents} for h in hist]
            out["measures"].append({"key": key, "what": words, "min": lo, "max": hi, "bins": bins})
        except Exception as e:  # noqa: BLE001
            say("%s not measured: %s" % (key, e))
    # the worst cells by aspect ratio: where they are
    try:                                                      # (above the top bin's lower edge: few cells, the worst)
        ar = [x for x in out["measures"] if x["key"] == "aspect_ratio"]
        above = float(r.get("worst_above", ar[0]["bins"][-1]["min"] if ar and ar[0]["bins"] else 10))
        crit = smesh.GetCriterion(SMESH.VOLUME, SMESH.FT_AspectRatio3D, SMESH.FT_MoreThan, above)
        ids = list(m.GetIdsFromFilter(smesh.GetFilterFromCriteria([crit])))
        f = smesh.GetFunctor(SMESH.FT_AspectRatio3D)
        f.SetMesh(m.GetMesh())
        worst = sorted(((f.GetValue(i), i) for i in ids[:5000]), reverse=True)[:10]
        out["worst"] = [{"id": i, "aspect_ratio": v, "at": list(m.BaryCenter(i))} for v, i in worst]
        out["over_threshold"] = len(ids)
        out["worst_above"] = above
    except Exception as e:  # noqa: BLE001
        say("worst cells not found: %s" % e)
    say("%d cells measured" % out["cells"])
    return out


def cmd_export(r):
    m = read_mesh(r["mesh"])
    fmt, dst = r["format"].lower(), r["out"]
    if fmt == "med":
        m.ExportMED(dst)
    elif fmt == "unv":
        if m.NbPolyhedrons():
            raise Refused("UNV cannot hold the mesh's %d polyhedra: export MED or CGNS" % m.NbPolyhedrons())
        m.ExportUNV(dst)
    elif fmt == "cgns":
        m.ExportCGNS(dst, True)
    elif fmt == "stl":
        m.ExportSTL(dst, True)
    elif fmt in ("gmf", "mesh"):
        m.ExportGMF(dst)
    else:
        raise Refused("no export to %s: MED, UNV, CGNS, STL or GMF" % fmt)
    say("wrote %s (%s)" % (os.path.basename(dst), fmt.upper()))
    return {"out": dst}


# ---- geometry operations (step 6): each takes the shape and returns the new one, saying what it did ------------------
def size_of(shape):
    x0, x1, y0, y1, z0, z1 = geompy.BoundingBox(shape)
    return max(x1 - x0, y1 - y0, z1 - z0)


def nfaces(shape):
    return len(faces_of(shape))


def op_heal(shape, o):
    tol = float(o.get("tol", 1e-5)) * size_of(shape)
    ops = ["FixShape", "FixFaceSize", "DropSmallEdges", "SameParameter"]
    params = ["FixShape.Tolerance3d", "FixShape.MaxTolerance3d", "FixFaceSize.Tolerance", "DropSmallEdges.Tolerance3d",
              "SameParameter.Tolerance3d"]
    vals = [str(tol), str(10 * tol), str(tol), str(tol), str(tol)]
    out = geompy.ProcessShape(shape, ops, params, vals)
    say("healed: small edges and faces under %.3g m dropped, tolerances fixed" % tol)
    return out


def op_sew(shape, o):
    faces = faces_of(shape)
    size = size_of(shape)
    for rel in ([float(o["tol"])] if "tol" in o else [1e-5, 1e-4, 1e-3]):
        try:
            sewn = geompy.MakeSewing(faces, rel * size)
            made = [geompy.MakeSolid([sh]) for sh in geompy.SubShapeAll(sewn, geompy.ShapeType["SHELL"])]
            if made and all(geompy.BasicProperties(m)[2] > 0 for m in made):
                say("sewed %d faces into %d solid(s), gaps up to %.3g m closed" % (len(faces), len(made), rel * size))
                return made[0] if len(made) == 1 else geompy.MakeCompound(made)
        except Exception:  # noqa: BLE001
            pass
    raise Refused("the faces could not be sewn into a closed solid: there are gaps larger than a thousandth of the part")


def op_fuse(shape, o):
    solids = geompy.SubShapeAll(shape, geompy.ShapeType["SOLID"])
    if len(solids) < 2:
        say("one solid already: nothing to fuse")
        return shape
    out = geompy.RemoveExtraEdges(geompy.MakeFuseList(solids, True, True), True)
    say("fused %d solids into one" % len(solids))
    return out


def op_merge_faces(shape, o):
    n0 = nfaces(shape)
    out = geompy.UnionFaces(shape)
    say("merged faces that continue one another: %d faces from %d" % (nfaces(out), n0))
    return out


def suppress(shape, ids, what):
    if not ids:
        say("no %s to remove" % what)
        return shape
    faces = faces_of(shape)
    subs = [geompy.GetSubShapeID(shape, faces[i - 1]) for i in ids if 0 < i <= len(faces)]
    out = geompy.SuppressFaces(shape, subs)
    if out is None or geompy.BasicProperties(out)[2] <= 0:
        raise Refused("removing the %s left no closed solid: remove fewer at once" % what)
    say("removed %d %s: %d faces left" % (len(subs), what, nfaces(out)))
    return out


def op_remove_faces(shape, o):
    return suppress(shape, [int(i) for i in o.get("faces", [])], "faces picked")


def op_remove_small_faces(shape, o):
    total = geompy.BasicProperties(shape)[1]
    below = float(o.get("below", 0.001))                     # a share of the whole area
    ids = [i + 1 for i, f in enumerate(faces_of(shape)) if geompy.BasicProperties(f)[1] < below * total]
    return suppress(shape, ids, "faces under %g %% of the area" % (100 * below))


def op_remove_holes(shape, o):
    """round holes (cylindrical faces) under a radius, filled: a solid plug the hole's size fused in, the faces then merged
    (SALOME's SuppressHoles refuses through-holes)"""
    rmax = float(o.get("radius", 0))                         # metres
    v0 = geompy.BasicProperties(shape)[2]
    plugs = []
    for f in faces_of(shape):
        k = geompy.KindOfShape(f)                            # CYLINDER2D: centre (3), axis (3), radius, height
        if not (str(k[0]).endswith("CYLINDER2D") and len(k) > 8 and k[7] < rmax):
            continue
        c, ax, r, h = k[1:4], k[4:7], k[7], k[8]
        plugs.append(geompy.MakeCylinder(geompy.MakeVertex(*c), geompy.MakeVectorDXDYDZ(*ax), r, h))
    if not plugs:
        say("no round holes under %g m radius" % rmax)
        return shape
    out = geompy.UnionFaces(geompy.RemoveExtraEdges(geompy.MakeFuseList([shape] + plugs, True, True), True))
    v = geompy.BasicProperties(out)[2]
    if v <= v0:
        raise Refused("the plugs did not fill the holes (volume %.4g m^3, was %.4g)" % (v, v0))
    say("filled %d round holes under %g m radius: %d faces left" % (len(plugs), rmax, nfaces(out)))
    return out


def caps_for(wires):
    caps = []
    for w in wires:
        try:
            caps.append(geompy.MakeFaceWires([w], 1))           # flat
        except Exception:  # noqa: BLE001
            caps.append(geompy.MakeFilling(geompy.MakeCompound([w]), theMethod=GEOM.FOM_Default))   # curved: a filling
    return caps


def op_cap_openings(shape, o):
    ok, closed_w, open_w = geompy.GetFreeBoundary(shape)
    loops = list(closed_w or [])
    if not loops:
        say("no openings: the surface is closed already")
        return shape
    caps = caps_for(loops)
    sewn = geompy.MakeSewing(faces_of(shape) + caps, 1e-5 * size_of(shape))
    shells = geompy.SubShapeAll(sewn, geompy.ShapeType["SHELL"])
    out = geompy.MakeSolid(shells[:1]) if shells else None
    if out is None or geompy.BasicProperties(out)[2] <= 0:
        raise Refused("the %d openings were capped but the surface still does not close" % len(loops))
    say("capped %d openings: one closed solid" % len(loops))
    return out


def op_fluid_inside(shape, o):
    """the passage inside a solid part (an intake, a manifold, a valve body): its openings are holes in the part's end
    faces; each hole is capped and the faces reachable from the holes without crossing an end face, with the caps,
    make the fluid's solid"""
    faces = faces_of(shape)
    info, _ = face_info(shape, 30.0)
    holes, ends = [], set()
    for i, f in enumerate(faces):
        wires = geompy.SubShapeAll(f, geompy.ShapeType["WIRE"])
        if len(wires) < 2:
            continue
        # the outer wire bounds the largest area: the others are holes (the openings)
        areas = []
        for w in wires:
            try:
                areas.append(geompy.BasicProperties(geompy.MakeFaceWires([w], 1))[1])
            except Exception:  # noqa: BLE001
                areas.append(0.0)
        outer = max(range(len(wires)), key=lambda k: areas[k])
        for k, w in enumerate(wires):
            if k != outer:
                holes.append((i, w))
        ends.add(i)
    if not holes:
        raise Refused("no openings found: the part has no face with a hole in it (an inlet or outlet port). If it is the fluid "
                      "already, skip this; else cap its openings first")
    # the faces touching a hole's edges, then everything reachable from them without crossing an end face
    hole_edges = set()
    for _, w in holes:
        for e in geompy.SubShapeAll(w, geompy.ShapeType["EDGE"]):
            hole_edges.add(geompy.GetSubShapeID(shape, e))
    seed = set()
    for i, f in enumerate(faces):
        if i in ends:
            continue
        if any(geompy.GetSubShapeID(shape, e) in hole_edges for e in geompy.SubShapeAll(f, geompy.ShapeType["EDGE"])):
            seed.add(i)
    inner, todo = set(seed), list(seed)
    while todo:
        i = todo.pop()
        for nb in info[i]["neighbours"]:
            j = nb - 1
            if j not in inner and j not in ends:
                inner.add(j)
                todo.append(j)
    caps = caps_for([w for _, w in holes])
    try:
        shell = geompy.MakeSewing([faces[i] for i in sorted(inner)] + caps, 1e-5 * size_of(shape))
        out = geompy.MakeSolid(geompy.SubShapeAll(shell, geompy.ShapeType["SHELL"])[:1])
    except Exception as e:  # noqa: BLE001
        raise Refused("the passage's %d faces and %d caps did not close into a solid (%s)" % (len(inner), len(caps), e))
    v = geompy.BasicProperties(out)[2]
    if v <= 0 or v >= geompy.BasicProperties(shape)[2] * 50:
        raise Refused("the passage did not close into a sensible solid (volume %.3g m^3)" % v)
    say("fluid volume inside the part: %d passage faces and %d openings capped, %.4g m^3" % (len(inner), len(caps), v))
    return out


OPS = {"heal": op_heal, "sew": op_sew, "fuse": op_fuse, "merge_faces": op_merge_faces, "remove_faces": op_remove_faces,
       "remove_small_faces": op_remove_small_faces, "remove_holes": op_remove_holes, "cap_openings": op_cap_openings,
       "fluid_inside": op_fluid_inside}


def cmd_ops(r):
    shape = read_cad(r["cad"])
    n0, v0 = nfaces(shape), geompy.BasicProperties(shape)[2]
    for k, o in enumerate(r.get("ops", [])):
        fn = OPS.get(o.get("op"))
        if not fn:
            raise Refused("operation %d: unknown %r (%s)" % (k + 1, o.get("op"), ", ".join(sorted(OPS))))
        try:
            shape = fn(shape, o)
        except Refused as e:
            raise Refused("operation %d (%s): %s" % (k + 1, o.get("op"), e))
    stem = r["out"]
    geompy.ExportBREP(shape, stem + ".brep")
    write_faces_stl(geompy, shape, stem + ".stl", float(r.get("defl", 0.0003)) * size_of(shape))
    props = geompy.BasicProperties(shape)
    ok, closed_w, open_w = geompy.GetFreeBoundary(shape)
    say("result: %d faces (was %d), volume %.4g m^3 (was %.4g)" % (nfaces(shape), n0, props[2], v0))
    return {"brep": stem + ".brep", "stl": stem + ".stl", "faces": nfaces(shape), "faces_before": n0, "volume": props[2],
            "volume_before": v0, "closed": ok and not open_w}


CMDS = {"inspect": cmd_inspect, "faces_stl": cmd_faces_stl, "quality": cmd_quality, "export": cmd_export, "ops": cmd_ops}
try:
    fn = CMDS.get(REQ.get("cmd"))
    if not fn:
        raise Refused("unknown command %r (%s)" % (REQ.get("cmd"), ", ".join(sorted(CMDS))))
    answer(fn(REQ))
except Refused as e:
    answer({"ok": False, "error": str(e)})
except Exception as e:  # noqa: BLE001
    traceback.print_exc()
    answer({"ok": False, "error": "SALOME stopped: %s" % e})
