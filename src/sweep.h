/* sweep.h -- SENSOR SWEEP: the pre-flight check of a case's dictionaries (see sweep.c) */
#ifndef SWEEP_H
#define SWEEP_H

#include "explain.h"
#include "foamcase.h"
#include "precheck.h"

typedef struct {
    int level;                   /* PC_OK (for the record), PC_WARN, PC_BAD */
    Explanation x;               /* title, text, file:line, and the fix */
    bool ask;                    /* a warning that still stops Run to ask: it would spoil the run (H-08) */
} Finding;

#define SWEEP_MAX 32
typedef struct {
    Finding f[SWEEP_MAX];        /* worst first */
    int n, worst, checks;        /* checks: how many of the sweep's checks applied to this case */
    bool ask;                    /* a finding asks before a run (any PC_BAD, or a warning with ask) */
} Sweep;

int sensor_sweep(Case *c, Sweep *w);   /* returns the worst level */

#endif
