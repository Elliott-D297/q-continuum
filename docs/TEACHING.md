# Teaching pieces: the glossary and "show the working"

Mesh overhaul plan step 1 (gap L1). These are the shared pieces that the later steps build on: the calculators (step 2),
the quality explainer (step 4) and the questionnaire (step 7).

## The glossary

- **Content:** `src/glossary.c`, 48 entries. Each one gives the term in fluid-mechanics words, a physical picture, a number
  or rule of thumb, where it lives in OpenFOAM, and related terms. The shared numbers (air at 10 m/s along a 1 m plate)
  are worked out once at the top of the file. They match the meshing tutorial in `tour.c`. Thresholds and keywords were
  checked against v2606: `primitiveMeshCheck.C` (non-orthogonality 70°, skewness 4, aspect ratio 1000),
  `caseDicts/meshQualityDict` (maxNonOrtho 65, maxInternalSkewness 4, maxBoundarySkewness 20), and the annotated
  `snappyHexMeshDict`, `surfaceFeatureExtractDict` and `blockMeshDict`.
- **Markup:** any string drawn with `ui_text_wrap` can carry `[[key]]` or `[[key|shown words]]`. This includes section
  descriptions, `prop_row` hints, `note()` and callouts. Without `|` the key is shown with its hyphens as spaces, so
  `[[prism-layers]]` reads "prism layers". The words are drawn with a dotted underline. Hovering shows the entry in
  short, and clicking opens the Glossary at it. The drawn text (and what journeys see) is the plain text, so the markup
  takes the same room as the words alone.
- **Glossary dialog:** opened with Ctrl+G, from Keyboard shortcuts, from the Computer (Ctrl+K), or by clicking a term.
  It has a search box (names, then aliases, then the text), the entry, See also buttons and Back.
- **Adding a term:** add an entry to `T[]` in `glossary.c`, then mark it in the text. `test_glossary` fails if a key or
  alias is used twice, if an entry is incomplete, if a related key is missing, or if any `[[term]]` in `src/` names no
  entry.

## Show the working

- **Core:** `src/working.c`. A `Working` holds Given, Equations, Working (the numbers put in), Result and Notes lines,
  with units. `wk_text()` gives the same as plain text, for the Copy button and the tests.
- **UI:** `teach_working(id, x, y, w, &wk)` draws a "Show the working" toggle, collapsed until clicked, and below it the
  panel. The page rebuilds the `Working` every frame from the numbers it actually used, so the working always matches
  the page.
- **In use:** under the STL mesher's cell size (`wk_surface_cell`) and its prism layers (`wk_layer_stack`).

## Worked examples (Given / Find / Solution)

These are the hand calculations behind the two panels. `test_glossary` checks the panels against them, and checks
`wk_surface_cell` against the mesher's own `mesher_surface_cell` across a range of targets.

### 1. The prism-layer stack

**Given:** a stack T = 10 mm thick, N = 10 layers, an expansion ratio r = 1.2, under surface cells h = 4 mm.

**Find:** the first layer t1, the outer layer tN, and the outer layer as a share of h.

**Solution:** the layers are t1, r t1, r² t1 ..., a geometric series, so

    T  = t1 (r^N − 1) / (r − 1)
    t1 = T (r − 1) / (r^N − 1) = 10 mm × 0.2 / (1.2^10 − 1) = 10 mm × 0.2 / 5.192 = 0.3852 mm = 385.2 µm
    tN = t1 r^(N−1) = 0.3852 mm × 1.2^9 = 0.3852 mm × 5.160 = 1.988 mm
    tN / h = 1.988 / 4 = 49.7 %

Check: 0.3852 × (1 + 1.2 + 1.44 + ... + 5.160) = 0.3852 × 25.96 = 10.00 mm.

The outer layer falls inside Q-Continuum's 30 % to 100 % band, so the mesher keeps T as asked. When it falls outside,
the mesher changes T, and the panel shows the T actually made.

### 2. The surface cell (snappyHexMesh)

**Given:** a base size B = 40 mm, a target surface size s = 30 % of B, a part L = 1 m long.

**Find:** the surface cell h, and the cells along the part.

**Solution:** snappyHexMesh can only halve cells, so it rounds the target to a whole refinement level:

    level = round(log2(100 / s)) = round(log2(3.333)) = round(1.737) = 2
    h = B / 2^level = 40 mm / 4 = 10 mm        (12 mm was asked for)
    n = L / h = 1000 mm / 10 mm = 100 cells

A mesher that doesn't refine by halving (SALOME, for example) makes h = B s / 100 = 12 mm.
