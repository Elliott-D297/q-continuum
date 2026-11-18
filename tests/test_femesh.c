/* test_femesh.c -- part studies for CalculiX (docs/FEA_PLAN.md F1): a 100 x 10 x 10 mm beam of quadratic tetrahedra
 * written as fea_mesh.py would, with its mid-side nodes in a scrambled order and half its tetrahedra inside out; read back
 * (orientation and node order put right, six faces found by angle, the file's own groups when it has them); a study fixed
 * at one end, 100 N down at the other, written and run in ccx against beam theory (F L³ / 3 E I = 0.1905 mm). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "femesh.h"
#include "festudy.h"
#include "ccxmodel.h"
#include "pl_util.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("ok    %s\n", msg); else { printf("FAIL  %s\n", msg); fails++; } } while (0)

enum { NX = 20, NY = 2, NZ = 2 };
static const double LX = 100, LY = 10, LZ = 10;
static int lid(int i, int j, int k) { return 1 + i + (2 * NX + 1) * (j + (2 * NY + 1) * k); }   /* the half-step lattice */

/* the beam's .qcmesh; groups: when true, the end x = 0 is a named face group ("Fixed end") */
static void write_beam(const char *path, bool groups)
{
    static const int PERM[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
    static const int E[6][2] = { { 0, 1 }, { 1, 2 }, { 2, 0 }, { 0, 3 }, { 1, 3 }, { 2, 3 } };
    int nt = NX * NY * NZ * 6;
    int (*tet)[10] = malloc(sizeof(int) * 10 * (size_t)nt);
    int nl = (2 * NX + 1) * (2 * NY + 1) * (2 * NZ + 1);
    char *used = calloc((size_t)nl + 2, 1);
    int t = 0;
    for (int k = 0; k < NZ; k++) for (int j = 0; j < NY; j++) for (int i = 0; i < NX; i++)
        for (int p = 0; p < 6; p++, t++) {
            int c[4][3] = { { 0, 0, 0 } };
            for (int s = 1; s < 4; s++) { memcpy(c[s], c[s - 1], sizeof c[s]); if (s < 4) { if (s <= 2) c[s][PERM[p][s - 1]] = 1; else c[s][0] = c[s][1] = c[s][2] = 1; } }
            int q[4][3];
            for (int s = 0; s < 4; s++) { q[s][0] = 2 * (i + c[s][0]); q[s][1] = 2 * (j + c[s][1]); q[s][2] = 2 * (k + c[s][2]); }
            int v[10];
            for (int s = 0; s < 4; s++) v[s] = lid(q[s][0], q[s][1], q[s][2]);
            for (int e = 0; e < 6; e++) { int a = E[e][0], b = E[e][1]; v[4 + e] = lid((q[a][0] + q[b][0]) / 2, (q[a][1] + q[b][1]) / 2, (q[a][2] + q[b][2]) / 2); }
            if (t % 2) { int x = v[1]; v[1] = v[2]; v[2] = x; }          /* inside out */
            for (int e = 0; e < 3; e++) { int x = v[4 + e]; v[4 + e] = v[9 - e]; v[9 - e] = x; }   /* mid-sides scrambled */
            memcpy(tet[t], v, sizeof v);
            for (int s = 0; s < 10; s++) used[v[s]] = 1;
        }
    FILE *f = fopen(path, "w");
    int nn = 0; for (int i = 1; i <= nl; i++) nn += used[i];
    fprintf(f, "QCMESH 1\nnodes %d\n", nn);
    for (int k = 0; k <= 2 * NZ; k++) for (int j = 0; j <= 2 * NY; j++) for (int i = 0; i <= 2 * NX; i++)
        if (used[lid(i, j, k)]) fprintf(f, "%d %.9g %.9g %.9g\n", lid(i, j, k), LX * i / (2 * NX), LY * j / (2 * NY), LZ * k / (2 * NZ));
    fprintf(f, "volumes %d\n", nt);
    for (int e = 0; e < nt; e++) { fprintf(f, "10"); for (int s = 0; s < 10; s++) fprintf(f, " %d", tet[e][s]); fprintf(f, "\n"); }
    if (groups) {                                   /* the end x = 0: its triangles, corners only */
        fprintf(f, "faces %d\n", NY * NZ * 2);
        for (int k = 0; k < NZ; k++) for (int j = 0; j < NY; j++) {
            int a = lid(0, 2 * j, 2 * k), b = lid(0, 2 * j + 2, 2 * k), c = lid(0, 2 * j + 2, 2 * k + 2), d = lid(0, 2 * j, 2 * k + 2);
            fprintf(f, "0 3 %d %d %d\n0 3 %d %d %d\n", a, b, c, a, c, d);   /* the Kuhn split's diagonal runs a to c */
        }
        fprintf(f, "groups 1\n0 Fixed end\n");
    } else fprintf(f, "faces 0\ngroups 0\n");
    fclose(f);
    free(tet); free(used);
}

static int group_at(const FeMesh *m, int axis, double v)
{
    for (int g = 0; g < m->ng; g++) if (fabs(m->gcen[3 * g + axis] - v) < 1e-6 && m->gplanar[g]) return g;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "part")) {   /* a mesh fea_mesh.py made of the 100 mm beam: fixed at x = 0, 100 N down at x = 100 */
        FeMesh pm; char e2[300];
        if (!femesh_load(argv[2], &pm, e2, sizeof e2)) { printf("FAIL  %s\n", e2); return 1; }
        int a = group_at(&pm, 0, 0), b = group_at(&pm, 0, 100);
        printf("      %d nodes, %d elements (%s), %d groups, ends %d %d\n", pm.n, pm.ne, pm.quadratic ? "quadratic" : "linear", pm.ng, a, b);
        if (a < 0 || b < 0) { printf("FAIL  the ends not found\n"); return 1; }
        FeStudy ps; fe_study_default(&ps);
        ps.nFix = 1; ps.fix[0].nf = 1; pl_strlcpy(ps.fix[0].face[0], pm.gname[a], 64);
        ps.nLoad = 1; ps.load[0].value = 100; ps.load[0].dir = DIR_MZ; ps.load[0].nf = 1; pl_strlcpy(ps.load[0].face[0], pm.gname[b], 64);
        pl_mkdirs(argv[3]);
        if (!fe_study_write(&ps, &pm, argv[3], e2, sizeof e2)) { printf("FAIL  %s\n", e2); return 1; }
        char cmd[PL_PATH * 2]; snprintf(cmd, sizeof cmd, "cd '%s' && ccx study > log.ccx 2>&1", argv[3]);
        if (system(cmd)) { printf("FAIL  ccx\n"); return 1; }
        char frd[PL_PATH]; pl_join(frd, sizeof frd, argv[3], "study.frd");
        CcxFrd r; if (!ccx_frd_load(frd, &r)) { printf("FAIL  no results\n"); return 1; }
        double hand = 100.0 * 1e6 / (3 * 210000.0 * (10.0 * 1000 / 12));
        printf("      tip %.5g mm against %.5g by hand (%+.1f %%), reactions %.4g N\n", r.uMax, hand, (r.uMax - hand) / hand * 100, r.rfSum[2]);
        bool ok = fabs(r.uMax - hand) < 0.03 * hand && fabs(r.rfSum[2] - 100) < 0.5;
        printf(ok ? "ok    within 3 %%\n" : "FAIL  more than 3 %% off\n");
        return ok ? 0 : 1;
    }
    if (argc < 2) { printf("usage: test_femesh <scratch> | part <mesh.qcmesh> <dir>\n"); return 2; }
    char d[PL_PATH], err[300]; pl_join(d, sizeof d, argv[1], "femesh");
    pl_remove_tree(d); pl_mkdirs(d);
    char mp[PL_PATH]; pl_join(mp, sizeof mp, d, "part.qcmesh");
    write_beam(mp, false);
    FeMesh m;
    CHECK(femesh_load(mp, &m, err, sizeof err), "the beam's mesh read");
    printf("      %d nodes, %d elements, %d turned, %d boundary faces, %d groups\n", m.n, m.ne, m.nFlipped, m.nf, m.ng);
    CHECK(m.ne == NX * NY * NZ * 6 && m.quadratic, "480 quadratic tetrahedra");
    int neg = 0;
    for (int e = 0; e < m.ne; e++) {
        const int *v = &m.en[FE_MAXK * e]; double a[3], b[3], c[3];
        for (int k = 0; k < 3; k++) { a[k] = m.x[3 * v[1] + k] - m.x[3 * v[0] + k]; b[k] = m.x[3 * v[2] + k] - m.x[3 * v[0] + k]; c[k] = m.x[3 * v[3] + k] - m.x[3 * v[0] + k]; }
        if ((a[1] * b[2] - a[2] * b[1]) * c[0] + (a[2] * b[0] - a[0] * b[2]) * c[1] + (a[0] * b[1] - a[1] * b[0]) * c[2] <= 0) neg++;
    }
    CHECK(m.nFlipped > 0 && !neg, "the inside-out ones turned: every volume positive");
    int bad = 0;
    for (int e = 0; e < m.ne; e++) {                 /* each mid-side node halfway along its edge, CalculiX's order */
        static const int E[6][2] = { { 0, 1 }, { 1, 2 }, { 2, 0 }, { 0, 3 }, { 1, 3 }, { 2, 3 } };
        const int *v = &m.en[FE_MAXK * e];
        for (int q = 0; q < 6; q++) for (int k = 0; k < 3; k++)
            if (fabs(m.x[3 * v[4 + q] + k] - 0.5 * (m.x[3 * v[E[q][0]] + k] + m.x[3 * v[E[q][1]] + k])) > 1e-9) { bad++; q = 6; break; }
    }
    CHECK(!bad, "every mid-side node on its edge, in CalculiX's order");
    CHECK(m.nf == 2 * (2 * NX * NY + 2 * NX * NZ + 2 * NY * NZ) && m.fk[0] == 6, "the boundary: 6-node triangles, two per cell face");
    CHECK(m.ng == 6, "six faces found by angle");
    int g0 = group_at(&m, 0, 0), g1 = group_at(&m, 0, LX);
    CHECK(g0 >= 0 && g1 >= 0 && fabs(m.garea[g0] - 100) < 1e-9 && m.gnorm[3 * g0] < -0.999 && m.gnorm[3 * g1] > 0.999, "the ends: 100 mm², normals out along -x and +x");
    CHECK(fabs(m.garea[0] - 1000) < 1e-9, "the largest face first (1000 mm²)");
    double outward = 0; for (int f = 0; f < m.nf; f++) { double n[3]; femesh_face_area(&m, f, n); int g = m.fg[f]; outward += n[0] * m.gnorm[3 * g] + n[1] * m.gnorm[3 * g + 1] + n[2] * m.gnorm[3 * g + 2]; }
    CHECK(fabs(outward - m.nf) < 1e-6 * m.nf, "every boundary triangle facing out with its face");

    /* the study */
    FeStudy s; fe_study_default(&s);
    pl_strlcpy(s.part, "beam.step", sizeof s.part);
    s.nFix = 1; s.fix[0].kind = FX_FIXED; s.fix[0].nf = 1; pl_strlcpy(s.fix[0].face[0], m.gname[g0], 64);
    s.nLoad = 1; s.load[0].kind = LD_FORCE; s.load[0].value = 100; s.load[0].dir = DIR_MZ; s.load[0].nf = 1; pl_strlcpy(s.load[0].face[0], m.gname[g1], 64);
    FeSummary sum; fe_study_summary(&s, &m, &sum);
    printf("      force (%.6g, %.6g, %.6g) N on %d nodes; %d held; problem \"%s\"\n", sum.force[0], sum.force[1], sum.force[2], sum.loadedNodes, sum.fixedNodes, sum.problem);
    CHECK(fabs(sum.force[2] + 100) < 1e-9 && fabs(sum.force[0]) < 1e-12 && !sum.problem[0], "the loads come to 100 N down; nothing missing");
    CHECK(sum.fixedNodes == (2 * NY + 1) * (2 * NZ + 1), "the end's 25 nodes held");
    CHECK(fe_study_save(d, &s), "study.ini written");
    FeStudy s2; CHECK(fe_study_load(d, &s2) && s2.nFix == 1 && s2.nLoad == 1 && s2.load[0].dir == DIR_MZ && s2.load[0].value == 100 && !strcmp(s2.fix[0].face[0], m.gname[g0]), "and read back");
    FeStudy s3 = s; s3.nFix = 1; s3.fix[0].kind = FX_Z; fe_study_summary(&s3, &m, &sum);
    CHECK(strstr(sum.problem, "free to move along x") != NULL, "held in z only: free along x, said");
    FeStudy s4 = s; s4.load[0].kind = LD_PRESSURE; s4.load[0].value = 2; fe_study_summary(&s4, &m, &sum);
    CHECK(fabs(sum.force[0] + 200) < 1e-9, "2 MPa on the 100 mm² end: 200 N pushing in (-x)");
    CHECK(fe_study_write(&s, &m, d, err, sizeof err), "study.inp and study_mesh.inp written");
    if (system("command -v ccx > /dev/null 2>&1") == 0) {
        char cmd[PL_PATH * 2]; snprintf(cmd, sizeof cmd, "cd '%s' && ccx study > log.ccx 2>&1", d);
        CHECK(system(cmd) == 0, "ccx ran the study");
        char frd[PL_PATH]; pl_join(frd, sizeof frd, d, "study.frd");
        CcxFrd r;
        CHECK(ccx_frd_load(frd, &r) && r.u && r.vm && r.rf, "study.frd: displacements, stresses, reactions");
        double hand = 100.0 * 1e6 / (3 * 210000.0 * (10.0 * 1000 / 12));
        printf("      tip %.5g mm against %.5g by hand (%+.1f %%); reactions (%.4g %.4g %.4g) N; von Mises %.4g MPa\n", r.uMax, hand, (r.uMax - hand) / hand * 100,
               r.rfSum[0], r.rfSum[1], r.rfSum[2], r.vmMax);
        CHECK(fabs(r.uMax - hand) < 0.03 * hand, "the tip within 3 % of beam theory");
        CHECK(fabs(r.rfSum[2] - 100) < 0.5, "the reactions balance the 100 N");
        CcxModel cm; ccx_model_load(d, "study.inp", &cm);
        CHECK(ccx_units(&cm, NULL, 0) == CCX_UNITS_MM && !strcmp(cm.elType, "C3D10"), "the deck: millimetre units, C3D10");
        ccx_frd_free(&r);
    } else printf("skip  no ccx\n");
    femesh_free(&m);

    /* a mesh with its own face group: the end x = 0 named, the rest "Other faces" */
    write_beam(mp, true);
    CHECK(femesh_load(mp, &m, err, sizeof err), "the beam with a named group read");
    int gf = fe_group(&m, "Fixed end");
    CHECK(m.fileGroups && m.ng == 2 && gf == 0 && fabs(m.garea[0] - 100) < 1e-9 && fe_group(&m, "Other faces") == 1, "\"Fixed end\" (100 mm²) and \"Other faces\"");
    femesh_free(&m);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
