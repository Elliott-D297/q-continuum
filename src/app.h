/* app.h -- application state shared by the pages */
#ifndef APP_H
#define APP_H

#include "env.h"
#include "foamcase.h"
#include "runner.h"
#include "ui.h"
#include "coupling.h"
#include "monitor.h"
#include "sound.h"
#include "redalert.h"
#include "explain.h"
#include "sweep.h"

typedef enum {
    PG_CASES = 0, PG_NEW, PG_OVERVIEW, PG_MESH, PG_BC, PG_PHYSICS, PG_NUMERICS, PG_RUN, PG_FILES, PG_SETUP, PG_RESULTS,
    PG_HISTORY,                                                       /* case pages (Setup, Results: other programs) */
    PG_SETTINGS, PG_COUPLING, PG_QUEUE, PG_STUDIES, PG_COUNT
} Page;

#define MAX_JOBS 8

#define WHATS_NEW_REV 7          /* bump with each batch of changes worth telling about (page_settings.c lists them) */
typedef enum { MD_NONE = 0, MD_OPEN_FOLDER, MD_CLONE, MD_CLEAN, MD_DELETE, MD_UNSAVED, MD_QUIT_RUNNING, MD_ADD_ENTRY, MD_RUN_RESULTS, MD_NEW_FILE, MD_RUN_SCRIPT, MD_CPL_NEW, MD_CPL_OPEN, MD_CPL_RESULTS, MD_PV_CLAUDE, MD_HIST_CONFIRM, MD_PRECHECK, MD_QUEUE_ADD, MD_MOVIE, MD_STUDY_NEW, MD_STUDY_OUT, MD_STUDY_EXP, MD_PROG_CLEAN, MD_CONFLICT, MD_SHORTCUTS, MD_WHATS_NEW, MD_PALETTE, MD_ENV, MD_REVIEW, MD_PLUME, MD_AWAY, MD_CPL_BUILD, MD_FLAVOUR } Modal;

typedef struct {
    Env      env;
    Settings set;
    Page     page;
    bool     quit;

    /* case list */
    CaseSummary *list;
    int      nList, capList;
    double   listScanned;
    char     search[128];
    int      catFilter;                 /* program shown: 0 all, 1.. PROG_* + 1 */
    int      physFilter;                /* OpenFOAM cases: 0 all, 1.. category + 1 */

    /* tutorials */
    TutorialIndex tut;
    bool     tutScanned;
    char     tutSearch[128];
    int      tutGroup;
    int      tutSel;
    char     cloneName[256];
    char     cloneErr[256];
    int      newProg;                   /* New case: the program (PROG_*) whose starters are shown */
    int      newSel;                    /*   the starter or example picked (-1 none)               */

    /* opened case */
    Case    *cs;
    LogSet   logs;
    MonSet   mons;                      /* the open case's function-object output (Run > Monitors) */
    int      chartTab;                  /* Run page chart: 0 residuals, 1 monitors                  */
    Job      jobs[MAX_JOBS + 1];        /* runs going at once, at most one per case; the last slot is a spare */
    Job     *cj;                        /* the open case's job: a slot of jobs[] */
    int      jobSeen[MAX_JOBS + 1];     /* per slot: the launch count whose log the monitor has restarted */
    char     pendingOpen[PL_PATH];      /* case to open after the unsaved-changes prompt */
    Page     pendingPage;

    /* per-page */
    UiScroll scr[24];
    int      bcField, bcPatch;          /* selected cell in the BC matrix (-1 none); bcField BC_INSPECT: every field on bcPatch */
    int      bcInternalSel;
    char     filesSel[PL_PATH];
    bool     filesRaw;
    int      runLog;                    /* index into logs, -1 = follow newest */
    int      runLogFilter;
    int      meshTab;                   /* Mesh page: 0 dictionaries, 1 SALOME, -1 not chosen */
    char     chartRegion[64];           /* residual chart: region shown ("" = all, "-" = not chosen yet) */
    bool     runFollow;
    Alert    alert;                     /* RED ALERT: the open case's running solver (app.c red_alert_tick) */
    int      alertDismissLevel, alertDismissSteps;   /* Continue pressed at this level and step count */
    bool     logMax;                    /* the log fills the whole page (Run, Coupling); Esc or Restore puts it back */
    bool     chartMax;                  /* the residuals chart fills the whole page (Run, Coupling); the same way back */
    int      runMode;                   /* 0 automatic steps, 1 Allrun */
    bool     stepOn[16];
    bool     continueRun;
    bool     stepInit;
    int      runProcs;                  /* processes for the next run (0 = not chosen yet) */
    int      runProcsFrom;              /* the default it was lowered from for a small mesh (0 = not lowered) */
    long     runProcsCells;             /*   the mesh's cell count behind that                              */
    bool     runProcsDict;              /*   the count came from the case's decomposeParDict                */
    bool     runConfirmed;              /* the user accepted overwriting existing results   */
    bool     runPending;                /* start the run on the next Run-page frame          */
    bool     copyPending;               /* copy the open case to the workspace, between frames */
    bool     precheckOk;                /* the user ran anyway past the pre-run warnings     */
    bool     bgStart;                   /* starting a queued run: no dialogs, no navigation  */
    int      settingsTab;
    int      histTab;                   /* History: 0 changes, 1 snapshots, 2 deleted results */
    int      histAction;                /* MD_HIST_CONFIRM: what is being confirmed            */
    int      numericsTab;
    int      physicsTab;

    /* coupling (preCICE) */
    CouplingSummary cplList[64];
    int      nCpl;
    bool     cplScanned;
    Coupling *cpl;
    int      cplTab, cplPart;           /* tab; participant shown in the monitor */
    char     cplBack[PL_PATH];          /* a participant opened from its coupled run: the way back */
    LogSet   cplLogs;
    char     cplValidate[2048];         /* precice-config-validate output        */
    int      cplValidateOk;             /* -1 not run, 0 failed, 1 ok            */
    double   cplLastRead;

    /* modals */
    Modal    modal;
    char     mText[PL_PATH];
    char     mText2[256];
    char     mErr[256];
    bool     mFlag;
    bool     mFlag2;
    char     mPath[1024];               /* dict path for "add entry" */
    char     mFile[PL_PATH];

    /* expanded nodes in tree editors */
    UiId     expanded[1024];
    bool     expandedState[1024];
    int      nExpanded;

    double   lastDiskCheck;
    double   lastInput;
} App;

extern App A;

/* terms.c: a feature's name in the current theme (LCARS, classic, Stardew, Pokémon) */
typedef enum { T_SWEEP = 0, T_ALERT, T_WATCH, T_PALETTE, T_REVIEW, T_REVIEW_GO, T_EXPLAIN, T_RUN_GO, T_ASK, T_LOG, T_AWAY, T__N } Term;
const char *term(Term t);
const char *term_in(Term t, int theme);
const char *alert_title(const Alert *a);

/* app.c */
void app_open_case(const char *path, Page page);
/* copy the open case into the workspace as <name>-copy[N] (writable) and open the copy */
void app_copy_case_to_workspace(void);
void app_close_case(void);
void app_rescan_cases(bool force);
void app_prog_dir(int prog, char *out, size_t cap);   /* the folder new projects of this program go in */
void app_discover_again(void);             /* look for projects outside the workspace now (Settings folders changed) */
bool app_save_all(bool quiet);
float app_fit_scale(void);                 /* the interface size in use: the setting, capped by the window */
void app_go(Page p);
/* a sound cue: plays only with a theme that has sounds (LCARS, Stardew, Pokemon) and Settings > Sounds on (see sound.c) */
void app_sound(Snd s);
int  app_sound_pack(void);              /* SOUND_* for the current theme, -1 for the silent classic ones */
/* open a coupled simulation folder on the Coupling page (page_coupling.c) */
void app_coupling_open(const char *path);
const char *app_bashrc(void);
bool app_require_foam(void);
void app_write_foam_file(void);
void app_open_paraview(void);
void app_paraview_claude(const char *request);   /* Claude Code in a terminal writes paraview/view.py */
bool app_paraview_claude_view(void);             /* open that script in ParaView; false if there is none */
bool app_claude_view_exists(void);
void app_claude_error(const char *logName, const char *fatalMsg);   /* Claude Code looks into a failed run */
void app_movie_modal(bool *close);              /* page_movie.c: the Make a movie dialog */
void app_movie_finished(Job *j);                /* page_movie.c: open the folder, say where it went */
bool app_job_running(void);
int  app_jobs_active(void);                     /* runs going, in all cases */
int  app_jobs_procs(void);                      /* processes they use */
bool app_can_start(void);                       /* this case isn't running and a slot is free; toasts why not */
void app_ask_delete_case(const char *path);   /* opens the confirmation dialog */
bool app_delete_case(const char *path, bool permanent, char *err, int cap);
bool app_can_pause(void);                       /* the open case's run is in its solver step */
void app_pause_run(void);                       /* the solver writes its time step and stops */
void app_alert_halve(void);                     /* RED ALERT: halve deltaT (maxCo with adjustTimeStep), saved */
bool app_add_bc(const char *fieldRel, const char *patch);   /* page_bc.c: the default condition for a patch */
void app_apply_fix(const Explanation *x);       /* the error translator's one-click fix */
const Sweep *run_sweep(Case *c, bool now);      /* page_run.c: SENSOR SWEEP of the case, cached 2 s (now: again) */
void app_resume_run(void);                      /* page_run.c: continue a paused / interrupted run */
bool app_resumable(bool *paused);               /* page_run.c: the open case's last run can be resumed */
int  app_queue_count(void);                     /* runs waiting in the queue */
bool app_queue_has(const char *path);
void app_queue_add(const char *path, int mode, int procs, bool cont, bool fresh);
void queue_tick(double now);                    /* start queued runs as cores free up */
Job *job_slot_for(const char *path);
bool run_start_queued(const char *path, int mode, int procs, bool cont, bool fresh);   /* page_run.c */
bool run_queue_ready(const char *path);
void plume_modal(bool *close);
void away_modal(bool *close);
void cpl_build_modal(bool *close);
/* flavour.c: the screen and game themes' moments, names and Run-page instruments */
enum { FEV_FAILED = 1, FEV_DIVERGE, FEV_COURANT, FEV_DONE, FEV_SWEEP };
void flavour_event(int ev, const char *casePath, const char *detail);
void flavour_modal(bool *close);
float flavour_run(Case *c, LogMon *m, bool running, float x, float y, float w);
const char *flavour_case_tag(const char *path);
void flavour_queue_progress(int started, bool emptyNow);
/* logo.c: the Q mark of each theme, and the window and desktop icon made from it */
void logo_draw(int theme, float x, float y, float s);
void logo_set_icon(int theme, bool desktop);
int  logo_export(const char *dir);                           /* --export-logos: logo-<theme>.png, 256 px */                           /* page_coupling.c: a coupled case from two cases */
void page_cpl_case(Rectangle r, int which);                  /* page_coupling.c: a coupled run's pages (0 overview, 1 setup, 2 participants, 3 run) */                                /* page_away.c: AWAY MISSION, a case packed for aerofly (5.1) */
int  run_steps_script(Case *c, int np, Str *out);             /* page_run.c: the run as a Slurm job's shell steps */                               /* page_cases.c: the impinging-plume starter (4.4) */                       /* page_run.c: its solver's parallel probe has answered */
bool app_deme_running(void);
void app_generate_mesh(void);                  /* page_case.c: blockMesh (and the region steps) as a job */
void app_start_tool(const char *title, const char *id, const char *label, const char *cmd, const char *log);

/* app_widgets.c -- page furniture and dictionary-bound controls */
#define PAGE_PAD 36.0f
#define CONTENT_MAX 1080.0f

float page_header(float x, float y, float w, const char *title, const char *subtitle);
float section_head_r(float x, float y, float w, const char *title, const char *desc, const char *right);
float section_head(float x, float y, float w, const char *title, const char *desc);
Rectangle prop_row(float x, float *y, float w, const char *label, const char *hint, float ctrlH);
void  callout(Rectangle r, int kind, const char *icon, const char *title, const char *text);
float callout_height(float w, const char *title, const char *text);
Color cat_color(Category c);
Color lc_for(const char *key);
void  lc_panel(Rectangle r, Color c, float hov, bool sel);
Color head_color(void);              /* page-level headings: accent in LCARS */           /* stable LCARS block colour for a label */
const char *cat_icon(Category c);
const char *prog_icon(int prog);         /* a program's icon and colour (PROG_*), for cards and the sidebar */
Color prog_color(int prog);
Color status_color(CaseStatus s);
float stat_tile(Rectangle r, const char *icon, const char *label, const char *value, const char *sub);
float region_bar(float x, float y, float w);           /* multi-region: region pills; height used */
float region_generated_note(float x, float y, float w);

bool  dv_text(FDFile *f, const char *path, Rectangle r, int flags, const char *ph);
bool  dv_number(FDFile *f, const char *path, Rectangle r, const char *defaultValue);
bool  dv_choice(FDFile *f, const char *path, Rectangle r, const char *const *items, int n, bool custom, const char *ph);
bool  dv_switch(FDFile *f, const char *path, float x, float y, const char *defaultOff);
bool  dv_value_editor(FDFile *f, const char *path, const char *key, const char *context, Rectangle r);

/* generic tree editor; returns content height. *edited set when the file changed (stop iterating) */
float dict_tree(FDFile *f, FDEntry *dict, const char *prefix, float x, float y, float w, int depth, bool *edited);
bool  tree_expanded(UiId id, bool def);
void  tree_toggle(UiId id, bool def);

/* suggestion tables (suggest.c) */
int  suggest_for_key(const char *key, const char *context, const char *const **out);
int  suggest_bc_types(const char *field, const char *cls, const char *const **out);
const char *hint_for_key(const char *key);
#define BC_INSPECT (-2)
bool app_put_value(FDFile *f, const char *path, const char *nv);   /* fd_put after value_problem (fieldinfo.h); toasts why not */
const char *bc_default_type(const char *field, const char *patchType);   /* what "Add condition" puts on a patch */
void bc_default_entries(FDFile *f, const char *patchPath, const char *type, const char *field, const char *cls);
extern const char *const SOLVER_APPS[];
extern const int N_SOLVER_APPS;

/* pages */
void page_cases(Rectangle r);
void page_new(Rectangle r);
void page_overview(Rectangle r);
void page_mesh(Rectangle r);
void page_bc(Rectangle r);
void page_physics(Rectangle r);
void page_numerics(Rectangle r);
void page_run(Rectangle r);
void run_case_script(const char *name, const char *args);
void page_files(Rectangle r);
void page_history(Rectangle r);
void history_modal(bool *close);
void history_invalidate(void);
void page_queue(Rectangle r);
void page_studies(Rectangle r);
void study_new_modal(bool *close);
void study_out_modal(bool *close);
void study_exp_modal(bool *close);
/* page_prog.c: the pages of the other programs' projects (A.cs->s.prog != 0) */
void page_prog_overview(Rectangle r);
void page_prog_setup(Rectangle r);
void page_prog_run(Rectangle r);
void page_prog_results(Rectangle r);
void prog_clean_modal(bool *close);
bool prog_start_run(const char *path, const char *input, int procs, bool quiet);   /* a run of the project, as a job */
bool prog_setup_dirty(void);                    /* the Setup page has edits not written yet */
bool prog_setup_pending(const char **name, const char **before, const char **after);   /* ... which file, from what to what */
void prog_setup_revert(void);
/* page_review.c: ROADMAP 3.5, the review before anything is written (term T_REVIEW) */
bool app_review_needed(void);                   /* edits waiting and the review is on (Settings) */
void review_open(int then);                     /* then: 0 just save, 1 save and run */
bool app_save_request(void);                    /* Save / Ctrl+S: the review first when it is on; true when written now */
void review_modal(bool *close);
bool prog_setup_save(void);
void prog_case_closed(void);                    /* forget the open project's Setup state */
/* page_deme.c: DEME parts of those pages */
void deme_toolchain(const char **demeDir, const char **nvcc, const char **gpu);   /* what it found ("" if not) */
float deme_setup_panel(const char *proj, float x, float y, float w);              /* returns height */
bool  deme_has_params(const char *proj);
float deme_run_panel(const char *proj, const char *logPath, float x, float y, float w, bool running);
float deme_particles_panel(const char *dir, const char *prefix, float x, float y, float w, float maxH, bool running, bool compact);
float deme_pair_panel(float x, float y, float w, bool running, bool (*startBoth)(const char *foamCase));
bool  deme_driver_source(const char *proj, const char *target, char *out, int cap);
void  deme_build_error(const char *proj, char *out, int cap);
int   deme_template_count(void);
const char *deme_template_id(int i);
bool  deme_write_project(const char *dir, const char *tplId, char *err, int cap);
void env_modal(bool *close);                    /* page_env.c: the Environment panel */
void deme_pair_tick(double now);               /* a coupled DEME + OpenFOAM pair stops together */
bool text_editor(FDFile *f, Rectangle r);          /* texteditor.c: true when the text changed */
bool text_editor_focused(void);
void text_editor_goto(int line);               /* the next file drawn opens at this line */
void page_settings(Rectangle r);
void page_coupling(Rectangle r);
int   bc_missing_report(Case *c, char *msg, int cap);         /* page_bc.c: patches without a BC */
const char *bc_type_on(Case *c, const char *field, int patch, char *out, int cap);   /* page_bc.c */
int   turb_missing_fields(Case *c, char *msg, int cap);
void  case_turbulence(Case *c, char *st, int stCap, char *model, int mCap);   /* simulationType and model, "" when unset */
/* page_case.c: blockMeshDict's base size (the largest cell): with write, every block count follows base; *current and
   *cellsOut report the present size and the total for base (see the Mesh page) */
bool  blocks_base_size(FDFile *bm, double scale, double base, bool write, double *current, long *cellsOut, char *why, int cap);
void  decomp_split(Case *c, long np, char *out, int cap);    /* page_dicts.c: (nx ny nz) for np */       /* page_dicts.c: fields the turbulence model needs */
float page_salome(Rectangle r, float x, float y, float w);   /* page_salome.c: returns height */
float page_mesh_patches(float x, float y, float w);
bool  mesh_change_pending(Case *c, char *why, int cap);       /* page_case.c: the next run meshes again */
float xsection_panel(Rectangle r, Case *c, bool compact);
float meshview_panel(Rectangle r, Case *c);                 /* page_meshview.c: Mesh > View, the mesh in 3-D */   /* page_xsection.c: a 2-D case's cross section */          /* page_case.c: cells, patches and their types */
void coupling_frame(double now);              /* poll the coupled run every frame */
void run_chart(Rectangle r, LogMon *m);
float chart_max_button(float right, float y);   /* page_run.c: Expand / Restore for the chart, right-aligned at right; returns its width */      /* page_run.c: residual chart  */
void run_console(Rectangle r, LogSet *ls, int sel);
void draw_modals(void);

#endif
