# CAD parts the geometry operations are tested on (tests/salome_tool.sh), written as BREP into the current folder by SALOME:
#   block_pipe.brep   a 0.2 x 0.1 x 0.1 m block with a 0.03 m radius passage drilled through along x (fluid_inside: the
#                     passage, pi r^2 L = 5.65e-4 m^3)
#   loose.brep        the same block's faces, not sewn (sew)
#   plate_holes.brep  a 0.2 x 0.2 x 0.02 m plate with four 5 mm holes and one 20 mm hole (remove_holes under 10 mm: four)
#   two_boxes.brep    two overlapping boxes, not fused (fuse)
import os
import salome
salome.salome_init()
from salome.geom import geomBuilder
g = geomBuilder.New()
here = os.getcwd()
block = g.MakeBoxDXDYDZ(0.2, 0.1, 0.1)
pipe = g.MakeCylinder(g.MakeVertex(-0.01, 0.05, 0.05), g.MakeVectorDXDYDZ(1, 0, 0), 0.03, 0.22)
bp = g.MakeCut(block, pipe)
g.ExportBREP(bp, os.path.join(here, "block_pipe.brep"))
g.ExportBREP(g.MakeCompound(g.SubShapeAll(bp, g.ShapeType["FACE"])), os.path.join(here, "loose.brep"))
plate = g.MakeBoxDXDYDZ(0.2, 0.2, 0.02)
for x, y, r in ((0.03, 0.03, 0.005), (0.17, 0.03, 0.005), (0.03, 0.17, 0.005), (0.17, 0.17, 0.005), (0.1, 0.1, 0.02)):
    plate = g.MakeCut(plate, g.MakeCylinder(g.MakeVertex(x, y, -0.01), g.MakeVectorDXDYDZ(0, 0, 1), r, 0.04))
g.ExportBREP(plate, os.path.join(here, "plate_holes.brep"))
b2 = g.MakeTranslation(g.MakeBoxDXDYDZ(0.1, 0.1, 0.1), 0.05, 0.05, 0)
g.ExportBREP(g.MakeCompound([g.MakeBoxDXDYDZ(0.1, 0.1, 0.1), b2]), os.path.join(here, "two_boxes.brep"))
print("made block_pipe loose plate_holes two_boxes")
