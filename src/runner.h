/* runner.h -- run OpenFOAM tools as background jobs and follow their log files */
#ifndef RUNNER_H
#define RUNNER_H

#include <stdbool.h>
#include <sys/types.h>
#include <stdint.h>
#include <time.h>
#include "pl_util.h"

/* ------------------------------------------------------------------ log monitor */
#define MAX_SERIES 64             /* multi-region: one series per field and region, e.g. "h (topAir)" */
typedef struct {
    char   name[200];            /* "h (topAir)": field and region */
    float *t, *v;                /* time, initial residual */
    int    n, cap;
    bool   hidden;
    int    stepTag;              /* step count when the last point was pushed (per-step maxima) */
} Series;

typedef struct {
    char   name[64];             /* "log.simpleFoam"                           */
    char   path[PL_PATH];
    long long offset;            /* bytes consumed                             */
    unsigned long long ino;      /* inode: a deleted and recreated log is a new run */
    time_t mtime;
    /* text kept for the console (oldest lines dropped beyond the cap)        */
    char  *text; int textLen, textCap;
    int   *lineStart; int nLines, capLines;
    char   partial[4096]; int partialLen;
    /* parsed state */
    Series series[MAX_SERIES]; int nSeries;
    Series courant;              /* max Courant number per step (over all fluid regions) */
    Series interfaceCo;          /* interFoam & co: "Interface Courant Number" max per step */
    Series dt;                   /* deltaT per step (solvers with adjustTimeStep print it) */
    unsigned long long boundBits;/* bit k: a "bounding <field>" line k steps ago (bit 0 = this step) */
    char   boundField[32];       /*   the field last bounded */
    char   region[40];           /* multi-region: region being solved ("" = none) */
    double time, startTimeSeen, execTime, clockTime, deltaT;
    double contLocal;            /* the last "time step continuity errors : sum local" */
    int    steps;
    bool   seenThisStep[MAX_SERIES];
    bool   ended, fatal, fatalDone, converged, meshOk, meshFailed;
    int    warnings, errors;
    char   fatalMsg[512];
    char   warnMsg[512];         /* the last FOAM Warning's text (not the allowSystemOperations notice every log starts with):
                                    some tools end on a warning and a failing exit code ("No times selected")      */
    bool   warnOpen;
    long   nCells;
    int    nProcs;               /* "nProcs : 4" in the solver's header (0 = not seen) */
    int    prog;                 /* the program writing it (PROG_*, program.h): 0 OpenFOAM; others parse in program.c */
    double endTime;              /* end the log itself announces ("MAIN: Time: 3/200", DEME "end=") (0 = unknown) */
    int    endSteps;             /*   ... or the number of steps                                                  */
    int    exitCode;             /* "Q-Continuum: exit N" written after a program's run (-1 = not yet)             */
    int    pstate;               /* program parsers' own state (program.c)                                        */
} LogMon;

#define MAX_LOGS 32
typedef struct {
    char   casePath[PL_PATH];
    LogMon logs[MAX_LOGS];
    int    n;
    double lastScan;
    int    prog;                 /* the program of the folder (PROG_*): its logs are parsed that way */
} LogSet;

extern double g_log_budget_ms;   /* milliseconds of log reading per poll and log (default 8) */
void    logset_open(LogSet *s, const char *casePath);
void    logset_close(LogSet *s);
void    logset_poll(LogSet *s, double now, bool force);
int     logset_newest(const LogSet *s);                  /* most recently modified          */
int     logset_primary(const LogSet *s, const char *app); /* the log with residuals to plot */
int     logset_find(const LogSet *s, const char *name);
void    logset_restart(LogSet *s, const char *name);      /* a job is about to rewrite this log */
void    logmon_reset(LogMon *m);
void    logmon_feed(LogMon *m, const char *buf, int n);     /* parse log text directly (tests, fuzzing) */
const char *logmon_line(const LogMon *m, int i, int *len);
/* for the other programs' parsers (program.c): a new time step / increment at t; a value of a series for this step
   (the first one per step is kept, or the largest with keepMax); an error and its text (appended while open) */
void    logmon_step(LogMon *m, double t);
void    logmon_value(LogMon *m, const char *series, double v, bool keepMax);
void    logmon_fatal(LogMon *m, const char *text);

/* ------------------------------------------------------------------ jobs */
typedef enum { JS_PENDING = 0, JS_RUNNING, JS_DONE, JS_FAILED, JS_SKIPPED, JS_STOPPED } StepState;

typedef struct {
    char id[32];
    char label[64];
    char cmd[8192];               /* a command that doesn't fit becomes a failing step, never a cut-off one */
    char log[64];                /* log file name inside the case, "" = none   */
    bool solver;
    bool append;                 /* add to the log instead of replacing it (a continued run) */
    StepState st;
    double t0, t1;
    int exitCode;
    int rec;                     /* its entry in the command record (cmdlog), 0 = not launched */
} JobStep;

#define MAX_STEPS 24
typedef struct {
    JobStep steps[MAX_STEPS];
    int     n, cur;
    pid_t   pid;
    bool    active, stopping;
    double  stopAt;
    char    casePath[PL_PATH];
    char    bashrc[PL_PATH];
    char    title[96];
    double  startedAt, finishedAt;
    bool    finished, failed, stopped;
    int     launches;            /* bumped each time a step starts: the app resets that step's log monitor */
    int     procs;               /* processes it runs on (1 = serial), for the cores-in-use count */
    bool    isRun;               /* a run of the case (not a tool such as a snapshot): recorded in its history */
    bool    pausing;             /* Pause was pressed: the solver is writing its time step and will end */
    bool    convStop;            /* ... or a monitored value settled ("Stop when converged") */
    bool    external;            /* a run found going in the case (started elsewhere, or before the app restarted) */
    pid_t   extPg[8];            /*   its process groups: followed until they are gone, stopped as a whole        */
    int     nExtPg;
    double  extChecked;
} Job;

void job_reset(Job *j, const char *title);
void job_add(Job *j, const char *id, const char *label, const char *cmd, const char *log, bool solver);
bool job_start(Job *j, const char *casePath, const char *bashrc, double now);
void job_poll(Job *j, double now);                     /* advances steps; call every frame */
void job_stop(Job *j, double now);
void job_adopt(Job *j, const char *casePath, const char *title, const char *log, const pid_t *pg, int n, int procs, double now);
int  job_detach(Job *j);                              /* quitting: run the remaining steps detached */
const char *step_state_name(StepState s);

/* ------------------------------------------------------------------ command record (ROADMAP 2.2)
   every step the app launches, newest last, for the console's Commands view: what ran, where, how it ended */
typedef struct {
    int    seq;                  /* 1, 2, ...; also groups: steps of one job share jobSeq */
    int    jobSeq;
    char   casePath[PL_PATH];
    char   bashrc[PL_PATH];
    char   label[64];
    char   cmd[8192];
    char   log[64];
    bool   append;
    time_t when;
    double t0, t1;
    StepState st;
    int    exitCode;
} CmdRec;
#define CMDLOG_MAX 48
int            cmdlog_count(void);
const CmdRec  *cmdlog_get(int i);                       /* 0 = oldest kept */
/* the command as a user would type it in a terminal: cd, the OpenFOAM environment, the step with its log */
void           cmdlog_script(const CmdRec *r, bool withSetup, char *out, int cap);

#endif
