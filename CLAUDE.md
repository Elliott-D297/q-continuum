# Q-Continuum

A desktop GUI for OpenFOAM v2606 in C and raylib (LCARS-styled), run on Ubuntu under WSL2.

**Ongoing work (2026-11-26): the improvement plan and CalculiX parity.** Read `docs/IMPROVEMENT_PLAN.md` first: the
request, decisions (push as each step lands; all CalculiX analysis types), steps with their state and a progress log.
Keep its log current. The whole plan was asked for at once (no per-step approval).

**Done: the mesh system overhaul** (`docs/MESH_OVERHAUL_PLAN.md`, steps 0-12, 2026-11-22). Read it before meshing,
teaching-tool or OpenFOAM feature-gap work.

**Planned next: the workflow review** (OpenFOAM, CalculiX, DEME; learner ease with progressive disclosure). Read
`docs/WORKFLOW_PLAN.md` before workflow, Setup, Results or program-page work; all its steps are done (2026-11-14).

**CalculiX part studies** (STL, STEP, SALOME meshes; faces picked in 3-D for fixtures and loads): read `docs/FEA_PLAN.md`
before touching `page_fea.c`, `femesh.c`, `festudy.c` or `assets/salome/fea_mesh.py`.

- `docs/VISUALIZATION.md` (guide) and `docs/VISUALIZATION_PLAN.md` (plan, log): Results > Visualization, `viz.c`, `vizcolor.c`, `page_viz.c`.
- `docs/BLOCKMESH.md`: the block builder, its templates and extrusion (plan steps 5-6): what it reads and refuses, v2606 facts.
- `docs/CALCULATORS.md`: the calculators, pre-flight checks and quality explainer (plan steps 2-4): worked examples, calibration.
- `docs/MESH_AUDIT.md`: what the mesh system has, what works, what is broken.
- `docs/FEATURE_GAPS.md`: gaps ranked for the learner and the expert, with effort.
- `FEATURES.md`: inventory of every user-visible feature and how it is tested (dated 2026-10-19; later work is in the
  audit).

Build in `build/` (`make -j8`), quick tests `ctest -L fast`, GUI journeys `tests/run_journey.sh`. The user's installed
copy is `~/.local/bin/q-continuum`. UI changes are checked in the real app at 1440x900 and at 1920x1080 with scale 1.5.
