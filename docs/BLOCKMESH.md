# Block builder and extrusion

Mesh overhaul plan steps 5 and 6 (gaps L3, E2 and the axisymmetric part of L3). Mesh > **Block builder** is the fourth way
to make a mesh, beside Dictionaries, the STL mesher and a SALOME script.

Code: `src/bmesh.c` (the blockMesh model, its reader, writer, checks, preview geometry and the templates; no drawing),
`src/page_blocks.c` (the page), `src/extrude.c` (extrudeMesh dictionaries). Keywords checked against OpenFOAM v2606:
`$FOAM_ETC/caseDicts/annotated/blockMeshDict`, `.../extrudeMeshDict`, `src/mesh/blockMesh` and the `climbingRod` tutorial.

## Templates

Each makes a whole blockMeshDict from sizes a learner knows. Wherever there is a wall, the **first cell** at it can be
typed or taken from the y+ calculator (Calculators > Wall spacing > *Use in the block builder*). The grading is worked out
from it (`bm_ratio_for_first`: the expansion ratio that gives that first cell over the edge in its cells).

| Template | Blocks | What it makes |
|---|---|---|
| Lid-driven cavity | 1 | movingWall, fixedWalls, frontAndBack (empty); both directions graded toward the walls |
| Channel or duct | 1 | inlet, outlet, walls; 2-D (frontAndBack empty) or a 3-D duct with a width |
| Backward-facing step | 3 | inlet, outlet, upperWall, lowerWall, frontAndBack; counts follow the step height for square cells |
| Flat plate | 2 | inlet, outlet, top, plate (wall), symmetry ahead of it (symmetryPlane), frontAndBack |
| Pipe (O-grid) | 5 | inlet, outlet, wall; a square core and four arc-edged blocks, so no cell is squashed at the centre |
| Cylinder in crossflow | 12 | inlet, outlet, cylinder (wall), sides (symmetry: top and bottom are two planes), frontAndBack |
| Airfoil (C-mesh) | 6 | airfoil (wall), farfield, outlet, frontAndBack; a NACA four-digit section with a closed trailing edge |
| Axisymmetric pipe (wedge) | 1 | inlet, outlet, wall, front and back (wedge), axis (empty, collapsed) |

The airfoil splits each surface at 30 % chord: blockMesh can't hold two different curved edges between the same two
vertices, and the upper and lower surfaces both join the leading and trailing edges. Its wake blocks use edgeGrading: at
the trailing edge the cells across match the airfoil's, at the outlet they start at a 300th of the wake's last cell.
With simpleGrading, the outlet's cells reached an aspect ratio of 22 900 and failed checkMesh.

Every template meshes with blockMesh and passes checkMesh with its defaults (ctest `bmesh_openfoam`):

| Template | Cells | Max non-orthogonality | Max skewness |
|---|---|---|---|
| cavity | 400 | 0 | 0 |
| channel (2-D / 3-D) | 2000 / 40 000 | 0 | 0 |
| step | 4800 | 0 | 0 |
| plate | 6600 | 0 | 0 |
| pipe | 50 000 | 31.8 | 1.20 |
| cylinder | 9904 | 43.2 | 1.49 |
| airfoil | 16 800 | 63.4 | 0.69 |
| wedge | 2000 | 0 | 0.33 |

## The tables

Vertices, blocks (eight vertices, three cell counts, the grading: three values for simpleGrading or twelve for edgeGrading,
each a number or `((length cells ratio) ...)` for multi-grading, a zone name), curved edges (arc, spline, polyLine,
BSpline) and patches (name, type, faces). Every change is checked as blockMesh would (`bm_check`):
- an inside-out block (its vertices clockwise);
- cells that don't match across a face two blocks share;
- a patch face that is no block's face, or lies between two blocks, or is in two patches;
- outside faces in no patch (they go to defaultFaces);
- a curved edge that is no block's edge, two edges on one pair of vertices, an arc whose point is in line with its ends.

Select a row to light it up in the 3-D preview. The preview takes the mouse wheel only after you click it, so the page
scrolls past it.

**Saving keeps the rest of the file.** The tables put `vertices`, `blocks`, `edges`, `boundary` and `scale` (or
`convertToMeters`) back through foamdict. Comments, `mergePatchPairs`, `defaultPatch`, patch entries such as `inGroups`
or `neighbourPatch`, and any other keys stay as they were. A template replaces the whole file, keeping the old one as
`blockMeshDict.bak` (and the very first as `blockMeshDict.orig`), and records its settings in `mesher/blocks.ini`. If the
file changed on disk since the builder read it, the first Save only says so; the second writes over it. Unsaved table
edits are replaced by a template only on a second click. A cell whose text is too long to edit safely (a very long spline,
many multi-graded edges) shows read-only rather than writing back a cut-off copy.

**What the builder won't touch.** A blockMeshDict using `$macros`, `#eval` / `#calc` / `#include`, named vertices,
projected geometry or faces, the old `patches` form, or the origin / angle forms of arc shows read-only, with the reason
and a button to open it as text. Of the 454 tutorial blockMeshDicts in v2606, 286 can be read. Put back through the
builder, **all 286 mesh to identical points, faces and boundaries** (ctest `bmesh_openfoam`). Numbers are written with
the fewest digits that read back as the same double; 10 digits had moved 8 tutorials' points in the 10th digit.

## Extrude (step 6)

Under the blocks, for any meshed case:

- **Make it axisymmetric** turns a 2-D mesh (one cell thick, front and back on empty patches) into a wedge about an axis
  in its plane, 5 degrees in all by default. Steps: topoSet splits the empty faces into front (-normal) and back
  (+normal), createPatch makes them the patches `front` and `back`, extrudeMesh turns `front` about the axis
  (`constructFrom patch`, `exposedPatchName back`), then checkMesh.
- **Extrude a patch** adds cells beyond a patch (`constructFrom mesh`): straight out along the faces' normals, in one
  direction, or round an axis (sector), in layers with a growth ratio.

The mesh before is kept in `constant/polyMesh.before-extrude`. Once extrudeMesh has succeeded, the job records the
extrusion in the case's mesh recipe (`mesher/extrude.ini`; a failed one is not recorded), and every Generate mesh repeats
it until *Stop extruding*: blockMesh's, the STL mesher's and the SALOME import's. The recipe holds one extrusion; starting
another replaces it (the page says so). The wedge's patches are `front` and `back`, or `wedgeFront` / `wedgeBack` when the
case already has a patch with either name.

Found with v2606 and handled:
- extrudeMesh turns the patch along its *outward* normal. For the wedge the axis must be r × n (r from the axis toward the
  mesh, n the front patch's outward normal), else all 400 cells of the test channel came out inside out.
- `exposedPatchName` must name a patch that already exists.
- Cells on the axis close up into prisms by themselves, and the axis patch is left with no faces.
- Points written at the case's `writePrecision` (6 in most tutorials) leave the wedge faces "not planar" every time the
  mesh is loaded. The job raises `writePrecision` to 15 while extrudeMesh writes, then puts it back.
- A mesh that crosses the axis is refused, with the coordinates.

Tests: `ctest -R extrude` (unit), `extrude_openfoam`:
- 2-D channels above the x axis, below it and beside the y axis, each to "Mesh OK", wedge 2 × 2.5 degrees, no "not
  planar", prisms on the axis;
- crossing the axis refused;
- an outlet extruded 0.5 m in 10 layers.

Journeys: `t_blocks`, `t_blocks_gallery`, `t_extrude`.
