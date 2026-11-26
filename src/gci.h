/* gci.h -- mesh independence from a grid study (mesh overhaul plan step 8, gap L8): Richardson extrapolation and Roache's
 * Grid Convergence Index, by the procedure of Celik et al. (2008), "Procedure for estimation and reporting of uncertainty
 * due to discretization in CFD applications", J. Fluids Eng. 130, 078001 (the ASME V&V 20 recipe):
 *   h = (1/N)^(1/D) for N cells in D dimensions (or 1/f for a refinement factor f), meshes sorted fine to coarse,
 *   r21 = h2/h1, r32 = h3/h2, e21 = f2 - f1, e32 = f3 - f2, s = sign(e32/e21),
 *   p = |ln|e32/e21| + q(p)| / ln r21,  q(p) = ln((r21^p - s) / (r32^p - s))   (fixed-point iteration),
 *   f_ext = (r21^p f1 - f2) / (r21^p - 1),  e_a = |(f1 - f2)/f1|,  e_ext = |(f_ext - f1)/f_ext|,
 *   GCI_fine = 1.25 e_a / (r21^p - 1).
 * Two meshes: p is taken as the scheme's formal order (2) and the safety factor 3 (Roache). Plain C: the Studies page shows
 * it, test_gci checks it against the paper's worked example. */
#ifndef GCI_H
#define GCI_H

#include <stdbool.h>
#include "working.h"

#define GCI_MAX 16
typedef struct {
    int    n;                         /* meshes                                          */
    double h[GCI_MAX];                /* a grid size each (any unit: only ratios count)   */
    double f[GCI_MAX];                /* the quantity on each                             */
    double formalOrder;               /* the schemes' order (2 for OpenFOAM's usual linear / linearUpwind); 0: 2 */
} GciIn;

enum { GCI_MONOTONIC = 0, GCI_OSCILLATING, GCI_DIVERGING, GCI_SAME, GCI__KINDS };
extern const char *const GCI_KIND_NAME[GCI__KINDS];

typedef struct {
    bool   ok;
    char   why[200];                  /* !ok: what is missing                             */
    int    used;                      /* meshes used (the finest three, or two)           */
    int    idx[3];                    /* which inputs: fine, medium, coarse               */
    double r21, r32;
    double p;                         /* apparent order (formal order with two meshes)    */
    bool   pAssumed;                  /* two meshes, or no apparent order: the formal one */
    int    kind;                      /* GCI_*                                            */
    double R;                         /* e21 / e32: 0..1 monotonic, < 0 oscillating, > 1 diverging */
    double fExt;                      /* the extrapolated (mesh-independent) value        */
    double ea, eext;                  /* relative changes: fine to medium, fine to extrapolated */
    double gci, gci32;                /* fine-grid GCI (21) and the medium one (32), fractions */
    double asym;                      /* GCI32 / (r21^p GCI21): about 1 in the asymptotic range (3 meshes) */
    int    verdict;                   /* 0 fine enough, 1 close, 2 refine (plain words in verdictText) */
    char   verdictText[400];
} GciOut;

/* the grid size of a mesh of N cells in D dimensions (2 or 3) */
double gci_h(double N, int D);
bool   gci_compute(const GciIn *in, GciOut *out, Working *wk);   /* wk may be NULL */

#endif
