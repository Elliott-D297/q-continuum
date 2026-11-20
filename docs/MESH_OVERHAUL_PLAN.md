# Mesh system overhaul and OpenFOAM feature sweep: the plan

**Read this first if you are a Claude session picking this work up.** It is the single source of truth for the
overhaul: what was asked, what was decided, where it stands. Update the progress log at the end whenever a step
changes state. Background: `MESH_AUDIT.md` (what exists) and `FEATURE_GAPS.md` (gaps, ranked, with effort).

## What was asked (2026-11-10)

Turn Q-Continuum's mesh system into a complete, well-taught meshing environment, then fill the rest of the app's
OpenFOAM gaps. Two users at once:

- **The learner** has had undergraduate fluid mechanics (Reynolds number, boundary layers, no-slip, Moody chart) and no
  CFD. Every CFD term is explained in fluid-mechanics words, with a physical picture and a number.
- **The expert** is never slowed or blocked: anything possible by hand-editing `system/` is possible here, and the raw
  dictionary is one click away without losing work.

One interface with layers (progressive disclosure), never a beginner mode and an expert mode.

## Rules for the work

- C and raylib only, in the existing style, widgets and module layout. New dependencies need a stated reason.
- OpenFOAM v2606 (ESI) at `/usr/lib/openfoam/openfoam2606`. **Check every dictionary keyword against the installed
  tutorials and `$FOAM_ETC/caseDicts/annotated` before writing it**; do not trust memory of older releases.
- Never block the UI thread on a tool: use `runner.c` jobs; errors go through `explain.c`.
- Do not break existing cases or features; the old mesh path stays until the new one fully replaces it.
- Dictionaries the GUI writes must round-trip: keep unknown keys, comments, `#include`, `#calc`, macros.
- Commit in small, logical steps. (Approved 2026-11-10; before that nothing was committed without being asked.)
- Every UI change is checked in the real app at 1440x900 at scale 1.0 and 1920x1080 at scale 1.5 (the user's), with the
  screenshots looked at: `tests/run_journey.sh` with `QC_WINDOW` and `QC_UI_SCALE`.
- Each calculator gets a hand-worked example in `docs/` (Given / Find / Solution).
- **The user approves one step at a time.** Do the approved step, report, and wait.

## How to build, test and install

```
cd build && make -j8                      # app and tests (cmake cache must point QC_BASHRC / QC_TUTORIALS at v2606)
ctest -L fast                             # unit tests, seconds
ctest                                     # all, including real OpenFOAM and SALOME runs (long)
tests/run_journey.sh tests/journeys/<name>.qcs <outdir>     # one GUI journey in a sandbox
install -m755 build/q-continuum ~/.local/bin/               # the user's installed copy
```

Journey notes: run mesher suites and journeys one after another, not in parallel (under load they hit their 600 s limits); a
new journey needs its seed case in `SEEDS` in `tests/run_all_journeys.sh`; `assert text` sees only the start of a long
drawn line; `wait N` counts frames; `click stl-step#4` opens the mesher's Generate step; `QC_SEED_CASES=<dir>`
copies a case into the sandbox; a failed job never satisfies `wait-until job finished`.

## Steps

| Step | Contents | Gaps (see `FEATURE_GAPS.md`) | Effort | State |
|---|---|---|---|---|
| 0 | Commit the existing work; fix the `build/` cache; open defects from the 2026-11-10 bug run | defects | M | **done 2026-11-11** |
| 1 | Shared teaching pieces: glossary widget (hover or click any term), "show the working" panel (equations, steps, units, collapsed by default) | L1 | M | **done 2026-11-20** |
| 2 | Calculators: y+ / wall spacing with cryogens (LN2, LH2, LOX), grading / expansion ratio, cells / memory / runtime; one dimensionless-numbers panel (Re, Ma, Pr, Ra, Fr, We, Co) with flags | L2, L7, L9, L10 | M | not approved |
| 3 | Pre-flight: 2-D is one cell thick, wedge angle under 5 degrees, warning for cases on `/mnt/c`; say on the page when a SALOME tet / poly mesh has no layers | L4, L11 | S | not approved |
| 4 | Quality explainer: per metric a picture, its effect on the solution, the threshold, the fix; failing cells to a cellSet and ParaView | L5 | M | not approved |
| 5 | blockMesh: templates (channel, pipe O-grid, backward step, cavity, flat plate, cylinder, airfoil C-mesh, wedge), then the general builder (vertices, blocks, counts, grading, multi-grading, arc / spline / polyLine edges, patches, 3-D preview) | L3, E2 | L + L | not approved |
| 6 | Axisymmetric (wedge) and general `extrudeMesh` | part of L3 | M | not approved |
| 7 | "What mesh do I need?" questionnaire over all paths, reasoning shown | L6 | S | not approved |
| 8 | Studies: Richardson extrapolation, Grid Convergence Index (Roache), plain verdict | L8 | M | not approved |
| 9 | Expert depth: STL mesher keeps hand edits (three-way merge as Physics > Setup does), castellate / snap / layers re-runnable separately, parallel meshing, click-picked `locationInMesh` | E1, E3, E4, E8 | L | not approved |
| 10 | Import (Fluent, STAR-CD, patch renaming), mesh tools panel (topoSet, createPatch, createBaffles, mirrorMesh, refineMesh, renumberMesh, transformPoints, surface tools), function objects (wallShearStress, Q, vorticity), saved presets | E6, E7, E9, E10 | L | not approved |
| 11 | Export `Allrun` / `Allclean`; a shipped command-line mode | E5 | M | not approved |
| 12 | Verification and finish (below). Includes step 10 of the earlier setup plan ("physics check and finish"), folded in here on 2026-11-10 | | M | not approved |

### Step 12: what "verified" means

1. Clean build with warnings on.
2. A mesh made through the GUI's own code path, then `checkMesh`, for: lid-driven cavity and pitzDaily or a backward
   step (blockMesh), a simple STL in a box with layers (snappyHexMesh), one 2-D and one axisymmetric case, one imported
   Gmsh mesh. (No standalone `gmsh` is installed: make the `.msh` through SALOME or install it.)
3. A short solver run on at least three of those.
4. Round trip: stock v2606 `blockMeshDict` and `snappyHexMeshDict` files saved unchanged through the GUI, then `diff`.
5. Calculators against hand calculations, one worked example each in `docs/`.
6. Learner walk-through, "a pipe with water at 2 m/s" to a checked mesh, fixing every unexplained term.
7. A user guide for the mesh system with worked examples; a final summary (built, verified how, limits, next steps).

## Decisions made

| Date | Decision |
|---|---|
| 2026-11-10 | Commit the existing uncommitted work first, then small commits |
| 2026-11-10 | Start with step 0 only |
| 2026-11-20 | Step 1 approved ("start on step 1 right now") |
| 2026-11-10 | The earlier setup plan's step 10 is folded into step 12 |
| 2026-11-10 | SALOME from an STL: tell the user it works better from a STEP file, and leave it there (no further work on making CAD-from-STL fast) |
| 2026-11-13 | User asked directly (outside the step order): with Advanced checked, give access to every mesh setting OpenFOAM allows. Done as "Every OpenFOAM setting" (below); it is not plan step 9 and step 9 stays unapproved |

## Progress log

| Date | Step | What happened |
|---|---|---|
| 2026-11-10 | Phase 0, 1 | Audit and gap list written; plan presented and approved as above |
| 2026-11-10 | 0 | Started |
| 2026-11-11 | 0 | Done and installed. Existing work committed (4 commits); CMake finds the packaged OpenFOAM, so tutorial tests run from `build/`. Defects closed: checkMesh's boundary-only skewness is reported as a note (`case_mesh_verdict`, test `mesh_verdict`, journey `t_mesh_verdict`); foamyHexMesh's one surface size is stated on the page (no curvature keyword could be verified in v2606); all cfMesh meshers re-mesh with half the layers when cells invert. Two regressions from the bug-run fixes were caught by the suites and fixed: the SALOME per-region CAD route is kept for small parts (a box lost its boundary groups and layers), and "even surface cells" is automatic only where cheap (a cube went to 4.2 M cells). Verified: ctest fast 25/25, stlmesh_layers / openfoam / salome / smooth, salome_tool, salome_import; journeys t_stl_mesher, t_stl_to_run, t_stl_smooth, t_stl_2d, t_stl_cfmesh, t_stl_meshers, t_stl_salome_algos, t_mesh_quality, t_mesh_view, t_mesh_verdict, t_bc_no_remesh, t_forces_3d, t_tmpl_external. Not run: the full journey suite (154) and the full ctest set. **Next: step 1, when the user approves it.** |
| 2026-11-13 | (request) | **Every OpenFOAM setting**, under Mesh settings > Advanced in the STL mesher (user: "when I check that I want advanced meshing, give me access to literally every thing I could change in a mesh that OpenFOAM will allow"). `src/meshopts.c` holds a table of 340-odd keywords: snappyHexMeshDict (top level, castellation, each boundary's refinementSurfaces entry, each refinement region, snapping, layers and each boundary's layers, quality while meshing), meshQualityDict, the background blockMeshDict, surfaceFeatureExtractDict, the 2-D extrudeMeshDict, cfMesh's meshDict (sizes, workflow, each boundary, each region, layers), foamyHexMeshDict + collapseDict, foamyQuadMeshDict + extrude2DMeshDict, and the command-line options of polyDualMesh, checkMesh, surfaceFeatureEdges and every program Generate runs. Keywords checked against the v2606 source and `caseDicts/annotated`; cfMesh's against the strings in `libmeshLibrary.so` (no cfMesh source installed). Each row shows the value Q-Continuum writes (`mesher_preview`: the dictionaries made in memory), else OpenFOAM's default; a changed one goes to `<case>/mesher/advanced.ini` and Generate applies it in `put_dict` through foamdict.c (`mo_apply`), so the rest of each dictionary stays as written. "Any other keyword" takes any path (a whole sub-dictionary as `{ ... }`, `!remove` to leave an entry out). The values count in `mesh.sig` (checkMesh's options don't), so the step bar says the mesh is out of date. Search, "Only mine", per-boundary / per-region pickers, undo per row, "Clear all mine". SALOME meshers: the panel says only checkMesh / polyDualMesh apply. Tests: ctest `meshopts` (fast: table consistency, round trip, apply, signature) and `meshopts_openfoam` (full: snappy and cartesianMesh with every keyword that has a default set to it mesh and pass checkMesh, foamDictionary reads each back; a refinement level and maxCellSize change the cell count), journey `t_stl_advanced` (set a value, add a free keyword, Generate, checkMesh OK), visual matrix at all five sizes. |
| 2026-11-14 | (request) | **Meshing tutorial** (user: "an individual tutorial (separate from the 1st install tutorial) of how to mesh parts, what makes a good mesh, and a couple of examples to go through; for the 1st launch attach it to the back end of the beginning walkthrough"). `src/tour.c` now runs two tours from tables; the meshing tutorial is 24 steps: seven ideas (what a mesh is; cells where the flow changes and smooth growth; checkMesh's numbers with thresholds; prism layers and y+ with a worked flat-plate example; tunnel size; which mesher; mesh independence), then three example cases made by a button (`src/meshexamples.c`: a 0.5 m square duct, internal; a 0.1 m sphere in a short tunnel, external; a NACA 0012, 2-D across Z), each walked through the STL mesher's steps (the tour opens each step, scrolls the control it describes into view and lights it), the duct and sphere meshed with Try it, and a checklist at the end. On the first start it follows the tour (its last button reads "On to the meshing tutorial"); also Settings > Help > Meshing tutorial and the palette (`help.meshtour`, `help.tourfull`). The examples mesh with snappyHexMesh to Mesh OK: duct 70 k cells, sphere 85 k, airfoil 30 k, each under 10 s (ctest `mesh_examples`, label full). Journey `t_mesh_tutorial` (the hand-off from the tour, every step, duct and sphere generated), checked at 1920x1080 / 1440x900 at 1.5 and 1280x800. |
| 2026-11-20 | 1 | Done and installed (see `docs/TEACHING.md`). **Glossary:** `src/glossary.c` (core), 48 entries (mesh, cells and shapes, sizes and refinement, the boundary layer and y+, how snappyHexMesh works, meshers, patch types, quality), each with what it is in fluid-mechanics words, a picture, numbers to know, the OpenFOAM keywords (checked against v2606: primitiveMeshCheck.C thresholds, meshQualityDict, the annotated snappyHexMeshDict / surfaceFeatureExtractDict / blockMeshDict) and related terms. **Markup:** `[[key]]` / `[[key\|words]]` in any `ui_text_wrap` string (section descriptions, `prop_row` hints, notes, callouts): dotted underline, hover card (`ui_tooltip_titled`), click opens the **Glossary dialog** (`src/teach.c`, MD_GLOSSARY; search, See also, Back; action help.glossary, Ctrl+G). Marked so far: the STL mesher (cell size, prism layers, mesher descriptions, 2-D, tunnel, volumetric controls) and Mesh > Quality (checkMesh, SALOME report). **Show the working:** `src/working.c` (core: Given / Equations / Working / Result / Notes with units, `wk_text`) and `teach_working()` (collapsed toggle, panel, Copy); used under the STL mesher's cell size (`wk_surface_cell`: target to refinement level to cells along the part) and prism layers (`wk_layer_stack`: geometric series, first and outer layer, outer share of the surface cell). Tests: `test_glossary` (fast; table consistency, markup, every `[[term]]` in src/ resolves, both workings against the hand examples in TEACHING.md and against `mesher_surface_cell`), journey `t_glossary` (hover, working, click, See also, Back, search, Ctrl+G); harness gained `hover <target>`. Checked at 1440x900 / 1.0, 1920x1080 / 1.5 and in the classic dark theme. |
