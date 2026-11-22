# Results > Visualization

The 3-D results of an OpenFOAM case inside the app, after STAR-CCM+'s scenes: a **scene** holds **displayers**, each showing
one thing (the boundary, a plane, an iso-surface, streamlines ...) coloured by any field. The plan, the STAR-CCM+ feature
list it works to and the design are in `docs/VISUALIZATION_PLAN.md`.

Open it with **Results > Visualization**. The first time, the scene is the boundary coloured by the pressure.

## The page

- **Toolbar:** the time (step back, play / pause, step on), the standard views (+X ... -Z, Iso), Fit, saved Views, one or
  two views (Linked keeps their cameras together), Image (a PNG at 1x, 2x or 4x) and Movie (every time, or an orbit, as an
  mp4 by ffmpeg). Files go in `<case>/visualization/`.
- **Left:** the scene's displayers (the eye shows or hides one, the arrow moves it up, the bin deletes it; Add puts in a new
  one), the selected displayer's settings, then the scene's options.
- **The view:** drag to turn, Shift-drag or right-drag to pan, the wheel to zoom (after a click in the view), a double click
  to fit. A click without a drag is a **probe**: the value under the pointer, where it is, the cell and the patch.
  Scalar bars (with units), the time, a title and a statistic are drawn into the image, so screenshots and movies have them.

## Displayers

| Kind | What it shows | Its settings |
|---|---|---|
| Boundary surfaces | The patches | Which patches, mesh edges, a clip plane |
| Section plane | A cut through every cell | Through, normal (or Normal x / y / z), a position slider |
| Iso-surface | Where a field has a value (up to four values) | The field, the values (its range is shown) |
| Threshold | The cells with a field in a range, as their outer faces | The field, from / to |
| Streamlines | Paths along U (RK4 through the cells) | Seeds from a point (a ball), a line, a plane grid or a patch; forward, backward or both; max length; lines, tubes or ribbons; animated flow |
| Vectors | Arrows of U on a plane or the boundary | Lines or 3-D arrows, spacing, length (0: automatic), scaled by magnitude, in the plane only |
| Surface flow (LIC) | The flow's streaks on a plane, coloured | Resolution, square side |
| Volume rendering | A field through the whole volume, the high end opaque | Resolution, opacity |
| Line probe | A field along a line: the plot below the views and a CSV | From, to, samples |

Every coloured displayer has: **Colour by** (any field; a vector's magnitude or x / y / z; derived: pressure in Pa,
total pressure, velocity gradient magnitude, vorticity magnitude, Q-criterion, helicity, cell volume), **colour map** (twelve,
reversible), **style** (smooth, banded in N levels, cell values), **range** (this part, the whole field, or manual), log scale,
**beyond the range** clamp or hide, iso-lines, opacity.

Adding something that sits inside the flow makes an opaque boundary see-through (its Opacity puts it back).

## Reports, copies, sweeps

- A **section plane** reports what is on it under its settings: its area, the area-weighted mean of what it is coloured by, and the flow through it along its normal, as a volume and (for an incompressible case) a mass flow, with **Copy** for a spreadsheet.
- The copy button on a displayer's row makes a second one with the same settings, for example another plane, or the same streamlines coloured another way.
- **Views > Forget the saved views** clears the saved views. **Movie > sweep the plane** moves the selected plane from one side of the mesh to the other, one frame per position, and puts it back afterwards.
- The page follows a case that is run again: new times appear, rewritten fields and a remade mesh are read again.

## Units and pressure

An incompressible solver's `p` is kinematic (m²/s², pressure over density) and relative to whatever the outlet holds (often
0: gauge). The scene colours by **Pressure (Pa)** by default for such cases, `p` times the density from Physics > Setup
(1.204 kg/m³ if none is set). Plain `p` stays available, labelled "m²/s², kinematic".

## Scene options

Title; time, statistics and the Q-Continuum mark in the image; lighting; perspective or parallel; background (dark, light,
black); **mirror** copies about the x, y, z = 0 planes; **periodic** copies about an axis; the histogram (volume-weighted,
of the selected displayer's field) and line-probe plot under the views.

## Saved with the case

`visualization/scene.ini`: the displayers, cameras, saved views and options, written a moment after each change and read
when the case opens again.

## Parallel runs

A run left decomposed (times only in `processor*/`) shows as it is: the processors' fields are put back on the whole
mesh with decomposePar's maps (`cellProcAddressing`, `faceProcAddressing`), exactly what reconstructPar would write. If the
maps don't fit `constant/polyMesh` (a mesh made in parallel and never reconstructed), the page says to reconstruct.

## Not built

Ray tracing and VR, particle tracks, overset scenes, Java macros (see the plan for why).

## Tests

`ctest -R viz_openfoam` (the engine on a blockMesh cube with known fields: ASCII, binary and decomposed), the journeys
`t_viz`, `t_viz_scene`, `t_viz_parallel` (seeds made by `tests/make_cube3d.sh`).
