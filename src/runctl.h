/* runctl.h -- what a solver lets you control when it runs (Run page, "Run controls"; user request 2026-11-25: "why am I
 * not allowed to have adaptive time step here? ... allow greater control of parameters when it comes to running the case,
 * especially solver specific").
 *
 * The table was made from OpenFOAM v2606's solver sources: which ones read adjustTimeStep (setDeltaT.H), which extra
 * Courant-like limits they read (maxAlphaCo, maxDi, maxAcousticCo), and which loop drives them (pisoControl,
 * pimpleControl, simpleControl, potentialFlow, stressAnalysis). rc_to_pimple turns an icoFoam / pisoFoam /
 * nonNewtonianIcoFoam case into a pimpleFoam one, which solves the same flow and can adapt its time step. */
#ifndef RUNCTL_H
#define RUNCTL_H
#include <stdbool.h>
#include "foamcase.h"

typedef enum { RC_ALG_NONE = 0, RC_ALG_PISO, RC_ALG_PIMPLE, RC_ALG_SIMPLE, RC_ALG_POTENTIAL, RC_ALG_STRESS } RcAlg;

enum {
    RC_STEADY      = 1 << 0,   /* iterations, not time */
    RC_ADJUST      = 1 << 1,   /* reads adjustTimeStep / maxCo / maxDeltaT */
    RC_ALPHACO     = 1 << 2,   /* VoF: maxAlphaCo (interface Courant) */
    RC_DI          = 1 << 3,   /* maxDi (solid diffusion number: chtMultiRegionFoam) */
    RC_ACOUSTIC    = 1 << 4,   /* maxAcousticCo (cavitatingFoam) */
    RC_PREF        = 1 << 5,   /* incompressible: pRefCell / pRefValue in the loop dictionary */
    RC_TRANSONIC   = 1 << 6,   /* compressible pressure-based: transonic switch */
    RC_VOF         = 1 << 7,   /* alpha sub-cycles, MULES (solvers/"alpha.*") */
    RC_MOMPRED     = 1 << 8,   /* reads momentumPredictor (icoFoam always predicts) */
    RC_KNOWN       = 1 << 15,  /* in the table (not guessed from the name) */
};

typedef struct {
    const char *app;
    unsigned    flags;
    RcAlg       alg;
    const char *upgrade;       /* a solver of the same flow that can adapt its step, that rc_to_pimple converts to */
} RcSolver;

const RcSolver *rc_solver(const char *app);          /* never NULL: an unknown name gets a guess (no RC_KNOWN) */
const char     *rc_alg_dict(RcAlg a);                /* "PISO", "PIMPLE", ... ("" for none) */
/* the loop dictionary the case's fvSolution really has, else the one the solver expects */
const char     *rc_alg_in(FDFile *fvSolution, const RcSolver *s);

/* turn an icoFoam / pisoFoam / nonNewtonianIcoFoam case into pimpleFoam with an adjustable step (maxCo given): the
   files are edited in memory (case_dict) and turbulenceProperties is written when missing; the caller saves them all
   and rescans. Returns the number of changes, -1 with why in msg. msg gets a one-line list of what changed. */
int  rc_to_pimple(Case *c, double maxCo, char *msg, int cap);

/* a fixed step for a target Courant number from one that was seen: dt * target / co (0 when it can't tell) */
double rc_dt_for_courant(double dt, double co, double target);

/* turning adjustTimeStep on: writeControl timeStep counts steps whose length now changes, so it becomes adjustable
   with the same interval in seconds (N x deltaT). Returns true when it changed it (what in msg). */
bool rc_write_by_time(FDFile *controlDict, char *msg, int cap);
#endif
