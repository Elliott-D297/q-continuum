/* cplbuild.h -- a coupled (preCICE) case made from two OpenFOAM cases: conjugate heat transfer across one interface patch
 * each. Copies the cases into <out>/<participant>/, writes precice-config.xml, each participant's system/preciceDict,
 * the adapter in its controlDict, the interface's temperature condition for what it reads, and a run.sh. The nuances are
 * checked first (cplb_check): time steps against the time window, end times, the interface patches, the adapter's solver
 * type and what it needs, 2-D or 3-D. */
#ifndef CPLBUILD_H
#define CPLBUILD_H

#include <stdbool.h>
#include "pl_util.h"

typedef struct {
    char   name[64];            /* participant name in preCICE: "Fluid" */
    char   src[PL_PATH];        /* the OpenFOAM case it is copied from */
    char   patch[128];          /* its interface patch */
    double k;                   /* thermal conductivity W/(m K): solverType basic (laplacianFoam knows only DT) */
    /* read from the case (cplb_inspect) */
    char   solver[64], solverType[16];   /* "compressible" / "basic" / "incompressible" / "" (not supported) */
    double deltaT, endTime;
    bool   adjustable;
    int    dims;                /* 2 with an empty patch, else 3 */
    char   patches[2048];       /* every patch, space-separated */
    bool   ok;                  /* a readable OpenFOAM case */
} CplPart;

typedef struct {
    CplPart p[2];               /* p[0] writes Temperature and reads Heat-Flux (Dirichlet side), p[1] the other way */
    double window, maxTime;     /* 0 = from the cases */
    int    maxIter;             /* 30 */
} CplBuild;

void cplb_inspect(CplPart *p);                       /* fills the read-from-the-case fields from p->src */
typedef struct { int sev; char text[400]; } CplCheck; /* sev 2 stops the build, 1 a warning, 0 for the record */
int  cplb_check(CplBuild *b, CplCheck *out, int max);  /* also fills window/maxTime when 0 */
bool cplb_write(const CplBuild *b, const char *outDir, char *err, int cap);

#endif
