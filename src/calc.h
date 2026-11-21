/* calc.h -- the calculators (mesh overhaul plan step 2): wall spacing from y+, cell grading, cells / memory / run time,
 * and the dimensionless numbers. Plain C, no drawing: each fills its result and a Working (working.h) written out the
 * way a hand calculation is. page_calc.c is the dialog; docs/CALCULATORS.md has a hand-worked example of each, which
 * test_calc checks these against. */
#ifndef CALC_H
#define CALC_H

#include <stdbool.h>
#include "working.h"

/* ---------------------------------------------------------------- fluids */
/* properties at one state: room-temperature air and water from the textbook tables (Cengel), the cryogens as saturated
   liquid at 1 atm from the NIST WebBook (interpolated between its two printed rows that bracket 101.325 kPa) */
typedef struct {
    const char *key, *name, *state;
    double T;          /* K                                    */
    double rho;        /* kg/m³                                */
    double mu;         /* Pa s                                 */
    double cp;         /* J/(kg K)                             */
    double k;          /* W/(m K)                              */
    double beta;       /* 1/K, volumetric thermal expansion    */
    double sigma;      /* N/m, surface tension (0: a gas)      */
    double c;          /* m/s, speed of sound                  */
} CalcFluid;

enum { CF_AIR = 0, CF_WATER, CF_LN2, CF_LOX, CF_LH2, CF__N };
extern const CalcFluid CALC_FLUID[CF__N];

/* ---------------------------------------------------------------- wall spacing from y+ */
enum { CW_PLATE = 0, CW_PIPE };     /* external flow along a flat plate as long as L; internal flow in a pipe of diameter L */

typedef struct {
    int    flow;       /* CW_*                                                           */
    double rho, mu;    /* kg/m³, Pa s                                                    */
    double U;          /* m/s: free stream (plate) or bulk (pipe)                        */
    double L;          /* m: plate length or pipe diameter                               */
    double yplus;      /* target y+ at the first cell's centre                           */
    double rough;      /* pipe: relative roughness ε/D (0 smooth)                        */
    double r;          /* layer growth ratio (each layer r times the one below)          */
    double cell;       /* m: the cells the layers blend into (0: not known)              */
} CalcWallIn;

typedef struct {
    double nu, Re, Cf, f, tauw, utau;
    double y;          /* m: first cell centre from the wall                             */
    double h1;         /* m: first cell height (2 y: the centre is half way)            */
    double delta;      /* m: boundary layer thickness (plate) or pipe radius             */
    bool   laminar;    /* Re below transition                                            */
    int    nCover;     /* layers whose stack reaches delta                               */
    double tCover;     /* m: their total                                                 */
    int    nBlend;     /* layers until the outer one is within LAYER_OUTER_MIN..MAX of cell (0: no cell) */
    double tBlend, outerBlend;
} CalcWallOut;

bool calc_wall(const CalcWallIn *in, CalcWallOut *out, Working *wk);   /* false: an input is missing (out zeroed) */

/* the number of layers a stack of first height h1 growing by r needs for its total to reach T, and that total */
int  calc_layers_to_total(double h1, double r, double T, double *total);

/* ---------------------------------------------------------------- grading */
enum { CG_N_RATIO = 0,   /* L, N cells, expansion ratio R (last / first, blockMesh's simpleGrading) */
       CG_FIRST_LAST,    /* L, first and last cell                                                  */
       CG_FIRST_GROWTH,  /* L, first cell, growth ratio r between neighbours (snappy's expansionRatio) */
       CG__N };

typedef struct {
    int    mode;       /* CG_*                       */
    double L;          /* m                          */
    int    N;          /* CG_N_RATIO                 */
    double R;          /* CG_N_RATIO                 */
    double d1, dn;     /* CG_FIRST_LAST: d1 and dn; CG_FIRST_GROWTH: d1 */
    double r;          /* CG_FIRST_GROWTH            */
} CalcGradeIn;

typedef struct {
    int    N;          /* whole cells                                     */
    double R;          /* last / first: what blockMesh's grading takes   */
    double r;          /* neighbour to neighbour                          */
    double d1, dn;     /* m, first and last cell with N whole cells       */
    bool   rounded;    /* N was rounded to a whole number and d1/R adjusted */
} CalcGradeOut;

bool calc_grading(const CalcGradeIn *in, CalcGradeOut *out, Working *wk);

/* ---------------------------------------------------------------- cells, memory, run time */
enum { CS_STEADY = 0, CS_TRANSIENT, CS_COMPRESSIBLE, CS_FREESURFACE, CS_HEAT, CS__N };
extern const char *const CALC_SOLVER_NAME[CS__N];

typedef struct {
    double cells;
    int    solver;     /* CS_*                                                   */
    double steps;      /* iterations (steady) or time steps (transient)          */
    int    nOuter;     /* transient: PIMPLE outer correctors (1 = PISO)          */
    int    procs;      /* processes                                              */
    double memAvailMB; /* the machine's free memory (0: unknown)                 */
    int    nCpu;
} CalcCostIn;

typedef struct {
    double meshMB;     /* snappyHexMesh's peak, serial                           */
    double solveMB;    /* the solver, all processes                              */
    double secPerStep; /* wall time a step on procs processes                    */
    double hours;      /* the whole run                                          */
    double speedup;    /* procs processes against one                            */
    int    level;      /* 0 fits, 1 tight (over 70 % of the free memory), 2 does not fit */
} CalcCostOut;

bool calc_cost(const CalcCostIn *in, CalcCostOut *out, Working *wk);
double calc_mesh_mb(double cells);        /* snappyHexMesh's peak memory for a mesh of that many cells, MB */

/* ---------------------------------------------------------------- dimensionless numbers */
enum { CN_RE = 0, CN_MA, CN_PR, CN_RA, CN_RI, CN_FR, CN_WE, CN_CO, CN__N };

typedef struct {
    int    flow;       /* CW_PLATE (external) or CW_PIPE (internal): sets the Reynolds thresholds */
    double rho, mu, cp, k, beta, sigma, c;
    double U, L;       /* m/s, m                                                 */
    double dT;         /* K: the temperature difference driving buoyancy (0: none) */
    double g;          /* m/s²                                                   */
    double dt, dx;     /* s, m: time step and cell size, for the Courant number (0: left out) */
} CalcDimIn;

typedef struct {
    double v[CN__N];
    bool   have[CN__N];       /* the inputs it needs were given                     */
    int    level[CN__N];      /* 0 fine / for information, 1 worth acting on, 2 must act */
    char   verdict[CN__N][48];/* "turbulent", "compressible" ...                    */
    char   why[CN__N][320];   /* what it means here, may carry [[term]] markup      */
} CalcDimOut;

extern const char *const CALC_DIM_NAME[CN__N];    /* "Reynolds" ... */
extern const char *const CALC_DIM_SYM[CN__N];     /* "Re" ...       */
extern const char *const CALC_DIM_EQ[CN__N];      /* "Re = ρ U L / μ" ... */

void calc_dimless(const CalcDimIn *in, CalcDimOut *out);
/* one number written out (Given / Equation / Working / Result) */
void calc_dimless_working(const CalcDimIn *in, const CalcDimOut *o, int which, Working *wk);

#endif
