# Meshing in Q-Continuum: a user's guide

For someone who knows fluid mechanics (Reynolds number, boundary layers, no-slip) and is new to CFD. Every page named
here also explains itself: dotted words open the Glossary (Ctrl+G), and "Show the working" opens the arithmetic.

## 1. Which way to mesh it

**Mesh > Which mesh?** asks what you are meshing, whether the flow varies in all three directions, what you need at the
walls, how good an answer you need, and the flow's speed and size. It names one way and says why, step by step:

| You have | The way | Where |
|---|---|---|
| A shape made of blocks: a cavity, a channel, a backward step, a flat plate, a cylinder, an airfoil, a pipe | blockMesh from a template: every cell a hexahedron lined up with the flow | Mesh > Block builder |
| A pipe or nozzle, the same all round its axis | one 5° wedge (axisymmetric) | Block builder, wedge template, or Extrude |
| A part drawn in CAD (STL, STEP) | snappyHexMesh (SALOME's triangles for a 2-D profile) | Mesh > STL mesher |
| A mesh made in Gmsh, SALOME, Fluent or STAR-CD | import | Mesh > SALOME script (Import a mesh) |

The button opens that page set up: the template with the first cell for your y+ and cell counts near the advised total,
or the mesher with prism layers on.

## 2. Worked example: a pipe with water at 2 m/s

1. A new case. **Mesh > Which mesh?**: A straight round pipe; 3-D; Pressure drop, lift, drag; An engineering answer;
   Water, 20 °C; 2 m/s; diameter 0.05 m.
2. The answer: *Block builder, "Pipe (O-grid)" template, graded to the walls*; Re 99,600 (turbulent); wall functions,
   y+ about 50, a first cell of 1 mm; about 0.6 million cells. Why each: O-grid so no cell is squeezed at the centre;
   wall functions because a pressure drop to a few per cent doesn't need the viscous sublayer resolved.
3. **Open the block builder with it**: the pipe template, 1.00 mm first cell, counts scaled to about 600 thousand cells.
4. **Write and mesh**: blockMesh, then checkMesh: Mesh OK (605,705 cells).
5. Mesh > Quality says what each checkMesh number means. Physics > Setup next.

(This is journey `tests/journeys/t_learner_pipe.qcs`, run with every test suite.)

## 3. Worked example: a part from CAD

1. **Mesh > STL mesher**, choose the file (STL, or STEP / IGES with SALOME). Geometry says whether it is closed.
2. Flow: through it or round it. The **point in the fluid** is worked out and checked (green in the view); for a part with
   more than one space inside, **Pick on the part** and click the inside face of the duct you mean.
3. Boundaries: name the inlets, outlets and walls (click faces in the view).
4. Mesh settings: the cell size (Coarse / Medium / Fine, or a base size), **prism layers** (the Calculators' *First layer
   from y+* sets them from your flow), the mesher, **Presets** to reuse settings on the next part.
5. Generate: surfaceFeatureExtract, blockMesh (the background), snappyHexMesh in two stages (castellate and snap, then the
   layers), checkMesh. **Redo the layers only** repeats just the layers after you change a layer setting. **Processes**
   meshes in parallel. Hand edits you make to the dictionaries in system/ are kept the next time it writes them.

## 4. Changing a mesh that is made

**Mesh > Tools**: rename a patch (in the mesh and every field file) or change its type; move, scale or turn it (a part
drawn in millimetres: scale 0.001); mirror half a symmetric geometry; refine everywhere; renumber (faster runs); a cell
zone from a box (porous media, fans, MRF); merge patches; baffles from a face zone; check an STL.

## 5. Is the mesh fine enough?

Results > **Is the mesh fine enough?** (or Studies > New study, Mesh refinement) makes copies on coarser and finer meshes.
When they have run, Studies shows **Richardson extrapolation and the Grid Convergence Index**: the value an infinitely fine
mesh would give, how far the finest one is from it, and a verdict. Docs: `CALCULATORS.md` section 8 works the published
example by hand (Celik et al. 2008: GCI 2.2 %).

## 6. Outside the app

**Run > Export Allrun** writes `Allrun` (the mesh exactly as the app last made it, then the run) and `Allclean`. Without the
window: `q-continuum allrun <case> [np]`, `q-continuum mesh <case>`, `q-continuum run <case> [np]`.

## Limits

- Cell counts in *Which mesh?* are starting points; the grid study is the answer.
- The STL mesher's parallel option and staged layers are for snappyHexMesh; SALOME and cfMesh mesh in one go.
- STAR-CCM+ `.ccm` files need `ccmToFoam`, which v2606 here is built without: export STAR-CD format.
- Mesh > Tools changes the mesh on disk; Generate mesh makes it again from its settings without them (Allrun replays them).
