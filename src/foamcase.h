/* foamcase.h -- an OpenFOAM case on disk: summary for the case list, full model when opened,
 * the tutorial index, and helpers for creating dictionary files. */
#ifndef FOAMCASE_H
#define FOAMCASE_H

#include "foamdict.h"
#include "pl_util.h"

typedef enum { CAT_INCOMP = 0, CAT_HEAT, CAT_MULTI, CAT_COMP, CAT_OTHER, CAT_COUNT } Category;
typedef enum { CS_NEW = 0, CS_MESHED, CS_RUNNING, CS_DONE, CS_FAILED, CS_STOPPED, CS_QUEUED, CS_PAUSED } CaseStatus;

Category    category_of(const char *app, const char *relHint);
const char *category_name(Category c);
const char *status_name(CaseStatus s);

typedef struct {
    char   path[PL_PATH];
    char   name[256];
    char   app[64];
    Category cat;
    double startTime, endTime, deltaT;
    long   nCells, nFaces, nPoints, nInternal;
    bool   meshed;
    int    nTimes;               /* result time directories (> start time)  */
    char   latest[32];
    bool   hasAllrun, hasZero, hasZeroOrig, hasProcessors;
    bool   readOnly;             /* the case folder can't be written: no saving, no running */
    char   allrun[24];           /* the run script: Allrun, else Allrun-parallel / Allrun-serial ("" = none) */
    bool   groupAllrun;          /* part of a tutorial group whose ../Allrun (or Allrun.pre) drives this case */
    CaseStatus status;
    time_t activity;             /* newest of: controlDict, logs, time dirs */
    char   lastLog[64];
    int    nRegions;             /* multi-region (CHT) cases: from regionProperties          */
    char   regions[16][64];
    bool   regionMeshed;         /* constant/<region>/polyMesh exists (splitMeshRegions ran) */
    time_t meshTime;             /* mtime of constant/polyMesh/owner, 0 if none               */
    bool   meshStale;            /* a mesh dictionary was changed after the mesh was written  */
    char   meshStaleDict[48];    /*   e.g. "system/blockMeshDict"                              */
    bool   regionStale;          /* region meshes older than the base mesh or topoSetDict     */
    int    nProcDirs;            /* processor0..N-1 (decomposed case)                         */
    int    procTimes;            /* time directories in processor0 later than startTime      */
    char   procLatest[32];       /* latest of those                                           */
    char   runningLog[64];       /* solver log written in the last 30 s by any process       */
    int    prog;                 /* PROG_* (program.h): 0 OpenFOAM, else the other program's project; for those
                                    app = the program's name, nTimes = result files, the fields below           */
    char   input[128];           /*   the first thing it runs (A.inp, heat.i, a DEME target ...)               */
    int    nInputs;
    char   lastRun[96];          /*   the newest run log, for the card                                         */
} CaseSummary;

bool case_is_case(const char *dir);                          /* an OpenFOAM case                          */
bool case_is_project(const char *dir);                       /* ... or any other program's project        */
bool case_has_results(const CaseSummary *s);
const char *foam_path_problem(const char *path);             /* why OpenFOAM can't run here, or NULL */                 /* times after startTime, here or in processor* */
const char *case_latest_result(const CaseSummary *s);        /* "" if none                                  */
void case_summarize(const char *path, CaseSummary *out);

typedef struct { char name[128]; char type[48]; int nFaces; bool pattern; char groups[160];
                 bool collapsed; } PatchInfo;   /* groups: inGroups, space-separated; collapsed: every blockMeshDict face
                                                   repeats a vertex ((0 1 1 0): an axis), so blockMesh drops the patch */

typedef struct { char rel[256]; FDFile *f; } CaseFile;

/* per-case Q-Continuum settings, <case>/.q-continuum.ini: the user's own steps and script arguments */
#define CFG_MAX_CMDS 8
#define CFG_MAX_ARGS 32
typedef struct {
    char pre[CFG_MAX_CMDS][256];  int nPre;      /* shell commands run just before the solver */
    char post[CFG_MAX_CMDS][256]; int nPost;     /* ... and just after it (after reconstructPar) */
    char argKey[CFG_MAX_ARGS][64], argVal[CFG_MAX_ARGS][128]; int nArgs;   /* case scripts: arguments */
    char monFile[256];                            /* Run page monitor: the function-object file shown  */
    char monHide[512];                            /*   its columns switched off, "|"-separated         */
    bool convOn;                                  /* stop the run when this column has settled:        */
    char convFile[256], convCol[64];
    double convTol;                               /*   relative change (0.001 = 0.1 %) ...             */
    int  convWin;                                 /*   ... over this many samples                      */
} CaseCfg;
void        casecfg_load(const char *casePath, CaseCfg *c);
bool        casecfg_save(const char *casePath, const CaseCfg *c);
const char *casecfg_args(const CaseCfg *c, const char *script);
void        casecfg_set_args(CaseCfg *c, const char *script, const char *args);

#define MAX_PATCHES 512
#define MAX_CASEFILES 192

typedef struct {
    CaseSummary s;
    PatchInfo patches[MAX_PATCHES];
    int       nPatches;
    int       patchSource;       /* 0 none, 1 polyMesh/boundary, 2 blockMeshDict, 3 fields */
    char      region[64];        /* multi-region: region being edited ("" = base mesh)      */
    char      fieldDir[128];     /* "0.orig" when present (the source of truth), else "0";
                                    with a region "0.orig/<region>" or "0/<region>"          */
    long      rCells, rFaces, rPoints;   /* the selected region's mesh (-1 unknown)           */
    NameList  fields;            /* files in fieldDir                                       */
    NameList  times;             /* all time directories                                    */
    CaseFile  files[MAX_CASEFILES];
    int       nFiles;
    CaseCfg   cfg;
} Case;

Case   *case_open(const char *path);
void    case_close(Case *c);
void    case_rescan(Case *c);                        /* summary, patches, fields, times      */
FDFile *case_dict(Case *c, const char *rel);         /* cached; NULL if the file is missing   */
void    case_set_region(Case *c, const char *region); /* "" = base mesh; rescans               */
const char *case_rpath(const Case *c, const char *rel);  /* "constant/X" -> "constant/<region>/X" */
FDFile *case_create_dict(Case *c, const char *rel, const char *cls, const char *body);
int     case_dirty_count(const Case *c);
int     case_save_all(Case *c);                      /* returns files saved, -1 on error      */
void    case_reload_changed(Case *c);                /* take in files changed outside the app (see fd_sync) */
int     case_reload_report(Case *c, char *what, int cap);   /* ... and say which: returns how many reloaded or merged */
int     case_conflicts(const Case *c, int *first);   /* files changed both here and on disk: the user chooses */
void    case_drop_file(Case *c, const char *rel);
enum { TR_LOGS = 1, TR_POST = 2, TR_ZERO = 4, TR_MESH = 8 };   /* times after startTime and processor* always */
#define TRASH_KEEP 5                                 /* trash entries kept per case ...  */
#define TRASH_DAYS 14                                /* ... and for at most this long    */
int     case_trash_results(Case *c, int what, bool permanent, const char *reason, char *msg, int cap);

/* pause: the solver writes the time step it is on and ends; Resume continues from that write */
#define PAUSE_REQ  ".qc-pause"                       /* a pause was asked for: the stopAt to put back */
#define PAUSE_MARK ".qc-paused"                      /* the last run ended by a pause: "t=<time>"     */
#define PAUSE_SIGNAL_OPT "-opt-switch stopAtWriteNowSignal=10"   /* SIGUSR1 = write and stop      */
bool case_runtime_modifiable(const char *casePath);
bool log_tail_state(const char *casePath, const char *logName, double *lastTime, bool *ended, bool *converged);
int  case_processes(const char *casePath, const char *app, pid_t *out, int max, bool needSwitch);
int  case_run_groups(const char *casePath, const char *app, pid_t *pg, int max, int *nSolver);
bool case_pause_request(const char *casePath, const char *app, char *err, int cap);
bool case_pause_pending(const char *casePath);
bool case_pause_settle(const char *casePath, bool paused, const char *latest);
bool case_paused(const char *casePath, char *t, int cap);
void case_pause_clear(const char *casePath);

void foam_header(Str *b, const char *cls, const char *location, const char *object);

/* ---------------- tutorials ---------------- */
typedef struct {
    char rel[256];               /* e.g. incompressible/simpleFoam/pitzDaily  */
    char name[96];
    char group[64];              /* first path component                      */
    char solver[64];
    bool hasAllrun;
    Category cat;
} Tutorial;

typedef struct {
    char root[PL_PATH];
    Tutorial *t;
    int n, cap;
} TutorialIndex;

void tutorials_scan(TutorialIndex *ix, const char *root);
void tutorials_free(TutorialIndex *ix);
int  tutorials_find(const TutorialIndex *ix, const char *suffix);   /* index or -1 */
bool tutorial_needs_group(const char *src, char *why, int cap);       /* copy the tutorial's whole group? why: the reason */

#endif
