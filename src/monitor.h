/* monitor.h -- follow function-object output (postProcessing/<name>/<time>/<file>) while a case runs: forces,
 * probes, field min/max, surface averages ... one series per column, across the time folders a restart adds */
#ifndef MONITOR_H
#define MONITOR_H

#include <stdbool.h>
#include "pl_util.h"

#define MON_MAX_COLS  48
#define MON_MAX_FILES 32
#define MON_MAX_PARTS 16
#define MON_MAX_SKIP  32

typedef struct {
    char   key[256];              /* "forces/force.dat" (region/name/file without the time folder) */
    char   label[160];            /* "forces · force.dat" */
    char   parts[MON_MAX_PARTS][PL_PATH];   /* the file in each time folder, oldest first */
    long long off[MON_MAX_PARTS];
    unsigned long long ino[MON_MAX_PARTS];
    int    nParts;
    char   partial[4096]; int partialLen; int partPos;   /* reading parts[partPos] */
    char   lastHeader[1024];      /* the last comment line with words: column names */
    char   probeNames[MON_MAX_COLS][24]; int nProbes;   /* "# Probe 3 (x y z)" lines */
    int    nCols;
    char   col[MON_MAX_COLS][48];
    float *t, *v[MON_MAX_COLS];
    int    n, cap;
} MonFile;

typedef struct {
    char    casePath[PL_PATH];
    MonFile f[MON_MAX_FILES];
    int     n;
    double  lastScan;
    char    skip[MON_MAX_SKIP][200];   /* function objects that write no time series (sampled sets, surfaces): never scanned again */
    int     nSkip;
} MonSet;

void monset_open(MonSet *s, const char *casePath);
void monset_close(MonSet *s);
void monset_poll(MonSet *s, double now);     /* new files, then new lines; cheap when nothing changed */
int  monset_find(const MonSet *s, const char *key);

/* has column c of f settled? relative change (max-min)/|mean| over the last `window` rows below tol */
bool mon_converged(const MonFile *f, int c, int window, double tol, double *change);

#endif
