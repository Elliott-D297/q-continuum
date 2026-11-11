# Q-Continuum: CAD (STEP, IGES, BREP) to STL, run by SALOME without its window
# ------------------------------------------------------------------------------
# Mesh > STL mesher runs this when the part chosen is a CAD file. It reads the file (SALOME converts its length unit to
# metres), fuses the solids of an assembly into one part, and writes
#   QC_CAD_OUT                the part's surface as an ASCII STL in metres, a solid a CAD face (face_<id>): what the
#                             mesher shows, groups into boundaries and meshes
#   QC_CAD_OUT minus .stl + .brep   the exact CAD in metres, which SALOME's CAD route (Remesh surface) meshes directly
# The facets follow the curvature: the largest gap between a facet and the surface (chord deviation) is QC_CAD_DEFL times
# the part's size (default 0.0003, about 0.02 mm on a 70 mm part), so a hole keeps its round shape at any mesh size.
import os
import sys
import salome
salome.salome_init()
import GEOM
from salome.geom import geomBuilder

g = geomBuilder.New()


def say(*a):
    print("Q-Continuum:", *a)
    sys.stdout.flush()


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


src = os.environ["QC_CAD_IN"]
out = os.environ["QC_CAD_OUT"]
defl = float(os.environ.get("QC_CAD_DEFL", "0.0003"))
ext = os.path.splitext(src)[1].lower()

# SALOME's STEP and IGES readers convert the file's own length unit to metres (the unit is read only to say what it was);
# BREP has no unit and is taken as metres (SALOME's own BREP files are)
try:
    if ext in (".step", ".stp"):
        shape = g.ImportSTEP(src)
        unit = g.GetSTEPUnit(src)
    elif ext in (".iges", ".igs"):
        shape = g.ImportIGES(src)
        unit = g.GetIGESUnit(src)
    elif ext in (".brep", ".brp"):
        shape = g.ImportBREP(src)
        unit = "none (BREP: metres assumed)"
    else:
        say("cannot read %s files: STEP (.step, .stp), IGES (.iges, .igs) or BREP (.brep)" % ext)
        sys.exit(2)
except Exception as e:
    say("%s is not a %s file SALOME can read (is it whole? saved from CAD as %s?)" % (os.path.basename(src), ext[1:].upper(), ext[1:].upper()))
    sys.exit(1)
if shape is None:
    say("%s is not a %s file SALOME can read (is it whole? saved from CAD as %s?)" % (os.path.basename(src), ext[1:].upper(), ext[1:].upper()))
    sys.exit(1)
unit = (unit or "").strip().lower() or "not given (metres)"

solids = g.SubShapeAll(shape, g.ShapeType["SOLID"])
faces = g.SubShapeAll(shape, g.ShapeType["FACE"])
if not faces:
    say("the file has no surfaces (only points or curves?)")
    sys.exit(1)
if not solids:
    # IGES (and some STEP) carry loose faces: sewn into closed shells, each made a solid; the tolerance grows from a
    # hundred-thousandth of the part's size until the shells close (CAD writes edges that miss by its own precision)
    x0, x1, y0, y1, z0, z1 = g.BoundingBox(shape)
    size = max(x1 - x0, y1 - y0, z1 - z0)
    for rel in (1e-5, 1e-4, 1e-3):
        try:
            sewn = g.MakeSewing(faces, rel * size) if len(faces) > 1 else shape
            made = [g.MakeSolid([sh]) for sh in g.SubShapeAll(sewn, g.ShapeType["SHELL"])]
            if made and all(g.BasicProperties(m)[2] > 0 for m in made):
                shape = made[0] if len(made) == 1 else g.MakeCompound(made)
                solids = made
                say("sewed the file's %d faces into %d solid(s) (gaps up to %.3g m closed)" % (len(faces), len(made), rel * size))
                break
        except Exception:
            pass
    if not solids:
        say("the file has surfaces but no closed solid: the mesher needs the part to be closed (watertight)")
if len(solids) > 1:
    try:
        shape = g.MakeFuseList(solids, True, True)       # an assembly: one part, touching faces removed
        say("fused the file's %d solids into one part" % len(solids))
    except Exception as e:
        say("could not fuse the file's %d solids (%s): kept as they are" % (len(solids), e))

x0, x1, y0, y1, z0, z1 = g.BoundingBox(shape)
size = max(x1 - x0, y1 - y0, z1 - z0)
write_faces_stl(g, shape, out, defl * size)              # a solid a CAD face, absolute chord deviation in metres
brep = os.path.splitext(out)[0] + ".brep"
g.ExportBREP(shape, brep)
nf = len(g.SubShapeAll(shape, g.ShapeType["FACE"]))
say("read %s: unit %s, %d solid(s), %d faces, %.4g x %.4g x %.4g m" % (os.path.basename(src), unit,
    len(solids), nf, x1 - x0, y1 - y0, z1 - z0))
say("wrote %s" % out)
