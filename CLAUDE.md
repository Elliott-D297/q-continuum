# Q-Continuum

A desktop GUI for OpenFOAM v2606 in C and raylib (LCARS-styled), run on Ubuntu under WSL2.

**Ongoing work: the mesh system overhaul.** Before doing anything on meshing, teaching tools or OpenFOAM feature
gaps, read `docs/MESH_OVERHAUL_PLAN.md`. It holds the request, the rules, the steps with their state, the decisions
made and a progress log. Keep its progress log current. The user approves one step at a time.

- `docs/MESH_AUDIT.md`: what the mesh system has, what works, what is broken.
- `docs/FEATURE_GAPS.md`: gaps ranked for the learner and the expert, with effort.
- `FEATURES.md`: inventory of every user-visible feature and how it is tested (dated 2026-10-19; later work is in the
  audit).

Build in `build/` (`make -j8`), quick tests `ctest -L fast`, GUI journeys `tests/run_journey.sh`. The user's installed
copy is `~/.local/bin/q-continuum`. UI changes are checked in the real app at 1440x900 and at 1920x1080 with scale 1.5.
