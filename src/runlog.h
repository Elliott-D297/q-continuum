/* runlog.h -- CAPTAIN'S LOG (ROADMAP 3.7): a record of every run, kept inside the case so it travels with it.
 * <case>/.qcontinuum/log/<id>.ini holds what the run was and how it ended; <id>.diff the setup change since the run
 * before (from the git setup history, safety.c). Written by the app when a run ends, read by History > Captain's log. */
#ifndef RUNLOG_H
#define RUNLOG_H

#include <stdbool.h>
#include <time.h>
#include "pl_util.h"

#define RUNLOG_DIR ".qcontinuum/log"
#define RUNLOG_MAX_RES 24
#define RUNLOG_MAX_STEPS 12

typedef struct {
    char   id[40];                    /* "20261020-231502" (+ "-2" ...): also the file name */
    time_t started, ended;
    double wall;                      /* seconds from the first step to the end */
    char   outcome[16];               /* finished / failed / stopped / paused / early / converged */
    char   reason[300];               /* why it failed or ended early, in words ("" = nothing to say) */
    char   version[32];               /* OpenFOAM "v2606", or the program's name */
    char   app[64];                   /* simpleFoam */
    int    procs;
    long   cells;
    double endTime, reached;          /* controlDict endTime, the last time the solver log reached */
    double exec;                      /* the solver's own ExecutionTime (s) */
    double courant;                   /* max Courant of the last step (-1 = none) */
    double nonOrtho, skew;            /* checkMesh's max non-orthogonality and skewness (-1 = no checkMesh) */
    char   rev[48], prevRev[48];      /* setup revisions: this run's, the previous run's */
    int    nRes;
    char   resName[RUNLOG_MAX_RES][48];
    double resVal[RUNLOG_MAX_RES];    /* the final initial residual of each equation */
    int    nSteps;
    char   stepLabel[RUNLOG_MAX_STEPS][48];
    char   stepState[RUNLOG_MAX_STEPS][12];
    double stepSec[RUNLOG_MAX_STEPS];
    bool   hasDiff;                   /* <id>.diff exists */
} RunRecord;

/* what the app knows when a run ends; runlog_write adds the rest from the case folder (logs, controlDict, checkMesh) */
typedef struct {
    const char *casePath;
    const char *outcome;
    const char *version;
    const char *solverLog;            /* "log.simpleFoam" ("" = the newest solver log) */
    int    procs;
    double wall;
    time_t ended;
    int    nSteps;
    const char *stepLabel[RUNLOG_MAX_STEPS];
    const char *stepState[RUNLOG_MAX_STEPS];
    double stepSec[RUNLOG_MAX_STEPS];
} RunEnd;

bool runlog_write(const RunEnd *in, char *id, int cap);              /* false when nothing could be written */
void runlog_write_bg(const RunEnd *in);                              /* the same in a forked child (logs can be 100 MB) */
int  runlog_list(const char *casePath, RunRecord *out, int max);     /* newest first */
bool runlog_read(const char *casePath, const char *id, RunRecord *r);
char *runlog_diff(const char *casePath, const char *id);             /* malloc'd, NULL when the record has no diff */
char *runlog_markdown(const char *casePath, const char *caseName);   /* malloc'd: every record, oldest first */
void runlog_summary(const RunRecord *r, char *out, int cap);         /* "simpleFoam · 4 procs · 12 225 cells · 45 s" */
void runlog_reap(void);                                              /* collect finished writer children */

#endif
