# Results > Visualization: the plan

**Read this first if you are a Claude session picking this work up.** The request (2026-11-21): "add a visualization tab to
the Results main tab, trying to have all the features that STAR-CCM+ 2606 has for visualization options. Detail a plan for
that and then implement all of it." The user approved implementing the whole plan in one go; keep the progress log at
the end current.

## What STAR-CCM+ offers, and what we build for each

STAR-CCM+ organises visualization as **scenes** holding **displayers** that show **parts** (boundaries, regions, or
*derived parts* such as section planes, iso-surfaces, streamlines and probes), with annotations, views, transforms,
animation and hardcopy around them. The table is the inventory we work to; each row names the module that does it.

| STAR-CCM+ (2606) | Here | Where |
|---|---|---|
| Scalar scene / Scalar displayer on boundaries | Boundary surfaces coloured by any field, per-patch visibility | `viz.c` boundary builder, `page_viz.c` |
| Part / Geometry displayer, Outline, Mesh (edges) overlay | Solid-colour surfaces, feature outline, mesh edges overlay per displayer | `viz.c`, `page_viz.c` |
| Contour styles: smooth filled, banded (N levels), cell filled (raw values), lines | Smooth (per-pixel colour from a 1-D colormap texture), banded (nearest-filtered N-level texture), cell values (flat per face), iso-lines on any coloured surface | `vizcolor.c`, `page_viz.c` |
| Colormaps (blue-red, cool-warm, viridis, magma, jet, spectrum, grayscale, ...), reverse | 12 presets, reverse | `vizcolor.c` |
| Range: auto (displayer / all / over time), manual, log scale, clip mode (clamp, transparent beyond range) | All four range modes, log, clamp or hide beyond range | `vizcolor.c`, `page_viz.c` |
| Scalar bar (colour legend): title, units, position, number format, ticks | Scalar bar per coloured displayer, title with units, placement, tick count, format | `page_viz.c` |
| Field functions: magnitude, components, user functions | Magnitude and x/y/z components of vectors; derived: pressure in Pa (kinematic p times rho), total pressure, velocity gradient magnitude, vorticity magnitude, Q-criterion, lambda-2-free helicity (u.omega), cell volume | `viz.c` (Gauss gradients) |
| Section plane, multiple / offset sections | Any number of planes (origin, normal, standard axes), each cut through every cell (polyhedra too) | `viz.c` cutter |
| Iso-surface (single or several values) | Iso-surfaces of any field, several values each | `viz.c` cutter (the same cell cutter on a point function) |
| Threshold part | Cells within a range shown as their outer faces | `viz.c` |
| Clipping (part displayed one side of a plane) | Clip plane on boundary surfaces | `viz.c`, `page_viz.c` |
| Vector displayer: glyphs (2-D / 3-D arrows, lines), scale by magnitude or uniform, spacing, tangential projection | Arrows (lines or 3-D), scaled or uniform, thinned on a screen-space grid, optional projection onto the surface, coloured by any field | `viz.c` glyphs |
| Vector displayer: LIC (line integral convolution) on planes and surfaces | LIC on section planes, modulated by the colour | `vizlic.c` |
| Streamlines: seeds from a point (with radius), a line, a plane grid, a part; forward / backward / both; max length; colour by field | All four seed kinds, both directions, step and length limits, RK4 through the cells | `viz.c` tracer |
| Streamline displayer: lines, tubes, ribbons; animated flow | Lines, tubes, ribbons; animated tracer dashes | `page_viz.c` |
| Volume rendering with a transfer function | Resampled volume drawn as view-aligned slice textures, opacity ramp | `viz.c` resampler, `page_viz.c` |
| Point probe, line probe with XY plot, histogram | Click probe (value, cell, position); line probe plot (CSV export); histogram of a field (volume-weighted) | `viz.c`, `page_viz.c` |
| Reports / statistics on parts | Min, max, volume-weighted mean of the field; area-weighted mean on a patch or section | `viz.c` |
| Views: standard (+X ... -Z, isometric), perspective / parallel, saved views, fit | All of them; saved views kept with the scene | `page_viz.c` (orbit.c camera) |
| Multiple scene windows, linked views | One or two side-by-side views, cameras linked or not | `page_viz.c` |
| Transforms: symmetry (mirror), rotational periodic copies | Mirror about x / y / z planes; N rotational copies about an axis | `page_viz.c` (draw-time transforms) |
| Lighting, transparency, background | Headlight on/off, per-displayer opacity, background presets | `page_viz.c` |
| Annotations: text, time, logo, report values | Title text, simulation time, a field statistic, the Q-Continuum mark | `page_viz.c` |
| Solution history / transient playback | Time slider, play / pause / loop, speed | `page_viz.c` |
| Scene animation, export movie | Orbit animation; frames for every time or orbit step to PNG and an mp4 with ffmpeg (as the existing Movie) | `page_viz.c` |
| Hardcopy (image export at a magnification) | Screenshot PNG at 1x / 2x / 4x | `page_viz.c` |
| Scene saved with the simulation | `visualization/scene.ini` in the case: displayers, view, saved views | `page_viz.c` |

**Not built, and why** (stated so nobody wonders): ray-traced and VR rendering (hardware and engines this app doesn't
have); particle-track displayers (no Lagrangian solvers wired into the app); overset / moving-mesh scene interpolation
(no such cases here); scene scripting with Java macros (the app's own scripting harness covers automation).

## The design

- **Core, `src/viz.c` / `viz.h` (no drawing, testable).** `VzMesh`: points, faces, owner / neighbour, boundary patches,
  face centres and area vectors, cell centres and volumes, cell-to-face and point-to-cell lists, typical cell size; read
  with the existing polyMesh readers in `xsection.c` (ASCII and binary, faceList and faceCompactList), now exported.
  `VzField`: a vol field's cell values (scalar or vector) and its boundary values per boundary face (from `value`
  entries: wall-only fields such as yPlus have nothing else), ASCII or binary; point values by averaging the cells
  (and, on boundaries, the boundary faces) round each point. Derived fields from Gauss gradients. Point location by
  walking from cell to cell across the face the point lies outside of; interpolation from the containing cell's points.
- **Builders** turn the mesh and a field into what is drawn: triangle sets with a value per vertex (boundary, sections,
  iso-surfaces, thresholds), line sets (iso-lines, streamlines, glyphs), images (LIC), voxel grids (volume). The section
  and iso-surface cutter is one routine: every cell cut by the zero set of a function given at the points (a plane's
  signed distance, or a field minus the iso value), each face's crossing turned into a segment and the segments of a cell
  chained into polygons.
- **Colour, `src/vizcolor.c`.** Colormap presets as control points; a 1-D texture per map (smooth: 256 texels, linear
  filter; banded: N texels, nearest), out-of-range texels for the clip modes. Vertices carry the normalised value as a
  texture coordinate, so the colour is looked up per pixel and bands stay crisp.
- **The page, `src/page_viz.c`.** Results gets two tabs: **Summary** (everything the page has today) and
  **Visualization**. Left: the scene's displayers with eye toggles, an Add menu and the selected displayer's settings.
  Top: time, view buttons, saved views, layout, transforms, screenshot, movie. Centre: one or two 3-D views (render
  textures, redrawn only on change), scalar bars, annotations, axis triad, probe readout. Bottom (when there are any):
  line-probe plot and histogram. Heavy work (loading a mesh, cutting a big mesh) runs on a worker thread.
- **Saved with the case**: `visualization/scene.ini`.

## Steps

| Step | What | State |
|---|---|---|
| V1 | Core: mesh, fields (cells and boundaries, ASCII and binary), point values, Gauss gradients and derived fields, point location and interpolation, statistics; tests on a blockMesh case with known linear and rotating fields | **done** |
| V2 | Builders: boundary surfaces, the cell cutter (sections, iso-surfaces), thresholds, clipping, iso-lines; tests (a section of a linear field is exact, an iso-surface of x is a flat plane of the box's area) | **done** |
| V3 | Colour maps, ranges, clip modes, scalar bars; the Results tabs and the Visualization page: scene list, boundary / section / iso / threshold displayers, views, picking probe, time | **done** |
| V4 | Vectors (glyphs, projection), streamlines (four seed kinds, lines / tubes / ribbons, animation), LIC | **done** |
| V5 | Volume rendering (resampled grid, slice textures, opacity ramp) | **done** |
| V6 | Line probe plot, histogram, statistics, annotations, transforms (mirror, periodic), saved views, two-view layout, lighting / background, screenshot, animation and movie, scene save / load | **done** |
| V7 | Journeys, the visual matrix, docs (a user guide in `docs/VISUALIZATION.md`), install | **done** |

## Progress log

| Date | Step | What happened |
|---|---|---|
| 2026-11-21 | (plan) | Plan written; the user approved implementing all of it |
| 2026-11-21 | V1, V2 | `src/viz.c` / `viz.h`: VzMesh (geometry, adjacency, a centre grid), VzField (cells, boundary faces, points), the readers in `xsection.c` exported (`fr_*`, and `fr_vol_field` for vector components and per-patch values, binary-safe), Gauss gradients and the derived fields, statistics and histogram, point location (walk, then nearest centre, then the neighbouring bins for concave boundaries), sampling, and every builder: boundary, the cell cutter (sections, iso-surfaces), thresholds, clipping, iso-lines, glyph points, seeds and RK4 streamlines, LIC, resampling, line probe, picking. Point values extrapolate each cell's (or boundary face's) value with its gradient: plain averaging was 0.125 off at the cube's edges for a linear field. `foamFormatConvert` writes dimensions as "[ 0 2 -2 ... ]": normalised. Tests: `viz_openfoam` (blockMesh cube, T = 2x + 3y - z + 1 and solid rotation: point values, gradients, vorticity 2 omega, Q omega^2, sections and iso-surfaces exact, streamlines circles within 3 %, ASCII and binary identical). On the user's 502 k-cell exhaust (binary, snappy): mesh 0.52 s, fields 0.25 s, a section 0.03 s, Q 0.12 s, 50 streamlines 0.05 s. |
| 2026-11-21 | V3-V7 | `src/vizcolor.c` (twelve maps, banded / smooth textures, ticks), `src/page_viz.c` (the page), Results gets Summary / Visualization tabs (`A.resultsTab`; the step strip and subtitle are left off the Visualization tab to give the view the height). All nine displayer kinds, every colour option, views, saved views, two views, mirror / periodic copies, probe, line-probe plot and CSV, histogram, overlays drawn into the image, screenshots, movies (`Scene movie` job), `visualization/scene.ini`. Found and fixed in the real app: an opaque boundary hid everything inside it (now made see-through when an inner displayer is added, with a toast); LIC z-fought with a section in the same plane (nudged toward the camera); duplicate scalar bars (one per field and range; new displayers default to the whole field's range so colours agree); the volume far too opaque (per-slice alpha from 1 - exp); the toolbar overflowed at 1.5 scale (wraps); the scene save waited for a frame that never came (the app draws on events) and a case switch dropped it (frames kept coming while a save is pending; saved before switching). Decomposed cases: times only in `processor*/` read through decomposePar's maps (`vz_mesh_procs`, `vz_field_load_procs`; ctest checks they equal the case's own). Builds run on a worker thread (old geometry drawn meanwhile, a spinner on the row; playback and movies wait for them): the worst frame on the test cube went from 971 ms to 169 ms. Journeys `t_viz`, `t_viz_scene`, `t_viz_parallel` (seeds `tests/make_cube3d.sh`), checked at 1440x900, 1280x800 and 1920x1080 at 1.5. The user's 502 k exhaust results had been cleared (only time 0 left), so it was timed through the engine only. |