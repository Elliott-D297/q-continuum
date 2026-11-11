# Mesh system audit

2026-11-10, branch `setup-plan-and-bughunt`. Written from reading the source, the feature inventory (`FEATURES.md`),
the test suite, and one evening's bug run of every mesher on a real part (a 110 k-triangle intake manifold, internal
flow, 10 prism layers). Items marked *not checked* were found by search only and have not been exercised.

## Environment (verified on this machine)

| Item | Finding |
|---|---|
| OpenFOAM | v2606 (ESI), `/usr/lib/openfoam/openfoam2606`; `$FOAM_TUTORIALS` and `$FOAM_ETC/caseDicts/annotated` present |
| Mesh utilities present | blockMesh, snappyHexMesh, cartesianMesh, cartesian2DMesh, tetMesh, pMesh, foamyHexMesh, foamyQuadMesh, extrudeMesh, extrude2DMesh, checkMesh, gmshToFoam, fluentMeshToFoam, fluent3DMeshToFoam, ideasUnvToFoam, star4ToFoam, topoSet, createPatch, createBaffles, splitMeshRegions, transformPoints, mirrorMesh, refineMesh, renumberMesh, surfaceCheck, surfaceTransformPoints, surfaceFeatureExtract, polyDualMesh, mergeMeshes, stitchMesh, autoPatch, collapseEdges, refineWallLayer, snappyRefineMesh, decomposePar, reconstructParMesh, redistributePar |
| Missing | `ccmToFoam`, `makeAxialMesh`, a standalone `gmsh` binary (Gmsh is available inside SALOME 9.16) |
| Other tools | SALOME 9.16 (`~/salome`), ParaView 5.13 (`~/.local/bin/paraview`), mpirun |
| Turbulence dictionary | `constant/turbulenceProperties` (not `momentumTransport`) in v2606: already what the app writes |
| WSL | Workspace defaults to `~/OpenFOAM/<user>-v2606/run` (Linux filesystem). The app detects Windows drives (`pl_slow_fs`) and slows its own polling there, but **shows no warning** to a user who puts a case on `/mnt/c` |

## Architecture

- **Build:** CMake, C11, raylib vendored in `deps/`; one `qc_core` library (no UI) plus `page_*.c` UI files. About
  50 k lines in `src/`. Warnings are errors for pointer arithmetic and implicit declarations.
- **Screens:** immediate-mode. `app.c` draws the LCARS frame and calls one `page_*()` per page; widgets live in `ui.c`
  and `app_widgets.c` (`ui_button`, `ui_switch`, `row_num`, `prop_row`, `callout`, `note`, `ui_tooltip`, `dict_tree`).
  15 themes share the same widgets; `terms.c` renames features per theme.
- **Dictionaries:** `foamdict.c` keeps the file text and each entry's byte span; an edit splices text into the span and
  re-parses. Comments, `#include`, `$macros` and `#codeStream` are never rewritten. Undo, outside-change merge and the
  Files page are built on it.
- **Running tools:** `runner.c` jobs. Each step is a shell command run in a child with the OpenFOAM `bashrc` sourced,
  its output streamed to a `log.<tool>` file and the Run page's console; nothing blocks the UI thread. `explain.c`
  turns `FOAM FATAL ERROR` text into plain words with one-click fixes. Every step lands in the per-case command log
  (Run > Commands), which can be copied as a script.
- **Mesh system:** Mesh page with five tabs:
  1. *Dictionaries* (`page_case.c`): blockMeshDict scale, base size, per-block cell counts, patch types; a
     snappyHexMeshDict card; Generate (blockMesh / features / snappy / multi-region steps / checkMesh).
  2. *STL mesher* (`stlmesh.c`, `page_stlmesh.c`, `assets/salome/stl_mesher.py`): a five-step flow (Geometry, Flow,
     Boundaries, Mesh settings, Generate) from an STL, STEP or IGES part to a checked mesh, with 11 meshers.
     Settings live in `<case>/mesher/mesher.ini`; the app writes the dictionaries from them.
  3. *SALOME script* (`page_salome.c`): a Python template, and import of UNV / MED / SALOME study / Gmsh `.msh`.
  4. *View* (`page_meshview.c`, `xsection.c`): boundary surface in 3-D with patches coloured and listed, and a slice.
  5. *Quality* (`page_meshquality.c`): checkMesh figures as bars, SALOME quality controls, worst cells, export.
- **Tests:** 64 ctest entries (unit tests plus real OpenFOAM runs) and 154 GUI journeys that drive the real app
  through a script (`--script`), each in a sandbox.

## Spec against reality

Status: **Works** (exercised by a test or tonight's run), **Partial**, **Missing**.

### Meshing paths

| Path | Status | What is there | What is not |
|---|---|---|---|
| blockMesh | Partial | Scale, one base size that rescales every block, per-block counts, patch types, raw dictionary; a 1 m box for blank cases; an axisymmetric wedge written by the Plume starter | No block builder (vertices, blocks, edges, grading are text only); no multi-grading helper; no templates (channel, pipe O-grid, step, plate, cylinder, C-mesh, wedge) |
| snappyHexMesh | Works, with gaps | Background mesh sized from the part, STL / STEP / IGES import, feature extraction with the angle explained, refinement by percent of base size, box / sphere / cylinder zones, prism layers, internal and wind-tunnel domains, 2-D | `locationInMesh` is computed, not pickable; castellate / snap / layers always run as one step; no OBJ import; no parallel meshing |
| cfMesh | Works | cartesianMesh, tetMesh, pMesh, cartesian2DMesh | Layers on pMesh fold on a curved part (now retried with half the layers) |
| Extrusion / 2-D | Partial | 2-D from an extruded STL (slab, one cell, `empty` faces) | No general `extrudeMesh` UI; no axisymmetric (wedge) maker outside the Plume starter |
| Import | Partial | Gmsh `.msh`, UNV, MED, SALOME study; patch roles set afterwards; mm-vs-m check with "Scale to metres" | Fluent and STAR-CD converters not offered; patches can be retyped but not renamed |
| Manipulation | Mostly missing | topoSet (cell zones from Setup, multi-region), splitMeshRegions (multi-region cases), transformPoints (scaling), createPatch (internal) | No UI for createPatch, createBaffles, mirrorMesh, refineMesh, renumberMesh, general topoSet, surface tools |
| Other meshers (beyond the spec) | Works / experimental | SALOME NETGEN tet and poly, Gmsh, Body Fitting, structured hex, prism sweep; foamyHexMesh | From an STL the SALOME CAD meshers are slow and unproven on fine parts; the page now says to use a STEP file |

### Teaching and decision tools

| Tool | Status | What is there | What is not |
|---|---|---|---|
| 1. Wall spacing / y+ | Partial | Physics > Setup computes a first-cell height (Cf = 0.026 Re^-1/7) and sends layer settings to the mesher; the Run page reads the y+ the run achieved and advises | No standalone calculator; no worked steps; fluid presets are air, N2, CO2 and a few liquids, no cryogens |
| 2. Grading calculator | Missing | | |
| 3. Cells, memory, runtime | Partial | Cell estimate before meshing (within about 2x on 31 of 34 test meshes); memory, Courant and core advice before a run; ETA during one | No runtime estimate before meshing; no hard stop on an oversized mesh in the STL mesher |
| 4. Quality explainer | Partial | checkMesh parsed into bars with thresholds; worst cells located; laplacian schemes chosen from the measured non-orthogonality | No per-metric picture or "what it does to the solution"; no click-to-cellSet-to-ParaView |
| 5. Mesh independence | Partial | Studies page: grid-refinement variants, queued runs, comparison table and chart | No Richardson extrapolation, no GCI, no verdict |
| 6. "What mesh do I need?" | Partial | A rule recommends a mesher for the part, with "Use it" | No questionnaire, reasoning is one line |
| 7. Glossary | Missing | Tooltips, a plain "why" line per step and field descriptions exist | No term-by-term glossary |
| 8. Pre-flight checks | Partial | Patch / field consistency and constraint types (Sensor sweep); watertight check; unit guess on import | No check that a 2-D case is one cell thick or that a wedge angle is valid |

### Expert affordances

| Item | Status | Notes |
|---|---|---|
| Raw dictionary, lossless | Works for hand-edited cases; **Partial for the STL mesher** | The engine is lossless (every tutorial dictionary is parsed and re-saved in `ctest foamdict_tutorials`). But the STL mesher rewrites `snappyHexMeshDict`, `blockMeshDict` and `meshDict` on every Generate, so hand edits there are lost (the page warns). Physics > Setup already solves this with a three-way merge; the mesher does not use it |
| Show the command | Works | Run > Commands; copy as terminal commands or as a script |
| Export Allrun / Allclean | Missing | Copy to clipboard only |
| Headless / CLI | Missing for users | `test_stlmesh init / prep` does it for the tests; nothing is shipped |
| Presets, shortcuts | Partial | Density and numerics presets, full shortcut sheet; no user-saved mesh presets |
| Parallel meshing | Missing | |

### Visual feedback

Works: STL / CAD view with size overlays and face picking, mesh surface view with patches coloured and labelled,
slice plane, all in raylib and LCARS-styled. There is no preview of blockMesh blocks.

## Broken or half-built (found 2026-11-10 on the intake part)

| Finding | State |
|---|---|
| Mesh > View could not read cfMesh meshes (binary `faceList`) | Fixed, test added |
| snappy layers collapsed on walls with mixed cell sizes | Fixed: one surface size under the layers, on by itself where the estimate says it costs at most half again as many cells (the intake: +18 %); off on flat-sided parts, where it multiplied the cells (a cube in a tunnel: 0.5 to 4.2 million) |
| SALOME CAD-from-STL crashed ("unclosed shape") for four meshers | Crash fixed: parts over 20 000 triangles, or whose regions do not sew, are built from one combined STL (a face per triangle); small flat-faced parts keep the per-region route, which their boundary groups and layers need. Meshing on the 110 k-face result was not run to the end; by the user's decision the page tells people to use a STEP file and no more work goes into this route |
| cfMesh with 10 layers: 117 inside-out cells (pMesh) | Fixed: all three cfMesh meshers check their result and mesh again with half the layers |
| polyDualMesh crashes on the snappy mesh, inverts cells after cfMesh layers | Fixed: the mesh before is put back, with the reason |
| snappy mesh "fails" checkMesh on skewness | Explained, not a defect: all 18 faces are boundary faces of the inlet where the layers end (snappy allows 20 there, checkMesh 4). The app now reports it as a note (`case_mesh_verdict`) |
| foamyHexMesh makes 31 k cells where others make 0.6 to 3.5 M | A limit of the mesher as shipped: every v2606 tutorial uses one uniform surface size, and no curvature sizing keyword could be verified. The page now says so |
| SALOME tet / poly make no layers for internal flow from an STL | By design (NETGEN layers need CAD faces; verified in SALOME 9.16: the hypothesis is accepted and makes 0 prisms). Only the log says so: plan step 3 |
| Tests needing tutorials failed from `build/` | Fixed: CMake finds the packaged OpenFOAM |
| 82 files of uncommitted work | Committed 2026-11-10 |
