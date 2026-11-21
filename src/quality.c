/* quality.c -- checkMesh's measures explained, and log.checkMesh read (see quality.h) */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quality.h"

/* checkMesh's limits: nonOrthThreshold_ 70, skewThreshold_ 4, aspectThreshold_ 1000, closedThreshold_ 1e-6
   (primitiveMeshCheck.C); determinant 1e-3, face weight 0.05, volume ratio 0.01 (polyMeshCheck.C / checkGeometry.C);
   concave face angles 10 degrees, flatness 0.8 (checkGeometry.C). snappyHexMesh's: meshQualityDict's defaults. */
const QualMetric QUAL[QM__N] = {
    [QM_NONORTHO] = { "nonortho", "Non-orthogonality", "non-orthogonality", "°", true, 70, 65, false, "nonOrthoFaces",
        "The angle between the line joining two cell centres and the normal of the face between them. The solver works out the "
        "gradient across a face along that line; when the line leans away from the face's normal, part of the gradient is "
        "missed.",
        "The diffusion terms (viscosity, heat conduction, the pressure equation) lose accuracy and the pressure solution needs "
        "extra correction loops. Past 70 degrees a run can wobble or diverge; past 90 the face is unusable.",
        "checkMesh flags faces over 70 degrees and fails above 90. snappyHexMesh keeps it under 65 while it meshes "
        "(maxNonOrtho). Hexahedra in line with the flow: under 20; a good tetrahedral mesh: under 50.",
        "Better cells where it peaks: smaller cells or a gentler refinement step there, or less snapping squeeze (snappyHexMesh's "
        "nSmoothPatch, nRelaxIter). When it stays between 70 and 85, the solver copes with nNonOrthogonalCorrectors 2 or 3 and the "
        "laplacian schemes' limited corrected 0.5 (Q-Continuum's Setup picks these from checkMesh's log)." },
    [QM_SKEW] = { "skew", "Skewness", "skewness", "", true, 4, 4, false, "skewFaces",
        "How far the line joining two cell centres passes from the centre of the face between them, as a share of the cells' "
        "size. Values on the face are interpolated where the line crosses it, but stand for the whole face.",
        "The value on the face is taken at the wrong place: the convection terms become less accurate (a first-order error "
        "creeps in) and the solution can grow wiggles near those faces.",
        "checkMesh fails a mesh with faces over 4. snappyHexMesh allows 4 inside the mesh and 20 on the boundary "
        "(maxInternalSkewness, maxBoundarySkewness), so a few boundary faces of 4 to 20 after snapping are normal and safe.",
        "Cells of similar size side by side: a smaller size step (refinement transitions), finer cells where the surface turns "
        "sharply, or smoothing the geometry. Boundary-only skewness from snapping is usually left alone." },
    [QM_ASPECT] = { "aspect", "Aspect ratio", "aspect-ratio", "", true, 1000, NAN, false, "highAspectRatioCells",
        "How much longer a cell is than it is wide. Long thin cells are right inside prism layers, where the flow changes "
        "fast across the wall and slowly along it.",
        "Harmless when the long side runs along the flow (boundary layers routinely reach 100 to 1000). Across the flow, or in "
        "the open where nothing aligns with it, it slows convergence of the linear solvers and smears gradients.",
        "checkMesh fails cells over 1000. In a prism layer 100 to 1000 is normal; away from walls keep it under about 20.",
        "Away from walls: more cells along the long side. In layers: a thicker first layer (a higher target y+) or larger cells "
        "along the wall bring it down; check that the long side runs with the flow." },
    [QM_VOLUME] = { "volume", "Cell volumes", "negative-volume", "m³", false, 0, 1e-13, false, "zeroVolumeCells",
        "Every cell must enclose space. A zero or negative volume is a cell turned inside out: its corner points crossed over, "
        "usually while snapping to a sharp corner or growing layers into a tight gap.",
        "The solver cannot run on it: the flux balance of that cell has no meaning, and the run stops at once or blows up in "
        "the first iterations.",
        "Any volume at or below zero fails. snappyHexMesh refuses cells under 1e-13 m³ while it meshes (minVol).",
        "Remesh: finer cells near the corner or gap where it happens, fewer or thinner prism layers there, or the geometry "
        "cleaned (a sliver face, a gap thinner than the cells). Never run on a mesh with these." },
    [QM_PYRAMID] = { "pyramid", "Face pyramids", "", "faces", true, 0, NAN, false, "wrongOrientedFaces",
        "Each face and its cell's centre make a pyramid; its volume must be positive. A negative pyramid means a face points "
        "into its cell instead of out: the cell is folded.",
        "As an inside-out cell: the fluxes through that face carry the wrong sign. The run fails or produces nonsense.",
        "Any incorrectly oriented face fails the check.",
        "As for negative volumes: remesh with finer cells or fewer layers where it happens. An imported mesh with these was "
        "usually written with the wrong face order: re-export it." },
    [QM_CONCAVE] = { "concave", "Concave faces", "", "°", true, 10, 80, true, "concaveFaces",
        "A face whose outline turns back on itself: one corner bends inward, like a chevron. Snapping to a curved surface "
        "makes them.",
        "The face's centre and normal are poorly defined, so the flux through it is too: a local error that grows with how "
        "concave it is.",
        "checkMesh (Check everything) lists faces with an inward corner over 10 degrees; snappyHexMesh allows up to 80 while it "
        "meshes (maxConcave).",
        "Smoother snapping (more nSmoothPatch) or finer cells on the curved surface." },
    [QM_WARPED] = { "warped", "Face flatness", "", "", false, 0.8, NAN, true, "warpedFaces",
        "How flat a four-or-more-cornered face is: its projected area over its true area, 1 for a flat face, 0 for one folded "
        "like a butterfly.",
        "A warped face has no single normal; the flux through it is computed for a flat face that isn't there.",
        "checkMesh (Check everything) lists faces under 0.8.",
        "Finer cells on curved surfaces; in blockMesh, block faces that are planar or curved gently (arc edges)." },
    [QM_DETERMINANT] = { "determinant", "Cell determinant", "", "", false, 0.001, 0.001, true, "cellDeterminant",
        "Whether a cell's faces point in enough directions to work out a gradient in all three. A flattened cell, its faces "
        "nearly all in one plane, scores near zero.",
        "Gradients in that cell are badly defined: the least-squares or Gauss gradient amplifies noise there.",
        "checkMesh (Check everything) fails cells under 0.001; snappyHexMesh keeps it above 0.001 (minDeterminant). A cube "
        "scores 1.",
        "Finer cells where cells are crushed (thin gaps, sharp edges) or fewer layers in the gap." },
    [QM_WEIGHT] = { "weight", "Face weight", "", "", false, 0.05, 0.05, true, "lowWeightFaces",
        "How evenly a face sits between the two cell centres: 0.5 when half way, near 0 when the face is almost on top of "
        "one centre (a tiny cell beside a big one).",
        "The interpolation to the face leans almost entirely on one cell: accuracy drops and the matrix becomes harder to "
        "solve.",
        "checkMesh (Check everything) fails faces under 0.05; snappyHexMesh keeps them above 0.05 (minFaceWeight).",
        "A gentler size change between neighbours: more buffer cells between refinement levels (nCellsBetweenLevels), or "
        "smaller layer growth." },
    [QM_VOLRATIO] = { "volratio", "Volume ratio", "", "", false, 0.01, 0.01, true, "faceVolumeRatio",
        "The smaller of two neighbouring cells' volumes over the larger: 1 for equal cells, 0.01 for a cell a hundred times "
        "smaller than its neighbour.",
        "A sudden size jump reflects and damps waves, and its truncation error is first order: a visible kink in the "
        "solution where the size jumps.",
        "checkMesh (Check everything) fails faces under 0.01; snappyHexMesh keeps them above 0.01 (minVolRatio). Aim for "
        "neighbours within a factor of 2 (ratio over 0.5) in the flow.",
        "Grade the cells: a growth ratio of 1.2 or less, more cells between refinement levels." },
    [QM_OPEN] = { "open", "Closed cells", "", "", true, 1e-6, NAN, false, "nonClosedCells",
        "Each cell's faces must close it like a box: their area vectors add up to zero. An open cell leaks: a face is missing "
        "or doesn't meet its neighbours.",
        "Mass is not conserved in an open cell: continuity errors, and a run that never settles.",
        "checkMesh fails cells whose openness is over 1e-6; the boundary as a whole must close too.",
        "Usually a broken import or a conversion that dropped faces: convert again, or run zipUpMesh (checkMesh suggests it)." },
};

static double num_after(const char *p, const char *key)
{
    const char *q = strstr(p, key);
    return q ? strtod(q + strlen(key), NULL) : NAN;
}

/* the number after "number of cells" / "number of faces" / "number of open cells" on the same line */
static long count_on_line(const char *line)
{
    const char *e = strchr(line, '\n'); size_t n = e ? (size_t)(e - line) : strlen(line);
    char b[400]; snprintf(b, sizeof b, "%.*s", (int)(n < sizeof b ? n : sizeof b - 1), line);
    const char *q = strstr(b, "number of");
    if (!q) return 0;
    while (*q && !(*q >= '0' && *q <= '9')) q++;
    return atol(q);
}

void qual_read(const char *t, QualReport *r)
{
    memset(r, 0, sizeof *r);
    for (int k = 0; k < QM__N; k++) { r->value[k] = r->average[k] = NAN; }
    if (!t) return;
    r->have = true;
    r->allGeometry = strstr(t, "-allGeometry") != NULL;
    r->meshOk = strstr(t, "\nMesh OK.") != NULL || !strncmp(t, "Mesh OK.", 8);
    const char *p;
    if ((p = strstr(t, "    cells:"))) r->cells = atol(p + 10);

    /* non-orthogonality */
    if ((p = strstr(t, "Mesh non-orthogonality Max: "))) {
        r->value[QM_NONORTHO] = strtod(p + 28, NULL);
        r->average[QM_NONORTHO] = num_after(p, "average: ");
        r->state[QM_NONORTHO] = QV_OK;
    }
    if ((p = strstr(t, "*Number of severely non-orthogonal"))) {
        const char *q = strstr(p, "faces: ");
        r->count[QM_NONORTHO] = q ? atol(q + 7) : 0;
        r->state[QM_NONORTHO] = QV_FLAGGED;
    }
    if ((p = strstr(t, "***Number of non-orthogonality errors: "))) { r->count[QM_NONORTHO] = atol(p + 39); r->state[QM_NONORTHO] = QV_FAILED; }
    /* skewness */
    if ((p = strstr(t, "Max skewness = "))) {
        r->value[QM_SKEW] = strtod(p + 15, NULL);
        r->state[QM_SKEW] = QV_OK;
        if (p >= t + 3 && !strncmp(p - 3, "***", 3)) {
            r->state[QM_SKEW] = QV_FAILED;
            const char *c = strchr(p, ',');
            if (c) r->count[QM_SKEW] = atol(c + 1);
        }
    }
    /* aspect ratio */
    if ((p = strstr(t, "Max aspect ratio = "))) { r->value[QM_ASPECT] = strtod(p + 19, NULL); r->state[QM_ASPECT] = QV_OK; }
    if ((p = strstr(t, "***High aspect ratio cells found, Max aspect ratio: "))) {
        r->value[QM_ASPECT] = strtod(p + 52, NULL); r->count[QM_ASPECT] = count_on_line(p); r->state[QM_ASPECT] = QV_FAILED;
    }
    /* volumes */
    if ((p = strstr(t, "Min volume = "))) { r->value[QM_VOLUME] = strtod(p + 13, NULL); r->state[QM_VOLUME] = QV_OK; }
    if ((p = strstr(t, "***Zero or negative cell volume detected."))) {
        r->value[QM_VOLUME] = num_after(p, "Minimum negative volume: ");
        const char *q = strstr(p, "Number of negative volume cells: ");
        r->count[QM_VOLUME] = q ? atol(q + 33) : 0;
        r->state[QM_VOLUME] = QV_FAILED;
    }
    /* face pyramids */
    if (strstr(t, "Face pyramids OK.")) { r->value[QM_PYRAMID] = 0; r->state[QM_PYRAMID] = QV_OK; }
    if ((p = strstr(t, "***Error in face pyramids: "))) {
        r->count[QM_PYRAMID] = atol(p + 27); r->value[QM_PYRAMID] = (double)r->count[QM_PYRAMID]; r->state[QM_PYRAMID] = QV_FAILED;
    }
    /* concave faces, flatness (-allGeometry) */
    if ((p = strstr(t, "*There are ")) && strstr(p, "concave angles")) {
        const char *q = p;
        while ((q = strstr(q, "*There are "))) {
            const char *e = strchr(q, '\n');
            const char *c = strstr(q, "concave angles");
            if (c && (!e || c < e)) {
                r->count[QM_CONCAVE] = atol(q + 11);
                r->value[QM_CONCAVE] = num_after(q, "Max concave angle = ");
                r->state[QM_CONCAVE] = QV_FLAGGED;
                break;
            }
            q += 11;
        }
    }
    if (r->allGeometry && r->state[QM_CONCAVE] == QV_UNMEASURED) r->state[QM_CONCAVE] = QV_OK;
    if ((p = strstr(t, "Face flatness (1 = flat, 0 = butterfly) : min = "))) {
        r->value[QM_WARPED] = strtod(p + 49, NULL);
        r->average[QM_WARPED] = num_after(p, "average = ");
        r->state[QM_WARPED] = QV_OK;
    }
    for (const char *q = t; (q = strstr(q, "*There are ")); q += 11) {
        const char *e = strchr(q, '\n'), *c = strstr(q, "ratio between projected and actual area");
        if (c && (!e || c < e)) { r->count[QM_WARPED] = atol(q + 11); r->state[QM_WARPED] = QV_FLAGGED; break; }
    }
    /* determinant, face weight, volume ratio (-allGeometry) */
    static const struct { int k; const char *val, *bad; } MIN3[] = {
        { QM_DETERMINANT, "Cell determinant (wellposedness) : minimum: ", "***Cells with small determinant" },
        { QM_WEIGHT, "Face interpolation weight : minimum: ", "***Faces with small interpolation weight" },
        { QM_VOLRATIO, "Face volume ratio : minimum: ", "***Faces with small volume ratio" } };
    for (int i = 0; i < 3; i++) {
        int k = MIN3[i].k;
        if ((p = strstr(t, MIN3[i].val))) {
            r->value[k] = strtod(p + strlen(MIN3[i].val), NULL);
            r->average[k] = num_after(p, "average: ");
            r->state[k] = QV_OK;
        }
        if ((p = strstr(t, MIN3[i].bad))) { r->count[k] = count_on_line(p); r->state[k] = QV_FAILED; }
    }
    /* closed cells */
    if ((p = strstr(t, "Max cell openness = "))) { r->value[QM_OPEN] = strtod(p + 20, NULL); r->state[QM_OPEN] = QV_OK; }
    if ((p = strstr(t, "***Open cells found, max cell openness: "))) {
        r->value[QM_OPEN] = strtod(p + 40, NULL); r->count[QM_OPEN] = count_on_line(p); r->state[QM_OPEN] = QV_FAILED;
    }
    if (strstr(t, "***Boundary openness")) r->state[QM_OPEN] = QV_FAILED;
}

void qual_fmt(int k, double v, char *out, int cap)
{
    if (isnan(v)) { snprintf(out, (size_t)cap, "-"); return; }
    if (k == QM_NONORTHO || k == QM_CONCAVE) snprintf(out, (size_t)cap, "%.1f°", v);
    else if (k == QM_PYRAMID) snprintf(out, (size_t)cap, "%.0f", v);
    else if (k == QM_VOLUME) snprintf(out, (size_t)cap, "%.3g m³", v);
    else if (fabs(v) >= 1e4 || (v != 0 && fabs(v) < 1e-3)) snprintf(out, (size_t)cap, "%.3g", v);
    else snprintf(out, (size_t)cap, "%.4g", v);
}

int qual_first_problem(const QualReport *r)
{
    for (int k = 0; k < QM__N; k++) if (r->state[k] == QV_FAILED) return k;
    for (int k = 0; k < QM__N; k++) if (r->state[k] == QV_FLAGGED) return k;
    return -1;
}
