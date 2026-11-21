/* calc.c -- the calculators (see calc.h) */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "calc.h"
#include "stlmesh.h"

/* ---------------------------------------------------------------- fluids
   Air and water at 20 °C, 1 atm: Cengel & Cimbala, Fluid Mechanics, tables A-9 and A-15 (speed of sound of air from
   √(γRT), γ 1.4, R 287 J/(kg K); of water, 1482 m/s). The cryogens: saturated liquid at 101.325 kPa, NIST Chemistry
   WebBook (Lemmon, McLinden, Friend), linear between the two printed saturation rows on either side of 1 atm
   (fetched 2026-11-21). NIST does not list the thermal expansion of the liquids: those are approximate (from the
   saturated-liquid density slope) and only feed the Rayleigh number. */
const CalcFluid CALC_FLUID[CF__N] = {
    [CF_AIR]   = { "air",   "Air",              "20 °C, 1 atm",                       293.15, 1.204,   1.825e-5,  1007,   0.02514, 3.411e-3, 0,         343.2 },
    [CF_WATER] = { "water", "Water",            "20 °C, 1 atm",                       293.15, 998.0,   1.002e-3,  4182,   0.598,   2.07e-4,  0.0728,    1482 },
    [CF_LN2]   = { "ln2",   "Liquid nitrogen",  "boiling at 1 atm, 77.36 K",          77.355, 806.08,  1.6067e-4, 2041.5, 0.14485, 5.7e-3,   8.880e-3,  851.4 },
    [CF_LOX]   = { "lox",   "Liquid oxygen",    "boiling at 1 atm, 90.19 K",          90.188, 1141.16, 1.9467e-4, 1699.3, 0.15085, 4.4e-3,   1.3145e-2, 904.4 },
    [CF_LH2]   = { "lh2",   "Liquid hydrogen",  "normal H2, boiling at 1 atm, 20.37 K", 20.369, 70.848, 1.3490e-5, 9772,   0.10372, 1.6e-2,   1.9117e-3, 1118.9 },
};

/* ---------------------------------------------------------------- helpers */
static void given_len(Working *w, const char *sym, double m, const char *what)
{
    char v[40], u[16]; wk_len(m, v, sizeof v, u, sizeof u);
    wk_given_s(w, sym, v, u, what);
}

static void result_len(Working *w, const char *sym, double m, const char *what)
{
    char v[40], u[16]; wk_len(m, v, sizeof v, u, sizeof u);
    wk_result_s(w, sym, v, u, what);
}

static const char *num(double v)   /* rotating buffers, for wk_step */
{
    static char b[8][40]; static int k;
    k = (k + 1) & 7;
    wk_num(b[k], sizeof b[k], v);
    return b[k];
}

static const char *len(double m)
{
    static char b[8][64]; static int k;
    k = (k + 1) & 7;
    char v[40], u[16]; wk_len(m, v, sizeof v, u, sizeof u);
    snprintf(b[k], sizeof b[k], "%s %s", v, u);
    return b[k];
}

int calc_layers_to_total(double h1, double r, double T, double *total)
{
    int n = 1;
    if (h1 > 0 && T > h1) {
        if (r > 1.0001) n = (int)ceil(log(1 + T * (r - 1) / h1) / log(r) - 1e-9);
        else n = (int)ceil(T / h1 - 1e-9);
    }
    if (n < 1) n = 1;
    if (n > 200) n = 200;
    if (total) *total = r > 1.0001 ? h1 * (pow(r, n) - 1) / (r - 1) : h1 * n;
    return n;
}

/* ---------------------------------------------------------------- wall spacing */
bool calc_wall(const CalcWallIn *in, CalcWallOut *o, Working *wk)
{
    memset(o, 0, sizeof *o);
    bool pipe = in->flow == CW_PIPE;
    if (wk) wk_init(wk, pipe ? "First cell height from y+: flow in a pipe" : "First cell height from y+: flow along a flat plate");
    if (!(in->rho > 0) || !(in->mu > 0) || !(in->U > 0) || !(in->L > 0) || !(in->yplus > 0)) {
        if (wk) wk_note(wk, "Needs the density, viscosity, speed, %s and y+.", pipe ? "diameter" : "length");
        return false;
    }
    double r = in->r > 1 ? in->r : 1;
    o->nu = in->mu / in->rho;
    o->Re = in->U * in->L / o->nu;
    if (pipe) {
        o->laminar = o->Re < 2300;
        double e = in->rough > 0 ? in->rough : 0;
        if (o->laminar) o->f = 64 / o->Re;
        else {
            double t = -1.8 * log10(pow(e / 3.7, 1.11) + 6.9 / o->Re);
            o->f = 1 / (t * t);
        }
        o->Cf = o->f / 4;
        o->delta = in->L / 2;
    } else {
        o->laminar = o->Re < 5e5;
        o->Cf = 0.026 * pow(o->Re, -1.0 / 7);              /* turbulent from the leading edge: the thinner, safe estimate */
        o->delta = o->laminar ? 4.91 * in->L / sqrt(o->Re) : 0.37 * in->L * pow(o->Re, -0.2);
    }
    o->tauw = 0.5 * in->rho * in->U * in->U * o->Cf;
    o->utau = sqrt(o->tauw / in->rho);
    o->y = in->yplus * o->nu / o->utau;
    o->h1 = 2 * o->y;
    o->nCover = calc_layers_to_total(o->h1, r, o->delta, &o->tCover);
    if (in->cell > 0) {
        /* the outer layer within LAYER_OUTER_MIN..MAX of the cells beyond, as the STL mesher wants it: the fewest layers
           whose outer one reaches LAYER_OUTER_MIN of the cell */
        int n = 1;
        double outer = o->h1;
        while (outer < LAYER_OUTER_MIN * in->cell && n < 200) { outer *= r; n++; if (r <= 1) break; }
        o->nBlend = n; o->outerBlend = outer;
        o->tBlend = r > 1.0001 ? o->h1 * (pow(r, n) - 1) / (r - 1) : o->h1 * n;
    }
    if (!wk) return true;

    char ru[32];
    wk_given(wk, "ρ", in->rho, "kg/m³", "density");
    wk_given(wk, "μ", in->mu, "Pa s", "dynamic viscosity");
    wk_given(wk, "U", in->U, "m/s", pipe ? "bulk (mean) velocity" : "free-stream velocity");
    given_len(wk, pipe ? "D" : "L", in->L, pipe ? "pipe diameter" : "plate length: the distance the boundary layer has grown over");
    wk_given(wk, "y+", in->yplus, "", in->yplus < 5 ? "target: inside the [[viscous-sublayer]] (wall resolved)" :
                                      in->yplus >= 30 ? "target: in the log layer ([[wall-function|wall functions]])" : "target: in the buffer layer (avoid)");
    if (pipe && in->rough > 0) wk_given(wk, "ε/D", in->rough, "", "relative roughness");
    if (r > 1) wk_given(wk, "r", r, "", "[[expansion-ratio|growth]] from one layer to the next");
    if (in->cell > 0) given_len(wk, "Δ", in->cell, "the cells the layers blend into");

    wk_eq(wk, pipe ? "Re = U D / ν,  ν = μ / ρ" : "Re = U L / ν,  ν = μ / ρ", NULL);
    if (pipe) {
        if (o->laminar) wk_eq(wk, "f = 64 / Re", "laminar pipe flow (Hagen-Poiseuille)");
        else wk_eq(wk, "1/√f = -1.8 log10[ (ε/D / 3.7)^1.11 + 6.9 / Re ]", "Haaland's fit to the Colebrook equation: the Moody chart");
        wk_eq(wk, "τw = f ρ U² / 8", "Darcy friction factor to wall shear stress");
    } else {
        wk_eq(wk, "Cf = 0.026 Re^(-1/7)", "local skin friction at x = L, turbulent flat plate (White)");
        wk_eq(wk, "τw = ½ ρ U² Cf", NULL);
    }
    wk_eq(wk, "u_τ = √(τw / ρ)", "[[friction-velocity]]: the velocity scale of the flow next to the wall");
    wk_eq(wk, "y = y+ ν / u_τ,  h₁ = 2 y", "y+ is measured at the cell centre, half way up the first cell");
    if (r > 1) wk_eq(wk, "T(N) = h₁ (r^N - 1) / (r - 1)", "the height of N layers, each r times the one below");

    wk_step(wk, "ν = %s / %s = %s m²/s", num(in->mu), num(in->rho), num(o->nu));
    wk_step(wk, "Re = %s × %s / %s = %s", num(in->U), num(in->L), num(o->nu), num(o->Re));
    if (pipe) {
        if (o->laminar) wk_step(wk, "f = 64 / %s = %s", num(o->Re), num(o->f));
        else wk_step(wk, "f = %s", num(o->f));
        wk_step(wk, "τw = %s × %s × %s² / 8 = %s Pa", num(o->f), num(in->rho), num(in->U), num(o->tauw));
    } else {
        wk_step(wk, "Cf = 0.026 × %s^(-1/7) = %s", num(o->Re), num(o->Cf));
        wk_step(wk, "τw = 0.5 × %s × %s² × %s = %s Pa", num(in->rho), num(in->U), num(o->Cf), num(o->tauw));
    }
    wk_step(wk, "u_τ = √(%s / %s) = %s m/s", num(o->tauw), num(in->rho), num(o->utau));
    wk_step(wk, "y = %s × %s / %s = %s,  h₁ = %s", num(in->yplus), num(o->nu), num(o->utau), len(o->y), len(o->h1));

    wk_result(wk, "Re", o->Re, "", o->laminar ? (pipe ? "laminar (under 2300)" : "laminar on a smooth plate (under 5 × 10⁵)") :
                                                (pipe ? "turbulent" : "turbulent"));
    wk_result(wk, "u_τ", o->utau, "m/s", "friction velocity");
    result_len(wk, "h₁", o->h1, "first cell height: the first prism layer");
    result_len(wk, pipe ? "R" : "δ", o->delta, pipe ? "pipe radius" : "boundary layer thickness at x = L");
    snprintf(ru, sizeof ru, "%d", o->nCover);
    if (r > 1) wk_result_s(wk, "N", ru, "", pipe ? "layers to reach the pipe's centre (more than needed: the core needs no layers)" :
                                                 "layers whose stack covers the boundary layer");
    if (in->cell > 0 && r > 1) {
        snprintf(ru, sizeof ru, "%d", o->nBlend);
        wk_result_s(wk, "N_blend", ru, "", "layers until the outer one is about a third of the cells beyond it");
        result_len(wk, "T", o->tBlend, "their total height");
    }
    if (o->laminar && in->yplus >= 30)
        wk_note(wk, "The flow is laminar here: there is no log layer for wall functions to sit in. Resolve the wall (y+ about 1) "
                    "or run without a turbulence model.");
    else if (!pipe && o->laminar)
        wk_note(wk, "Re is below transition on a smooth plate: the turbulent correlation gives a thinner first cell than a laminar "
                    "boundary layer needs, so the estimate errs on the safe side.");
    if (in->yplus >= 5 && in->yplus < 30)
        wk_note(wk, "y+ from 5 to 30 is the buffer layer: neither the sublayer nor the log law holds there. Aim for about 1 or 30 to 300.");
    wk_note(wk, "A correlation for a smooth flat plate or pipe: on a real part y+ varies (higher at a leading edge, near zero at "
                "separation). Check the y+ field after the first run (Results).");
    return true;
}

/* ---------------------------------------------------------------- grading */
static double first_of(double L, int N, double r)
{
    return fabs(r - 1) < 1e-12 ? L / N : L * (r - 1) / (pow(r, N) - 1);
}

/* the growth r for N cells over L starting at d1 (bisection; r > 1 when d1 < L / N) */
static double growth_for(double L, int N, double d1)
{
    if (N <= 1) return 1;
    double even = L / N;
    if (fabs(d1 - even) < 1e-12 * L) return 1;
    double lo, hi;
    if (d1 < even) { lo = 1; hi = 2; while (first_of(L, N, hi) > d1 && hi < 1e6) hi *= 2; }
    else { lo = 0.5; hi = 1; while (first_of(L, N, lo) < d1 && lo > 1e-6) lo /= 2; }
    for (int i = 0; i < 200; i++) {
        double m = 0.5 * (lo + hi);
        if (first_of(L, N, m) > d1) lo = m; else hi = m;      /* the first cell shrinks as r grows, either side of 1 */
    }
    return 0.5 * (lo + hi);
}

bool calc_grading(const CalcGradeIn *in, CalcGradeOut *o, Working *wk)
{
    memset(o, 0, sizeof *o);
    if (wk) wk_init(wk, "Cell grading along an edge");
    double L = in->L;
    if (!(L > 0)) { if (wk) wk_note(wk, "Needs the edge length."); return false; }
    if (in->mode == CG_N_RATIO) {
        if (in->N < 1 || !(in->R > 0)) { if (wk) wk_note(wk, "Needs the number of cells and the expansion ratio."); return false; }
        o->N = in->N; o->R = in->R;
        o->r = o->N > 1 ? pow(o->R, 1.0 / (o->N - 1)) : 1;
        o->d1 = first_of(L, o->N, o->r);
        o->dn = o->d1 * pow(o->r, o->N - 1);
        if (!wk) return true;
        given_len(wk, "L", L, "edge length");
        wk_given(wk, "N", o->N, "", "cells along it");
        wk_given(wk, "R", o->R, "", "expansion ratio: last cell / first cell (blockMesh's simpleGrading)");
        wk_eq(wk, "r = R^(1/(N-1))", "the growth from one cell to the next");
        wk_eq(wk, "Δ₁ = L (r - 1) / (r^N - 1),  Δ_N = R Δ₁", "the cells form a geometric series summing to L");
        wk_step(wk, "r = %s^(1/%d) = %s", num(o->R), o->N - 1, num(o->r));
        wk_step(wk, "Δ₁ = %s × (%s - 1) / (%s^%d - 1) = %s", num(L), num(o->r), num(o->r), o->N, len(o->d1));
        wk_step(wk, "Δ_N = %s × %s = %s", num(o->R), len(o->d1), len(o->dn));
    } else if (in->mode == CG_FIRST_LAST) {
        double d1 = in->d1, dn = in->dn;
        if (!(d1 > 0) || !(dn > 0) || d1 >= L || dn >= L) { if (wk) wk_note(wk, "Needs a first and last cell, each shorter than the edge."); return false; }
        double r = fabs(dn - d1) < 1e-15 ? 1 : (L - d1) / (L - dn);      /* L = (r dn - d1) / (r - 1) */
        double Nf = fabs(r - 1) < 1e-12 ? L / d1 : 1 + log(dn / d1) / log(r);
        o->N = (int)fmax(1, round(Nf));
        o->rounded = fabs(Nf - o->N) > 1e-6;
        o->R = dn / d1;                                      /* keep the ratio asked for; the cells shift a little */
        o->r = o->N > 1 ? pow(o->R, 1.0 / (o->N - 1)) : 1;
        o->d1 = first_of(L, o->N, o->r);
        o->dn = o->d1 * o->R;
        if (!wk) return true;
        given_len(wk, "L", L, "edge length");
        given_len(wk, "Δ₁", d1, "first cell");
        given_len(wk, "Δ_N", dn, "last cell");
        wk_eq(wk, "L = (r Δ_N - Δ₁) / (r - 1)  →  r = (L - Δ₁) / (L - Δ_N)", "the sum of the geometric series, solved for r");
        wk_eq(wk, "N = 1 + ln(Δ_N / Δ₁) / ln r,  R = Δ_N / Δ₁", NULL);
        wk_step(wk, "r = (%s - %s) / (%s - %s) = %s", num(L), num(d1), num(L), num(dn), num(r));
        wk_step(wk, "N = 1 + ln(%s) / ln(%s) = %s  →  %d", num(dn / d1), num(r), num(Nf), o->N);
        if (o->rounded) wk_step(wk, "with N = %d and R = %s: r = %s, Δ₁ = %s, Δ_N = %s", o->N, num(o->R), num(o->r), len(o->d1), len(o->dn));
    } else {
        double d1 = in->d1, r = in->r;
        if (!(d1 > 0) || d1 >= L || !(r > 0)) { if (wk) wk_note(wk, "Needs a first cell shorter than the edge and a growth ratio."); return false; }
        double Nf = fabs(r - 1) < 1e-12 ? L / d1 : log(1 + L * (r - 1) / d1) / log(r);
        if (!(Nf > 0) || !isfinite(Nf)) { if (wk) wk_note(wk, "Those cells shrink to nothing before the edge ends: use a ratio above 1, or a larger first cell."); return false; }
        o->N = (int)fmax(1, round(Nf));
        o->rounded = fabs(Nf - o->N) > 1e-6;
        o->d1 = d1;
        o->r = growth_for(L, o->N, d1);                     /* keep the first cell asked for (a wall's y+); the growth shifts */
        o->R = pow(o->r, o->N - 1);
        o->dn = d1 * o->R;
        if (!wk) return true;
        given_len(wk, "L", L, "edge length");
        given_len(wk, "Δ₁", d1, "first cell");
        wk_given(wk, "r", r, "", "growth from one cell to the next ([[expansion-ratio]] in snappyHexMesh's layers)");
        wk_eq(wk, "L = Δ₁ (r^N - 1) / (r - 1)  →  N = ln(1 + L (r - 1) / Δ₁) / ln r", NULL);
        wk_eq(wk, "R = r^(N-1)", "the ratio blockMesh wants: last cell / first cell");
        wk_step(wk, "N = ln(1 + %s × (%s - 1) / %s) / ln(%s) = %s  →  %d", num(L), num(r), num(d1), num(r), num(Nf), o->N);
        if (o->rounded) wk_step(wk, "with N = %d whole cells and Δ₁ kept, r becomes %s", o->N, num(o->r));
        wk_step(wk, "R = %s^%d = %s", num(o->r), o->N - 1, num(o->R));
    }
    char nb[16]; snprintf(nb, sizeof nb, "%d", o->N);
    wk_result_s(wk, "N", nb, "", "cells");
    wk_result(wk, "R", o->R, "", "blockMesh: simpleGrading / edgeGrading value");
    wk_result(wk, "r", o->r, "", "neighbour to neighbour");
    result_len(wk, "Δ₁", o->d1, "first cell");
    result_len(wk, "Δ_N", o->dn, "last cell");
    if (o->r > 1.3 || o->r < 1 / 1.3)
        wk_note(wk, "Neighbouring cells differ by more than 30 %%: the truncation error grows where the size jumps. Keep r within 1.2 "
                    "(1.3 at most): more cells, or a smaller ratio.");
    wk_note(wk, "blockMesh's grading is R, last over first, for the whole edge; snappyHexMesh's expansionRatio is r, one layer to "
                "the next. R = r^(N-1): 20 cells at r = 1.1 is R = 6.1.");
    return true;
}

/* ---------------------------------------------------------------- cells, memory, run time */
const char *const CALC_SOLVER_NAME[CS__N] = { "Steady (simpleFoam)", "Transient (pimpleFoam)", "Compressible (rhoSimpleFoam)",
                                              "Free surface (interFoam)", "Heat (buoyantSimpleFoam)" };

/* CALIBRATION, 2026-11-21, this machine (32 threads, 31 GB), OpenFOAM v2606 (docs/CALCULATORS.md, "Where the numbers come
   from"): simpleFoam k-epsilon on a 3-D pitzDaily, 30 iterations, serial: 391 k cells 0.73 s an iteration and 644 MB, 4.69 M
   cells 14.4 s and 6.24 GB; a second, per cell, of 1.87e-6 and 3.07e-6: the linear solvers need more sweeps on a larger
   mesh, fitted as (N / 1e6)^0.2. snappyHexMesh on the motorBike tutorial: 354 k cells 869 MB peak, 1.89 M cells 3.86 GB:
   180 MB + 2.0 kB a cell. */
#define CALC_SEC_CELL_ITER 2.25e-6          /* at a million cells */
#define CALC_SEC_EXP 0.2
#define CALC_MESH_KB_CELL 2.0
#define CALC_MESH_MB_BASE 180.0
#define CALC_PAR_EFF 0.64            /* measured: 8 processes ran the 4.69 M-cell case 2.07 times as fast (6.97 s an iteration): 8 × 0.64³ */
/* kB per cell: the same figures as precheck.c's kb_per_cell (measured with v2606), so the Run page agrees */
static const double KB_CELL[CS__N] = { 1.3, 1.3, 2.0, 2.5, 1.6 };
/* seconds per cell per iteration (or per outer corrector) on one core, relative to simpleFoam k-epsilon (calibrated, below) */
static const double REL_COST[CS__N] = { 1.0, 1.25, 1.8, 2.2, 1.5 };
static const double SEC_CELL_ITER = CALC_SEC_CELL_ITER;
static const double MESH_KB_CELL = CALC_MESH_KB_CELL, MESH_MB_BASE = CALC_MESH_MB_BASE;
static const double PAR_EFF = CALC_PAR_EFF;       /* parallel efficiency per doubling of processes */

double calc_mesh_mb(double cells) { return MESH_MB_BASE + cells * MESH_KB_CELL / 1024.0; }

bool calc_cost(const CalcCostIn *in, CalcCostOut *o, Working *wk)
{
    memset(o, 0, sizeof *o);
    if (wk) wk_init(wk, "Memory and run time");
    if (!(in->cells > 0)) { if (wk) wk_note(wk, "Needs a cell count."); return false; }
    int s = in->solver >= 0 && in->solver < CS__N ? in->solver : CS_STEADY;
    int P = in->procs > 0 ? in->procs : 1;
    bool tr = s == CS_TRANSIENT || s == CS_FREESURFACE;
    int nO = tr && in->nOuter > 0 ? in->nOuter : 1;
    double steps = in->steps > 0 ? in->steps : tr ? 1000 : 1000;
    o->meshMB = calc_mesh_mb(in->cells);
    o->solveMB = 150.0 * P + in->cells * KB_CELL[s] / 1024.0;
    o->speedup = P * pow(PAR_EFF, log2(P));
    double c = SEC_CELL_ITER * pow(in->cells / 1e6, CALC_SEC_EXP);   /* per cell and iteration, one core */
    o->secPerStep = in->cells * c * REL_COST[s] * nO / o->speedup;
    o->hours = o->secPerStep * steps / 3600;
    double need = fmax(o->meshMB, o->solveMB);
    o->level = in->memAvailMB > 0 ? (need > 0.9 * in->memAvailMB ? 2 : need > 0.7 * in->memAvailMB ? 1 : 0) : 0;
    if (!wk) return true;
    char b[48];
    snprintf(b, sizeof b, "%.0f", in->cells);
    wk_given_s(wk, "N", b, "cells", "");
    wk_given_s(wk, "solver", CALC_SOLVER_NAME[s], "", "");
    snprintf(b, sizeof b, "%.0f", steps);
    wk_given_s(wk, tr ? "steps" : "iterations", b, "", tr ? "time steps" : "SIMPLE iterations");
    if (tr) { snprintf(b, sizeof b, "%d", nO); wk_given_s(wk, "nOuter", b, "", "PIMPLE outer correctors a step"); }
    snprintf(b, sizeof b, "%d", P); wk_given_s(wk, "P", b, "", "processes");
    wk_eq(wk, "M_mesh = 180 MB + N × 2.0 kB", "snappyHexMesh's peak: it holds the background mesh, the refined one and the surface");
    wk_eq(wk, "M_solve = 150 MB × P + N × m_cell", "each process loads OpenFOAM's libraries; each cell holds its fields and matrix rows");
    wk_eq(wk, "t_step = N × c × k_solver × nOuter / S(P),  c = 2.25 µs × (N / 10⁶)^0.2", "c: measured on this machine with simpleFoam; k_solver: the solver's cost against it");
    wk_eq(wk, "S(P) = P × e^log₂P", "the speed-up on P processes, e the parallel efficiency kept at each doubling");
    wk_step(wk, "M_mesh = %.0f + %.0f × %.2f kB = %.0f MB", MESH_MB_BASE, in->cells, MESH_KB_CELL, o->meshMB);
    wk_step(wk, "M_solve = 150 × %d + %.0f × %.2f kB = %.0f MB", P, in->cells, KB_CELL[s], o->solveMB);
    wk_step(wk, "S = %d × %.2f^%.2f = %.2f", P, PAR_EFF, log2(P), o->speedup);
    wk_step(wk, "c = 2.25 µs × %s^0.2 = %s µs", num(in->cells / 1e6), num(c * 1e6));
    wk_step(wk, "t_step = %.0f × %s µs × %.2f × %d / %.2f = %s s", in->cells, num(c * 1e6), REL_COST[s], nO, o->speedup, num(o->secPerStep));
    snprintf(b, sizeof b, "%.1f", o->meshMB / 1024); wk_result_s(wk, "M_mesh", b, "GB", "to mesh (snappyHexMesh, one process)");
    snprintf(b, sizeof b, "%.1f", o->solveMB / 1024); wk_result_s(wk, "M_solve", b, "GB", "to solve");
    wk_result(wk, "t_step", o->secPerStep, "s", tr ? "a time step" : "an iteration");
    if (o->hours < 1) { snprintf(b, sizeof b, "%.0f", o->hours * 60); wk_result_s(wk, "t", b, "min", "the whole run"); }
    else { snprintf(b, sizeof b, "%.1f", o->hours); wk_result_s(wk, "t", b, "h", "the whole run"); }
    if (in->memAvailMB > 0) {
        if (o->level == 2) wk_note(wk, "More than this machine has free (%.1f GB): Linux would kill the job. Coarser cells, or fewer processes.", in->memAvailMB / 1024);
        else if (o->level == 1) wk_note(wk, "Over 70 %% of the %.1f GB free: tight.", in->memAvailMB / 1024);
        else wk_note(wk, "Fits in the %.1f GB free.", in->memAvailMB / 1024);
    }
    if (in->nCpu > 0 && P > in->nCpu) wk_note(wk, "More processes than the %d cores: each runs slower, often slower overall.", in->nCpu);
    if (P > 1 && in->cells / P < 20000)
        wk_note(wk, "Under 20 000 cells a process: the processes spend more time talking than computing. Fewer is often faster.");
    wk_note(wk, "Rough: within a factor of two. A run with a harder flow (separation, strong swirl, poor cells) needs more iterations, "
                "not slower ones.");
    return true;
}

/* ---------------------------------------------------------------- dimensionless numbers */
const char *const CALC_DIM_NAME[CN__N] = { "Reynolds", "Mach", "Prandtl", "Rayleigh", "Richardson", "Froude", "Weber", "Courant" };
const char *const CALC_DIM_SYM[CN__N] = { "Re", "Ma", "Pr", "Ra", "Ri", "Fr", "We", "Co" };
const char *const CALC_DIM_EQ[CN__N] = { "Re = ρ U L / μ", "Ma = U / c", "Pr = μ cp / k", "Ra = g β ΔT L³ / (ν α),  α = k / (ρ cp)",
                                         "Ri = Gr / Re²,  Gr = Ra / Pr", "Fr = U / √(g L)", "We = ρ U² L / σ", "Co = U Δt / Δx" };

void calc_dimless(const CalcDimIn *in, CalcDimOut *o)
{
    memset(o, 0, sizeof *o);
    bool pipe = in->flow == CW_PIPE;
    double nu = in->rho > 0 && in->mu > 0 ? in->mu / in->rho : 0;
    double alpha = in->rho > 0 && in->cp > 0 && in->k > 0 ? in->k / (in->rho * in->cp) : 0;
    if (nu > 0 && in->U > 0 && in->L > 0) {
        double Re = o->v[CN_RE] = in->U * in->L / nu; o->have[CN_RE] = true;
        double lo = pipe ? 2300 : 5e5, hi = pipe ? 4000 : 3e6;
        if (Re < lo) {
            snprintf(o->verdict[CN_RE], 48, "laminar");
            snprintf(o->why[CN_RE], 320, "Viscosity holds the flow in layers. Physics: Laminar (no turbulence model); the mesh resolves the wall "
                     "anyway.%s", pipe ? "" : " A bluff body still separates and can shed vortices: laminar is right only while the wake stays steady.");
        } else if (Re < hi) {
            o->level[CN_RE] = 1;
            snprintf(o->verdict[CN_RE], 48, "transitional");
            snprintf(o->why[CN_RE], 320, "Between laminar and turbulent (%s): where the flow trips depends on disturbances. k-omega SST with "
                     "the wall resolved ([[y+]] about 1), or the transition model, handles it best.", pipe ? "2300 to 4000 in a pipe" : "5 × 10⁵ to 3 × 10⁶ on a plate");
        } else {
            snprintf(o->verdict[CN_RE], 48, "turbulent");
            snprintf(o->why[CN_RE], 320, "Inertia wins: the flow is turbulent and needs a turbulence model (k-omega SST is the usual start). "
                     "Higher Re, thinner [[boundary-layer]]: a smaller [[first-layer|first cell]] for the same y+.");
        }
    }
    if (in->c > 0 && in->U > 0) {
        double Ma = o->v[CN_MA] = in->U / in->c; o->have[CN_MA] = true;
        if (Ma < 0.3) { snprintf(o->verdict[CN_MA], 48, "incompressible"); snprintf(o->why[CN_MA], 320, "Density changes under 5 %%: an incompressible solver is right."); }
        else {
            o->level[CN_MA] = Ma < 0.8 ? 1 : 2;
            snprintf(o->verdict[CN_MA], 48, "%s", Ma < 0.8 ? "compressible" : Ma < 1.2 ? "transonic" : "supersonic");
            snprintf(o->why[CN_MA], 320, "Above 0.3 the density changes with the speed: Physics > Setup, Ideal gas (rhoSimpleFoam or rhoPimpleFoam). %s",
                     Ma >= 0.8 ? "Shocks need fine cells where they stand and a transient or robust start." : "Results from an incompressible solver would be several per cent off.");
        }
    }
    double Pr = 0;
    if (in->mu > 0 && in->cp > 0 && in->k > 0) {
        Pr = o->v[CN_PR] = in->mu * in->cp / in->k; o->have[CN_PR] = true;
        snprintf(o->verdict[CN_PR], 48, "%s", Pr < 0.1 ? "liquid metal" : Pr < 2 ? "gas-like" : "liquid");
        snprintf(o->why[CN_PR], 320, "Momentum against heat diffusion. The thermal boundary layer is about Pr^(-1/3) = %.2g times the velocity "
                 "one%s", pow(Pr, -1.0 / 3), Pr > 2 ? ": thinner, so heat transfer needs a finer first cell than the flow alone." : ".");
    }
    double Ra = 0;
    if (in->g > 0 && in->beta > 0 && in->dT > 0 && in->L > 0 && nu > 0 && alpha > 0) {
        Ra = o->v[CN_RA] = in->g * in->beta * in->dT * pow(in->L, 3) / (nu * alpha); o->have[CN_RA] = true;
        if (Ra < 1e9) { snprintf(o->verdict[CN_RA], 48, "laminar convection"); snprintf(o->why[CN_RA], 320, "Buoyant flow stays laminar below about 10⁹ (a vertical wall)."); }
        else {
            o->level[CN_RA] = 1;
            snprintf(o->verdict[CN_RA], 48, "turbulent convection");
            snprintf(o->why[CN_RA], 320, "Above about 10⁹ natural convection turns turbulent: a turbulence model, and the wall resolved for the heat flux.");
        }
    }
    if (o->have[CN_RA] && o->have[CN_RE] && Pr > 0) {
        double Ri = o->v[CN_RI] = (Ra / Pr) / (o->v[CN_RE] * o->v[CN_RE]); o->have[CN_RI] = true;
        if (Ri < 0.1) { snprintf(o->verdict[CN_RI], 48, "forced convection"); snprintf(o->why[CN_RI], 320, "The flow carries the heat; buoyancy hardly matters. Gravity can be left out."); }
        else {
            o->level[CN_RI] = 1;
            snprintf(o->verdict[CN_RI], 48, "%s", Ri < 10 ? "mixed convection" : "natural convection");
            snprintf(o->why[CN_RI], 320, "Buoyancy %s: Physics > Setup, Heat with gravity on (buoyantSimpleFoam or buoyantPimpleFoam).",
                     Ri < 10 ? "and the flow both matter" : "drives the flow");
        }
    }
    if (in->g > 0 && in->U > 0 && in->L > 0) {
        double Fr = o->v[CN_FR] = in->U / sqrt(in->g * in->L); o->have[CN_FR] = true;
        snprintf(o->verdict[CN_FR], 48, "%s", Fr < 1 ? "subcritical" : "supercritical");
        snprintf(o->why[CN_FR], 320, "Matters only with a free surface (Physics: Free surface). %s", Fr < 1 ?
                 "Surface waves can travel upstream: the outlet level steers the flow." : "Waves are swept downstream; a hydraulic jump can form where it slows.");
    }
    if (in->sigma > 0 && in->rho > 0 && in->U > 0 && in->L > 0) {
        double We = o->v[CN_WE] = in->rho * in->U * in->U * in->L / in->sigma; o->have[CN_WE] = true;
        snprintf(o->verdict[CN_WE], 48, "%s", We < 1 ? "surface tension wins" : We < 1000 ? "both matter" : "inertia wins");
        snprintf(o->why[CN_WE], 320, "Matters only with a free surface. %s", We < 1 ?
                 "Drops and bubbles hold their shape: the surface tension σ must be right, and the cells across the interface fine." :
                 We < 1000 ? "The interface deforms and can break up: give σ, and keep the time step under the capillary limit." :
                 "Surface tension hardly shapes the flow; σ can stay approximate.");
    }
    if (in->dt > 0 && in->dx > 0 && in->U > 0) {
        double Co = o->v[CN_CO] = in->U * in->dt / in->dx; o->have[CN_CO] = true;
        o->level[CN_CO] = Co > 5 ? 2 : Co > 1 ? 1 : 0;
        snprintf(o->verdict[CN_CO], 48, "%s", Co <= 1 ? "stable" : Co <= 5 ? "too large" : "far too large");
        snprintf(o->why[CN_CO], 320, "The fluid crosses %.2g cells a time step. %s", Co, Co <= 1 ?
                 "PISO and PIMPLE stay stable and accurate below 1 (free surface: 0.5 at the interface)." :
                 "Above 1 a transient run tends to blow up: a smaller time step, or adjustTimeStep with maxCo 1 ([[courant]]).");
    }
}

void calc_dimless_working(const CalcDimIn *in, const CalcDimOut *o, int k, Working *wk)
{
    char t[96]; snprintf(t, sizeof t, "%s number", CALC_DIM_NAME[k]);
    wk_init(wk, t);
    double nu = in->rho > 0 ? in->mu / in->rho : 0, alpha = in->rho > 0 && in->cp > 0 ? in->k / (in->rho * in->cp) : 0;
    wk_eq(wk, CALC_DIM_EQ[k], NULL);
    if (!o->have[k]) { wk_note(wk, "Not enough inputs for it."); return; }
    switch (k) {
    case CN_RE:
        wk_given(wk, "ρ", in->rho, "kg/m³", ""); wk_given(wk, "U", in->U, "m/s", ""); given_len(wk, "L", in->L, in->flow == CW_PIPE ? "pipe diameter" : "length along the flow");
        wk_given(wk, "μ", in->mu, "Pa s", "");
        wk_step(wk, "Re = %s × %s × %s / %s = %s", num(in->rho), num(in->U), num(in->L), num(in->mu), num(o->v[k])); break;
    case CN_MA:
        wk_given(wk, "U", in->U, "m/s", ""); wk_given(wk, "c", in->c, "m/s", "speed of sound");
        wk_step(wk, "Ma = %s / %s = %s", num(in->U), num(in->c), num(o->v[k])); break;
    case CN_PR:
        wk_given(wk, "μ", in->mu, "Pa s", ""); wk_given(wk, "cp", in->cp, "J/(kg K)", ""); wk_given(wk, "k", in->k, "W/(m K)", "");
        wk_step(wk, "Pr = %s × %s / %s = %s", num(in->mu), num(in->cp), num(in->k), num(o->v[k])); break;
    case CN_RA:
        wk_given(wk, "g", in->g, "m/s²", ""); wk_given(wk, "β", in->beta, "1/K", "thermal expansion"); wk_given(wk, "ΔT", in->dT, "K", "");
        given_len(wk, "L", in->L, "height of the heated wall"); wk_given(wk, "ν", nu, "m²/s", ""); wk_given(wk, "α", alpha, "m²/s", "thermal diffusivity");
        wk_step(wk, "Ra = %s × %s × %s × %s³ / (%s × %s) = %s", num(in->g), num(in->beta), num(in->dT), num(in->L), num(nu), num(alpha), num(o->v[k])); break;
    case CN_RI:
        wk_given(wk, "Ra", o->v[CN_RA], "", ""); wk_given(wk, "Pr", o->v[CN_PR], "", ""); wk_given(wk, "Re", o->v[CN_RE], "", "");
        wk_step(wk, "Gr = %s / %s = %s", num(o->v[CN_RA]), num(o->v[CN_PR]), num(o->v[CN_RA] / o->v[CN_PR]));
        wk_step(wk, "Ri = %s / %s² = %s", num(o->v[CN_RA] / o->v[CN_PR]), num(o->v[CN_RE]), num(o->v[k])); break;
    case CN_FR:
        wk_given(wk, "U", in->U, "m/s", ""); wk_given(wk, "g", in->g, "m/s²", ""); given_len(wk, "L", in->L, "depth or length");
        wk_step(wk, "Fr = %s / √(%s × %s) = %s", num(in->U), num(in->g), num(in->L), num(o->v[k])); break;
    case CN_WE:
        wk_given(wk, "ρ", in->rho, "kg/m³", ""); wk_given(wk, "U", in->U, "m/s", ""); given_len(wk, "L", in->L, "drop or jet size");
        wk_given(wk, "σ", in->sigma, "N/m", "surface tension");
        wk_step(wk, "We = %s × %s² × %s / %s = %s", num(in->rho), num(in->U), num(in->L), num(in->sigma), num(o->v[k])); break;
    case CN_CO:
        wk_given(wk, "U", in->U, "m/s", ""); wk_given(wk, "Δt", in->dt, "s", "time step"); given_len(wk, "Δx", in->dx, "cell size");
        wk_step(wk, "Co = %s × %s / %s = %s", num(in->U), num(in->dt), num(in->dx), num(o->v[k])); break;
    }
    wk_result_s(wk, CALC_DIM_SYM[k], num(o->v[k]), "", o->verdict[k]);
    wk_note(wk, "%s", o->why[k]);
}
