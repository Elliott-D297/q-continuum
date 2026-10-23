/* test_units.c -- pure logic against hand-worked values: decomposition splits, graded cell sizes, blockMeshDict geometry,
 * Courant and Fourier numbers and their working, ETA, the error catalogue, path handling and the deletion guards.
 *
 *   test_units <scratch dir>
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "explain.h"
#include "foamcase.h"
#include "physics.h"
#include "coupling.h"
#include "fieldinfo.h"
#include "redalert.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (tol) * fmax(1.0, fabs((double)(b))))

static const char *CAVITY_BMD =
    "FoamFile { version 2.0; format ascii; class dictionary; object blockMeshDict; }\n"
    "scale 0.1;\n"
    "vertices ( (0 0 0) (1 0 0) (1 1 0) (0 1 0) (0 0 0.1) (1 0 0.1) (1 1 0.1) (0 1 0.1) );\n"
    "blocks ( hex (0 1 2 3 4 5 6 7) (20 20 1) simpleGrading (1 1 1) );\n"
    "boundary ( );\n";

static void test_decomp(void)
{
    struct { long np; int dirs; long want[3]; } T[] = {
        { 1, 3, { 1, 1, 1 } }, { 4, 3, { 2, 2, 1 } }, { 4, 2, { 2, 2, 1 } }, { 7, 3, { 7, 1, 1 } }, { 12, 3, { 3, 2, 2 } },
        { 9, 3, { 3, 3, 1 } }, { 16, 2, { 4, 4, 1 } }, { 6, 2, { 3, 2, 1 } } };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        long d[3]; decomp_split_n(T[i].np, T[i].dirs, d);
        CHECK(d[0] == T[i].want[0] && d[1] == T[i].want[1] && d[2] == T[i].want[2], "split %ld (%dD) = (%ld %ld %ld)", T[i].np, T[i].dirs, d[0], d[1], d[2]);
    }
    for (long np = 1; np <= 1024; np++) {                  /* always a split of exactly np; 2-D never splits z */
        long d[3]; decomp_split_n(np, 2, d);
        if (d[0] * d[1] * d[2] != np || d[2] != 1) { CHECK(0, "split %ld (2D) = (%ld %ld %ld)", np, d[0], d[1], d[2]); break; }
        decomp_split_n(np, 3, d);
        if (d[0] * d[1] * d[2] != np) { CHECK(0, "split %ld (3D) = (%ld %ld %ld)", np, d[0], d[1], d[2]); break; }
    }
}

static void test_grading(void)
{
    CHECK(NEAR(graded_min_cell(1, 10, 1), 0.1, 1e-12), "uniform 10 cells over 1 m");
    for (double r = 0.05; r < 30; r *= 1.7) {               /* the progression adds up to L and has last/first = r */
        int n = 15; double L = 0.3;
        double q = pow(r, 1.0 / (n - 1)), first = L * (q - 1) / (pow(q, n) - 1), sum = 0;
        for (int i = 0; i < n; i++) sum += first * pow(q, i);
        CHECK(NEAR(sum, L, 1e-12), "graded cells sum to %g, not %g (r %g)", sum, L, r);
        double m = graded_min_cell(L, n, r), want = fmin(first, first * pow(q, n - 1));
        CHECK(NEAR(m, want, 1e-12), "smallest cell %g, want %g (r %g)", m, want, r);
    }
    CHECK(graded_min_cell(1, 1, 5) == 1, "one cell is the whole edge");
}

static void test_blockmesh(void)
{
    FDFile *f = fd_from_text("", CAVITY_BMD);
    BlockMeshInfo b; blockmesh_info(f, &b);
    CHECK(b.ok, "cavity blockMeshDict not read: %s", b.why);
    CHECK(b.cells == 400 && b.blocks == 1, "cavity: %ld cells in %d blocks", b.cells, b.blocks);
    CHECK(NEAR(b.hi[0], 0.1, 1e-12) && NEAR(b.hi[2], 0.01, 1e-12), "cavity box to (%g %g %g)", b.hi[0], b.hi[1], b.hi[2]);
    CHECK(b.oneCellDir == 2, "cavity is 2-D in z, got %d", b.oneCellDir);
    CHECK(NEAR(b.dxMinThin, 0.005, 1e-12), "cavity cells 0.1 m / 20 = 0.005 m, got %g", b.dxMinThin);
    fd_free(f);
    /* graded: 0.1 m in x with 10 cells and simpleGrading 4 */
    f = fd_from_text("", "vertices ( (0 0 0) (1 0 0) (1 1 0) (0 1 0) (0 0 1) (1 0 1) (1 1 1) (0 1 1) ); convertToMeters 0.1;\n"
                         "blocks ( hex (0 1 2 3 4 5 6 7) block0 (10 10 10) simpleGrading (4 1 1) );\n");
    blockmesh_info(f, &b);
    CHECK(b.ok && b.oneCellDir == -1, "graded block: %s", b.why);
    CHECK(NEAR(b.dxMin, graded_min_cell(0.1, 10, 4), 1e-12), "graded block smallest cell %g", b.dxMin);
    fd_free(f);
    f = fd_from_text("", "vertices ( ($x0 0 0) ); blocks ( hex (0 1 2 3 4 5 6 7) (1 1 1) simpleGrading (1 1 1) );");
    blockmesh_info(f, &b);
    CHECK(!b.ok && b.why[0], "variables in the vertices must be reported, not guessed");
    fd_free(f);
}

static void test_numbers(void)
{
    CHECK(NEAR(courant(1, 0.005, 0.005), 1.0, 1e-12), "cavity Co");
    CHECK(NEAR(courant(1, 0.5, 0.005), 100, 1e-12), "J7 Co");
    char t[200];
    courant_text(1, 0.005, 0.005, t, sizeof t);
    CHECK(!strcmp(t, "Co ≈ |U|·Δt/Δx = 1.0 m/s × 0.005 s / 0.005 m = 1.0"), "Courant working: %s", t);
    courant_text(1, 0.5, 0.005, t, sizeof t);
    CHECK(strstr(t, "= 100") != NULL, "Courant working for Co 100: %s", t);
    /* stainless steel: k 16.2, rho 8000, cp 500 -> alpha 4.05e-6 m2/s; dt 1 s, dx 1 mm -> Fo 4.05 */
    CHECK(NEAR(fourier(16.2 / (8000 * 500), 1, 0.001), 4.05, 1e-9), "steel Fo");
    char ft[300]; fourier_text(16.2, 8000, 500, 1, 0.001, ft, sizeof ft);
    CHECK(strstr(ft, "= 4.05") != NULL, "Fourier working: %s", ft);
    /* 4.3 Knudsen: N2 (d 4.17e-10 m) at 300 K and 101 325 Pa: lambda = kB T / (sqrt2 pi d^2 p) = 5.29e-8 m by hand */
    CHECK(NEAR(mean_free_path(300, 101325, 4.17e-10), 5.2914e-8, 1e-11), "N2 mean free path %g", mean_free_path(300, 101325, 4.17e-10));
    /* the biconic Run 35 freestream: 22.7 Pa, 138.9 K -> 1.09e-4 m; over 1.46 mm cells Kn 0.075 = slip */
    double lb = mean_free_path(138.873, 22.7352, 4.17e-10);
    CHECK(NEAR(lb, 1.0917e-4, 2e-8), "biconic lambda %g", lb);
    CHECK(!strcmp(knudsen_regime(lb / 0.00146), "slip"), "biconic regime %s", knudsen_regime(lb / 0.00146));
    CHECK(!strcmp(knudsen_regime(0.005), "continuum") && !strcmp(knudsen_regime(0.5), "transitional") && !strcmp(knudsen_regime(20), "free molecular"), "regimes");
    const GasDiameter *gd = gas_for_molweight(28.0134);
    CHECK(gd && !strcmp(gd->name, "N2"), "molWeight 28.0134 is %s", gd ? gd->name : "nothing");
    gd = gas_for_molweight(28.96); CHECK(gd && !strcmp(gd->name, "air"), "molWeight 28.96 is %s", gd ? gd->name : "nothing");
    CHECK(gas_for_molweight(11640.3) == NULL, "non-dimensional molWeight is no gas");
    char kt[400]; knudsen_text(300, 101325, 4.17e-10, 0.001, kt, sizeof kt);
    CHECK(strstr(kt, "= 5.3e-08 m") && strstr(kt, "Kn = λ/Δx = 5.3e-08 m / 0.001 m = 5.3e-05 (continuum)"), "Knudsen working: %s", kt);
    /* bug hunt H-01/H-07: typed values that would break the file or the case are refused */
    CHECK(value_problem("endTime", "0.5", "abc", "dictionary") != NULL, "endTime abc refused");
    CHECK(value_problem("deltaT", "0.005", "0.001; startTime 5", "dictionary") != NULL, "injected entry refused");
    CHECK(value_problem("writeInterval", "20", "}", "dictionary") != NULL, "brace refused");
    CHECK(value_problem("deltaT", "0.005", "-0.005", "dictionary") != NULL, "negative deltaT refused");
    CHECK(value_problem("writeInterval", "20", "1e999", "dictionary") != NULL, "infinite refused");
    CHECK(value_problem("nCorrectors", "2", "2.5", "dictionary") != NULL, "fractional count refused");
    CHECK(value_problem("boundaryField/movingWall/value", "uniform (1 0 0)", "uniform (1 0)", "volVectorField") != NULL, "2-component vector refused");
    CHECK(value_problem("deltaT", "0.005", "0.001", "dictionary") == NULL, "deltaT 0.001 fine");
    CHECK(value_problem("deltaT", "0.005", "$dtBase", "dictionary") == NULL, "macro fine");
    CHECK(value_problem("boundaryField/movingWall/value", "uniform (1 0 0)", "uniform (2 0 0)", "volVectorField") == NULL, "vector fine");
    CHECK(value_problem("div(phi,U)", "Gauss linear", "bounded Gauss linearUpwind grad(U)", "dictionary") == NULL, "scheme with brackets fine");
    CHECK(value_problem("transportModel", "Newtonian", "\"a;b\"", "dictionary") == NULL, "quoted ';' fine");
    char h[32];
    human_num(2280, h, sizeof h); CHECK(!strcmp(h, "2280"), "2280 shows as %s", h);
    human_num(0.0025, h, sizeof h); CHECK(!strcmp(h, "0.0025"), "0.0025 shows as %s", h);
    human_num(2.36e19, h, sizeof h); CHECK(!strcmp(h, "2.4e+19"), "2.36e19 shows as %s", h);
    CHECK(NEAR(eta_seconds(0, 0.25, 0.5, 10), 10, 1e-12), "half way in 10 s: 10 s to go");
    CHECK(eta_seconds(0, 0.5, 0.5, 10) == 0, "at the end: 0 s");
    CHECK(eta_seconds(0, 0, 0.5, 10) < 0, "no progress: unknown");
}

static void write_case(const char *dir, const char *controlDict)
{
    char p[PL_PATH]; pl_join(p, sizeof p, dir, "system");
    pl_mkdirs(p);
    pl_join(p, sizeof p, dir, "system/controlDict");
    pl_write_file(p, controlDict, strlen(controlDict));
}

static void test_explain(const char *scratch)
{
    char dir[PL_PATH]; pl_join(dir, sizeof dir, scratch, "explainCase");
    write_case(dir, "FoamFile { object controlDict; }\nstartTime 0.001;\nendTime 0.02;\ndeltaT 0.001;\nwriteControl adjustable;\nwriteInterval 10;\n");
    Explanation x;
    CHECK(foam_explain(dir, "log.reconstructPar", "No times selected", &x), "'No times selected' not recognised");
    CHECK(!strcmp(x.title, "No results were written"), "title: %s", x.title);
    CHECK(strstr(x.text, "t = 0.001 + 10 = 10.001 s") != NULL, "runTime working: %s", x.text);
    CHECK(x.line == 6 && !strcmp(x.file, "system/controlDict"), "points to %s:%d", x.file, x.line);
    write_case(dir, "startTime 0;\nendTime 1;\ndeltaT 0.005;\nwriteControl timeStep;\nwriteInterval 400;\n");
    foam_explain(dir, "log.reconstructPar", "No times selected", &x);
    CHECK(strstr(x.text, "400 steps × deltaT 0.005 s = t = 2 s") && strstr(x.text, "at most 200 steps"), "timeStep working: %s", x.text);
    CHECK(!foam_explain(dir, "log.x", "Something else entirely", &x), "an unknown message must not be explained");
    /* 1.10 disk full: OpenFOAM only says the stream failed */
    char msg[PL_PATH + 200];
    snprintf(msg, sizeof msg, "error in IOstream \"%s/constant/polyMesh/faces\" for operation Foam::Ostream& Foam::operator<<(Ostream&, int32_t)", dir);
    CHECK(foam_explain(dir, "log.blockMesh", msg, &x), "a failed write not recognised");
    CHECK(!strcmp(x.title, "Writing constant/polyMesh/faces failed"), "title: %s", x.title);
    CHECK(strstr(x.text, "disk is full") && strstr(x.text, "free"), "text: %s", x.text);
    CHECK(!foam_explain(dir, "log.x", "error in IOstream \"x\" for operation Foam::Istream& operator>>", &x), "a read error is not a full disk");
}

static void test_paths(const char *scratch)
{
    CHECK(foam_path_problem("/home/u/my case") != NULL, "a space must be refused");
    CHECK(foam_path_problem("/home/u/it's") != NULL, "a quote must be refused");
    CHECK(foam_path_problem("/home/u/cas$e") != NULL, "a $ must be refused");
    CHECK(foam_path_problem("/mnt/c/Users/u/OpenFOAM/cavity") == NULL, "/mnt/c is a valid case path");
    CHECK(foam_path_problem("/home/u/régolithe_ç") == NULL, "UTF-8 letters are fine");
    char j[64];
    pl_join(j, sizeof j, "a", "b");   CHECK(!strcmp(j, "a/b"), "join a b: %s", j);
    pl_join(j, sizeof j, "a/", "b");  CHECK(!strcmp(j, "a/b"), "join a/ b: %s", j);
    pl_join(j, sizeof j, "", "b");    CHECK(!strcmp(j, "b"), "join '' b: %s", j);
    pl_join(j, sizeof j, "a", NULL);  CHECK(!strcmp(j, "a/"), "join a NULL: %s", j);
    /* a case reached through a symlink is a case; deleting the link removes the link, not the case behind it */
    char real[PL_PATH], link[PL_PATH], cd[PL_PATH];
    pl_join(real, sizeof real, scratch, "realCase");
    pl_join(link, sizeof link, scratch, "linkedCase");
    write_case(real, "endTime 1;\n");
    unlink(link);
    CHECK(symlink(real, link) == 0, "could not make the symlink");
    CHECK(case_is_case(link), "a symlinked case is not recognised");
    CHECK(pl_remove_tree(link) == 0, "removing the link failed");
    pl_join(cd, sizeof cd, real, "system/controlDict");
    CHECK(pl_is_file(cd) && !pl_exists(link), "removing a symlinked case deleted what it points to");
    /* the guards */
    CHECK(pl_remove_tree("/") != 0, "rm -rf / must be refused");
    CHECK(pl_remove_tree(pl_home()) != 0, "rm -rf $HOME must be refused");
    CHECK(pl_is_dir(pl_home()), "home is still there");
}

static void test_log_tail(const char *scratch)
{
    char dir[PL_PATH]; pl_join(dir, sizeof dir, scratch, "tailCase");
    pl_mkdirs(dir);
    char p[PL_PATH]; pl_join(p, sizeof p, dir, "log.icoFoam");
    const char *log = "Time = 0.005\n\nCourant Number mean: 0 max: 0\nTime = 0.01\n\nExecutionTime = 0 s\n\nEnd\n";
    pl_write_file(p, log, strlen(log));
    double t; bool ended, conv;
    CHECK(log_tail_state(dir, "log.icoFoam", &t, &ended, &conv) && NEAR(t, 0.01, 1e-12) && ended && !conv, "log tail: t %g ended %d", t, ended);
}

/* ROADMAP 4.6: the default process count never splits a small mesh below 10 000 cells per rank */
/* coupled runs: what a participant's run.sh needs (the tutorials failed a second in without ccx_preCICE, fenics or
   their downloaded meshes) */
static void put(const char *dir, const char *name, const char *text)
{
    char p[PL_PATH]; pl_join(p, sizeof p, dir, name);
    pl_write_file(p, text, strlen(text));
}
static void test_coupling_missing(const char *scratch)
{
    char d[PL_PATH]; pl_join(d, sizeof d, scratch, "cplSolid");
    pl_mkdirs(d);
    char am[PL_PATH]; pl_join(am, sizeof am, d, "all.msh"); unlink(am);   /* a fresh start: the last run made it */
    char why[600];
    put(d, "run.sh", "#!/bin/bash\nset -e -u\n. ../../tools/log.sh\nexec > >(tee --append \"$LOGFILE\") 2>&1\n"
                     "if [ ! -f all.msh ]; then\n    echo missing\n    exit\nfi\nexport OMP_NUM_THREADS=1\nqc_no_such_solver_xyz -i solid -precice-participant Solid\nclose_log\n");
    int n = coupling_missing(d, NULL, why, sizeof why);
    CHECK(n == 2 && strstr(why, "qc_no_such_solver_xyz is not installed") && strstr(why, "all.msh is missing"), "ccx-style run.sh: %d [%s]", n, why);
    CHECK(!strstr(why, "1 is not"), "a redirection taken for a command: [%s]", why);
    put(d, "all.msh", "x\n");
    put(d, "run.sh", "#!/bin/bash\nsh -c true && ls >/dev/null 2>&1\n");
    CHECK(coupling_missing(d, NULL, why, sizeof why) == 0, "installed commands: [%s]", why);
    put(d, "solid.py", "import os, sys\nfrom qc_no_such_module_xyz import Adapter\nimport json as j\n");
    put(d, "run.sh", "#!/bin/bash\npython3 solid.py\n");
    n = coupling_missing(d, NULL, why, sizeof why);         /* python3 answers in the background: ask until it has */
    for (int t = 0; t < 100 && n == 0; t++) { usleep(50000); n = coupling_missing(d, NULL, why, sizeof why); }
    CHECK(n == 1 && strstr(why, "qc_no_such_module_xyz") && !strstr(why, "json") && !strstr(why, " os"), "python imports: %d [%s]", n, why);
    CHECK(coupling_missing(d, "sh -c true", why, sizeof why) == 0, "a custom command is checked instead of run.sh: [%s]", why);
}

static void test_procs_for_cells(void)
{
    struct { long want, cells, got; } T[] = {
        { 4, 400, 1 },            /* cavity: was 4 (or 9) ranks of 44-100 cells */
        { 4, 12225, 1 }, { 4, 25000, 2 }, { 4, 40000, 4 }, { 4, 354000, 4 },   /* pitzDaily, ..., motorBike */
        { 8, 0, 8 }, { 8, -1, 8 },  /* cell count unknown: left alone */
        { 0, 400, 1 }, { 1, 5, 1 } };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        long n = procs_for_cells(T[i].want, T[i].cells, 10000);
        CHECK(n == T[i].got, "procs_for_cells(%ld, %ld) = %ld, want %ld", T[i].want, T[i].cells, n, T[i].got);
    }
}


/* 2.5: units from dimensions, the turbulence model's fields, the tutorials' initial values */
static void test_fieldinfo(const char *scratch)
{
    char u[64];
    struct { const char *d, *u; } D[] = {
        { "[0 1 -1 0 0 0 0]", "m/s" }, { "[0 2 -3 0 0 0 0]", "m²/s³" }, { "[0 0 -1 0 0 0 0]", "1/s" },
        { "[0 2 -2 0 0 0 0]", "m²/s²" }, { "[1 -1 -2 0 0 0 0]", "Pa" }, { "[0 0 0 1 0 0 0]", "K" },
        { "[0 2 -1 0 0 0 0]", "m²/s" }, { "[1 -1 -1 0 0 0 0]", "Pa·s" }, { "[0 0 0 0 0 0 0]", "dimensionless" },
        { "[1 0 -3 0 0 0 0]", "W/m²" }, { "[1 1 -3 -1 0 0 0]", "W/(m·K)" }, { "[0 1 -2 0 0]", "m/s²" },
        { "[1 -3 0 0 0 0 0]", "kg/m³" }, { "[0 -1 0 0 0 0 0]", "1/m" }, { "[1 0 -1 -1 0 0 0]", "kg/(s·K)" } };
    for (size_t i = 0; i < sizeof D / sizeof D[0]; i++) {
        CHECK(dims_unit(D[i].d, u, sizeof u) && !strcmp(u, D[i].u), "dims_unit %s = '%s', want '%s'", D[i].d, u, D[i].u);
    }
    CHECK(!dims_unit("[0 0.5 0 0 0 0 0]", u, sizeof u), "fractional exponents are left alone");
    CHECK(!dims_unit("uniform 0", u, sizeof u), "no dimension set");
    CHECK(field_info("U") && field_info("epsilon") && field_info("alpha.water") && !field_info("zzz"), "field_info lookups");
    CHECK(turb_field_used("omega", "RAS", "kEpsilon") == 0, "omega unused by kEpsilon");
    CHECK(turb_field_used("epsilon", "RAS", "kEpsilon") == 1, "epsilon used by kEpsilon");
    CHECK(turb_field_used("omega", "RAS", "kOmegaSST") == 1, "omega used by kOmegaSST");
    CHECK(turb_field_used("nuTilda", "RAS", "kOmegaSST") == 0, "nuTilda unused by kOmegaSST");
    CHECK(turb_field_used("nuTilda", "RAS", "SpalartAllmaras") == 1, "nuTilda used by SA");
    CHECK(turb_field_used("k", "laminar", "") == 0, "k unused when laminar");
    CHECK(turb_field_used("k", "LES", "dynamicKEqn") == 1, "k used by dynamicKEqn");
    CHECK(turb_field_used("epsilon", "LES", "dynamicKEqn") == 0, "dynamicKEqn is not k-epsilon");
    CHECK(turb_field_used("U", "RAS", "kEpsilon") == -1, "U is not a turbulence field");
    CHECK(turb_field_used("ReThetat", "RAS", "kOmegaSSTLM") == 1, "kOmegaSSTLM transition fields");
    /* a tiny tutorials tree: two cases, a region folder, a macro value and a nonuniform list (both skipped) */
    char t[PL_PATH], p[PL_PATH], out[PL_PATH];
    pl_join(t, sizeof t, scratch, "tuts");
    pl_remove_tree(t);
    const char *H = "FoamFile { class %s; object %s; }\ndimensions [0 1 -1 0 0 0 0];\n\ninternalField   %s;\n";
    struct { const char *rel, *cls, *val; } F[] = {
        { "a/c1/0/U", "volVectorField", "uniform (3 4 0)" }, { "a/c1/0/k", "volScalarField", "uniform 0.375" },
        { "b/c2/0.orig/k", "volScalarField", "uniform 0.1" }, { "b/c2/0.orig/solid/T", "volScalarField", "uniform 300" },
        { "b/c3/0/k", "volScalarField", "uniform $kInit" }, { "b/c3/0/T", "volScalarField", "nonuniform List<scalar> 2(1 2)" },
        { "b/c3/0/k.orig", "volScalarField", "uniform 99" }, { "b/c3/system/k", "volScalarField", "uniform 77" } };
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) {
        pl_join(p, sizeof p, t, F[i].rel);
        char d[PL_PATH]; pl_dirname(d, sizeof d, p); pl_mkdirs(d);
        char body[400]; snprintf(body, sizeof body, H, F[i].cls, pl_basename(p), F[i].val);
        pl_write_file(p, body, strlen(body));
    }
    pl_join(out, sizeof out, scratch, "tut-ranges.txt");
    int n = tut_ranges_scan(t, out);
    CHECK(n == 3, "tutorial scan found %d fields, want 3 (U, k, T)", n);
    tut_ranges_start(t, out);                      /* fresh cache: no rescan, just read */
    TutRange r;
    CHECK(tut_range("k", &r) && r.n == 2 && NEAR(r.min, 0.1, 1e-9) && NEAR(r.max, 0.375, 1e-9), "k range n=%d %g..%g", r.n, r.min, r.max);
    CHECK(tut_range("U", &r) && r.n == 1 && NEAR(r.median, 5, 1e-9), "U by magnitude: %g", r.median);
    CHECK(tut_range("T", &r) && r.n == 1 && NEAR(r.min, 300, 1e-9), "T from the region folder");
    CHECK(!tut_range("p", &r), "no p in the tree");
}

/* 3.3: RED ALERT over real logs (cut before their end, as if still running) and synthetic diverging ones */
static int alert_on_text(const char *text, bool adjust, double maxCo, Alert *a)
{
    LogMon m; memset(&m, 0, sizeof m);
    logmon_feed(&m, text, (int)strlen(text));
    int l = red_alert_check(&m, adjust, maxCo, a);
    logmon_reset(&m);
    return l;
}

static char *log_cut(const char *name)
{
    char p[PL_PATH]; snprintf(p, sizeof p, "%s/%s", QC_LOG_DIR, name);
    char *t = pl_read_file(p, NULL);
    if (!t) return NULL;
    char *e = strstr(t, "\nEnd\n"); if (e) e[1] = 0;            /* still running */
    e = strstr(t, "[stack trace]"); if (e) *e = 0;              /* the moment before the crash */
    e = strstr(t, "--> FOAM FATAL"); if (e) *e = 0;
    return t;
}

static void test_redalert(void)
{
    static const char *calm[] = { "icoFoam_cavity.log", "icoFoam_cavity_parallel4.log", "simpleFoam_pitzDaily.log", "interFoam_damBreak.log",
        "buoyantSimpleFoam_fvDOM_radiationRoom.log", "chtMultiRegionFoam_multiRegionHeater_parallel4.log", "pimpleFoam_droneProp_nOuter.log",
        "chtMultiRegionTwoPhaseEulerFoam_ln2Nozzle_parallel8.log", "continued_stopped.log" };
    Alert a;
    for (size_t i = 0; i < sizeof calm / sizeof calm[0]; i++) {
        char *t = log_cut(calm[i]);
        CHECK(t, "read %s", calm[i]);
        if (!t) continue;
        int l = alert_on_text(t, strstr(calm[i], "damBreak") || strstr(calm[i], "droneProp") || strstr(calm[i], "ln2"), 1, &a);
        CHECK(l == RA_NONE, "%s raised level %d: %s", calm[i], l, a.text);
        free(t);
    }
    char *t = log_cut("icoFoam_cavity_200x200_sigfpe.log");
    CHECK(t && alert_on_text(t, false, 0, &a) == RA_ALERT && (a.kind & RA_COURANT), "the diverging 200x200 cavity alerts on Courant: %s", t ? a.text : "");
    free(t);
    /* synthetic: residual growth, a collapsing time step, bounding */
    Str b; str_init(&b);
    for (int i = 1; i <= 40; i++) {
        double r = i < 20 ? 1e-3 * pow(0.7, i) : 1e-3 * pow(0.7, 19) * pow(3.0, i - 19);
        if (r > 0.9) r = 0.9;
        str_catf(&b, "Time = %g\n\nCourant Number mean: 0.1 max: 0.5\nsmoothSolver:  Solving for Ux, Initial residual = %g, Final residual = 1e-9, No Iterations 3\n", i * 0.01, r);
    }
    CHECK(alert_on_text(b.s, false, 0, &a) == RA_ALERT && (a.kind & RA_RESIDUAL) && strstr(a.text, "Ux"), "growing residual: %s", a.text);
    str_free(&b); str_init(&b);
    for (int i = 1; i <= 30; i++)
        str_catf(&b, "Courant Number mean: 0.1 max: 0.9\ndeltaT = %g\nTime = %g\n\nsmoothSolver:  Solving for Ux, Initial residual = 1e-4, Final residual = 1e-9, No Iterations 3\n",
                 1e-3 * pow(0.6, i), i * 0.01);
    CHECK(alert_on_text(b.s, true, 1, &a) == RA_ALERT && (a.kind & RA_DT), "collapsing deltaT: %s", a.text);
    CHECK(alert_on_text(b.s, false, 1, &a) == RA_NONE, "fixed deltaT: the dt rule doesn't apply");
    str_free(&b); str_init(&b);
    for (int i = 1; i <= 130; i++)
        str_catf(&b, "Time = %d\n\nsmoothSolver:  Solving for Ux, Initial residual = %g, Final residual = 1e-9, No Iterations 3\nbounding k, min: -0.1 max: 2 average: 1\n", i, 1e-2 * pow(0.98, i));
    CHECK(alert_on_text(b.s, false, 0, &a) == RA_WATCH && (a.kind & RA_BOUNDING) && strstr(a.text, "k was bounded"), "bounding every step: %s", a.text);
    str_free(&b);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: test_units <scratch dir>\n"); return 2; }
    char base[PL_PATH], scratch[PL_PATH];
    pl_mkdirs(argv[1]);
    if (!realpath(argv[1], base)) { printf("bad scratch dir %s\n", argv[1]); return 2; }   /* absolute: the symlink test needs it */
    pl_join(scratch, sizeof scratch, base, "units");
    pl_mkdirs(scratch);
    test_decomp(); test_grading(); test_blockmesh(); test_numbers(); test_explain(scratch); test_paths(scratch); test_log_tail(scratch); test_procs_for_cells(); test_coupling_missing(scratch); test_fieldinfo(scratch); test_redalert();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "passed", checks, fails);
    return fails ? 1 : 0;
}
