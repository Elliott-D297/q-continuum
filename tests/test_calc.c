/* test_calc.c -- the calculators (mesh overhaul plan step 2, calc.c) against the hand-worked examples of
   docs/CALCULATORS.md, worked independently (by hand and in a separate script), and against the app's other users of the
   same formulas: the y+ helper on Physics > Setup (sim_wall_spacing) and the Run page's memory check (precheck.c).
     test_calc */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "calc.h"
#include "simsetup.h"
#include "working.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)
#define NEAR(a, b, rel, what) CHECK(fabs((double)(a) - (double)(b)) <= (rel) * fabs((double)(b)), "%s: %.6g, expected %.6g", what, (double)(a), (double)(b))

static void wall(void)
{
    /* example 1: air at 10 m/s along a 1 m plate, y+ = 1, growth 1.2 */
    CalcWallIn in = { .flow = CW_PLATE, .rho = 1.204, .mu = 1.825e-5, .U = 10, .L = 1, .yplus = 1, .r = 1.2 };
    CalcWallOut o; Working wk;
    CHECK(calc_wall(&in, &o, &wk), "wall: plate");
    NEAR(o.Re, 659726, 1e-5, "plate Re");
    NEAR(o.Cf, 3.83386e-3, 1e-4, "plate Cf");
    NEAR(o.tauw, 0.230798, 1e-4, "plate tau_w");
    NEAR(o.utau, 0.437827, 1e-4, "plate u_tau");
    NEAR(o.y, 34.6205e-6, 1e-4, "plate y");
    NEAR(o.h1, 69.241e-6, 1e-4, "plate h1");
    NEAR(o.delta, 0.0253705, 1e-4, "plate delta");
    CHECK(o.nCover == 24, "plate nCover %d, expected 24", o.nCover);
    NEAR(o.tCover, 0.027176, 1e-4, "plate tCover");
    CHECK(!o.laminar, "plate: turbulent");
    char t[4096]; wk_text(&wk, t, sizeof t);
    CHECK(strstr(t, "Cf = 0.026") && strstr(t, "u_τ"), "plate working text");
    /* the same as Physics > Setup's helper (which takes the first cell as 2 y too) */
    SimSetup s; memset(&s, 0, sizeof s); s.nu = in.mu / in.rho;
    double h1, d, ut, Re = sim_wall_spacing(&s, in.U, in.L, 1, &h1, &d, &ut);
    NEAR(o.Re, Re, 1e-9, "plate Re = sim_wall_spacing"); NEAR(o.h1, h1, 1e-9, "plate h1 = sim_wall_spacing");
    NEAR(o.delta, d, 1e-9, "plate delta = sim_wall_spacing"); NEAR(o.utau, ut, 1e-9, "plate u_tau = sim_wall_spacing");

    /* blending into 2 mm surface cells: layers until the outer one reaches 0.3 × 2 mm = 0.6 mm */
    in.cell = 0.002;
    calc_wall(&in, &o, NULL);
    double outer = o.h1; int n = 1;
    while (outer < 0.6e-3) { outer *= 1.2; n++; }
    CHECK(o.nBlend == n && n == 13, "plate nBlend %d, expected %d (13)", o.nBlend, n);

    /* layers that don't grow never blend into larger cells */
    CalcWallIn flat = in; flat.r = 1; calc_wall(&flat, &o, NULL);
    CHECK(o.nBlend == 0, "r = 1: no blend count (%d)", o.nBlend);

    /* example 2: liquid nitrogen at 2 m/s in a smooth 25 mm pipe, y+ = 1 */
    const CalcFluid *f = &CALC_FLUID[CF_LN2];
    CalcWallIn p = { .flow = CW_PIPE, .rho = f->rho, .mu = f->mu, .U = 2, .L = 0.025, .yplus = 1, .r = 1.2 };
    CHECK(calc_wall(&p, &o, &wk), "wall: pipe");
    NEAR(o.Re, 250850, 1e-4, "ln2 Re");
    NEAR(o.f, 0.0148395, 1e-4, "ln2 f (Haaland)");
    NEAR(o.tauw, 5.98089, 1e-4, "ln2 tau_w");
    NEAR(o.y, 2.31400e-6, 1e-4, "ln2 y");
    NEAR(o.delta, 0.0125, 1e-9, "ln2 radius");
    /* Haaland vs the Colebrook equation itself (iterated), smooth pipe: within 2 % */
    double cf = 0.02;
    for (int i = 0; i < 50; i++) { double t2 = -2 * log10(2.51 / (o.Re * sqrt(cf))); cf = 1 / (t2 * t2); }
    NEAR(o.f, cf, 0.02, "Haaland ~ Colebrook");
    /* laminar pipe */
    p.U = 0.001; calc_wall(&p, &o, NULL);
    CHECK(o.laminar && fabs(o.f - 64 / o.Re) < 1e-12, "laminar pipe f = 64/Re");
    /* missing input */
    CalcWallIn bad = in; bad.U = 0;
    CHECK(!calc_wall(&bad, &o, &wk) && o.h1 == 0, "wall: no speed -> false");
}

static void grading(void)
{
    CalcGradeOut o; Working wk;
    /* example 3a: 0.1 m, 20 cells, R = 10 */
    CalcGradeIn a = { .mode = CG_N_RATIO, .L = 0.1, .N = 20, .R = 10 };
    CHECK(calc_grading(&a, &o, &wk), "grading a");
    NEAR(o.r, 1.128838, 1e-6, "a r"); NEAR(o.d1, 1.252266e-3, 1e-6, "a d1"); NEAR(o.dn, 1.252266e-2, 1e-6, "a dn");
    /* the cells add up to L */
    double sum = 0, c = o.d1; for (int i = 0; i < o.N; i++) { sum += c; c *= o.r; }
    NEAR(sum, 0.1, 1e-9, "a sum");
    /* 3b: first 1 mm, last 10 mm over 0.1 m: r = 1.1, N = 25.16 -> 25 */
    CalcGradeIn b = { .mode = CG_FIRST_LAST, .L = 0.1, .d1 = 0.001, .dn = 0.01 };
    CHECK(calc_grading(&b, &o, &wk), "grading b");
    CHECK(o.N == 25 && o.rounded, "b N %d, expected 25 (rounded)", o.N);
    NEAR(o.R, 10, 1e-12, "b R"); NEAR(o.r, 1.100694, 1e-6, "b r"); NEAR(o.d1, 1.006243e-3, 1e-6, "b d1");
    /* 3c: 0.05 m from 0.1 mm growing by 1.2: N = 25.31 -> 25, first cell kept */
    CalcGradeIn cc = { .mode = CG_FIRST_GROWTH, .L = 0.05, .d1 = 1e-4, .r = 1.2 };
    CHECK(calc_grading(&cc, &o, &wk), "grading c");
    CHECK(o.N == 25, "c N %d, expected 25", o.N);
    NEAR(o.d1, 1e-4, 1e-12, "c d1 kept");
    sum = 0; c = o.d1; for (int i = 0; i < o.N; i++) { sum += c; c *= o.r; }
    NEAR(sum, 0.05, 1e-9, "c sum");
    CHECK(o.r > 1.2, "c: fewer cells -> a little more growth (%.6g)", o.r);
    /* edges too short for more than one cell: the cell is the edge */
    CalcGradeIn one = { .mode = CG_FIRST_GROWTH, .L = 0.01, .d1 = 0.009, .r = 1.2 };
    CHECK(calc_grading(&one, &o, NULL) && o.N == 1 && o.d1 == 0.01 && o.dn == 0.01, "one cell: N %d d1 %g dn %g", o.N, o.d1, o.dn);
    CalcGradeIn two = { .mode = CG_FIRST_LAST, .L = 0.1, .d1 = 0.06, .dn = 0.07 };
    CHECK(calc_grading(&two, &o, NULL) && o.N >= 2, "two sizes: two cells at least (%d)", o.N);
    sum = 0; c = o.d1; for (int i = 0; i < o.N; i++) { sum += c; c *= o.r; }
    NEAR(sum, 0.1, 1e-9, "two sizes sum");
    /* uniform */
    CalcGradeIn u = { .mode = CG_N_RATIO, .L = 1, .N = 10, .R = 1 };
    calc_grading(&u, &o, NULL); NEAR(o.d1, 0.1, 1e-12, "uniform d1");
    /* shrinking (R < 1) */
    CalcGradeIn sh = { .mode = CG_FIRST_GROWTH, .L = 0.05, .d1 = 0.004, .r = 0.9 };
    CHECK(!calc_grading(&sh, &o, NULL), "4 mm shrinking by 0.9 never reaches 50 mm (at most 40): refused");
    sh.d1 = 0.008;
    CHECK(calc_grading(&sh, &o, NULL), "shrinking c");
    sum = 0; c = o.d1; for (int i = 0; i < o.N; i++) { sum += c; c *= o.r; }
    NEAR(sum, 0.05, 1e-9, "shrinking sum");
}

static void dimless(void)
{
    /* example 4: water at 2 m/s in a 50 mm pipe, 10 K heating, time step 0.1 ms on 1 mm cells */
    const CalcFluid *f = &CALC_FLUID[CF_WATER];
    CalcDimIn in = { .flow = CW_PIPE, .rho = f->rho, .mu = f->mu, .cp = f->cp, .k = f->k, .beta = f->beta, .sigma = f->sigma, .c = f->c,
                     .U = 2, .L = 0.05, .dT = 10, .g = 9.81, .dt = 1e-4, .dx = 1e-3 };
    CalcDimOut o;
    calc_dimless(&in, &o);
    for (int k = 0; k < CN__N; k++) CHECK(o.have[k], "dimless: %s missing", CALC_DIM_SYM[k]);
    NEAR(o.v[CN_RE], 99600.8, 1e-5, "Re"); CHECK(!strcmp(o.verdict[CN_RE], "turbulent"), "Re verdict %s", o.verdict[CN_RE]);
    NEAR(o.v[CN_MA], 1.34953e-3, 1e-4, "Ma"); CHECK(o.level[CN_MA] == 0, "Ma level");
    NEAR(o.v[CN_PR], 7.0073, 1e-4, "Pr");
    NEAR(o.v[CN_RA], 1.76452e7, 1e-4, "Ra");
    NEAR(o.v[CN_RI], 2.53834e-4, 1e-4, "Ri"); CHECK(!strcmp(o.verdict[CN_RI], "forced convection"), "Ri verdict");
    NEAR(o.v[CN_FR], 2.85569, 1e-4, "Fr");
    NEAR(o.v[CN_WE], 2741.76, 1e-4, "We");
    NEAR(o.v[CN_CO], 0.2, 1e-12, "Co"); CHECK(o.level[CN_CO] == 0, "Co level");
    /* flags */
    CalcDimIn hi = in; hi.U = 200; hi.c = 343.2; hi.dt = 1e-4;
    calc_dimless(&hi, &o);
    CHECK(o.level[CN_MA] == 1 && !strcmp(o.verdict[CN_MA], "compressible"), "Ma 0.58: compressible");
    CHECK(o.level[CN_CO] == 2, "Co 20: far too large");
    hi.flow = CW_PLATE; hi.U = 2; hi.L = 0.05; calc_dimless(&hi, &o);
    CHECK(!strcmp(o.verdict[CN_RE], "laminar"), "plate Re 1e5: laminar (%s)", o.verdict[CN_RE]);
    /* without a temperature difference there is no Rayleigh or Richardson number */
    CalcDimIn nt = in; nt.dT = 0; calc_dimless(&nt, &o);
    CHECK(!o.have[CN_RA] && !o.have[CN_RI], "no dT: no Ra, Ri");
    Working wk; calc_dimless_working(&in, &o, CN_RE, &wk);
    char t[2048]; wk_text(&wk, t, sizeof t);
    CHECK(strstr(t, "Re = ρ U L / μ"), "Re working has its equation");
}

static void fluids(void)
{
    for (int i = 0; i < CF__N; i++) {
        const CalcFluid *f = &CALC_FLUID[i];
        CHECK(f->key && f->name && f->state && f->rho > 0 && f->mu > 0 && f->cp > 0 && f->k > 0 && f->beta > 0 && f->c > 0,
              "fluid %d incomplete", i);
    }
    /* Prandtl numbers anyone can look up: air 0.73, water 7.0, liquid nitrogen ~2.3, liquid hydrogen ~1.3 */
    NEAR(CALC_FLUID[CF_AIR].mu * CALC_FLUID[CF_AIR].cp / CALC_FLUID[CF_AIR].k, 0.7309, 0.01, "air Pr");
    NEAR(CALC_FLUID[CF_WATER].mu * CALC_FLUID[CF_WATER].cp / CALC_FLUID[CF_WATER].k, 7.01, 0.01, "water Pr");
    NEAR(CALC_FLUID[CF_LN2].mu * CALC_FLUID[CF_LN2].cp / CALC_FLUID[CF_LN2].k, 2.26, 0.03, "LN2 Pr");
    NEAR(CALC_FLUID[CF_LH2].mu * CALC_FLUID[CF_LH2].cp / CALC_FLUID[CF_LH2].k, 1.27, 0.03, "LH2 Pr");
}

static void cost(void)
{
    CalcCostIn in = { .cells = 1e6, .solver = CS_STEADY, .steps = 1000, .procs = 1, .memAvailMB = 16000, .nCpu = 8 };
    CalcCostOut o; Working wk;
    CHECK(calc_cost(&in, &o, &wk), "cost");
    NEAR(o.solveMB, 150 + 1e6 * 1.3 / 1024, 1e-9, "solve memory = precheck's simpleFoam figure");
    CHECK(o.level == 0, "1 M cells fit in 16 GB");
    double t1 = o.secPerStep;
    in.procs = 8; calc_cost(&in, &o, NULL);
    NEAR(o.speedup, 14.4356 / 6.974, 0.10, "8 processes: the measured speed-up (4.69 M cells: 14.44 s serial, 6.97 s on 8)");
    CHECK(o.secPerStep < t1, "8 processes faster than one");
    in.cells = 4e7; in.memAvailMB = 16000; calc_cost(&in, &o, NULL);
    CHECK(o.level == 2, "40 M cells don't fit in 16 GB");
    CHECK(calc_mesh_mb(2e6) > calc_mesh_mb(1e6), "mesh memory grows with cells");
    /* the measurements the model was fitted to (docs/CALCULATORS.md): within 10 % */
    CalcCostIn m1 = { .cells = 391200, .solver = CS_STEADY, .steps = 30, .procs = 1 };
    calc_cost(&m1, &o, NULL); NEAR(o.secPerStep, 0.7296, 0.10, "simpleFoam 391 k cells, s an iteration");
    m1.cells = 4694400; calc_cost(&m1, &o, NULL); NEAR(o.secPerStep, 14.4356, 0.10, "simpleFoam 4.69 M cells, s an iteration");
    NEAR(o.solveMB - 150, 6243708 / 1024.0, 0.10, "simpleFoam 4.69 M cells, memory");
    NEAR(calc_mesh_mb(353578), 869140 / 1024.0, 0.10, "snappyHexMesh motorBike 354 k cells, peak memory");
    NEAR(calc_mesh_mb(1892342), 3864228 / 1024.0, 0.10, "snappyHexMesh motorBike 1.89 M cells, peak memory");
}

int main(void)
{
    wall(); grading(); dimless(); fluids(); cost();
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("calc: all passed\n");
    return 0;
}
