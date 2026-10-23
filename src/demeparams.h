/* demeparams.h -- the DEME page's data, kept free of graphics so the tests can drive it (see demeparams.c):
 * a project's params.ini, the progress and results its driver prints, and particle frames (DEME's sphere CSVs). */
#ifndef DEMEPARAMS_H
#define DEMEPARAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* ---------------------------------------------------------------- params.ini */
/* key = value lines in [sections]; a comment "# Label | unit | help" just above a key describes it */
typedef struct {
    char section[48], key[64], value[128], label[96], unit[24], help[240];
    int  line;                                     /* index into lines */
} DemeParam;

typedef struct {
    char      path[1024];
    char    **lines; int nLines;
    DemeParam *p;    int n;
    long long stamp;                               /* mtime in ns + size: an outside edit within the same second still shows */
    bool      dirty;                               /* edited since the last save */
} DemeParams;

bool        deme_params_load(DemeParams *d, const char *path);    /* false: no file (d is left empty) */
long long   deme_params_stamp(const char *path);                 /* 0: no file */
void        deme_params_free(DemeParams *d);
bool        deme_params_save(DemeParams *d);                     /* writes the lines back, comments and order kept */
DemeParam  *deme_param(DemeParams *d, const char *key);
double      deme_param_num(const DemeParams *d, const char *key, double def);
bool        deme_param_set(DemeParams *d, const char *key, const char *value);   /* false: no such key */
bool        deme_param_set_num(DemeParams *d, const char *key, double v);

/* Rayleigh time step of the smallest sphere: pi r sqrt(rho / G) / (0.1631 nu + 0.8766), G = E / 2(1 + nu) */
double deme_rayleigh_dt(double r, double rho, double E, double nu);

/* presets: named sets of key values (gravity of a body, a material); returns how many keys it set */
typedef struct { const char *name, *note; const char *kv[8][2]; } DemePreset;
extern const DemePreset DEME_GRAVITY[];   extern const int DEME_N_GRAVITY;
extern const DemePreset DEME_MATERIALS[]; extern const int DEME_N_MATERIALS;
int deme_apply_preset(DemeParams *d, const DemePreset *p);

/* ---------------------------------------------------------------- progress */
#define DEME_SERIES 4096
#define DEME_RESULTS 16
typedef struct {
    long   offset;                                 /* bytes of the log read so far */
    bool   any;                                    /* a "QC t=" line was seen */
    double t, end, ke, vmax;
    int    frame, frames;
    long   n;
    double rate;                                   /* simulated seconds per wall second, from the latest lines */
    double t0, wall0;                              /* the first sample this session saw, for the rate */
    float  ts[DEME_SERIES], kes[DEME_SERIES]; int nS;
    char   resName[DEME_RESULTS][48], resVal[DEME_RESULTS][48]; int nRes;
    char   extra[160];                             /* the latest other "QC <text>" line, e.g. discharged mass */
    int    crash;                                  /* DEME_CRASH_*: a known way the run failed, from the log */
} DemeProgress;

enum { DEME_CRASH_NONE = 0, DEME_CRASH_FORCES, DEME_CRASH_MEMORY, DEME_CRASH_NO_PARAMS };
const char *deme_crash_text(int crash);           /* what it means and what to change, for the page */

void deme_progress_reset(DemeProgress *p);
/* reads what the log has gained since the last call (from the start if it shrank); now = wall clock */
void deme_progress_poll(DemeProgress *p, const char *logPath, double now);
/* parses one line: "QC t=... end=... frame=i/n ke=... vmax=... n=...", "QC result name=value unit", "QC <text>" */
void deme_progress_line(DemeProgress *p, const char *line, double now);

/* ---------------------------------------------------------------- frames */
typedef struct {
    float *x, *y, *z, *r, *v;
    int    n, cap;
    float  lo[3], hi[3], vmax, rmin, rmax;
} DemeFrame;

bool deme_frame_load(DemeFrame *f, const char *csv);    /* columns found by the header (X,Y,Z,r,absv) */
void deme_frame_free(DemeFrame *f);

#endif
