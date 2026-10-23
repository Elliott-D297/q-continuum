/* coupling.h -- preCICE coupled simulations: a folder with precice-config.xml and one folder per participant
 * (the layout of the preCICE tutorials). Finds participants, checks the set-up against each OpenFOAM case,
 * runs all participants together and follows preCICE's iteration and convergence logs. */
#ifndef COUPLING_H
#define COUPLING_H

#include <sys/types.h>
#include "foamcase.h"
#include "xmlspan.h"

#define CPL_MAX_PART 8
#define CPL_MAX_DIRS 8

typedef struct {
    char   name[64];                         /* participant name in precice-config.xml           */
    char   dirs[CPL_MAX_DIRS][128]; int nDirs;   /* candidate folders (relative to the coupling)  */
    int    dirSel;                           /* chosen candidate, -1 none                         */
    bool   foam;                             /* chosen folder is an OpenFOAM case                 */
    char   solver[64];
    bool   hasRunSh;
    int    procs;                            /* 1 = serial                                        */
    char   cmd[512];                         /* user's command; "" = default                      */
    /* run state */
    pid_t  pid;
    bool   running, done, failed, stopped;
    int    exitCode;
    double t0, t1;
    /* preCICE logs in the participant folder */
    float *itWin, *itN; int nIt, capIt;      /* iterations per time window                        */
    float *cvX, *cvR; int nCv, capCv;        /* first convergence column per coupling iteration    */
    char   cvName[64];
    time_t itMtime, cvMtime;
    long long itSize, cvSize;    /* with the mtime: a write in the same second as the last read */
} CParticipant;

typedef struct {
    char   path[PL_PATH];                    /* folder holding precice-config.xml                 */
    char   name[128];
    FDFile *cfg;                             /* precice-config.xml as text (edits, save)          */
    XTree  tree;
    const char *cfgText; int cfgLen;         /* tree was read from this text                      */
    CParticipant p[CPL_MAX_PART]; int n;
    bool   running, stopping;
    double startedAt, stopAt;
} Coupling;

typedef struct { char path[PL_PATH]; char name[128]; int nPart; time_t mtime; } CouplingSummary;

int   coupling_scan(const char *root, CouplingSummary *out, int max);   /* folders with precice-config.xml, depth <= 3 */
Coupling *coupling_open(const char *path);
void  coupling_close(Coupling *c);
void  coupling_refresh(Coupling *c);                 /* re-read the XML (after edits) and the participants   */
XNode *coupling_root(Coupling *c);                   /* <precice-configuration>, or NULL                      */
XNode *coupling_scheme(Coupling *c);                 /* the coupling-scheme:* element, or NULL                */
const char *coupling_dir(const Coupling *c, const CParticipant *p, char *buf, int cap);   /* absolute, "" if none */
void  coupling_default_cmd(const Coupling *c, const CParticipant *p, char *buf, int cap);
void  coupling_save_choices(const Coupling *c);       /* folder, processes and commands: .q-continuum-coupling.ini */

bool  coupling_start(Coupling *c, const char *bashrc, const char *preciceEnv, double now, char *err, int cap);
void  coupling_poll(Coupling *c, double now);
void  coupling_stop(Coupling *c, double now);
void  coupling_read_precice_logs(Coupling *c);

/* set-up checks: kind 0 ok, 1 warning, 2 error */
typedef struct { int kind; char who[64]; char msg[640]; int fix; } CCheck;
/* fix: 1 = add the adapter to controlDict, 2 = run download-meshes.sh, 100 + k = use the participant's folder k instead */

/* what folder d needs to start (cmd, or its run.sh): commands not on PATH, Python modules python3 can't find, files
   run.sh tests for with [ ! -f ... ]. Returns how many problems; `what` says them in words. */
int  coupling_missing(const char *d, const char *cmd, char *what, int cap);
int   coupling_check(Coupling *c, CCheck *out, int max);
int   coupling_existing_results(Coupling *c, char *buf, int cap);   /* OpenFOAM participants holding results; buf lists them */
bool  coupling_fix_adapter(Coupling *c, int part);

#endif
