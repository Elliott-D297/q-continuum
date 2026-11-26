# Improvement plan (Part A) and CalculiX parity (Part B)

**Read this first if you are a Claude session picking this work up.** Single source of truth for the work asked on
2026-11-26: what was asked, the rules, the steps and their state, and a progress log at the end. Keep the log current.

## What was asked (2026-11-26)

1. "What are some other areas to improve on q-continuum? ... just make a plan" → the six areas of Part A.
2. "do all that: then I want you to put the same functionality into calculix's solver as you did into open foam. I want
   to be able to upload an stl file, and achieve full results from there" → Part B.

Decisions (asked 2026-11-26):

- **Push as each step lands**: fetch, merge the work branch into `main` (never force), push both.
- **CalculiX analyses: all of them**: linear and nonlinear static (large deflection, plasticity), modal and buckling,
  steady and transient heat with thermal stress, transient dynamics and frequency response.
- No step-by-step approval: the user asked for the whole plan in one go (unlike `MESH_OVERHAUL_PLAN.md`).

## Rules for the work

The rules of `MESH_OVERHAUL_PLAN.md` hold (C + raylib, existing style; no UI-thread blocking; round-trip edits; small
commits; GUI checks). In addition:

- Every UI change: `tests/run_visual_matrix.sh <journey> <out>` and the contact sheets looked at (1440x900 at 1.0 and
  1.5, 1920x1080 at 1.25, 2560x1440 at 1.5, 1280x800 at 1.0).
- CalculiX 2.21 (`/usr/bin/ccx`). **Check every keyword against the CalculiX 2.21 manual / its test decks before
  writing it**; each analysis type gets a hand-worked check (Given / Find / Solution) in `docs/FEA_GUIDE.md` and a ctest
  that runs ccx against it.
- SALOME 9.16 is the only mesher for parts (no Gmsh). Headless gotchas: memory `salome-scripting-gotchas`.
- Journeys are run one at a time; never rebuild `build/q-continuum` while journeys run.

## Part A: the improvement plan

| Step | Contents | Effort | State |
|---|---|---|---|
| A0 | Housekeeping: commit the run-controls work; merge into `main`, push; fix the Cases empty state (F-01); `FEATURES.md` refreshed (date, features since 2026-10-19, statuses from the journeys that exist); `FEATURE_GAPS.md` marked superseded; local junk (`*:Zone.Identifier`) removed | S | |
| A1 | Test suite: fix the journeys that fail (`j5_pause_resume`, `t_bh_startfrom`, `perf_bigLog`, `t_mesh_view_off`, `t_away`, `t_error_fix`); deflake (`t_bc_no_remesh`, `j1_first_case`, `t_mesh_import_salome`, `t_stl_salome_algos`); skip when a tool is missing (SPARTA, SALOME) instead of failing; GitHub Actions CI (build, `ctest -L fast`, cppcheck) | M | |
| A2 | Physics: guided conjugate heat transfer (`chtMultiRegionSimpleFoam` / `chtMultiRegionFoam`) in Physics > Setup from a mesh's cell zones (regions, materials, interfaces); starters with hand-solved answers for `scalarTransportFoam` and `laplacianFoam` | L | |
| A3 | Compare and report: compare 2+ cases (reports table, residuals and monitors overlaid, scenes side by side); export a report (Markdown + PNGs, PDF when a converter exists); reference / experimental CSV over line probes and monitors | M | |
| A4 | Run page: compact stats so the chart fits at 1440x900 / 1.5; a run queue (cases and study points one after another, time left); HPC handoff (SLURM / PBS script from Allrun) | M | |
| A5 | Code health: split the largest pages by tab (`page_stlmesh.c`, `page_setup.c`, `page_run.c`), `simsetup.c` by concern; fuzz seeds and cppcheck in CI | M | |

Left out unless asked again: particle-laden flow (`DPMFoam` / `MPPICFoam`) and moving / overset meshes (each L; the
plan called them "later, if wanted"). The CalculiX ideas of the old list (gravity, bearing loads, rollers, contact,
quality checks, face refinement, CFD pressure to FEA) moved into Part B.

## Part B: CalculiX with what OpenFOAM has (STL to full results)

Today (FEA_PLAN.md, 2026-11-15): STL / STEP / SALOME mesh → NETGEN C3D10 → pick faces → fixed / x / y / z supports,
force / pressure → linear static → deformed shape coloured by von Mises or displacement, safety factor, reactions.

What OpenFOAM has that a part study lacks, and its CalculiX counterpart:

| OpenFOAM side | CalculiX counterpart |
|---|---|
| Physics > Setup picks the solver from physics switches | Study type: static (linear / nonlinear), modal, buckling, heat (steady / transient), thermal stress, dynamics (transient / frequency response) |
| Boundary conditions matrix, roles | Supports (fixed, axis, roller / symmetry on any plane, prescribed displacement, pinned cylinder), loads (force, pressure, gravity, rotation, bearing, remote force / moment, torque), thermal (temperature, heat flux, convection, radiation, heat generation), amplitudes in time |
| Fluid presets, transport | Material library with yield, UTS, hardening, thermal and damping; custom materials |
| Mesh: quality tab and explainer, calculators, "What mesh do I need?", presets | Element order, local refinement on picked faces, quality (aspect ratio, Jacobian, angles) with explainer, elements-through-thickness advice, memory / time estimate |
| Pre-flight Sensor sweep | Rigid-body (free) motion found from the supports, missing loads, units, singularity warnings (sharp corners, point fixtures), yield check after |
| Run controls (time step, PIMPLE, linear solvers, writing) | Increments (initial / min / max, fixed / auto), NLGEOM, solver (SPOOLES / PaStiX / PARDISO), threads, output frequency, modes and frequency range, damping, time span |
| Live residuals, monitors, divergence watch | Live convergence from `.sta` / `.cvg` (residual force per iteration, cutbacks), stop, a too-many-cutbacks warning |
| Reports (forces, probes, surface values) | Reports: max stress / displacement / temperature, reactions per support, mass, frequencies and participation, buckling factors, probe histories |
| Results > Visualization (scenes, sections, isosurfaces, probes, line plots, movies) | The same page fed an FE mesh: displacement, von Mises, principal stresses, components, strain, plastic strain, temperature, heat flux, safety factor; deformation scale; mode-shape and time animation |
| Studies: parameter sweeps, GCI | Load / material / mesh sweeps, mesh convergence (GCI on max stress and displacement) |
| Glossary, show-the-working, starters that teach | FEA terms, hand checks (beam, Kt, Euler buckling, natural frequency, plane wall conduction), the existing starters |
| Allrun export, command-line mode | `run.sh` export, `q-continuum fea <project>` |
| (none) | One-way CFD → FEA: wall pressure / temperature from an OpenFOAM case onto the part's faces |

| Step | Contents | Effort | State |
|---|---|---|---|
| B1 | Study model v2: analysis type and step settings in `study.ini`; materials with yield, hardening, thermal, damping; supports and loads lists extended (all of the table above); `.inp` writer for every analysis, checked against ccx 2.21; hand checks per analysis as ctests | L | |
| B2 | Setup page: study type cards (layer 0 the question in engineering words), the new supports / loads / thermal editors with face picking, materials editor, amplitudes; multi-body STL (separate shells) bonded or in contact | L | |
| B3 | Mesh: element order, local refinement on picked faces (SALOME local size), quality report with explainer, elements-through-thickness advice, memory / time estimate | M | |
| B4 | Pre-flight: free motions from the supports, missing loads, units, singular points, nothing loaded thermally ... each with a fix button | M | |
| B5 | Run controls and live run: per analysis increments, NLGEOM, solver, threads, output; live convergence chart from `.sta` / `.cvg`, cutback watch, stop | M | |
| B6 | Results: FE mesh and fields into the Visualization engine (`VzMesh` from tetrahedra, faces grouped as patches; nodal fields from the `.frd`: U, S (von Mises, principal, components), E, PEEQ, NT, HFL, RF, safety factor); deformation scale; mode / time animation; reports panel; frequencies / buckling tables | L | |
| B7 | Studies: sweeps of load / material / mesh size, mesh convergence with GCI on chosen results | M | |
| B8 | Teaching and export: glossary terms, hand checks with the working, the guide (`docs/FEA_GUIDE.md`), `run.sh` export, `q-continuum fea <project>`, the report export of A3 for FEA | M | |
| B9 | CFD → FEA: map an OpenFOAM case's wall pressure (and temperature) onto picked faces as loads | M | |
| B10 | Verification: every analysis from an STL end to end in a journey; visual matrix; install; push | M | |

## Progress log

| Date | Step | Notes |
|---|---|---|
| 2026-11-26 | plan | Written. |
