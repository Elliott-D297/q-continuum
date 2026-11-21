/* test_quality.c -- the mesh quality explainer's core (mesh overhaul plan step 4, quality.c).
     test_quality <fixtures dir> <$FOAM_ETC/caseDicts/meshQualityDict> [<OpenFOAM src dir>]
   log.checkMesh read right (a real v2606 run that passes, one with non-orthogonal and skew faces, and one with every failure
   line as checkMesh prints it); every measure fully explained; the limits match snappyHexMesh's meshQualityDict defaults and,
   when the OpenFOAM source is installed, checkMesh's own thresholds in primitiveMeshCheck.C. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pl_util.h"
#include "quality.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static char *slurp(const char *dir, const char *f)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, f);
    char *t = pl_read_file(p, NULL);
    CHECK(t, "cannot read %s", p);
    return t;
}

static double dict_value(const char *t, const char *key)
{
    char k[64]; snprintf(k, sizeof k, "\n%s ", key);
    const char *p = strstr(t, k);
    return p ? strtod(p + strlen(k), NULL) : NAN;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: test_quality <fixtures> <meshQualityDict> [<src>]\n"); return 2; }
    const char *fx = argv[1];
    QualReport r;

    /* the table: every entry complete */
    for (int k = 0; k < QM__N; k++) {
        const QualMetric *m = &QUAL[k];
        CHECK(m->key && m->name && m->what && m->effect && m->threshold && m->fix && m->sets && m->unit && m->gloss,
              "measure %d incomplete", k);
        CHECK(strlen(m->what) > 60 && strlen(m->effect) > 60 && strlen(m->fix) > 40, "%s: thin text", m->key);
    }

    /* a mesh that passes */
    char *t = slurp(fx, "checkmesh_ok.log");
    qual_read(t, &r);
    CHECK(r.have && r.meshOk && r.allGeometry && r.cells == 400, "ok: have %d ok %d allGeometry %d cells %ld", r.have, r.meshOk, r.allGeometry, r.cells);
    for (int k = 0; k < QM__N; k++) CHECK(r.state[k] == QV_OK, "ok: %s state %d", QUAL[k].key, r.state[k]);
    CHECK(qual_first_problem(&r) == -1, "ok: no problem");
    CHECK(r.value[QM_NONORTHO] == 0 && r.value[QM_ASPECT] == 1 && r.value[QM_WEIGHT] == 0.5, "ok: values %g %g %g",
          r.value[QM_NONORTHO], r.value[QM_ASPECT], r.value[QM_WEIGHT]);
    free(t);

    /* the sheared cavity: 760 faces over 70 degrees (flagged), skewness 4 on one face (failed) */
    t = slurp(fx, "checkmesh_skew.log");
    qual_read(t, &r);
    CHECK(!r.meshOk, "skew: not OK");
    CHECK(fabs(r.value[QM_NONORTHO] - 75.9638) < 1e-3 && r.state[QM_NONORTHO] == QV_FLAGGED && r.count[QM_NONORTHO] == 760,
          "skew: nonortho %g state %d count %ld", r.value[QM_NONORTHO], r.state[QM_NONORTHO], r.count[QM_NONORTHO]);
    CHECK(r.value[QM_SKEW] == 4 && r.state[QM_SKEW] == QV_FAILED && r.count[QM_SKEW] == 1, "skew: skew %g state %d count %ld",
          r.value[QM_SKEW], r.state[QM_SKEW], r.count[QM_SKEW]);
    CHECK(fabs(r.value[QM_DETERMINANT] - 0.00277057) < 1e-9 && r.state[QM_DETERMINANT] == QV_OK, "skew: determinant");
    CHECK(qual_first_problem(&r) == QM_SKEW, "skew: first problem is skewness (failed before flagged), got %d", qual_first_problem(&r));
    free(t);

    /* every failure line */
    t = slurp(fx, "checkmesh_bad.log");
    qual_read(t, &r);
    struct { int k, st; long n; double v; } want[] = {
        { QM_NONORTHO, QV_FAILED, 2, 75.9638 }, { QM_ASPECT, QV_FAILED, 17, 2345.6 }, { QM_VOLUME, QV_FAILED, 6, -3.2e-12 },
        { QM_PYRAMID, QV_FAILED, 12, 12 }, { QM_CONCAVE, QV_FLAGGED, 44, 37.5 }, { QM_DETERMINANT, QV_FAILED, 9, 0.00277057 },
        { QM_WEIGHT, QV_FAILED, 5, 0.5 }, { QM_VOLRATIO, QV_FAILED, 8, 1 }, { QM_OPEN, QV_FAILED, 3, 0.0123 } };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        int k = want[i].k;
        CHECK(r.state[k] == want[i].st, "bad: %s state %d, want %d", QUAL[k].key, r.state[k], want[i].st);
        CHECK(r.count[k] == want[i].n, "bad: %s count %ld, want %ld", QUAL[k].key, r.count[k], want[i].n);
        CHECK(fabs(r.value[k] - want[i].v) <= 1e-6 * fabs(want[i].v) + 1e-15, "bad: %s value %g, want %g", QUAL[k].key, r.value[k], want[i].v);
    }
    CHECK(qual_first_problem(&r) == QM_NONORTHO, "bad: first problem");
    free(t);
    qual_read(NULL, &r);
    CHECK(!r.have && qual_first_problem(&r) == -1, "no log");

    /* the limits against snappyHexMesh's meshQualityDict */
    t = pl_read_file(argv[2], NULL);
    CHECK(t, "cannot read %s", argv[2]);
    if (t) {
        CHECK(dict_value(t, "maxNonOrtho") == QUAL[QM_NONORTHO].snappy, "maxNonOrtho %g vs %g", dict_value(t, "maxNonOrtho"), QUAL[QM_NONORTHO].snappy);
        CHECK(dict_value(t, "maxInternalSkewness") == QUAL[QM_SKEW].snappy, "maxInternalSkewness");
        CHECK(dict_value(t, "maxConcave") == QUAL[QM_CONCAVE].snappy, "maxConcave");
        CHECK(dict_value(t, "minVol") == QUAL[QM_VOLUME].snappy, "minVol");
        CHECK(dict_value(t, "minDeterminant") == QUAL[QM_DETERMINANT].snappy, "minDeterminant");
        CHECK(dict_value(t, "minFaceWeight") == QUAL[QM_WEIGHT].snappy, "minFaceWeight");
        CHECK(dict_value(t, "minVolRatio") == QUAL[QM_VOLRATIO].snappy, "minVolRatio");
        CHECK(strstr(QUAL[QM_SKEW].threshold, "20") && dict_value(t, "maxBoundarySkewness") == 20, "maxBoundarySkewness 20 named");
        free(t);
    }
    /* ... and checkMesh's own, when the source is there */
    if (argc > 3) {
        char p[1024]; snprintf(p, sizeof p, "%s/OpenFOAM/meshes/primitiveMesh/primitiveMeshCheck/primitiveMeshCheck.C", argv[3]);
        t = pl_read_file(p, NULL);
        if (t) {
            CHECK(strstr(t, "aspectThreshold_  = 1000") && QUAL[QM_ASPECT].limit == 1000, "aspectThreshold 1000");
            CHECK(strstr(t, "nonOrthThreshold_ = 70") && QUAL[QM_NONORTHO].limit == 70, "nonOrthThreshold 70");
            CHECK(strstr(t, "skewThreshold_    = 4") && QUAL[QM_SKEW].limit == 4, "skewThreshold 4");
            CHECK(strstr(t, "closedThreshold_  = 1.0e-6") && QUAL[QM_OPEN].limit == 1e-6, "closedThreshold 1e-6");
            free(t);
        } else printf("(no OpenFOAM source: checkMesh's thresholds not cross-checked)\n");
    }
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("quality: all passed\n");
    return 0;
}
