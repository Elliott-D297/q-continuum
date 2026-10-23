/* physics.h -- the numbers behind the checks: cell sizes from a blockMeshDict (before any meshing) or a mesh's bounding
 * box, Courant and Fourier numbers with their working written out, the decomposition split, a run's ETA. Pure functions
 * of their inputs (plus reading a dictionary), so they are unit-tested against hand-worked values.
 */
#ifndef PHYSICS_H
#define PHYSICS_H

#include <stdbool.h>
#include "foamdict.h"

/* (nx ny nz) with nx*ny*nz = np: prime factors, largest first, each into the smallest direction so far; dirs = 2 keeps
   z = 1 (a 2-D case) */
void decomp_split_n(long np, int dirs, long out[3]);

/* a blockMeshDict's geometry, read without running blockMesh */
typedef struct {
    bool   ok;                  /* vertices and hex blocks could be read (no $variables, #calc or code) */
    char   why[160];            /* why not                                                              */
    double lo[3], hi[3];        /* bounding box of the vertices, after scale, in m                       */
    long   cells;
    int    blocks;
    double dxMin;               /* the smallest cell edge in any block, after grading, in m              */
    double dxMinThin;           /* ... leaving out the 1-cell direction of a 2-D case                   */
    int    oneCellDir;          /* the direction with 1 cell in every block (2-D), or -1                 */
} BlockMeshInfo;
void blockmesh_info(FDFile *bm, BlockMeshInfo *out);

/* the smallest of n cells along an edge of length L graded with expansion ratio r (last cell / first cell) */
double graded_min_cell(double L, int n, double r);

/* Co = |U| dt / dx; the working: "Co ≈ |U|·Δt/Δx = 1 m/s × 0.005 s / 0.005 m = 1.0" */
double courant(double U, double dt, double dx);
void   courant_text(double U, double dt, double dx, char *out, int cap);
/* Fo = α dt / dx², α = k / (ρ cp) */
double fourier(double alpha, double dt, double dx);
void   fourier_text(double k, double rho, double cp, double dt, double dx, char *out, int cap);

/* at most one rank per `perRank` cells (at least 1): a 400-cell cavity on 9 ranks spends its time in MPI. cells <= 0
   (not known yet) leaves `want` alone */
long procs_for_cells(long want, long cells, long perRank);

/* seconds of wall clock to the end time, from the rate so far; < 0 when it can't be told */
double eta_seconds(double tStart, double tNow, double tEnd, double wallSoFar);

/* ROADMAP 4.3 continuum check. The hard-sphere mean free path λ = k_B T / (√2 π d² p) [m] and Kn = λ / Δx. d is the
   molecule's VHS diameter (Bird 1994, Table A1, at 273 K) looked up from the mixture's molar mass; regimes: Kn < 0.01
   continuum, 0.01-0.1 slip, 0.1-10 transitional, > 10 free molecular */
typedef struct { const char *name; double molWeight, d; } GasDiameter;
const GasDiameter *gas_for_molweight(double molWeight);          /* the closest within 3 %, else NULL */
double mean_free_path(double T, double p, double d);
const char *knudsen_regime(double kn);                           /* "continuum", "slip", "transitional", "free molecular" */
void   knudsen_text(double T, double p, double d, double dx, char *out, int cap);

/* a number for people: 3 significant digits, no exponent between 1e-3 and 1e5 ("0.005", "1.0", "2280", "2.4e+19") */
void   human_num(double v, char *out, int cap);

#endif
