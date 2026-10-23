# Q-Continuum SALOME mesh script
# ------------------------------------------------------------------------------
# Runs without the SALOME window from the Mesh page (SALOME tab, "Run script and convert"), or open it in SALOME
# (File > Load Script) to see what it builds. It must write salome/mesh.unv; Q-Continuum then runs
# ideasUnvToFoam on it and checkMesh. Face groups become OpenFOAM patches with the same names.
#
# Edit the PARAMETERS and the GEOMETRY section; the rest rarely needs changes.
import os
import salome
salome.salome_init()
import GEOM
import SMESH
from salome.geom import geomBuilder
from salome.smesh import smeshBuilder

geompy = geomBuilder.New()
smesh = smeshBuilder.New()
# the folder of this script; SALOME's GUI (File > Load Script) runs it without __file__, from that folder
HERE = os.path.dirname(os.path.abspath(globals().get("__file__") or os.path.join(os.getcwd(), "mesh.py")))

# ---- PARAMETERS (SI units) ---------------------------------------------------
L, D = 0.6, 0.1              # main pipe length and diameter
d, l = 0.06, 0.3             # branch diameter and length (branch at mid-length, along +x)
MAX_SIZE = 0.012             # largest element
MIN_SIZE = 0.003             # smallest element (curved and small features)
FINENESS = 2                 # NETGEN: 0 very coarse ... 4 very fine

# ---- GEOMETRY ----------------------------------------------------------------
main = geompy.MakeCylinderRH(D / 2, L)                                              # along z, base at z = 0
branch = geompy.MakeCylinder(geompy.MakeVertex(0, 0, L / 2), geompy.MakeVectorDXDYDZ(1, 0, 0), d / 2, l)
shape = geompy.MakeFuseList([main, branch], True, True)
geompy.addToStudy(shape, "domain")

# ---- PATCHES: each face goes to the first rule that matches its centre (x, y, z) --
TOL = 1e-6
RULES = [
    ("inlet",  lambda x, y, z: abs(z) < TOL),
    ("outlet", lambda x, y, z: abs(z - L) < TOL),
    ("branch", lambda x, y, z: abs(x - l) < TOL),
    ("walls",  lambda x, y, z: True),
]

groups = {}
for face in geompy.SubShapeAllSortedCentres(shape, geompy.ShapeType["FACE"]):
    x, y, z = geompy.PointCoordinates(geompy.MakeCDG(face))
    for name, rule in RULES:
        if rule(x, y, z):
            groups.setdefault(name, []).append(face)
            break
geo_groups = {}
for name, faces in groups.items():
    g = geompy.CreateGroup(shape, geompy.ShapeType["FACE"])
    geompy.UnionList(g, faces)
    geompy.addToStudyInFather(shape, g, name)
    geo_groups[name] = g
    print("patch %-12s %d face(s)" % (name, len(faces)))

# ---- MESH (tetrahedra; NETGEN) -------------------------------------------------
mesh = smesh.Mesh(shape, "mesh")
algo = mesh.Tetrahedron(algo=smeshBuilder.NETGEN_1D2D3D)
p = algo.Parameters()
p.SetMaxSize(MAX_SIZE)
p.SetMinSize(MIN_SIZE)
p.SetFineness(FINENESS)
p.SetSecondOrder(0)
if not mesh.Compute():
    raise SystemExit("Q-Continuum: SALOME could not mesh the geometry: check the sizes and the shape")
for name, g in geo_groups.items():
    mesh.Group(g, name)
print("mesh: %d nodes, %d tetrahedra" % (mesh.NbNodes(), mesh.NbTetras()))

# ---- EXPORT ----------------------------------------------------------------------
mesh.ExportUNV(os.path.join(HERE, "mesh.unv"))                                      # read by ideasUnvToFoam
geompy.ExportSTL(shape, os.path.join(HERE, "mesh.stl"), True, MIN_SIZE / 4, True)   # the surface, e.g. for snappyHexMesh
print("wrote", os.path.join(HERE, "mesh.unv"))
