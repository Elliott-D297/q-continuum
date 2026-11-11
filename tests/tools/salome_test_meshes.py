# Meshes the import has to cope with (tests/salome_import.sh), written into the current folder by SALOME:
#   poly.med   a hex and a polyhedron (UNV can't hold polyhedra), groups inlet and walls, one face in no group
#   quad.med   second-order NETGEN tetrahedra of a cylinder, a group per face, one named with a space ("side wall")
#   quad.unv   the same as UNV (ideasUnvToFoam can't read second-order elements)
#   study.hdf  a saved study with the cylinder's geometry and quad's mesh in it
import os
import salome
salome.salome_init()
import GEOM
import SMESH
from salome.geom import geomBuilder
from salome.smesh import smeshBuilder
geompy = geomBuilder.New()
smesh = smeshBuilder.New()
here = os.getcwd()

m = smesh.Mesh()
m.SetName("poly")
n = {}
for i in range(3):
    for j in range(2):
        for k in range(2):
            n[i, j, k] = m.AddNode(0.1 * i, 0.1 * j, 0.1 * k)
def hexn(i):
    return [n[i, 0, 0], n[i + 1, 0, 0], n[i + 1, 1, 0], n[i, 1, 0], n[i, 0, 1], n[i + 1, 0, 1], n[i + 1, 1, 1], n[i, 1, 1]]
m.AddVolume(hexn(0))
h = hexn(1)
fs = [[h[0], h[1], h[2], h[3]], [h[4], h[7], h[6], h[5]], [h[0], h[4], h[5], h[1]], [h[1], h[5], h[6], h[2]], [h[2], h[6], h[7], h[3]], [h[3], h[7], h[4], h[0]]]
m.AddPolyhedralVolume(sum(fs, []), [len(f) for f in fs])
m.MakeBoundaryElements(SMESH.BND_2DFROM3D, "skin")
skin = [g for g in m.GetGroups() if g.GetName() == "skin"][0]
ids = skin.GetIDs()
at = lambda f, x: all(abs(m.GetNodeXYZ(v)[0] - x) < 1e-9 for v in m.GetElemNodes(f))
m.MakeGroupByIds("inlet", SMESH.FACE, [f for f in ids if at(f, 0)])
m.MakeGroupByIds("walls", SMESH.FACE, [f for f in ids if not at(f, 0) and not at(f, 0.2)])
m.RemoveGroup(skin)
m.ExportMED(os.path.join(here, "poly.med"))

cyl = geompy.MakeCylinderRH(0.05, 0.2)
geompy.addToStudy(cyl, "cylinder")
faces = geompy.SubShapeAllSortedCentres(cyl, geompy.ShapeType["FACE"])
q = smesh.Mesh(cyl, "quad")
p = q.Tetrahedron(algo=smeshBuilder.NETGEN_1D2D3D).Parameters()
p.SetMaxSize(0.02)
p.SetSecondOrder(1)
q.Compute()
for i, f in enumerate(faces):
    q.GroupOnGeom(f, "side wall" if 0 < i < len(faces) - 1 else "end_%d" % i, SMESH.FACE)
q.ExportMED(os.path.join(here, "quad.med"))
q.ExportUNV(os.path.join(here, "quad.unv"))
salome.myStudy.SaveAs(os.path.join(here, "study.hdf"), False, False)
print("made poly.med quad.med quad.unv study.hdf: %d + %d volumes" % (m.NbVolumes(), q.NbVolumes()))
