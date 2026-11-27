/* test_fea_an.c -- every kind of part study (improvement plan B1) written by festudy.c, run in CalculiX 2.21 and checked
 * against its hand-worked answer, on a 100 x 10 x 10 mm steel bar of quadratic tetrahedra (E 210 GPa, nu 0.3, rho 7850,
 * k 50 W/(m K), alpha 12e-6 /K, yield 250 MPa, UTS 400 MPa at 21 % elongation):
 *   nonlinear   uniaxial, nominal 280 MPa: true stress sigma e^eps and log strain eps = sigma/E + (sigma - 250)/H   (3 %)
 *   frequency   a cantilever: 1.875^2 / (2 pi) sqrt(E I / (rho A L^4)) = 835.5 Hz                          (4 %)
 *   buckling    fixed-free under 1 N: pi^2 E I / (4 L^2) = 43 180                                         (5 %)
 *   heat        ends at 400 and 300 K: 350 K midway; 400 K and convection h 500 to 300 K: 350 K at the
 *               cooled end (h L / k = 1); 1e5 W/m² in at one end, 300 K at the other: 500 K; 1 W made inside,
 *               both ends at 300 K: 302.5 K midway (q L² / 8 k)                                           (0.5 %)
 *   thermal     held axially, 100 K warmer: -E alpha dT = -252 MPa, von Mises 252                          (2 %)
 *   dynamic     a step load on the cantilever, barely damped: about twice the static tip deflection        (10 %)
 *   harmonic    shaken at its tip, 2 % damped: the peak at the first frequency, about static / (2 zeta)    (3 %, 25 %)
 * test_femesh's static case covers linear static (0.1905 mm). Needs ccx on the PATH (exit 77 without). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "festudy.h"
#include "femesh.h"
#include "ccxmodel.h"
#include "pl_util.h"

static int fails;
#define CHECK(c, ...) do { if (c) { printf("ok    "); printf(__VA_ARGS__); printf("\n"); } else { printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

enum { NX = 20, NY = 2, NZ = 2 };
static const double LX = 100, LY = 10, LZ = 10;
static int lid(int i, int j, int k) { return 1 + i + (2 * NX + 1) * (j + (2 * NY + 1) * k); }

static void write_bar(const char *path)
{
    static const int PERM[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
    static const int E[6][2] = { { 0, 1 }, { 1, 2 }, { 2, 0 }, { 0, 3 }, { 1, 3 }, { 2, 3 } };
    int nt = NX * NY * NZ * 6, nl = (2 * NX + 1) * (2 * NY + 1) * (2 * NZ + 1);
    int (*tet)[10] = malloc(sizeof *tet * (size_t)nt);
    char *used = calloc((size_t)nl + 2, 1);
    int t = 0;
    for (int k = 0; k < NZ; k++) for (int j = 0; j < NY; j++) for (int i = 0; i < NX; i++)
        for (int p = 0; p < 6; p++, t++) {
            int c[4][3] = { { 0, 0, 0 } };
            for (int s = 1; s < 4; s++) { memcpy(c[s], c[s - 1], sizeof c[s]); if (s <= 2) c[s][PERM[p][s - 1]] = 1; else c[s][0] = c[s][1] = c[s][2] = 1; }
            int q[4][3], v[10];
            for (int s = 0; s < 4; s++) { q[s][0] = 2 * (i + c[s][0]); q[s][1] = 2 * (j + c[s][1]); q[s][2] = 2 * (k + c[s][2]); v[s] = lid(q[s][0], q[s][1], q[s][2]); }
            for (int e = 0; e < 6; e++) { int a = E[e][0], b = E[e][1]; v[4 + e] = lid((q[a][0] + q[b][0]) / 2, (q[a][1] + q[b][1]) / 2, (q[a][2] + q[b][2]) / 2); }
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
    fprintf(f, "faces 0\ngroups 0\n");
    fclose(f); free(tet); free(used);
}

static FeMesh M;
static char G[6][64];                           /* the faces: x = 0, x = L, y = 0, y = H, z = 0, z = H */
static const char *X0, *XL, *Y0, *YH, *Z0, *ZH;
static void find_faces(void)
{
    double where[6][2] = { { 0, 0 }, { 0, LX }, { 1, 0 }, { 1, LY }, { 2, 0 }, { 2, LZ } };
    for (int i = 0; i < 6; i++) for (int g = 0; g < M.ng; g++)
        if (fabs(M.gcen[3 * g + (int)where[i][0]] - where[i][1]) < 1e-6 && M.gplanar[g]) pl_strlcpy(G[i], M.gname[g], 64);
    X0 = G[0]; XL = G[1]; Y0 = G[2]; YH = G[3]; Z0 = G[4]; ZH = G[5];
}

static void fix(FeStudy *s, int kind, const char *face) { FeFixture *f = &s->fix[s->nFix++]; memset(f, 0, sizeof *f); f->kind = kind; f->nf = 1; pl_strlcpy(f->face[0], face, 64); }
static void load(FeStudy *s, int kind, const char *face, double v, int dir) { FeLoad *l = &s->load[s->nLoad++]; memset(l, 0, sizeof *l); l->kind = kind; l->value = v; l->dir = dir; if (face) { l->nf = 1; pl_strlcpy(l->face[0], face, 64); } }
static void therm(FeStudy *s, int kind, const char *face, double v, double v2) { FeThermal *t = &s->th[s->nTh++]; memset(t, 0, sizeof *t); t->kind = kind; t->value = v; t->value2 = v2; if (face) { t->nf = 1; pl_strlcpy(t->face[0], face, 64); } }

static char Dir[PL_PATH];
static bool run(const FeStudy *s, const char *name)
{
    char d[PL_PATH]; pl_join(d, sizeof d, Dir, name);
    pl_remove_tree(d); pl_mkdirs(d);
    char err[300];
    if (!fe_study_write(s, &M, d, err, sizeof err)) { printf("FAIL  %s: %s\n", name, err); fails++; return false; }
    char cmd[PL_PATH * 2]; snprintf(cmd, sizeof cmd, "cd '%s' && OMP_NUM_THREADS=2 ccx study > log.ccx 2>&1", d);
    if (system(cmd)) { printf("FAIL  %s: ccx failed (%s/log.ccx)\n", name, d); fails++; return false; }
    return true;
}

/* every value block of a name in a .frd: f(time, node, values) for each node of each increment */
typedef void (*BlockFn)(double t, int node, const double *v, int nv, void *u);
static int OnlyStep;                            /* > 0: blocks of that step only (step 1 of a dynamic study is its modes) */
static void frd_blocks(const char *name, const char *block, BlockFn fn, void *u)
{
    char p[PL_PATH], dp[PL_PATH]; pl_join(dp, sizeof dp, Dir, name); pl_join(p, sizeof p, dp, "study.frd");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[512]; double t = 0; bool in = false; int step = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "    1PSTEP", 10)) { int a, b, c; if (sscanf(line + 10, "%d %d %d", &a, &b, &c) == 3) step = c; continue; }
        if (!strncmp(line, "  100C", 6)) { char a[32], b[32]; sscanf(line, "%31s %31s %lf", a, b, &t); continue; }
        if (!strncmp(line, " -4", 3)) { char nm[32]; sscanf(line + 3, "%31s", nm); in = !strcmp(nm, block) && (!OnlyStep || step == OnlyStep); continue; }
        if (!strncmp(line, " -3", 3)) { in = false; continue; }
        if (in && !strncmp(line, " -1", 3)) {
            char buf[16]; memcpy(buf, line + 3, 10); buf[10] = 0;
            int node = atoi(buf); double v[6]; int nv = 0;
            for (int k = 0; k < 6; k++) { size_t at = 13 + 12 * (size_t)k; if (strlen(line) < at + 12) break; memcpy(buf, line + at, 12); buf[12] = 0; v[nv++] = atof(buf); }
            fn(t, node, v, nv, u);
        }
    }
    fclose(f);
}

static int node_at(double x, double y, double z) { for (int i = 0; i < M.n; i++) if (fabs(M.x[3 * i] - x) < 1e-6 && fabs(M.x[3 * i + 1] - y) < 1e-6 && fabs(M.x[3 * i + 2] - z) < 1e-6) return M.id[i]; return -1; }
static double xOf(int id) { for (int i = 0; i < M.n; i++) if (M.id[i] == id) return M.x[3 * i]; return -1; }

typedef struct { double sum; int n; double x; int want; double mx, tmx; } Acc;
static void at_x(double t, int node, const double *v, int nv, void *u) { (void)t; (void)nv; Acc *a = u; if (fabs(xOf(node) - a->x) < 1e-6) { a->sum += v[0]; a->n++; } }
static void node_comp(double t, int node, const double *v, int nv, void *u) { Acc *a = u; if (node == a->want && nv >= 3) { double m = fabs(v[2]); if (m > a->mx) { a->mx = m; a->tmx = t; } } }
static void node_x(double t, int node, const double *v, int nv, void *u) { (void)t; (void)nv; Acc *a = u; if (node == a->want) a->sum = v[0]; }

/* a harmonic's amplitude at the tip per frequency: DISP real, DISPI imaginary parts */
#define MAXF 4096
static double HF[MAXF], HR[MAXF], HI[MAXF]; static int nH, tipNode;
static void h_re(double t, int node, const double *v, int nv, void *u) { (void)u; if (node == tipNode && nv >= 3 && nH < MAXF) { HF[nH] = t; HR[nH] = v[2]; HI[nH] = 0; nH++; } }
static int iH;
static void h_im(double t, int node, const double *v, int nv, void *u) { (void)u; (void)t; if (node == tipNode && nv >= 3 && iH < nH) HI[iH++] = v[2]; }

/* the mean first value of a block over the nodes, at the last increment that wrote it */
static void frd_last_mean(const char *name, const char *block, Acc *a)
{
    char p[PL_PATH], dp[PL_PATH]; pl_join(dp, sizeof dp, Dir, name); pl_join(p, sizeof p, dp, "study.frd");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[512]; bool in = false; double sum = 0; int n = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, " -4", 3)) { char nm[32]; sscanf(line + 3, "%31s", nm); in = !strcmp(nm, block); if (in) { sum = 0; n = 0; } continue; }
        if (!strncmp(line, " -3", 3)) { if (in && n) { a->sum = sum / n; a->n = n; } in = false; continue; }
        if (in && !strncmp(line, " -1", 3) && strlen(line) >= 25) { char b[16]; memcpy(b, line + 13, 12); b[12] = 0; sum += atof(b); n++; }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: test_fea_an <scratch>\n"); return 2; }
    if (system("command -v ccx >/dev/null 2>&1")) { printf("SKIP: no ccx on the PATH\n"); return 77; }
    pl_join(Dir, sizeof Dir, argv[1], "fea_an"); pl_remove_tree(Dir); pl_mkdirs(Dir);
    char mp[PL_PATH], err[300]; pl_join(mp, sizeof mp, Dir, "bar.qcmesh");
    write_bar(mp);
    if (!femesh_load(mp, &M, err, sizeof err)) { printf("FAIL  %s\n", err); return 1; }
    find_faces();
    CHECK(X0[0] && XL[0] && Y0[0] && YH[0] && Z0[0] && ZH[0], "the six faces found");
    const double E = 210000, nu = 0.3, rho = 7.85e-9, I = LY * LZ * LZ * LZ / 12, A = LY * LZ;
    (void)nu;
    tipNode = node_at(LX, LY / 2, LZ / 2);
    /* ---- plan B4: what the supports leave free (no solver: the rigid motions none of them stops) */
    {
        FeStudy s; FeFree fr;
        fe_study_default(&s); fix(&s, FX_FIXED, X0); fe_free_motions(&s, &M, &fr);
        CHECK(fr.n == 0, "free motions: one fixed face, %d free (0)", fr.n);
        /* a roller on x = 0 stops x and the turns about y and z (its nodes spread over y and z): y, z and a turn about x free */
        fe_study_default(&s); fix(&s, FX_ROLLER, X0); fe_free_motions(&s, &M, &fr);
        int nrot = 0; bool xturn = false; for (int i = 0; i < fr.n; i++) if (fr.m[i].rotation) { nrot++; if (fabs(fr.m[i].axis[0]) > 0.98) xturn = true; }
        CHECK(fr.n == 3 && nrot == 1 && xturn, "free motions: one roller, %d free (3: slides along y and z, turns about x)", fr.n);
        /* rollers on three faces square to each other hold it completely (the usual quarter-model symmetry planes) */
        fe_study_default(&s); fix(&s, FX_ROLLER, X0); fix(&s, FX_ROLLER, Y0); fix(&s, FX_ROLLER, Z0); fe_free_motions(&s, &M, &fr);
        CHECK(fr.n == 0, "free motions: three square rollers, %d free (0)", fr.n);
        fe_study_default(&s); fix(&s, FX_FIXED, X0); fix(&s, FX_ROLLER, Y0); fix(&s, FX_ROLLER, Z0); fe_free_motions(&s, &M, &fr);
        CHECK(fr.n == 0, "free motions: fixed end and rollers, %d free (0)", fr.n);
        /* held in z on z = 0 (stops z and the turns about x and y) and in x on x = 0 (x and the turns about y and z): y alone free */
        fe_study_default(&s); fix(&s, FX_Z, Z0); fix(&s, FX_X, X0); fe_free_motions(&s, &M, &fr);
        CHECK(fr.n == 1 && !fr.m[0].rotation && fabs(fr.m[0].axis[1]) > 0.98, "free motions: held in z on z = 0 and in x on x = 0: %d free, sliding along y (1)", fr.n);
        FePreflight pf; fe_preflight(&s, &M, 0, &pf);
        bool says = false; for (int i = 0; i < pf.n; i++) if (pf.it[i].sev == 2 && strstr(pf.it[i].text, "free to")) says = true;
        CHECK(says, "pre-flight: says what is free and stops the run");
    }

    /* ---- nonlinear: uniaxial, symmetric rollers, 280 MPa nominal */
    {
        FeStudy s; fe_study_default(&s); s.analysis = AN_NONLINEAR; s.inc = 0.05;
        fix(&s, FX_ROLLER, X0); fix(&s, FX_ROLLER, Y0); fix(&s, FX_ROLLER, Z0);
        load(&s, LD_FORCE, XL, 280 * A, DIR_PX);
        if (run(&s, "nonlinear")) {
            double H = (400 - 250) / (0.21 - 400 / E), eps = 0;
            for (int it = 0; it < 200; it++) { double st = 280 * exp(eps); eps = st / E + (st > 250 ? (st - 250) / H : 0); }
            double want = LX * (exp(eps) - 1);
            Acc a = { 0 }; a.want = node_at(LX, 0, 0);
            frd_blocks("nonlinear", "DISP", node_x, &a);
            /* the curve is nearly flat (H / E = 0.3 %): 1 MPa of stress is 0.14 % of strain, so the end's movement is checked loosely
               and the stress itself closely: equilibrium (Cauchy stress = force / the area now, from the stretch) and the curve
               (stress against the plastic strain CalculiX reports) */
            CHECK(fabs(a.sum - want) < 0.05 * want, "nonlinear: end moves %.4f mm, by hand %.4f mm (true stress, log strain, bilinear)", a.sum, want);
            Acc sx = { 0 }, pe = { 0 }; sx.x = pe.x = -1;
            frd_last_mean("nonlinear", "STRESS", &sx); frd_last_mean("nonlinear", "PE", &pe);
            double lam = 1 + a.sum / LX, sEq = 280 * lam, sCurve = 250 + H * pe.sum;
            CHECK(fabs(sx.sum - sEq) < 0.005 * sEq, "nonlinear: stress %.2f MPa, force / the area now %.2f (NLGEOM)", sx.sum, sEq);
            CHECK(fabs(sx.sum - sCurve) < 0.005 * sCurve, "nonlinear: plastic strain %.5f on the curve gives %.2f MPa (the plastic data written)", pe.sum, sCurve);
        }
    }
    /* ---- frequency: cantilever */
    double f1fe = 0;
    {
        FeStudy s; fe_study_default(&s); s.analysis = AN_FREQUENCY; s.modes = 4;
        fix(&s, FX_FIXED, X0);
        if (run(&s, "frequency")) {
            char d[PL_PATH], lg[PL_PATH]; pl_join(d, sizeof d, Dir, "frequency"); pl_join(lg, sizeof lg, d, "log.ccx");
            CcxRun r; ccx_run_load(d, "study", lg, &r);
            double want = 1.875 * 1.875 / (2 * M_PI) * sqrt(E * I / (rho * A * pow(LX, 4)));
            f1fe = r.nModes ? r.hz[0] : 0;
            CHECK(r.nModes >= 1 && fabs(f1fe - want) < 0.04 * want, "frequency: f1 %.1f Hz, Euler-Bernoulli %.1f Hz", f1fe, want);
        }
    }
    /* ---- buckling: fixed-free under 1 N */
    {
        FeStudy s; fe_study_default(&s); s.analysis = AN_BUCKLE; s.modes = 2;
        fix(&s, FX_FIXED, X0);
        load(&s, LD_FORCE, XL, 1, DIR_MX);
        if (run(&s, "buckling")) {
            char p[PL_PATH], d[PL_PATH]; pl_join(d, sizeof d, Dir, "buckling"); pl_join(p, sizeof p, d, "study.dat");
            char *t = pl_read_file(p, NULL); double bf = 0;
            const char *q = t ? strstr(t, "B U C K L I N G   F A C T O R") : NULL;
            if (q) { q = strstr(q, "\n      1 "); if (q) sscanf(q, " %*d %lf", &bf); }
            free(t);
            double want = M_PI * M_PI * E * I / (4 * LX * LX);
            CHECK(fabs(bf - want) < 0.05 * want, "buckling: factor %.0f, Euler %.0f", bf, want);
        }
    }
    /* ---- heat */
    {
        FeStudy s; fe_study_default(&s); s.analysis = AN_HEAT;
        therm(&s, TH_TEMP, X0, 400, 0); therm(&s, TH_TEMP, XL, 300, 0);
        if (run(&s, "heat_ends")) { Acc a = { 0 }; a.x = 50; frd_blocks("heat_ends", "NDTEMP", at_x, &a); double T = a.n ? a.sum / a.n : 0;
            CHECK(fabs(T - 350) < 0.5, "heat: ends 400 and 300 K, %.3f K midway (350)", T); }
        fe_study_default(&s); s.analysis = AN_HEAT;
        therm(&s, TH_TEMP, X0, 400, 0); therm(&s, TH_CONVECT, XL, 500, 300);
        if (run(&s, "heat_film")) { Acc a = { 0 }; a.x = LX; frd_blocks("heat_film", "NDTEMP", at_x, &a); double T = a.n ? a.sum / a.n : 0;
            CHECK(fabs(T - 350) < 1.75, "heat: convection h 500 W/m2K to 300 K, cooled end %.3f K (350)", T); }
        fe_study_default(&s); s.analysis = AN_HEAT;
        therm(&s, TH_FLUX, X0, 1e5, 0); therm(&s, TH_TEMP, XL, 300, 0);
        if (run(&s, "heat_flux")) { Acc a = { 0 }; a.x = 0; frd_blocks("heat_flux", "NDTEMP", at_x, &a); double T = a.n ? a.sum / a.n : 0;
            CHECK(fabs(T - 500) < 2.5, "heat: 1e5 W/m2 in, heated end %.3f K (500)", T); }
        fe_study_default(&s); s.analysis = AN_HEAT;
        therm(&s, TH_HEATGEN, NULL, 1, 0); therm(&s, TH_TEMP, X0, 300, 0); therm(&s, TH_TEMP, XL, 300, 0);
        if (run(&s, "heat_gen")) { Acc a = { 0 }; a.x = 50; frd_blocks("heat_gen", "NDTEMP", at_x, &a); double T = a.n ? a.sum / a.n : 0;
            CHECK(fabs(T - 302.5) < 0.02, "heat: 1 W made inside, %.4f K midway (302.5)", T); }
    }
    /* ---- thermal stress: held axially, rollers on the symmetry planes, 100 K warmer */
    {
        FeStudy s; fe_study_default(&s); s.analysis = AN_THERMSTRESS; s.T0 = 300;
        fix(&s, FX_ROLLER, X0); fix(&s, FX_ROLLER, XL); fix(&s, FX_ROLLER, Y0); fix(&s, FX_ROLLER, Z0);
        therm(&s, TH_TEMP, X0, 400, 0); therm(&s, TH_TEMP, XL, 400, 0);
        if (run(&s, "thermal")) {
            char d[PL_PATH], p[PL_PATH]; pl_join(d, sizeof d, Dir, "thermal"); pl_join(p, sizeof p, d, "study.frd");
            CcxFrd r; double vm = ccx_frd_load(p, &r) ? r.vmMax : 0;
            if (vm) ccx_frd_free(&r);
            CHECK(fabs(vm - 252) < 0.02 * 252, "thermal stress: von Mises %.1f MPa, E alpha dT 252", vm);
        }
    }
    /* ---- dynamic: a step load on the cantilever's tip */
    double dStat = 100 * pow(LX, 3) / (3 * E * I);
    {
        FeStudy s; fe_study_default(&s); s.analysis = AN_DYNAMIC; s.modes = 6; s.damping = 0.001; s.amp = AMP_STEP; s.time = 0.004; s.inc = 5e-6;
        fix(&s, FX_FIXED, X0);
        load(&s, LD_FORCE, XL, 100, DIR_MZ);
        if (run(&s, "dynamic")) {
            Acc a = { 0 }; a.want = tipNode;
            OnlyStep = 2; frd_blocks("dynamic", "DISP", node_comp, &a); OnlyStep = 0;
            CHECK(fabs(a.mx - 2 * dStat) < 0.10 * 2 * dStat, "dynamic: step load, tip peaks at %.4f mm (t %.2f ms), twice static %.4f", a.mx, a.tmx * 1e3, 2 * dStat);
        }
    }
    /* ---- harmonic: the same load shaking, 2 % damping */
    {
        FeStudy s; fe_study_default(&s); s.analysis = AN_HARMONIC; s.modes = 4; s.damping = 0.02; s.fLo = 300; s.fHi = 1500; s.nPts = 40;
        fix(&s, FX_FIXED, X0);
        load(&s, LD_FORCE, XL, 100, DIR_MZ);
        if (run(&s, "harmonic")) {
            nH = 0; iH = 0;
            OnlyStep = 2;
            frd_blocks("harmonic", "DISP", h_re, NULL);
            frd_blocks("harmonic", "DISPI", h_im, NULL);
            OnlyStep = 0;
            double pk = 0, fpk = 0;
            for (int i = 0; i < nH; i++) { double a = hypot(HR[i], HI[i]); if (a > pk) { pk = a; fpk = HF[i]; } }
            double want = dStat / (2 * 0.02);
            CHECK(f1fe > 0 && fabs(fpk - f1fe) < 0.03 * f1fe, "harmonic: peak at %.1f Hz, the first frequency %.1f Hz (%d points)", fpk, f1fe, nH);
            CHECK(fabs(pk - want) < 0.25 * want, "harmonic: peak tip amplitude %.3f mm, static / (2 zeta) %.3f", pk, want);
        }
    }
    /* ---- gravity: the cantilever under its own weight, q L^4 / (8 E I) with q = rho g A */
    {
        FeStudy s; fe_study_default(&s); s.analysis = AN_STATIC;
        fix(&s, FX_FIXED, X0);
        load(&s, LD_GRAVITY, NULL, 9.81, DIR_MZ);
        if (run(&s, "gravity")) {
            Acc a = { 0 }; a.want = tipNode; frd_blocks("gravity", "DISP", node_comp, &a);
            double want = rho * 9810 * A * pow(LX, 4) / (8 * E * I);
            CHECK(fabs(a.mx - want) < 0.03 * want, "gravity: tip %.4e mm under its weight, q L^4 / 8 E I %.4e", a.mx, want);
        }
    }
    /* ---- rotation: spun about z through x = 0, free to stretch (rollers): rho w^2 L^3 / (3 E) */
    {
        FeStudy s; fe_study_default(&s); s.analysis = AN_STATIC;
        fix(&s, FX_ROLLER, X0); fix(&s, FX_ROLLER, Y0); fix(&s, FX_ROLLER, Z0);
        double w = 10000; load(&s, LD_ROTATION, NULL, w * 60 / (2 * M_PI), DIR_PZ);
        if (run(&s, "rotation")) {
            Acc a = { 0 }; a.want = node_at(LX, 0, 0); frd_blocks("rotation", "DISP", node_x, &a);
            double want = rho * w * w * pow(LX, 3) / (3 * E);
            CHECK(fabs(a.sum - want) < 0.03 * want, "rotation: end moves %.4e mm at %.0f rpm, rho w^2 L^3 / 3 E %.4e", a.sum, w * 60 / (2 * M_PI), want);
        }
    }
    /* ---- a moved support: x = L pulled 0.1 mm, x = 0 on a roller: E d / L = 210 MPa */
    {
        FeStudy s; fe_study_default(&s); s.analysis = AN_STATIC;
        fix(&s, FX_ROLLER, X0); fix(&s, FX_ROLLER, Y0); fix(&s, FX_ROLLER, Z0);
        FeFixture *f = &s.fix[s.nFix++]; memset(f, 0, sizeof *f); f->kind = FX_DISP; f->value = 0.1; f->dir = DIR_PX; f->nf = 1; pl_strlcpy(f->face[0], XL, 64);
        if (run(&s, "displacement")) {
            char d[PL_PATH], p[PL_PATH]; pl_join(d, sizeof d, Dir, "displacement"); pl_join(p, sizeof p, d, "study.frd");
            CcxFrd r; double vm = ccx_frd_load(p, &r) ? r.vmMax : 0; if (vm) ccx_frd_free(&r);
            CHECK(fabs(vm - 210) < 0.01 * 210, "moved support: von Mises %.2f MPa, E d / L 210", vm);
        }
    }
    /* ---- rollers off the axes (an equation per node): the bar turned 30 degrees about z, pulled along itself */
    {
        FeMesh keep = M;
        double *xs = malloc(sizeof(double) * 3 * (size_t)M.n), c = cos(M_PI / 6), sn = sin(M_PI / 6);
        memcpy(xs, M.x, sizeof(double) * 3 * (size_t)M.n);
        for (int i = 0; i < M.n; i++) { double x = M.x[3 * i], y = M.x[3 * i + 1]; M.x[3 * i] = c * x - sn * y; M.x[3 * i + 1] = sn * x + c * y; }
        for (int g = 0; g < M.ng; g++) { double nx = M.gnorm[3 * g], ny = M.gnorm[3 * g + 1]; M.gnorm[3 * g] = c * nx - sn * ny; M.gnorm[3 * g + 1] = sn * nx + c * ny; }
        FeStudy s; fe_study_default(&s); s.analysis = AN_STATIC;
        fix(&s, FX_ROLLER, X0); fix(&s, FX_ROLLER, Y0); fix(&s, FX_ROLLER, Z0);
        load(&s, LD_FORCE, XL, 210 * A, DIR_NORMAL);           /* normal: into the face, so -2.1e4 N: compression */
        if (run(&s, "tilted")) {
            char d[PL_PATH], p[PL_PATH], *t; pl_join(d, sizeof d, Dir, "tilted"); pl_join(p, sizeof p, d, "study.inp");
            t = pl_read_file(p, NULL); bool eq = t && strstr(t, "*EQUATION"); free(t);
            pl_join(p, sizeof p, d, "study.frd");
            CcxFrd r; double vm = ccx_frd_load(p, &r) ? r.vmMax : 0; if (vm) ccx_frd_free(&r);
            CHECK(eq && fabs(vm - 210) < 0.01 * 210, "tilted rollers (*EQUATION): von Mises %.2f MPa, F / A 210", vm);
        }
        memcpy(M.x, xs, sizeof(double) * 3 * (size_t)M.n); free(xs);
        for (int g = 0; g < M.ng; g++) { double nx = M.gnorm[3 * g], ny = M.gnorm[3 * g + 1]; M.gnorm[3 * g] = c * nx + sn * ny; M.gnorm[3 * g + 1] = -sn * nx + c * ny; }
        (void)keep;
    }
    femesh_free(&M);
    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
