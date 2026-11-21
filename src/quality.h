/* quality.h -- the mesh quality explainer (mesh overhaul plan step 4, gap L5): checkMesh's measures, each with what it is
 * in fluid-mechanics words, what it does to the solution, the thresholds (checkMesh's own, from OpenFOAM v2606's
 * primitiveMeshCheck.C / polyMeshCheck.C / checkGeometry.C, and snappyHexMesh's meshQualityDict defaults from
 * $FOAM_ETC/caseDicts/meshQualityDict), and the fix; and log.checkMesh read into a value and a verdict per measure.
 * Plain C: page_meshquality.c draws it (with a picture per measure), test_quality checks it. */
#ifndef QUALITY_H
#define QUALITY_H

#include <stdbool.h>

enum { QM_NONORTHO = 0, QM_SKEW, QM_ASPECT, QM_VOLUME, QM_PYRAMID, QM_CONCAVE, QM_WARPED, QM_DETERMINANT, QM_WEIGHT,
       QM_VOLRATIO, QM_OPEN, QM__N };

typedef struct {
    const char *key;        /* stable id (UI ids, tests)                                   */
    const char *name;       /* heading                                                     */
    const char *gloss;      /* glossary key, or ""                                         */
    const char *unit;       /* for the value: "°", "" ...                                  */
    bool  worseHigh;        /* a larger value is worse (non-orthogonality); else smaller   */
    double limit;           /* checkMesh's: past it the check is flagged or failed (NAN: a count only) */
    double snappy;          /* snappyHexMesh's own limit while it meshes (meshQualityDict), NAN: none */
    bool  allGeometry;      /* measured only by checkMesh -allGeometry ("Check everything") */
    const char *sets;       /* the face or cell sets checkMesh writes for it, space-separated */
    const char *what;       /* what it measures, in fluid-mechanics words                  */
    const char *effect;     /* what a bad value does to the solution                       */
    const char *threshold;  /* the numbers, in words                                       */
    const char *fix;        /* what fixes it                                               */
} QualMetric;

extern const QualMetric QUAL[QM__N];

enum { QV_UNMEASURED = 0, QV_OK, QV_FLAGGED, QV_FAILED };   /* flagged: checkMesh's "*" warning; failed: its "***" */

typedef struct {
    bool   have;            /* log.checkMesh was read                                     */
    bool   allGeometry;     /* it was a -allGeometry run                                  */
    bool   meshOk;          /* "Mesh OK."                                                 */
    long   cells;
    double value[QM__N];    /* the worst value (max or min), NAN when not printed         */
    double average[QM__N];  /* where printed, else NAN                                    */
    long   count[QM__N];    /* faces or cells past the limit, 0 if none or not printed   */
    int    state[QM__N];    /* QV_*                                                       */
} QualReport;

/* read checkMesh's output (the text of log.checkMesh) */
void qual_read(const char *log, QualReport *r);

/* "70°", "4", "1000" ...: a value of measure k as text */
void qual_fmt(int k, double v, char *out, int cap);

/* the measure to open first: the worst failed, else the worst flagged, else -1 */
int  qual_first_problem(const QualReport *r);

#endif
