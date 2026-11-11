# Feature gaps and implementation plan

2026-11-10. Companion to `MESH_AUDIT.md`. Effort: **S** up to half a day, **M** one to two days, **L** three or more.
Effort figures are estimates, not measurements.

## Sweep of the whole app

| Area | Status | Notes |
|---|---|---|
| Case setup | Works | Nine physics templates, tutorial browser (every v2606 tutorial), blank case, controlDict on the Run page, units from dimensions, fluid presets, gravity, `turbulenceProperties` |
| Solver selection | Works by physics, not by name | Physics > Setup picks the solver from switches (steady or transient, gas, heat, free surface, rotating zones). It writes simpleFoam, pimpleFoam, buoyant*, rhoSimpleFoam / rhoPimpleFoam and interFoam cases. Not covered: rhoCentralFoam (Plume starter only), laplacianFoam, icoFoam (tutorial clones only) |
| Boundary conditions | Works | Patch-by-field matrix, roles (wall, inlet, outlet), consistency with the mesh's patch types, Sensor sweep before a run. *Not checked:* how cyclic and wedge patches are presented to a learner |
| Initial conditions | Works | setFields regions, mapFields, potentialFoam start (Setup's "Starting values") |
| Numerics | Works | Robust / Balanced / Accurate presets with explanations, PISO / SIMPLE / PIMPLE controls, relaxation, residual control; schemes follow the measured mesh quality |
| Run control | Works | Time step, Courant with explanation, adjustable step, parallel runs, live residuals and monitors, pause / stop / resume, divergence watch |
| Function objects | Partial | Written by Setup: forces, forceCoeffs, yPlus, probes, surfaceFieldValue, volFieldValue, fieldAverage, solverInfo, wallHeatFlux, MachNo. Missing: wallShearStress (outside the Plume starter), Q, vorticity |
| ParaView handoff | Works | `.foam` file, launch from WSL, movie maker |
| Dimensionless numbers | Partial | Re, Ma, Ra and Ri appear inline in Setup with regime words; Co before a run. No single panel; no Pr, Fr, We |

## Gaps ranked

### By value to the learner

| # | Gap | Why it matters | Effort |
|---|---|---|---|
| L1 | Glossary: any CFD term on the Mesh pages explained in fluid-mechanics words | Every other tool assumes vocabulary the learner lacks | M |
| L2 | y+ / wall-spacing calculator with worked steps, fluid presets including LN2, LH2, LOX | The most common first question; the number already exists but not the reasoning | M |
| L3 | blockMesh templates: channel, pipe (O-grid), backward step, cavity, flat plate, cylinder, airfoil C-mesh, wedge | Today a learner with "a pipe with water at 2 m/s" must draw CAD or edit text | L |
| L4 | Pre-flight: 2-D is one cell thick, wedge angle under 5 degrees, a warning for cases on `/mnt/c` | Silent, classic mistakes | S |
| L5 | Quality explainer: a picture, the effect on the solution and the fix per metric; failing cells to a cellSet and ParaView | The Quality tab shows numbers a learner cannot act on | M |
| L6 | "What mesh do I need?" questionnaire with reasoning | The one-line recommendation does not teach | S |
| L7 | Grading / expansion-ratio calculator | Needed for L3 and for layers | S |
| L8 | Richardson extrapolation and GCI in Studies | Turns the existing grid study into an answer | M |
| L9 | One dimensionless-numbers panel (Re, Ma, Pr, Ra, Fr, We, Co) with flags | Ties the setup to the course they took | S |
| L10 | Runtime and memory estimate before meshing, with a stop above the machine's memory | Protects a laptop from a 40 M-cell mesh | S |
| L11 | Say out loud when a SALOME tet / poly mesh has no layers, and why | Tonight's confusion | S |

### By value to the expert

| # | Gap | Why it matters | Effort |
|---|---|---|---|
| E1 | STL mesher keeps hand edits to its dictionaries (three-way merge, as Setup does) | Today's rewrite on Generate breaks the round-trip promise | M |
| E2 | blockMesh builder: vertices, blocks, counts, grading and multi-grading, arc / spline edges, patches, with a 3-D preview | The largest missing meshing path | L |
| E3 | snappy stages (castellate, snap, layers) re-runnable one at a time | Layer tuning costs a full remesh today (4 to 9 minutes on the intake) | M |
| E4 | Parallel meshing (decomposePar, `snappyHexMesh -parallel`, reconstructParMesh) | Large meshes | M |
| E5 | Export `Allrun` / `Allclean`; a shipped command-line mode (`q-continuum mesh <case>`) | HPC and reproducibility | M |
| E6 | Mesh tools panel: topoSet, createPatch, createBaffles, mirrorMesh, refineMesh, renumberMesh, transformPoints, surface tools | Everyday utilities with no UI | M to L |
| E7 | Fluent and STAR-CD import; rename patches after import | Common exchange formats | S |
| E8 | Pick `locationInMesh` by clicking, with an inside check | Multi-volume parts where the computed point is wrong | S |
| E9 | Function objects: wallShearStress, Q, vorticity | Standard post-processing | S |
| E10 | Saved mesh presets | Reuse across cases | S |

### Open defects (from `MESH_AUDIT.md`)

None left after plan step 0 (2026-11-10). Two findings turned out not to be defects and are explained in the app
instead: the snappy skewness "failure" (boundary faces only) and foamyHexMesh's low cell count (one surface size).

## Proposed plan

Order: defects and safety first, then the teaching layer on what exists, then the new meshing paths, then expert depth.
Each step ends with its tests and a GUI check at 1440x900 and 1920x1080 at 1.5 scale.

| Step | Contents | Gaps | Effort |
|---|---|---|---|
| 0 | Commit the existing work in logical commits; fix the `build/` cache; open defects | defects | M |
| 1 | Shared teaching pieces: glossary widget (hover / click any term), "show the working" panel (equations, steps, units, collapsed) | L1 | M |
| 2 | Calculators on those pieces: y+ with cryogens, grading, cells / memory / runtime; dimensionless-numbers panel | L2, L7, L9, L10 | M |
| 3 | Pre-flight additions and the layer notice | L4, L11 | S |
| 4 | Quality explainer and cellSet to ParaView | L5 | M |
| 5 | blockMesh: templates first (each a parametric generator with a preview), then the general builder | L3, E2 | L + L |
| 6 | Axisymmetric and general extrusion | part of L3 | M |
| 7 | "What mesh do I need?" over all paths, once they exist | L6 | S |
| 8 | Studies: Richardson and GCI | L8 | M |
| 9 | Expert depth: mesher three-way merge, staged snappy, parallel meshing, click-picked location | E1, E3, E4, E8 | L |
| 10 | Import, mesh tools panel, function objects, presets | E6, E7, E9, E10 | L |
| 11 | Allrun / Allclean export and the command-line mode | E5 | M |
| 12 | Verification list from the spec, user guide with worked examples, learner walk-through | | M |

Every dictionary keyword a step writes is checked against the v2606 tutorials and `$FOAM_ETC/caseDicts/annotated`
before it ships, and each calculator gets a hand-worked example in `docs/`.

## Decisions

Made on 2026-11-10 and recorded in `MESH_OVERHAUL_PLAN.md`, which is the live copy of this plan: commit the existing
work first; start with step 0 only; fold the earlier setup plan's step 10 into step 12.
