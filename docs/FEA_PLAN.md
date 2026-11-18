# Part simulation in CalculiX (a SolidWorks Simulation-like flow)

Requested 2026-11-15: "work on being able to input stl and step files, as well as salome generated meshes into calculix in
order to simulate a result, and be able to put forces where you want. sort of like solidworks simulations".

## The flow

1. **New case > CalculiX > A part from a file**: STL, STEP, IGES, BREP, or a SALOME mesh (MED, UNV, a saved study .hdf).
   The project folder keeps a copy of the file and `study.ini` (the study: part, units, mesh size, material, fixtures,
   loads).
2. **Mesh** (a background job, SALOME without its window, `assets/salome/fea_mesh.py`):
   - CAD (STEP, IGES, BREP): NETGEN 1D-2D-3D on the exact geometry, second order, a face group per CAD face, so the user
     picks the real faces.
   - STL: the triangles remeshed at the target size (mmg, else NETGEN), NETGEN_3D inside, made quadratic; the faces are
     grouped by the angle between triangles (sharp edges split faces), in C.
   - A SALOME mesh: the mesh with the most volumes, its face groups kept (name them in SALOME: "fixed", "load" ...);
     linear tetrahedra made quadratic.
   - Written as `part.qcmesh` (plain text: nodes, volume elements, boundary faces with their group).
3. **Study page** (Setup for these projects): the part in 3-D (orbit, zoom), faces coloured as they are picked; a list
   of fixtures (fixed; held in x, y or z) and loads (a force: total N, along the face's normal or x, y, z or a vector;
   a pressure: MPa) each on the faces picked; the material (library); the mesh size (coarse / medium / fine).
4. **Write and run**: `study.inp` + `study_mesh.inp` in millimetres, newtons, MPa, tonnes/mm³. Forces as consistent
   nodal loads on the faces (6-node triangles: the corners take nothing, each mid-side a third). Fixtures as *BOUNDARY
   on the faces' nodes. Output: U, S, RF.
5. **Results** in 3-D: the deformed part coloured by von Mises stress or displacement, a colour bar, the largest
   value marked, the safety factor against yield; reactions against the loads (existing C2 panel).

## Steps

| Step | What | State |
|---|---|---|
| F1 | Core: `fea_mesh.py`; `femesh.c` (read, quadratic, orientation, boundary faces, face groups by angle); `festudy.c` (study.ini, .inp writer with consistent loads); tests against beam theory with ccx | done 2026-11-15 |
| F2 | New case entry, Study page: 3-D view, face picking, fixtures/loads/material editor, mesh job, run | done 2026-11-15 |
| F3 | 3-D results: deformed contour, colour bar, max marker, probe | done 2026-11-15 |
| F4 | Journeys, screenshots at 1920x1080/1.5 and 1440x900, install | done 2026-11-15 |

## Progress log

| Date | Step | Notes |
|---|---|---|
| 2026-11-15 | F1 | Done. `fea_mesh.py` (SALOME without its window): CAD (STEP, IGES, BREP; assemblies bonded by a partition) meshed by NETGEN 1D-2D-3D, second order, a group per CAD face; STL remeshed (MMG, else NETGEN) and filled by NETGEN_3D, made quadratic; a SALOME mesh (MED, UNV, study .hdf) with its face groups, linear tetrahedra made quadratic. `femesh.c`: reads it, turns inside-out elements, matches mid-side nodes to their edges by position, finds the boundary faces (outward), groups them (the file's groups, else by a 35 degree angle, largest first). `festudy.c`: study.ini (part, material, fixtures fixed/x/y/z, loads force along the normal or an axis / pressure, faces by name), a summary (what holds it, what is missing), and study.inp + study_mesh.inp in mm-N-MPa with consistent nodal loads (6-node triangles: mid-sides a third each). Beam 100 x 10 x 10 mm, 100 N: tip 0.1906 mm from BREP, 0.1910 from STL, 0.1906 from MED and UNV, against 0.1905 by hand. Tests: ctest `femesh` (fast; scrambled and inside-out tetrahedra, groups, study, ccx) and `fea_routes` (full; SALOME makes each kind of file). |
| 2026-11-15 | F2-F4 | Done and installed. **New case > CalculiX > A part from a file**: a file dialog (Windows' own under WSL) or a pasted path (C:\\... turned into /mnt/c/...), the unit for unitless files (STL, BREP, MED, UNV, .hdf), a name; the file is copied into the project with study.ini and meshing starts in the background. **Setup** (`page_fea.c`) for a project with study.ini: the part (size, element count), Coarse / Medium / Fine (a tenth, twentieth, fortieth of the part), Mesh again; the material from the library; fixtures (fixed, or held in x, y or z) and loads (a force in N along the face's normal or an axis, or a pressure in MPa), each with Pick faces: clicking a face in the 3-D view adds or removes it (a drag turns the part, right-drag moves it, the wheel zooms; the face under the mouse is named with its area). Faces are coloured by role (fixtures teal, loads orange), outlined where CAD faces meet, with arrows for forces. A line says what's missing (nothing holds it, free along x, no load) or what the loads come to; Run the study. Every run writes study.inp from study.ini first (`prog_start_run`). **Results**: the deformed part (largest movement drawn at 8 % of the part) coloured by von Mises stress or displacement, a colour bar, the largest value marked, the value under the mouse (stress, movement, node); the C2 panel (safety factor, reactions) below. Harness: `clickat <target> fx fy`. Journey `t_fea_part`: the flange STEP, the base rim fixed, 1000 N on the boss's top, run, results; checked at 1920x1080 / 1.5 and 1440x900. |
