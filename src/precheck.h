/* precheck.h -- before a run: will it fit in memory, is the time step stable, are there cores for it */
#ifndef PRECHECK_H
#define PRECHECK_H

#include <stdbool.h>
#include "foamcase.h"

enum { PC_OK = 0, PC_WARN, PC_BAD, PC_UNKNOWN };

typedef struct {
    int    level;                 /* PC_* */
    char   title[64];
    char   brief[96];             /* one line for the Run page */
    char   text[800];
} PreItem;

typedef struct {
    PreItem mem, co, cores;
    int    worst;                 /* the highest level of the three (PC_UNKNOWN counts as OK) */
    double memNeedMB, memAvailMB;
    double co_;                   /* typical Courant number, < 0 when not known */
    int    freeCores;
} PreCheck;

/* procs: processes the run will use; ourProcs: processes the app's other runs use now */
void precheck_run(const CaseSummary *s, int procs, int ourProcs, int nCpu, PreCheck *out);

/* pieces, exposed for tests */
bool   mesh_bounds(const char *casePath, double lo[3], double hi[3]);     /* from constant/polyMesh/points */
double field_max_velocity(const char *casePath);                         /* |U| from the initial field, < 0 unknown */
double mem_available_mb(void);
double load_average(void);

#endif
