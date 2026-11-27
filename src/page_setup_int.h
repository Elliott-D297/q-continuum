/* page_setup_int.h -- what page_setup.c and page_setup_steps.c share (improvement plan A5, 2026-11-26): the open case's
 * setup (SU), the step numbers and their status, and the helpers both draw with. Not for other pages. */
#ifndef PAGE_SETUP_INT_H
#define PAGE_SETUP_INT_H
#include "app.h"
#include "simsetup.h"

/* Setup's steps (the page) */
enum { SS_MODELS = 0, SS_MATERIALS, SS_BOUNDARIES, SS_ZONES, SS_START, SS_REPORTS, SS_SOLVER, SS_CHECK, SS__N };
enum { SS_OK = 0, SS_ATTN, SS_ERR };
typedef struct { int state; char sub[160]; } SsStatus;
typedef struct {
    char     casePath[PL_PATH];
    SimSetup s;
    double   size;               /* the part's length, m                       */
    double   height;             /* buoyancy's length, m: the mesh's extent along gravity (else size) */
    bool     external;           /* the STL sits in a wind tunnel              */
    double   cdMt;               /* system/controlDict when its controls were last taken in */
    int      step;               /* the open step (SS_*)                       */
    bool     picking;            /* the template cards, over a setup that has one */
    bool     more[SS__N];            /* a step's More rows shown                   */
} SetupState;
extern SetupState SU;
extern const char *const SS_TITLE[SS__N];
extern const char *const TMPL_ICON[TMPL__N];

float boundaries_step(Case *c, float x, float y, float w);
float check_step(Case *c, float x, float y, float w, const SsStatus st[SS__N]);
bool su_files_stale(Case *c);
void su_gravity_auto(Case *c);
float su_issues_callout(int step, float x, float y, float w);
void su_list_set(char *list, size_t cap, const char *name, bool on);
float materials_step(float x, float y, float w);
int su_mesh_zones(const char *casePath, char (*out)[64]);
float models_step(Case *c, float x, float y, float w);
float su_more_toggle(int step, float x, float y);
double su_mtime_ns(const char *p);
float su_note(float x, float y, float w, const char *t);
bool su_num_in(UiId id, Rectangle r, double *v, const char *fmt, double lo, double hi, const char *unit);
float reports_step(Case *c, float x, float y, float w);
double su_reynolds(void);
bool su_row_count(float x, float *y, float w, bool hints, const char *id, const char *label, const char *hint, int *v, int lo, int hi);
bool su_row_num(float x, float *y, float w, bool hints, const char *id, const char *label, const char *hint, double *v, double lo, double hi,
                    const char *unit, const char *after);
bool su_row_vec(float x, float *y, float w, bool hints, const char *id, const char *label, const char *hint, double v[3], const char *unit);
void su_save(void);
float solver_step(float x, float y, float w);
float start_step(float x, float y, float w);
int su_step_issues(int step, char *out, size_t cap);
void su_sync(Case *c);
float su_wall_rows(Case *c, float x, float y, float w, bool hints);
void su_water_default(Case *c);
bool su_write_files(Case *c, bool thenRun);
float zones_step(Case *c, float x, float y, float w);
#endif
