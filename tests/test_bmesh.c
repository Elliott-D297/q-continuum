/* test_bmesh.c -- the blockMesh model (mesh overhaul plan step 5, bmesh.c).
     test_bmesh unit
         reading (plain, multi-grading, edges, patch extras; macros and projections refused), writing and reading back,
         the checks (inside-out blocks, cells that don't match across a face, faces in no patch or in two), the grading points
     test_bmesh templates <outdir>
         every template with its defaults: no errors from the checks, written as <outdir>/<key>/system/blockMeshDict
         (tests/bmesh_openfoam.sh runs blockMesh and checkMesh on each)
     test_bmesh roundtrip <tutorials> <outdir>
         every blockMeshDict under <tutorials> the builder can read, read and put back into a copy of itself with bm_apply:
         <outdir>/<n>/orig and <outdir>/<n>/ours (the script meshes both and compares the points and faces) */
#define _GNU_SOURCE
#include <ftw.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "bmesh.h"
#include "foamdict.h"
#include "pl_util.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static BmModel M, M2;

static int errors(const BmModel *m, const char *needle)
{
    BmIssue is[64]; int n = bm_check(m, is, 64), e = 0;
    for (int i = 0; i < n; i++) {
        if (needle && strstr(is[i].text, needle)) return 1;
        if (!needle && is[i].level >= 2) { e++; fprintf(stderr, "  issue: %s\n", is[i].text); }
    }
    return needle ? 0 : e;
}

static const char *CAVITY =
    "FoamFile { version 2.0; format ascii; class dictionary; object blockMeshDict; }\n"
    "// a comment that must survive\nscale 0.1;\n"
    "vertices ( (0 0 0) (1 0 0) (1 1 0) (0 1 0) (0 0 0.1) (1 0 0.1) (1 1 0.1) (0 1 0.1) );\n"
    "blocks ( hex (0 1 2 3 4 5 6 7) (20 20 1) simpleGrading (1 ((0.5 0.5 4) (0.5 0.5 0.25)) 1) );\n"
    "edges ( arc 2 3 (0.5 1.1 0) spline 6 7 ((0.7 1.05 0.1) (0.3 1.05 0.1)) );\n"
    "boundary (\n movingWall { type wall; inGroups (lid); faces ((3 7 6 2)); }\n"
    " fixedWalls { type wall; faces ((0 4 7 3) (2 6 5 1) (1 5 4 0)); }\n"
    " frontAndBack { type empty; faces ((0 3 2 1) (4 5 6 7)); }\n);\n"
    "mergePatchPairs ( );\n";

static void unit(void)
{
    char why[400];
    CHECK(bm_read_text(CAVITY, &M, why, sizeof why), "read cavity: %s", why);
    CHECK(M.scale == 0.1 && M.nv == 8 && M.nb == 1 && M.ne == 2 && M.np == 3, "cavity counts: scale %g nv %d nb %d ne %d np %d", M.scale, M.nv, M.nb, M.ne, M.np);
    CHECK(M.b[0].g[1].n == 2 && M.b[0].g[1].ratio[0] == 4 && M.b[0].g[1].ratio[1] == 0.25, "multi-grading read");
    CHECK(M.e[0].kind == BE_ARC && M.e[1].kind == BE_SPLINE && M.e[1].np == 2, "edges read");
    CHECK(strstr(M.p[0].extra, "inGroups (lid);") != NULL, "patch extra kept: '%s'", M.p[0].extra);
    CHECK(errors(&M, NULL) == 0, "cavity: no errors");
    CHECK(bm_cells(&M) == 400, "cavity cells");
    /* written and read back: the same model */
    char *w = bm_write(&M, "test");
    CHECK(bm_read_text(w, &M2, why, sizeof why), "read back: %s", why);
    CHECK(M2.nv == 8 && M2.nb == 1 && M2.ne == 2 && M2.np == 3 && M2.b[0].g[1].n == 2 && !memcmp(M.v, M2.v, sizeof(double) * 3 * 8) &&
          strstr(M2.p[0].extra, "inGroups") && M2.e[1].np == 2, "write/read round trip");
    free(w);
    /* put back into the file: everything else stays */
    FDFile *f = fd_from_text("blockMeshDict", CAVITY);
    M.b[0].n[0] = 30;
    CHECK(bm_apply(f, &M), "apply");
    CHECK(strstr(f->text, "// a comment that must survive") && strstr(f->text, "mergePatchPairs") && strstr(f->text, "(30 20 1)"), "apply keeps the rest");
    CHECK(bm_read(f, &M2, why, sizeof why) && M2.b[0].n[0] == 30, "applied file reads");
    fd_free(f);
    /* refused: macros, #eval, projections, named vertices */
    CHECK(!bm_read_text("vertices ( ($x0 0 0) ); blocks ( );", &M, why, sizeof why) && strstr(why, "$x0"), "macro refused: %s", why);
    CHECK(!bm_read_text("n #eval \"2*3\";\nvertices ( (0 0 0) );\nblocks ( hex (0 0 0 0 0 0 0 0) ($n 1 1) simpleGrading (1 1 1) );", &M, why, sizeof why), "#eval refused");
    CHECK(!bm_read_text("geometry { s { type sphere; } }\nvertices ( (0 0 0) ); blocks ( );", &M, why, sizeof why) && strstr(why, "geometry"), "geometry refused: %s", why);
    CHECK(!bm_read_text("vertices ( name v0 (0 0 0) ); blocks ( );", &M, why, sizeof why), "named vertex refused");
    CHECK(!bm_read_text("vertices ( (0 0 0) ); blocks ( );\nedges ( arc 0 1 origin (0 0 0) );", &M, why, sizeof why), "arc origin form refused");
    /* the checks */
    bm_read_text(CAVITY, &M, why, sizeof why);
    BmModel *x = &M2;
    *x = M; int t = x->b[0].v[0]; x->b[0].v[0] = x->b[0].v[4]; x->b[0].v[4] = t;   /* not a hex any more: still flagged */
    *x = M; for (int i = 0; i < 4; i++) { int s = x->b[0].v[i]; x->b[0].v[i] = x->b[0].v[i + 4]; x->b[0].v[i + 4] = s; }
    CHECK(errors(x, "inside out"), "inside-out block caught");
    *x = M; x->p[1].nf = 2;
    CHECK(errors(x, "in no patch"), "loose face caught");
    *x = M; x->p[0].f[0][0] = 0; x->p[0].f[0][1] = 1; x->p[0].f[0][2] = 2; x->p[0].f[0][3] = 7;
    CHECK(errors(x, "no block's face"), "foreign face caught");
    *x = M; x->p[2].f[1][0] = 3; x->p[2].f[1][1] = 7; x->p[2].f[1][2] = 6; x->p[2].f[1][3] = 2;
    CHECK(errors(x, "in two patches"), "face in two patches caught");
    *x = M; x->b[0].v[2] = 99;
    CHECK(errors(x, "vertices run"), "vertex out of range caught");
    /* two blocks with different counts across their face */
    BmModel *two = &M2;
    bm_init(two);
    double P[12][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 2, 1, 0 }, { 0, 0, 1 }, { 1, 0, 1 }, { 2, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 }, { 2, 1, 1 } };
    memcpy(two->v, P, sizeof P); two->nv = 12;
    int b0[8] = { 0, 1, 4, 3, 6, 7, 10, 9 }, b1[8] = { 1, 2, 5, 4, 7, 8, 11, 10 };
    memcpy(two->b[0].v, b0, sizeof b0); memcpy(two->b[1].v, b1, sizeof b1);
    for (int i = 0; i < 2; i++) { two->b[i].n[0] = two->b[i].n[1] = two->b[i].n[2] = 5; for (int k = 0; k < 3; k++) { two->b[i].g[k].n = 1; two->b[i].g[k].len[0] = two->b[i].g[k].cells[0] = two->b[i].g[k].ratio[0] = 1; } }
    two->nb = 2;
    CHECK(!errors(two, "share a face but not"), "matching blocks pass");
    two->b[1].n[1] = 7;
    CHECK(errors(two, "share a face but not"), "mismatched counts caught");
    /* grading points: 10 cells, ratio 10: first and last as the calculator has them; multi-grading symmetric */
    BmGrade g = { 1, { 1 }, { 1 }, { 10 } };
    double pts[64];
    bm_grade_points(&g, 10, pts);
    double r = pow(10, 1.0 / 9), d1 = (r - 1) / (pow(r, 10) - 1);
    CHECK(fabs(pts[1] - d1) < 1e-12 && fabs(pts[10] - 1) < 1e-12 && fabs((pts[10] - pts[9]) / pts[1] - 10) < 1e-9, "grade points");
    BmGrade g2 = { 2, { 0.5, 0.5 }, { 0.5, 0.5 }, { 4, 0.25 } };
    bm_grade_points(&g2, 20, pts);
    CHECK(fabs(pts[10] - 0.5) < 1e-12 && fabs(pts[1] - (1 - pts[19])) < 1e-12, "two-sided grading symmetric");
    /* the ratio for a first cell */
    double R = bm_ratio_for_first(0.05, 20, 1e-4);
    double rr = pow(R, 1.0 / 19), f1 = 0.05 * (rr - 1) / (pow(rr, 20) - 1);
    CHECK(fabs(f1 - 1e-4) < 1e-10, "ratio for first: first %g", f1);
    /* preview geometry: the arc bulges to its point */
    bm_read_text(CAVITY, &M, why, sizeof why);
    double mid[3]; bm_edge_point(&M, 0, 1, 0.5, mid);            /* edge 1 is vertices 3 -> 2 */
    CHECK(fabs(mid[1] - 1.1) < 1e-9 && fabs(mid[0] - 0.5) < 1e-9, "arc midpoint (%g, %g)", mid[0], mid[1]);
    BmDraw d; bm_draw_build(&M, &d);
    CHECK(d.nSeg > 12 * 20 && fabs(d.hi[1] - 0.11) < 1e-6, "draw: %d segments, top %g", d.nSeg, d.hi[1]);
    bm_draw_free(&d);
}

static void templates(const char *out)
{
    char err[300];
    for (int t = 0; t < BT__N; t++) {
        const BmTemplate *T = &BM_TEMPLATES[t];
        CHECK(T->key && T->name && T->what && T->patches && T->nPar > 0, "template %d incomplete", t);
        CHECK(bm_template(t, NULL, &M, err, sizeof err), "%s: %s", T->key, err);
        int e = errors(&M, NULL);
        CHECK(e == 0, "%s: %d errors from the checks", T->key, e);
        BmIssue is[32]; int n = bm_check(&M, is, 32);
        for (int i = 0; i < n; i++) CHECK(is[i].level < 1 || !strstr(is[i].text, "in no patch"), "%s: %s", T->key, is[i].text);
        char dir[1024]; snprintf(dir, sizeof dir, "%s/%s/system", out, T->key);
        pl_mkdirs(dir);
        char p[1100]; snprintf(p, sizeof p, "%s/blockMeshDict", dir);
        char *w = bm_write(&M, T->name);
        CHECK(pl_write_file(p, w, strlen(w)), "write %s", p);
        CHECK(bm_read_text(w, &M2, err, sizeof err) && M2.nb == M.nb && M2.ne == M.ne, "%s reads back: %s", T->key, err);
        free(w);
        printf("%-10s %3d blocks %4d edges %10.0f cells\n", T->key, M.nb, M.ne, bm_cells(&M));
    }
    /* a parameter out of range is refused, with its name */
    double v[BT_MAXPAR];
    for (int i = 0; i < BM_TEMPLATES[BT_PIPE].nPar; i++) v[i] = BM_TEMPLATES[BT_PIPE].par[i].def;
    v[bm_template_par(BT_PIPE, "core")] = 0.95;
    CHECK(!bm_template(BT_PIPE, v, &M, err, sizeof err) && strstr(err, "Core size"), "pipe core 0.95 refused: %s", err);
    /* the 3-D channel */
    double c[BT_MAXPAR];
    for (int i = 0; i < BM_TEMPLATES[BT_CHANNEL].nPar; i++) c[i] = BM_TEMPLATES[BT_CHANNEL].par[i].def;
    c[bm_template_par(BT_CHANNEL, "W")] = 0.1; c[bm_template_par(BT_CHANNEL, "first")] = 1e-4;
    CHECK(bm_template(BT_CHANNEL, c, &M, err, sizeof err) && errors(&M, NULL) == 0 && M.b[0].n[2] == 20, "3-D channel");
    char dir[1024]; snprintf(dir, sizeof dir, "%s/channel3d/system", out); pl_mkdirs(dir);
    char p[1100]; snprintf(p, sizeof p, "%s/blockMeshDict", dir);
    char *w = bm_write(&M, "3-D channel"); pl_write_file(p, w, strlen(w)); free(w);
}

static const char *RT_OUT; static int rtRead, rtRefused, rtN;
static int rt_visit(const char *p, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st; (void)ftw;
    if (type != FTW_F || strcmp(pl_basename(p), "blockMeshDict")) return 0;
    FDFile *f = fd_load(p);
    if (!f || !f->ok) { fd_free(f); return 0; }
    char why[400];
    if (!bm_read(f, &M, why, sizeof why)) { rtRefused++; fd_free(f); return 0; }
    rtRead++;
    char d[1024]; snprintf(d, sizeof d, "%s/%03d", RT_OUT, rtN++);
    char a[1100], b[1100]; snprintf(a, sizeof a, "%s/orig/system", d); snprintf(b, sizeof b, "%s/ours/system", d);
    pl_mkdirs(a); pl_mkdirs(b);
    char fa[1200], fb[1200]; snprintf(fa, sizeof fa, "%s/blockMeshDict", a); snprintf(fb, sizeof fb, "%s/blockMeshDict", b);
    pl_write_file(fa, f->text, (size_t)f->len);
    CHECK(bm_apply(f, &M), "apply %s", p);
    pl_write_file(fb, f->text, (size_t)f->len);
    char src[1100]; snprintf(src, sizeof src, "%s/source", d); pl_write_file(src, p, strlen(p));
    fd_free(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_bmesh unit | templates <out> | roundtrip <tutorials> <out>\n"); return 2; }
    if (!strcmp(argv[1], "unit")) unit();
    else if (!strcmp(argv[1], "templates") && argc > 2) templates(argv[2]);
    else if (!strcmp(argv[1], "roundtrip") && argc > 3) {
        RT_OUT = argv[3]; pl_mkdirs(RT_OUT);
        nftw(argv[2], rt_visit, 32, FTW_PHYS);
        printf("roundtrip: %d blockMeshDicts read, %d refused (macros, #eval, projections ...)\n", rtRead, rtRefused);
        CHECK(rtRead >= 20, "only %d tutorial blockMeshDicts read", rtRead);
    } else return 2;
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("bmesh %s: all passed\n", argv[1]);
    return 0;
}
