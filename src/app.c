/* app.c -- Q-Continuum: main loop, navigation shell, case lifecycle */
#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "app.h"
#include "undo.h"
#include "actions.h"
#include "assets.h"
#include "safety.h"
#include "runlog.h"
#include "program.h"
#include "script.h"
#include "rlgl.h"

App A;

static const char *PAGE_TITLES[PG_COUNT] = { "Cases", "New case", "Overview", "Mesh", "Boundary conditions", "Physics",
                                             "Numerics", "Run", "Files", "Setup", "Results", "History", "Settings", "Coupling", "Queue",
                                             "Studies" };

/* the open case's pages: OpenFOAM's, or the other programs' (Setup and Results instead of Mesh ... Numerics) */
static const Page FOAM_PAGES[] = { PG_OVERVIEW, PG_MESH, PG_BC, PG_PHYSICS, PG_NUMERICS, PG_RUN, PG_FILES, PG_HISTORY };
static const Page PROG_PAGES[] = { PG_OVERVIEW, PG_SETUP, PG_RUN, PG_RESULTS, PG_FILES, PG_HISTORY };
/* a preCICE coupled run: Setup is its coupling (data, meshes, scheme, XML), Results its participants */
static const Page CPL_PAGES[] = { PG_OVERVIEW, PG_SETUP, PG_RESULTS, PG_RUN, PG_FILES, PG_HISTORY };
static bool coupled_open(void) { return A.cs && A.cs->s.prog == PROG_COUPLED; }
static int case_pages(const Page **out)
{
    if (coupled_open()) { *out = CPL_PAGES; return (int)(sizeof CPL_PAGES / sizeof CPL_PAGES[0]); }
    bool other = A.cs && A.cs->s.prog;
    *out = other ? PROG_PAGES : FOAM_PAGES;
    return other ? (int)(sizeof PROG_PAGES / sizeof PROG_PAGES[0]) : (int)(sizeof FOAM_PAGES / sizeof FOAM_PAGES[0]);
}
/* the page's name for the open case: a coupled run calls Setup "Coupling setup" and Results "Participants" */
static const char *page_label(Page p)
{
    if (coupled_open() && p == PG_SETUP) return "Coupling setup";
    if (coupled_open() && p == PG_RESULTS) return "Participants";
    return p >= 0 && p < PG_COUNT ? PAGE_TITLES[p] : "";
}
/* a coupled run going in the open case (its participants run outside the job slots) */
static bool coupled_running(void) { return coupled_open() && A.cpl && A.cpl->running && !strcmp(A.cpl->path, A.cs->s.path); }
static const char *page_icon(Page p)
{
    switch (p) {
    case PG_OVERVIEW: return ICON_HOUSE; case PG_MESH: return ICON_GRID_3X3; case PG_BC: return ICON_LAYERS; case PG_PHYSICS: return ICON_ATOM;
    case PG_NUMERICS: return ICON_SIGMA; case PG_RUN: return ICON_ACTIVITY; case PG_FILES: return ICON_FILE_TEXT; case PG_SETUP: return ICON_SLIDERS_HORIZONTAL;
    case PG_RESULTS: return ICON_BOXES; case PG_HISTORY: return ICON_CLOCK; default: return ICON_FILE_TEXT;
    }
}

const char *app_page_title(int p) { return p >= 0 && p < PG_COUNT ? page_label((Page)p) : ""; }

/* ============================================================================ helpers */
const char *app_bashrc(void)
{
    const FoamInstall *f = env_active(&A.env, &A.set);
    return f ? f->bashrc : "";
}

bool app_require_foam(void)
{
    if (app_bashrc()[0]) return true;
    ui_toast(TOAST_ERR, "OpenFOAM was not found. Install it or set its etc/bashrc in Settings.");
    return false;
}

bool app_job_running(void) { return A.cj->active; }

/* ------------------------------------------------------------------ runs at once: one job slot per case */
int app_jobs_active(void)
{
    int n = 0;
    for (int i = 0; i <= MAX_JOBS; i++) if (A.jobs[i].active) n++;
    return n;
}

/* processes the running jobs use (their decomposition, or 1 for a serial run) */
int app_jobs_procs(void)
{
    int n = 0;
    for (int i = 0; i <= MAX_JOBS; i++) if (A.jobs[i].active) n += A.jobs[i].procs > 0 ? A.jobs[i].procs : 1;
    return n;
}

/* ------------------------------------------------------------------ pause */
bool app_can_pause(void)
{
    if (!A.cs || !A.cj->active || strcmp(A.cj->casePath, A.cs->s.path) || A.cj->cur >= A.cj->n) return false;
    if (!A.cj->steps[A.cj->cur].solver) return false;
    /* Allrun runs mesh tools in its solver step too: only while the solver itself is going (checked every 2 s) */
    static double at; static bool val; static char path[PL_PATH];
    if (GetTime() - at > 2.0 || strcmp(path, A.cs->s.path)) {
        at = GetTime(); pl_strlcpy(path, A.cs->s.path, sizeof path);
        pid_t p[4];
        val = case_processes(A.cs->s.path, A.cs->s.app[0] ? A.cs->s.app : NULL, p, 4, false) > 0;
    }
    return val;
}

/* RED ALERT (3.3): watch the open case's running solver twice a second; a new or worse alert sounds the klaxon and,
   off the Run page, says so */
static void red_alert_tick(double now)
{
    static double at; static int lastLevel; static char lastCase[PL_PATH];
    if (now - at < 0.5) return;
    at = now;
    bool mine = A.cs && A.cj->active && !strcmp(A.cj->casePath, A.cs->s.path);
    if (!mine) { memset(&A.alert, 0, sizeof A.alert); lastLevel = 0; A.alertDismissLevel = 0; return; }
    if (strcmp(lastCase, A.cs->s.path)) { pl_strlcpy(lastCase, A.cs->s.path, sizeof lastCase); lastLevel = 0; A.alertDismissLevel = 0; }
    int prim = logset_primary(&A.logs, A.cs->s.app);
    const LogMon *m = prim >= 0 ? &A.logs.logs[prim] : NULL;
    FDFile *cd = case_dict(A.cs, "system/controlDict");
    char adj[16] = "", mc[32] = "";
    if (cd) { fd_get(cd, "adjustTimeStep", adj, sizeof adj); fd_get(cd, "maxCo", mc, sizeof mc); }
    bool adjust = !strcmp(adj, "yes") || !strcmp(adj, "on") || !strcmp(adj, "true");
    red_alert_check(m, adjust, atof(mc), &A.alert);
    /* Continue: quiet until it gets worse, or 100 more steps go by still alerting */
    if (A.alertDismissLevel && m && (A.alert.level > A.alertDismissLevel || m->steps > A.alertDismissSteps + 100)) A.alertDismissLevel = 0;
    bool shown = A.alert.level > 0 && !A.alertDismissLevel;
    if (shown && A.alert.level > lastLevel) {
        if (A.alert.level == RA_ALERT) { app_sound(SND_ALERT); flavour_event((A.alert.kind & RA_COURANT) && TH.retro == RETRO_NERV ? FEV_COURANT : FEV_DIVERGE, A.cs->s.path, A.alert.title); }
        if (A.page != PG_RUN) ui_toast(A.alert.level == RA_ALERT ? TOAST_ERR : TOAST_WARN, "%s in %s: see the Run page", alert_title(&A.alert), A.cs->s.name);
    }
    lastLevel = shown ? A.alert.level : 0;
    /* the themes' Courant moment (NERV's PATTERN BLUE, the paperclip): once a run, when the Courant number passes 1 */
    static int coFiredFor = -1;
    if (m && m->courant.n && m->courant.v[m->courant.n - 1] > 1 && coFiredFor != A.cj->launches) {
        coFiredFor = A.cj->launches;
        char t[96]; pl_fmt_ui(t, sizeof t, "Courant number %.2g", m->courant.v[m->courant.n - 1]);
        flavour_event(FEV_COURANT, A.cs->s.path, t);
    }
}

/* the alert's actions: halve the time step (or maxCo, with adjustTimeStep) in the running case's controlDict, saved at
   once so the solver reads it at its next step (runTimeModifiable) */
/* 3.2: the error translator's fix, saved at once (it's one the user asked for by name; Ctrl+Z undoes it) */
void app_apply_fix(const Explanation *x)
{
    if (!A.cs || !x->fix) return;
    if (x->fix == FIX_MESH) { app_generate_mesh(); return; }
    FDFile *f = case_dict(A.cs, x->fixFile);
    if (!f) { ui_toast(TOAST_ERR, "Could not read %s", x->fixFile); return; }
    bool ok = x->fix == FIX_ADD_BC ? app_add_bc(x->fixFile, x->fixValue) : explain_apply(f, x);
    if (!ok || !fd_save(f)) { ui_toast(TOAST_ERR, "The fix didn't apply to %s: change it on the Files page", x->fixFile); return; }
    case_rescan(A.cs);
    ui_toast(TOAST_OK, "%s: saved %s. Run again to check (Ctrl+Z undoes it)", x->fixLabel, x->fixFile);
}

void app_alert_halve(void)
{
    FDFile *cd = A.cs ? case_dict(A.cs, "system/controlDict") : NULL;
    if (!cd) return;
    char adj[16] = "", v[64] = "";
    fd_get(cd, "adjustTimeStep", adj, sizeof adj);
    const char *key = (!strcmp(adj, "yes") || !strcmp(adj, "on") || !strcmp(adj, "true")) && fd_find(cd, "maxCo") ? "maxCo" : "deltaT";
    char raw[64] = ""; fd_get(cd, key, raw, sizeof raw);
    if (!fd_get_resolved(cd, key, v, sizeof v) || !(atof(v) > 0)) { ui_toast(TOAST_ERR, "No number for %s in system/controlDict to halve", key); return; }
    char nv[64]; snprintf(nv, sizeof nv, "%g", atof(v) / 2);
    if (!fd_put(cd, key, nv) || !fd_save(cd)) { ui_toast(TOAST_ERR, "Could not write system/controlDict"); return; }
    char rtm[16] = ""; fd_get(cd, "runTimeModifiable", rtm, sizeof rtm);
    bool live = strcmp(rtm, "no") && strcmp(rtm, "false") && strcmp(rtm, "off");
    ui_toast(TOAST_OK, "%s %s%s%s -> %s, saved: %s", key, raw[0] == '$' ? raw : "", raw[0] == '$' ? " = " : "", v, nv, live ? "the solver reads it at its next time step" :
             "runTimeModifiable is off, so it takes effect at the next run");
    case_rescan(A.cs);
}

void app_pause_run(void)
{
    if (!A.cs) return;
    app_save_all(true);                                  /* the controlDict edit must not meet unsaved edits */
    char err[400];
    if (case_pause_request(A.cs->s.path, A.cs->s.app, err, sizeof err)) {
        if (A.cj->active && !strcmp(A.cj->casePath, A.cs->s.path)) A.cj->pausing = true;
        case_reload_changed(A.cs);
        ui_toast(TOAST_INFO, "Pausing: %s writes the time step it is on, then stops", A.cs->s.app[0] ? A.cs->s.app : "the solver");
    } else ui_toast(TOAST_ERR, "%s", err);
}

/* the slot of the case at path: the one that ran it last, else an unused one, else the longest finished;
   all busy (MAX_JOBS runs going): the spare, which can't start (app_can_start says why) */
Job *job_slot_for(const char *path)
{
    for (int i = 0; i < MAX_JOBS; i++) if (!strcmp(A.jobs[i].casePath, path)) return &A.jobs[i];
    Job *best = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &A.jobs[i];
        if (j->active) continue;
        if (j == A.cj && A.cs && strcmp(path, A.cs->s.path)) continue;   /* the open case's slot stays its own */
        if (!j->casePath[0]) return j;
        if (!best || j->finishedAt < best->finishedAt) best = j;
    }
    if (best) { job_reset(best, NULL); best->casePath[0] = 0; return best; }
    return &A.jobs[MAX_JOBS];
}

/* a run going in a case without a job of ours (started in a terminal, by another program, or before Q-Continuum
   restarted): take it over as a job, so it shows as running, its log is followed, and Stop and Pause work */
static void adopt_running(const char *path, const CaseSummary *s)
{
    if (!s->runningLog[0]) return;
    for (int i = 0; i <= MAX_JOBS; i++) if (A.jobs[i].active && !strcmp(A.jobs[i].casePath, path)) return;
    pid_t pg[8]; int nSolver = 0;
    int n = case_run_groups(path, s->app, pg, 8, &nSolver);
    if (n == 0) return;
    Job *j = job_slot_for(path);
    if (j == &A.jobs[MAX_JOBS]) return;                       /* every slot busy */
    char title[128]; snprintf(title, sizeof title, "%s (started earlier)", s->app[0] ? s->app : "Run");
    job_adopt(j, path, title, s->runningLog, pg, n, nSolver, GetTime());
    A.jobSeen[j - A.jobs] = j->launches;                      /* its log is already being read: don't restart it */
}

/* runs going in cases other than the open one (shown in the sidebar) */
static int other_jobs(Job **out, int max)
{
    int n = 0;
    for (int i = 0; i <= MAX_JOBS && n < max; i++)
        if (A.jobs[i].active && (!A.cs || strcmp(A.jobs[i].casePath, A.cs->s.path))) out[n++] = &A.jobs[i];
    return n;
}

/* may another run start in the open case? */
bool app_can_start(void)
{
    if (A.cj->active) { ui_toast(TOAST_WARN, "This case is already running"); return false; }
    if (app_jobs_active() >= MAX_JOBS) { ui_toast(TOAST_WARN, "%d runs are already going: wait for one to finish", MAX_JOBS); return false; }
    return true;
}

/* the theme's set of cues; -1 for the classic themes, which are silent */
int app_sound_pack(void)
{
    if (A.set.theme >= THEME_SEVERANCE && A.set.theme <= THEME_HALFLIFE) return SOUND_SEVERANCE + (A.set.theme - THEME_SEVERANCE);
    return A.set.theme == THEME_LCARS ? SOUND_LCARS : A.set.theme == THEME_STARDEW ? SOUND_STARDEW
         : A.set.theme == THEME_POKEMON ? SOUND_POKEMON : A.set.theme == THEME_JARVIS ? SOUND_JARVIS : -1;
}

static double bigCueAt = -10;                          /* a run's cue drowns out the toast tones right after it */
void app_sound(Snd s)
{
    static const float level[3] = { 0.2f, 0.45f, 0.85f };
    if (s == SND_DONE || s == SND_ALERT || s == SND_STOPPED) bigCueAt = GetTime();
    else if (s == SND_ERROR && GetTime() - bigCueAt < 1.0) return;
    int l = A.set.soundLevel < 0 || A.set.soundLevel > 2 ? 1 : A.set.soundLevel;
    int pack = app_sound_pack();
    if (pack >= 0) sound_set_pack(pack);
    sound_play(s, A.set.sounds && pack >= 0, level[l], g_ui_no_input);
}
static void ui_sound(UiSound s) { app_sound(s == UI_SND_CLICK ? SND_CLICK : s == UI_SND_DENIED ? SND_DENIED : SND_ERROR); }

/* ROADMAP 2.6: where you were (case, page, region) goes to settings.ini when it changes; the next launch reopens it */
static void session_note(void)
{
    const char *c = A.cs ? A.cs->s.path : "";
    const char *pg = A.cs || A.page < PG_OVERVIEW ? PAGE_TITLES[A.page] : "";
    const char *rg = A.cs ? A.cs->region : "";
    if (!strcmp(c, A.set.lastCase) && !strcmp(pg, A.set.lastPage) && !strcmp(rg, A.set.lastRegion)) return;
    pl_strlcpy(A.set.lastCase, c, sizeof A.set.lastCase);
    pl_strlcpy(A.set.lastPage, pg, sizeof A.set.lastPage);
    pl_strlcpy(A.set.lastRegion, rg, sizeof A.set.lastRegion);
    settings_save(&A.set);
}

static void session_restore(void)
{
    if (!A.set.lastCase[0]) return;
    char cd[PL_PATH]; pl_join(cd, sizeof cd, A.set.lastCase, "system/controlDict");
    if (!pl_is_file(cd)) return;                         /* moved or deleted since: start on Cases */
    Page pg = PG_OVERVIEW;
    for (int p = 0; p < PG_COUNT; p++) if (!strcmp(A.set.lastPage, PAGE_TITLES[p])) pg = (Page)p;
    char region[64]; pl_strlcpy(region, A.set.lastRegion, sizeof region);
    app_open_case(A.set.lastCase, pg);
    if (A.cs && region[0]) for (int i = 0; i < A.cs->s.nRegions; i++) if (!strcmp(A.cs->s.regions[i], region)) case_set_region(A.cs, region);
}

void app_go(Page p)
{
    if (p >= PG_OVERVIEW && p <= PG_HISTORY && !A.cs) p = PG_CASES;
    /* the other programs have Setup and Results where OpenFOAM has Mesh ... Numerics */
    if (A.cs && A.cs->s.prog && (p == PG_MESH || p == PG_BC || p == PG_PHYSICS || p == PG_NUMERICS)) p = PG_SETUP;
    if (A.cs && !A.cs->s.prog && (p == PG_SETUP || p == PG_RESULTS)) p = PG_OVERVIEW;
    A.page = p;
}

void app_write_foam_file(void)
{
    if (!A.cs) return;
    char p[PL_PATH + 64], n[300];
    snprintf(n, sizeof n, "%s.foam", A.cs->s.name);
    pl_join(p, sizeof p, A.cs->s.path, n);
    if (!pl_exists(p)) pl_touch(p);
}

void app_open_paraview(void)
{
    if (!A.cs) return;
    if (A.cs->s.prog) { app_go(PG_RESULTS); return; }        /* other programs: their result files are listed there */
    app_write_foam_file();
    char p[PL_PATH + 64], n[300], err[256] = "";
    snprintf(n, sizeof n, "%s.foam", A.cs->s.name);
    pl_join(p, sizeof p, A.cs->s.path, n);
    if (launch_paraview(&A.env, &A.set, p, NULL, err, sizeof err)) ui_toast(TOAST_OK, "Opening %s in ParaView", n);
    else ui_toast(TOAST_ERR, "%s", err);
}

/* ParaView with Claude: Claude Code, the CLI on this machine, opens in a terminal in the case folder with a prompt to write
   paraview/view.py, a script the ParaView GUI runs to build views suited to the case. It runs interactively: Claude asks
   before it writes a file or runs a command. Once the script exists, ParaView opens with it. */
#define PVC_DIR    "paraview"
#define PVC_SCRIPT "paraview/view.py"

bool app_claude_view_exists(void)
{
    if (!A.cs) return false;
    char p[PL_PATH]; pl_join(p, sizeof p, A.cs->s.path, PVC_SCRIPT);
    return pl_is_file(p);
}

bool app_paraview_claude_view(void)
{
    if (!A.cs) return false;
    char p[PL_PATH], foam[PL_PATH + 64], n[300], err[256] = "";
    pl_join(p, sizeof p, A.cs->s.path, PVC_SCRIPT);
    if (!pl_is_file(p)) return false;
    app_write_foam_file();
    snprintf(n, sizeof n, "%s.foam", A.cs->s.name);
    pl_join(foam, sizeof foam, A.cs->s.path, n);
    if (launch_paraview(&A.env, &A.set, foam, p, err, sizeof err)) ui_toast(TOAST_OK, "Opening ParaView with Claude's view (" PVC_SCRIPT ")");
    else ui_toast(TOAST_ERR, "%s", err);
    return true;
}

void app_paraview_claude(const char *request)
{
    Case *c = A.cs;
    if (!c) return;
    if (!A.env.claude[0]) { ui_toast(TOAST_ERR, "Claude Code (claude) is not installed on this machine"); return; }
    app_write_foam_file();
    char dir[PL_PATH]; pl_join(dir, sizeof dir, c->s.path, PVC_DIR);
    pl_mkdirs(dir);
    const char *lt = case_latest_result(&c->s);
    Str pr; str_init(&pr);
    str_catf(&pr, "Prepare a ParaView visualisation of the OpenFOAM case in the current directory.\n"
                  "Case: %s. Solver: %s. ", c->s.name, c->s.app[0] ? c->s.app : "unknown");
    if (lt[0]) str_catf(&pr, "Latest results: t = %s%s\n", lt, c->s.procTimes && !c->s.nTimes ? " (only in processor* folders: a decomposed case)." : ".");
    else str_cat(&pr, "No results yet: show the mesh and the initial fields.\n");
    if (c->s.nRegions) {
        str_cat(&pr, "Multi-region case, regions:");
        for (int r = 0; r < c->s.nRegions; r++) str_catf(&pr, " %s", c->s.regions[r]);
        str_cat(&pr, ".\n");
    }
    str_catf(&pr, "\nWrite " PVC_SCRIPT ", a ParaView Python script that the ParaView GUI runs with `paraview --script=" PVC_SCRIPT "`:\n"
                  "- It finds the case from its own location: the parent of the folder holding the script when __file__ is set, "
                  "else the current directory. It opens %s.foam there with the OpenFOAM reader (CaseType 'Decomposed Case' when the "
                  "results are only in processor* folders; the regions the case needs for a multi-region case).\n"
                  "- It goes to the last time.\n"
                  "- It builds the views that best show this case's physics, chosen from its fields, boundary conditions and geometry "
                  "(for example a slice through the middle coloured by velocity with streamlines for a flow, temperature for heat "
                  "transfer, an alpha iso-surface for free-surface flow). A layout with several views only when it helps.\n"
                  "- Colour maps rescaled to the data, scalar bars shown, a camera that frames the domain.\n", c->s.name);
    if (request && *request) str_catf(&pr, "\nThe user asked for: %s\n", request);
    str_cat(&pr, "\nCheck it with `pvpython " PVC_SCRIPT "` and fix it until it runs without errors (calls that need the GUI may be "
                 "wrapped in try/except). Change nothing outside " PVC_DIR "/. End with two or three lines on what the view shows; "
                 "the user then opens it from Q-Continuum.\n");
    char pp[PL_PATH]; pl_join(pp, sizeof pp, dir, ".claude-prompt.txt");
    bool ok = pl_write_file(pp, pr.s, strlen(pr.s));
    str_free(&pr);
    if (!ok) { ui_toast(TOAST_ERR, "Could not write %s", pp); return; }
    char qc[PL_PATH + 8], run[PL_PATH + 120];
    pl_shell_quote(qc, sizeof qc, A.env.claude);
    snprintf(run, sizeof run, "%s \"$(cat " PVC_DIR "/.claude-prompt.txt)\"", qc);
    if (launch_terminal_run(&A.env, &A.set, c->s.path, run))
        ui_toast(TOAST_INFO, "Claude Code is open in a terminal. When it has written " PVC_SCRIPT ", use Open Claude's view.");
    else ui_toast(TOAST_ERR, "No terminal emulator found to run Claude Code in");
}

/* Ask Claude about an error: Claude Code opens in a terminal in the case folder with the error, the end of the log and
   what changed since the last run that finished, and is asked to find the cause. Interactive: it asks before it changes
   a file or runs a command. */
void app_claude_error(const char *logName, const char *fatalMsg)
{
    Case *c = A.cs;
    if (!c) return;
    if (!A.env.claude[0]) { ui_toast(TOAST_ERR, "Claude Code (claude) is not installed on this machine: curl -fsSL https://claude.ai/install.sh | bash"); return; }
    char dir[PL_PATH]; pl_join(dir, sizeof dir, c->s.path, ".qc-claude");
    pl_mkdirs(dir);
    Str pr; str_init(&pr);
    str_catf(&pr, "An OpenFOAM run of the case in the current directory failed. Find out why and how to fix it.\n\n"
                  "Case: %s. Solver: %s. OpenFOAM %s. %s.\n", c->s.name, c->s.app[0] ? c->s.app : "unknown",
             env_active(&A.env, &A.set) ? env_active(&A.env, &A.set)->version : "", c->s.nRegions ? "Multi-region case" : "Single-region case");
    str_catf(&pr, "The run was started from Q-Continuum, a GUI for OpenFOAM (%s). The failing log is %s.\n\n", A.runMode == 1 ? "the case's Allrun script" : "its built-in steps", logName);
    str_catf(&pr, "The error Q-Continuum read from it:\n%s\n\n", fatalMsg && *fatalMsg ? fatalMsg : "(no FOAM FATAL message: read the end of the log)");
    /* the end of the log, so Claude starts from the right place */
    char lp[PL_PATH]; pl_join(lp, sizeof lp, c->s.path, logName);
    size_t len; char *t = pl_read_file(lp, &len);
    if (t) {
        const char *st = t + len; int lines = 0;
        while (st > t && lines < 80) { st--; if (*st == '\n') lines++; }
        str_catf(&pr, "The last lines of %s:\n```\n%s\n```\n\n", logName, st);
        free(t);
    }
    char good[48]; time_t gt;
    if (hist_last_good_run(c->s.path, good, sizeof good, &gt)) {
        char *d = hist_diff_now(c->s.path, good);
        if (d && strlen(d) < 12000) str_catf(&pr, "What changed in the setup since the last run that finished (git diff from .qc-history):\n```\n%s\n```\n\n", d);
        else if (d) str_catf(&pr, "The setup changed a lot since the last run that finished: `git --git-dir=.qc-history --work-tree=. diff %s` shows how.\n\n", good);
        free(d);
    }
    str_cat(&pr, "Please:\n1. Read the log and the case files that matter (system/, constant/, 0 or 0.orig/) and explain the cause in plain words.\n"
                 "2. Propose the smallest fix. Ask before changing any file, and say which files you change.\n"
                 "3. You may run quick checks (checkMesh, foamDictionary, a few time steps in a copy), but ask before anything that takes "
                 "more than a minute, and never delete results.\n"
                 "4. End with what to do next in Q-Continuum (for example: run again, or continue from the latest time).\n");
    char pp[PL_PATH]; pl_join(pp, sizeof pp, dir, "error-prompt.txt");
    bool ok = pl_write_file(pp, pr.s, strlen(pr.s));
    str_free(&pr);
    if (!ok) { ui_toast(TOAST_ERR, "Could not write %s", pp); return; }
    char qc[PL_PATH + 8], run[PL_PATH + 120];
    pl_shell_quote(qc, sizeof qc, A.env.claude);
    snprintf(run, sizeof run, "%s \"$(cat .qc-claude/error-prompt.txt)\"", qc);
    if (launch_terminal_run(&A.env, &A.set, c->s.path, run)) ui_toast(TOAST_INFO, "Claude Code is looking at the error in a terminal. It asks before changing anything.");
    else ui_toast(TOAST_ERR, "No terminal emulator found to run Claude Code in");
}

bool app_save_all(bool quiet)
{
    if (!A.cs) return true;
    if (prog_setup_dirty() && !prog_setup_save()) return false;
    case_reload_changed(A.cs);                               /* take in what changed on disk first */
    if (case_conflicts(A.cs, NULL)) { A.modal = MD_CONFLICT; return false; }
    char what[300] = ""; int k = 0, nd = 0;                 /* the history message: which files */
    for (int i = 0; i < A.cs->nFiles; i++) {
        if (!A.cs->files[i].f->dirty) continue;
        if (nd++ < 4 && k < (int)sizeof what - 80) k += snprintf(what + k, sizeof what - (size_t)k, "%s%s", k ? ", " : "", A.cs->files[i].rel);
    }
    if (nd > 4) snprintf(what + k, sizeof what - (size_t)k, " and %d more", nd - 4);
    int n = case_save_all(A.cs);
    if (n < 0) {
        if (A.cs->s.readOnly) ui_toast(TOAST_ERR, "Could not save: the case folder is read-only. Copy it to the workspace from the Overview page.");
        else ui_toast(TOAST_ERR, "Could not save. Check that the case folder is writable.");
        return false;
    }
    if (n > 0 && !quiet) ui_toast(TOAST_OK, "Saved %d file%s", n, n == 1 ? "" : "s");
    if (n > 0) {
        case_rescan(A.cs);
        if (A.set.history) { char m[400]; snprintf(m, sizeof m, "Saved %s", what); hist_commit(A.cs->s.path, m); history_invalidate(); }
    }
    return true;
}

/* ============================================================================ case list */
static void list_add(const char *path)
{
    for (int i = 0; i < A.nList; i++) if (!strcmp(A.list[i].path, path)) return;
    if (A.nList == A.capList) {
        A.capList = A.capList ? A.capList * 2 : 32;
        A.list = realloc(A.list, sizeof(CaseSummary) * (size_t)A.capList);
    }
    case_summarize(path, &A.list[A.nList++]);
}

static int by_activity(const void *a, const void *b)
{
    const CaseSummary *x = a, *y = b;
    return (y->activity > x->activity) - (y->activity < x->activity);
}

/* where projects outside the workspace are looked for: the home folder, or QC_TEST_DEME_HOME so test sandboxes never see the user's */
static const char *project_home(void)
{
    const char *t = getenv("QC_TEST_DEME_HOME");
    return t && *t ? t : pl_home();
}

/* where a program's new projects go: its own folder beside the software, not the OpenFOAM workspace (Settings > Workspace
   changes them). In a test sandbox they are inside QC_TEST_DEME_HOME. */
void app_prog_dir(int prog, char *out, size_t cap)
{
    static const char *const DEF[PROG_COUNT] = { [PROG_DEME] = "deme", [PROG_CHRONO] = "chrono", [PROG_CALCULIX] = "calculix",
        [PROG_ASTER] = "simvia/aster", [PROG_SATURNE] = "simvia/saturne", [PROG_MOOSE] = "moose", [PROG_ELMER] = "elmer",
        [PROG_SPARTA] = "sparta" };
    if (prog <= PROG_OPENFOAM || prog >= PROG_COUNT || !DEF[prog]) { pl_expand_home(out, cap, A.set.workspace); return; }
    if (A.set.progDir[prog][0] && !getenv("QC_TEST_DEME_HOME")) { pl_expand_home(out, cap, A.set.progDir[prog]); return; }
    pl_join(out, cap, project_home(), DEF[prog]);
}

static void scan_projects(const char *dir, int depth, bool foamToo, NameList *out)
{
    static const char *const SKIP[] = { "miniforge", "miniconda3", "anaconda3", "snap", "src", "projects", "simvia", "salome", "opt",
                                        "elmer", "moose_container", "calculix", "node_modules", "Downloads", "go",
                                        "plenum", NULL };
    int pg = prog_detect(dir);
    if (pg > PROG_OPENFOAM || (pg == PROG_OPENFOAM && foamToo)) { names_add(out, dir); return; }
    if (pg == PROG_OPENFOAM || depth <= 0) return;
    NameList l = { 0 };
    pl_list_dir(dir, &l, true, false);
    bool atHome = !strcmp(dir, project_home());             /* installs and sources at the top of home, not project folders */
    for (int i = 0; i < l.n && i < 400; i++) {
        const char *n = l.names[i];
        bool skip = n[0] == '.' || pl_is_number(n) || pl_starts_with(n, "processor") || !strcmp(n, "build") || !strcmp(n, "RESU") ||
                    !strcmp(n, "postProcessing") || !strcmp(n, "node_modules") || !strcmp(n, "__pycache__") || !strcmp(n, "constant") ||
                    !strcmp(n, "system");
        for (int k = 0; SKIP[k] && !skip && atHome; k++) if (!strcmp(n, SKIP[k])) skip = true;
        if (atHome && pl_starts_with(n, "q-continuum")) skip = true;   /* the app's own source folders, any version */
        if (skip) continue;
        char p[PL_PATH]; pl_join(p, sizeof p, dir, n);
        scan_projects(p, depth - 1, foamToo, out);
    }
    names_free(&l);
}

/* the other programs' projects outside the workspace: ~/deme, ~/OpenFOAM (DEME drivers beside cases), the home folder
   three deep (~/heatBench/saturne/A) and the folders in Settings (where OpenFOAM cases count too). The walk reads
   file heads in a few hundred folders (seconds on a full home): it runs in a forked child every two minutes and the
   list it finds is cached ($XDG_CACHE_HOME/q-continuum/projects.txt), so the window never waits for it. */
static void discover_into(NameList *out)
{
    char d[PL_PATH];
    pl_join(d, sizeof d, project_home(), "deme"); scan_projects(d, 2, false, out);
    pl_join(d, sizeof d, project_home(), "OpenFOAM"); scan_projects(d, 3, false, out);
    if (!getenv("QC_TEST_DEME_HOME")) scan_projects(project_home(), 3, false, out);
    for (int g = PROG_DEME + 1; g < PROG_COUNT; g++) { app_prog_dir(g, d, sizeof d); scan_projects(d, 2, false, out); }   /* ~/calculix etc. are skipped at home */
    for (int i = 0; i < A.set.demeDirs.n; i++) scan_projects(A.set.demeDirs.names[i], 2, true, out);
}

static struct { NameList found; pid_t pid; double at; bool loaded; } DISC = { .at = -1e9 };

static void disc_file(char *out, size_t cap)
{
    const char *x = getenv("XDG_CACHE_HOME");
    char dir[PL_PATH];
    if (x && *x) snprintf(dir, sizeof dir, "%s/q-continuum", x); else snprintf(dir, sizeof dir, "%s/.cache/q-continuum", pl_home());
    pl_mkdirs(dir);
    if (!pl_fmt(out, cap, "%s/projects.txt", dir)) out[0] = 0;
}

static void disc_load(void)
{
    char f[PL_PATH]; disc_file(f, sizeof f);
    names_free(&DISC.found);
    char *t = pl_read_file(f, NULL);
    for (char *l = t ? strtok(t, "\n") : NULL; l; l = strtok(NULL, "\n")) if (*l) names_add(&DISC.found, l);
    free(t);
    DISC.loaded = true;
}

/* folders added in Settings change what is found: look again at once */
void app_discover_again(void) { DISC.at = -1e9; }

static void discovery_tick(void)
{
    if (!DISC.loaded) disc_load();
    if (DISC.pid > 0) {
        int st;
        if (waitpid(DISC.pid, &st, WNOHANG) == DISC.pid) { DISC.pid = 0; disc_load(); A.listScanned = -100; }   /* rescan with them */
        return;
    }
    if (GetTime() - DISC.at < 120) return;
    DISC.at = GetTime();
    pid_t p = fork();
    if (p == 0) {                                          /* no GL, no UI in here: a folder walk and a file */
        NameList out = { 0 };
        discover_into(&out);
        Str b; str_init(&b);
        for (int i = 0; i < out.n; i++) { str_cat(&b, out.names[i]); str_cat(&b, "\n"); }
        char f[PL_PATH]; disc_file(f, sizeof f);
        pl_write_file(f, b.s ? b.s : "", b.len);
        _exit(0);
    }
    if (p > 0) DISC.pid = p;
}

void app_rescan_cases(bool force)
{
    double now = GetTime();
    if (!force && now - A.listScanned < 3.0) return;
    A.listScanned = now;
    A.nList = 0;
    char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
    NameList l = { 0 };
    pl_list_dir(ws, &l, true, false);
    for (int i = 0; i < l.n; i++) {
        char p[PL_PATH]; pl_join(p, sizeof p, ws, l.names[i]);
        if (case_is_project(p)) { list_add(p); continue; }
        NameList l2 = { 0 };                             /* one level of grouping folders */
        pl_list_dir(p, &l2, true, false);
        for (int k = 0; k < l2.n; k++) {
            char p2[PL_PATH]; pl_join(p2, sizeof p2, p, l2.names[k]);
            if (case_is_project(p2)) list_add(p2);
        }
        names_free(&l2);
    }
    names_free(&l);
    for (int i = 0; i < A.set.recent.n; i++) if (case_is_project(A.set.recent.names[i])) list_add(A.set.recent.names[i]);
    /* the other programs' projects outside the workspace (discover_into) */
    if (getenv("QC_TEST_DEME_HOME")) {                    /* a test sandbox: a small tree, found at once (deterministic) */
        NameList out = { 0 };
        discover_into(&out);
        for (int i = 0; i < out.n; i++) list_add(out.names[i]);
        names_free(&out);
    } else {
        discovery_tick();
        for (int i = 0; i < DISC.found.n; i++) if (case_is_project(DISC.found.names[i])) list_add(DISC.found.names[i]);
    }
    if (A.nList > 1) qsort(A.list, (size_t)A.nList, sizeof(CaseSummary), by_activity);
    for (int i = 0; i < A.nList; i++) adopt_running(A.list[i].path, &A.list[i]);
    for (int i = 0; i < A.nList; i++) if (app_queue_has(A.list[i].path)) A.list[i].status = CS_QUEUED;
    for (int k = 0; k <= MAX_JOBS; k++)
        if (A.jobs[k].active)
            for (int i = 0; i < A.nList; i++) if (!strcmp(A.list[i].path, A.jobs[k].casePath)) A.list[i].status = CS_RUNNING;
}

/* ============================================================================ open / close */
void app_close_case(void)
{
    undo_clear();
    prog_case_closed();
    case_close(A.cs);
    A.cs = NULL;
    A.cj = &A.jobs[MAX_JOBS];                             /* no case open: the spare, never running */
    logset_close(&A.logs);
    monset_close(&A.mons);
}

static void reset_case_state(void)
{
    A.bcField = A.bcPatch = -1;
    A.filesSel[0] = 0;
    A.runLog = -1;
    A.runFollow = true;
    A.stepInit = false;
    A.runProcs = 0;
    pl_strlcpy(A.chartRegion, "-", sizeof A.chartRegion);
    A.meshTab = -1;
    A.continueRun = false;
    for (int i = 0; i < 24; i++) A.scr[i] = (UiScroll){ 0 };
}

void app_copy_case_to_workspace(void)
{
    if (!A.cs) return;
    char ws[PL_PATH]; app_prog_dir(A.cs->s.prog, ws, sizeof ws);     /* OpenFOAM: the workspace; others: their own folder */
    const char *bad = foam_path_problem(ws);
    if (bad) { ui_toast(TOAST_ERR, "The folder's path has %s, which OpenFOAM can't run in. Choose another one in Settings.", bad); return; }
    if (!pl_mkdirs(ws)) { ui_toast(TOAST_ERR, "Could not create the folder %s", ws); return; }
    char dst[PL_PATH], name[300];
    for (int i = 1; i < 100; i++) {
        snprintf(name, sizeof name, i == 1 ? "%s-copy" : "%s-copy%d", A.cs->s.name, i);
        pl_join(dst, sizeof dst, ws, name);
        if (!pl_exists(dst)) break;
    }
    if (pl_exists(dst)) { ui_toast(TOAST_ERR, "Could not find a free name for the copy in %s", ws); return; }
    char qs[PL_PATH + 8], qd[PL_PATH + 8], cmd[PL_PATH * 3 + 64];
    pl_shell_quote(qs, sizeof qs, A.cs->s.path);
    pl_shell_quote(qd, sizeof qd, dst);
    if (!pl_fmt(cmd, sizeof cmd, "cp -a %s %s && chmod -R u+w %s", qs, qd, qd) || pl_run_wait(cmd, NULL, 0) != 0) {
        ui_toast(TOAST_ERR, "Copying the case to %s failed", dst); return;
    }
    /* edits that couldn't be saved in the read-only original go into the copy, which is where they were meant to land */
    int carried = 0;
    for (int i = 0; i < A.cs->nFiles; i++) {
        FDFile *f = A.cs->files[i].f;
        if (!f->dirty) continue;
        char to[PL_PATH]; pl_join(to, sizeof to, dst, A.cs->files[i].rel);
        if (fd_save_as(f, to)) carried++;
        else ui_toast(TOAST_WARN, "Could not write your edit of %s into the copy", A.cs->files[i].rel);
        fd_reload(f);                                        /* the original goes back to what is on disk */
    }
    if (carried) ui_toast(TOAST_OK, "Copied to %s with your %d edited file%s saved: you're now working on the copy", dst, carried, carried == 1 ? "" : "s");
    else ui_toast(TOAST_OK, "Copied to %s: you're now working on the copy", dst);
    app_rescan_cases(true);
    app_open_case(dst, A.page);
}

void app_open_case(const char *path, Page page)
{
    if (A.cs && (case_dirty_count(A.cs) > 0 || prog_setup_dirty()) && strcmp(path, A.cs->s.path) != 0) {
        snprintf(A.pendingOpen, sizeof A.pendingOpen, "%s", path);
        A.pendingPage = page;
        A.modal = MD_UNSAVED;
        return;
    }
    if (A.cs && !strcmp(A.cs->s.path, path)) { A.page = page; return; }
    Case *c = case_open(path);
    if (!c) { ui_toast(TOAST_ERR, "%s is not a case or project Q-Continuum knows: no OpenFOAM system/controlDict, nor another program's input", path); return; }
    app_close_case();
    A.cs = c;
    adopt_running(c->s.path, &c->s);
    A.cj = job_slot_for(c->s.path);
    reset_case_state();
    /* multi-region: start on the first region once the regions exist (the base mesh has no fields of its own) */
    if (c->s.nRegions) {
        char p0[PL_PATH], rel[160]; snprintf(rel, sizeof rel, "0.orig/%s", c->s.regions[0]);
        pl_join(p0, sizeof p0, c->s.path, rel);
        if (c->s.regionMeshed || pl_is_dir(p0)) case_set_region(c, c->s.regions[0]);
    }
    logset_open(&A.logs, c->s.path);
    monset_open(&A.mons, c->s.path);
    A.chartTab = 0;
    if (!c->s.prog) app_write_foam_file();
    if (A.set.history) hist_open_bg(c->s.path);         /* git init + commit: 0.7 s on /mnt/c, so not in this frame */
    if (case_pause_pending(c->s.path) && !c->s.runningLog[0]) {
        int prim = logset_primary(&A.logs, c->s.app);
        case_pause_settle(c->s.path, prim >= 0 && A.logs.logs[prim].ended, case_latest_result(&c->s));
        case_rescan(c); case_reload_changed(c);
    }
    A.page = page;
    char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
    if (!pl_starts_with(c->s.path, ws)) { settings_add_recent(&A.set, c->s.path); settings_save(&A.set); }
}

/* ============================================================================ deleting cases */
void app_ask_delete_case(const char *casePath)
{
    char path[PL_PATH]; pl_strlcpy(path, casePath, sizeof path);   /* it may point into the open case, which deleting frees */
    CaseSummary s; case_summarize(path, &s);
    pl_strlcpy(A.mPath, path, sizeof A.mPath);
    A.mText[0] = 0; A.mErr[0] = 0; A.mFlag = false;
    char sz[32]; pl_human_size(sz, sizeof sz, pl_dir_size(path, 12));
    const char *latest = case_latest_result(&s);
    if (latest[0]) snprintf(A.mText2, sizeof A.mText2, "%s on disk, with results up to t = %s (%d time folders%s).", sz, latest,
                            s.nTimes + s.procTimes, s.procTimes ? ", partly in processor*" : "");
    else snprintf(A.mText2, sizeof A.mText2, "%s on disk, no results yet.", sz);
    if (A.set.deleteConfirm == 2) {                            /* Settings: don't ask. Still the Trash, never rm -rf */
        char err[400];
        if (app_delete_case(path, false, err, sizeof err)) ui_toast(TOAST_OK, "Moved %s to the Trash", pl_basename(path));
        else ui_toast(TOAST_ERR, "%s", err);
        return;
    }
    A.modal = MD_DELETE;
}

/* trash (freedesktop, recoverable) or rm -rf; refuses anything that is not a case or is being run */
bool app_delete_case(const char *casePath, bool permanent, char *err, int cap)
{
    char path[PL_PATH]; pl_strlcpy(path, casePath, sizeof path);
    char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
    if (!path[0] || !strcmp(path, "/") || !strcmp(path, pl_home()) || !strcmp(path, ws) || !case_is_project(path)) {
        snprintf(err, (size_t)cap, "%s is not a case or project folder; nothing was deleted.", path); return false;
    }
    for (int k = 0; k <= MAX_JOBS; k++)
        if (A.jobs[k].active && !strcmp(A.jobs[k].casePath, path)) { snprintf(err, (size_t)cap, "A run is going in this case. Stop it first."); return false; }
    CaseSummary s; case_summarize(path, &s);
    if (s.runningLog[0]) { snprintf(err, (size_t)cap, "%s is still being written: another process is running this case.", s.runningLog); return false; }
    if (A.cs && !strcmp(A.cs->s.path, path)) { app_close_case(); A.page = PG_CASES; }
    if (permanent) {
        if (pl_remove_tree(path) != 0 && pl_exists(path)) { snprintf(err, (size_t)cap, "Could not delete everything in %s.", path); return false; }
    } else {
        char q[PL_PATH + 8], cmd[PL_PATH + 64], out[512] = "";
        pl_shell_quote(q, sizeof q, path);
        snprintf(cmd, sizeof cmd, "gio trash -- %s 2>&1; echo \"rc=$?\"", q);
        pl_run_wait(cmd, out, sizeof out);
        if (pl_exists(path)) {
            char *rc = strstr(out, "rc="); if (rc) *rc = 0; pl_trim_inplace(out);
            snprintf(err, (size_t)cap, "Moving to the trash failed (%s). Tick 'Delete permanently' to remove it instead.", out[0] ? out : "gio trash");
            return false;
        }
    }
    for (int i = 0; i < A.set.recent.n; i++)                    /* forget it in the recent list */
        if (!strcmp(A.set.recent.names[i], path)) {
            free(A.set.recent.names[i]);
            memmove(&A.set.recent.names[i], &A.set.recent.names[i + 1], sizeof(char *) * (size_t)(A.set.recent.n - i - 1));
            A.set.recent.n--; settings_save(&A.set); break;
        }
    app_rescan_cases(true);
    return true;
}

void app_start_tool(const char *title, const char *id, const char *label, const char *cmd, const char *log)
{
    if (!A.cs || !app_can_start()) return;
    if (!app_require_foam()) return;
    app_save_all(true);
    job_reset(A.cj, title);
    job_add(A.cj, id, label, cmd, log, false);
    if (job_start(A.cj, A.cs->s.path, app_bashrc(), GetTime())) ui_toast(TOAST_INFO, "%s started", label);
    else ui_toast(TOAST_ERR, "Could not start %s", label);
}

/* ============================================================================ shell */
/* navigation targets have stable ids for test scripts: "nav:run", "nav:new case", whichever theme draws them */
static UiId nav_id(const char *label)
{
    char b[96]; int k = snprintf(b, sizeof b, "nav:");
    for (const char *p = label; *p && k < (int)sizeof b - 1; p++) b[k++] = (char)tolower((unsigned char)*p);
    b[k] = 0;
    return ui_id(b);
}
/* the mark: each theme's own Q (logo.c) */
static void draw_logo(float x, float y, float s) { logo_draw(ui_theme_id(), x, y, s); }

/* ROADMAP 2.4: what needs doing, on the page buttons themselves, in words with a glyph (not a colour alone) */
typedef struct { const char *text; const char *icon; int kind; } Badge;     /* kind: TOAST_* */
static Badge nav_badge(Page p)
{
    static double at = -10; static char path[PL_PATH]; static int bcMiss;
    Badge b = { NULL, NULL, TOAST_INFO };
    if (!A.cs) return b;
    Case *c = A.cs;
    if (ui_time() - at > 0.5 || strcmp(path, c->s.path)) {       /* the BC count walks every field: twice a second */
        at = ui_time(); pl_strlcpy(path, c->s.path, sizeof path);
        char m[8]; bcMiss = c->s.meshed && c->nPatches > 0 ? bc_missing_report(c, m, sizeof m) : 0;
    }
    bool running = A.cj->active && !strcmp(A.cj->casePath, c->s.path);
    static char t[32];
    switch (p) {
    case PG_MESH:
        if (!c->s.meshed) b = (Badge){ "No mesh", ICON_TRIANGLE_ALERT, TOAST_WARN };
        else if (c->s.meshStale) b = (Badge){ "Out of date", ICON_TRIANGLE_ALERT, TOAST_WARN };
        break;
    case PG_BC:
        if (bcMiss > 0) { snprintf(t, sizeof t, "%d missing", bcMiss); b = (Badge){ t, ICON_CIRCLE_X, TOAST_ERR }; }
        break;
    case PG_RUN:
        if (running) b = (Badge){ "Running", ICON_ACTIVITY, TOAST_INFO };
        else if (c->s.status == CS_FAILED || (A.cj->failed && !strcmp(A.cj->casePath, c->s.path)))   /* any step, not just the solver */
            b = (Badge){ "Failed", ICON_CIRCLE_X, TOAST_ERR };
        else if (c->s.status == CS_PAUSED) b = (Badge){ "Paused", ICON_PAUSE, TOAST_INFO };
        else if (c->s.status == CS_STOPPED) b = (Badge){ "Stopped", ICON_SQUARE, TOAST_WARN };
        break;
    default: break;
    }
    return b;
}
static Color badge_color(int kind) { return kind == TOAST_ERR ? TH.err : kind == TOAST_WARN ? TH.warn : TH.text2; }

static bool nav_item(Rectangle r, const char *icon, const char *label, bool sel, bool dot, Badge bd)
{
    UiId id = ui_id(label);
    ui_register(nav_id(label), r, label);
    bool clicked = ui_row(id, r, sel);
    if (sel) ui_rect((Rectangle){ r.x - 10, r.y + 8, 3, r.height - 16 }, 1.5f, TH.accent);
    ui_icon(icon, r.x + 18, r.y + r.height / 2, 16, sel ? TH.text : TH.text2);
    float bw = bd.text ? ui_text_w(F_MED, 11.5f, bd.text) + 26 : 0;   /* the badge's room: the label gives way */
    ui_text_clip(sel ? F_MED : F_REG, 14, label, r.x + 38, r.y + (r.height - 14) / 2 - 1, r.width - 38 - bw - 6, sel ? TH.text : TH.text2);
    if (bd.text) {                                       /* right-aligned: glyph + words, coloured too */
        float tw = ui_text_w(F_MED, 11.5f, bd.text);
        float bx = r.x + r.width - 10 - tw;
        Color bc = badge_color(bd.kind);
        ui_text(F_MED, 11.5f, bd.text, bx, r.y + (r.height - 12) / 2, bc);
        ui_icon(bd.icon, bx - 10, r.y + r.height / 2, 12, bc);
    } else if (dot) {
        float pulse = 0.55f + 0.45f * sinf((float)ui_time() * 4.0f);
        DrawCircleV((Vector2){ r.x + r.width - 16, r.y + r.height / 2 }, 4, ui_alpha(TH.accent, pulse));
    }
    return clicked;
}

static void draw_sidebar(Rectangle r)
{
    ui_rect(r, 0, TH.sidebar);
    DrawRectangleRec((Rectangle){ r.x + r.width - 1, r.y, 1, r.height }, TH.lineSoft);
    float x = r.x + 18, w = r.width - 36;
    draw_logo(x + 2, r.y + 22, 30);
    ui_text(F_SEMI, 18, "Q-Continuum", x + 44, r.y + 21, TH.text);
    const FoamInstall *fi = env_active(&A.env, &A.set);
    char ver[64]; snprintf(ver, sizeof ver, fi ? "simulation workbench" : "OpenFOAM not found");
    ui_text(F_REG, 12, ver, x + 44, r.y + 42, fi ? TH.text3 : TH.err);

    /* the open case and its pages; the general pages are in the bar along the top (nav_bar) */
    Job *oj[3]; int no = other_jobs(oj, 3);
    const Page *cp; int nItems = case_pages(&cp);
    float need = 64 + nItems * 40;
    float avail = r.height - 56 - no * 58 - 96;
    float k = need > avail ? fmaxf(0.72f, avail / need) : 1.0f;
    float rh = roundf(36 * fminf(1.0f, k + 0.1f));
    float y = r.y + 96;
    if (A.cs) {
        ui_text(F_MED, 11.5f, ui_upper("Open case"), x + 2, y, TH.text3);
        y += 22;
        Color cc = cat_color(A.cs->s.cat);
        ui_rect((Rectangle){ x + 2, y + 3, 22, 22 }, 6, ui_alpha(cc, 0.18f));
        ui_icon(cat_icon(A.cs->s.cat), x + 13, y + 14, 13, cc);
        ui_text_clip(F_SEMI, 14, A.cs->s.name, x + 34, y + 1, w - 34, TH.text);
        ui_text_clip(F_REG, 12, A.cs->s.prog ? prog_info(A.cs->s.prog)->name : A.cs->s.app[0] ? A.cs->s.app : "no application set", x + 34, y + 19, w - 34, TH.text3);
        y += roundf(46 * k);
        for (int i = 0; i < nItems; i++) {
            bool running = cp[i] == PG_RUN && A.cj->active && !strcmp(A.cj->casePath, A.cs->s.path);
            if (coupled_open() && cp[i] == PG_RUN) running = coupled_running();
            if (nav_item((Rectangle){ x, y, w, rh }, page_icon(cp[i]), page_label(cp[i]), A.page == cp[i], running, nav_badge(cp[i]))) app_go(cp[i]);
            y += roundf(40 * k);
        }
    } else {
        ui_text(F_MED, 11.5f, ui_upper("No case open"), x + 2, y, TH.text3);
        y += 22;
        y += ui_text_wrap(F_REG, 13, "Open one from Cases, or make one with New case: its pages appear here.", (Rectangle){ x + 2, y, w - 4, 80 }, 19, TH.text2, true) + 12;
        float bw = ui_button_width(ICON_LIBRARY, "Cases");
        if (ui_button(ui_id("side-cases"), (Rectangle){ x, y, bw, 34 }, ICON_LIBRARY, "Cases", BTN_SECONDARY)) { app_rescan_cases(true); app_go(PG_CASES); }
        float nw = ui_button_width(ICON_PLUS, "New case");
        if (bw + 8 + nw <= w && ui_button(ui_id("side-new"), (Rectangle){ x + bw + 8, y, nw, 34 }, ICON_PLUS, "New case", BTN_SECONDARY)) app_go(PG_NEW);
    }

    float by = r.y + r.height - 56;
    for (int i = 0; i < no; i++) {                       /* runs in the other cases, newest at the bottom */
        Rectangle jr = { x, by - 58 * (no - i), w, 48 };
        ui_rect(jr, 9, TH.accentSoft);
        ui_spinner(jr.x + 20, jr.y + 24, 7, TH.accent);
        ui_text(F_MED, 12.5f, "Running", jr.x + 38, jr.y + 8, TH.text);
        ui_text_clip(F_REG, 12, pl_basename(oj[i]->casePath), jr.x + 38, jr.y + 25, jr.width - 46, TH.text2);
        if (ui_clicked(jr)) { if (case_is_project(oj[i]->casePath)) app_open_case(oj[i]->casePath, PG_RUN); }
        if (ui_hovered(jr)) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    }
    Rectangle env = { x, by, w, 40 };
    Color dc = fi ? TH.ok : TH.err;
    DrawCircleV((Vector2){ env.x + 18, env.y + 20 }, 4, dc);
    ui_text(F_REG, 12.5f, fi ? "Environment ready" : "Set up OpenFOAM", env.x + 34, env.y + 4, TH.text2);
    char sub[96]; snprintf(sub, sizeof sub, "%s%s", A.env.wsl ? "WSL · " : "", fi ? fi->version : "see Settings");
    ui_text(F_REG, 12, sub, env.x + 34, env.y + 21, TH.text3);
    ui_register(ui_id("env-status"), env, fi ? "Environment ready" : "Set up OpenFOAM");
    if (ui_hovered(env)) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    if (ui_clicked(env)) A.modal = MD_ENV;
}

/* the general pages, one bar along the top: everything that isn't about the open case */
static const struct { Page p; const char *icon; const char *label; } NAV[] = {
    { PG_CASES, ICON_LIBRARY, "Cases" }, { PG_NEW, ICON_FOLDER_PLUS, "New case" }, { PG_QUEUE, ICON_LIST, "Queue" },
    { PG_STUDIES, ICON_TABLE, "Studies" }, { PG_COUPLING, ICON_WORKFLOW, "Coupling" } };
#define N_NAV ((int)(sizeof NAV / sizeof NAV[0]))

static bool nav_busy(Page p)
{
    return (p == PG_QUEUE && app_queue_count() > 0) || (p == PG_COUPLING && A.cpl && A.cpl->running);
}

static void nav_go(Page p)
{
    if (p == PG_CASES) app_rescan_cases(true);
    app_go(p);
}

static void nav_bar(Rectangle r)
{
    ui_rect(r, 0, TH.sidebar);
    DrawRectangleRec((Rectangle){ r.x, r.y + r.height - 1, r.width, 1 }, TH.lineSoft);
    float x = r.x + PAGE_PAD - 12, cy = r.y + r.height / 2;
    for (int i = 0; i <= N_NAV; i++) {
        bool settings = i == N_NAV;
        Page p = settings ? PG_SETTINGS : NAV[i].p;
        const char *icon = settings ? ICON_SETTINGS : NAV[i].icon, *label = settings ? "Settings" : NAV[i].label;
        float w = ui_text_w(F_MED, 13.5f, label) + 46;
        if (settings) x = r.x + r.width - PAGE_PAD + 12 - w;   /* Settings at the far end */
        Rectangle t = { x, cy - 17, w, 34 };
        ui_register(nav_id(label), t, label);
        bool on = A.page == p, hov = ui_hovered(t);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (on) ui_rect(t, 8, TH.selected); else if (hov) ui_rect(t, 8, TH.hover);
        ui_icon(icon, t.x + 18, cy, 15, on ? TH.accent : TH.text2);
        ui_text(on ? F_SEMI : F_MED, 13.5f, label, t.x + 34, cy - 9, on ? TH.text : TH.text2);
        if (nav_busy(p)) {
            float pulse = 0.55f + 0.45f * sinf((float)ui_time() * 4.0f);
            DrawCircleV((Vector2){ t.x + w - 8, t.y + 8 }, 3.5f, ui_alpha(TH.accent, pulse));
        }
        if (ui_clicked(t)) nav_go(p);
        x += w + 4;
    }
}

/* a case name for the breadcrumb, at most maxW wide: the middle goes ("regolith…_run3"), the full name is the tooltip
   (bug hunt H-21: a 200-character name ran under the header buttons) */
/* the open case is a participant opened from its coupled run's Participants page */
static bool participant_open(void)
{
    if (!A.cplBack[0] || !A.cs) return false;
    size_t n = strlen(A.cplBack);
    return !strncmp(A.cs->s.path, A.cplBack, n) && A.cs->s.path[n] == '/';
}
static void crumb_go(Page p)
{
    if (p == PG_COUNT) { char back[PL_PATH]; pl_strlcpy(back, A.cplBack, sizeof back); app_open_case(back, PG_RESULTS); return; }
    app_go(p);
}

static const char *crumb_name(const char *name, int font, float size, float maxW)
{
    static char out[300];
    char full[300]; snprintf(full, sizeof full, "`%s`", name);
    if (ui_text_w(font, size, ui_cap_words(full)) <= maxW) { pl_strlcpy(out, full, sizeof out); return out; }
    int n = (int)strlen(name), keep = n;
    while (keep > 4) {
        keep--;
        int a = (keep + 1) / 2, b = keep - a;
        snprintf(out, sizeof out, "`%.*s…%s`", a, name, name + n - b);
        if (ui_text_w(font, size, ui_cap_words(out)) <= maxW) break;
    }
    return out;
}

static void draw_topbar(Rectangle r)
{
    float x = r.x + PAGE_PAD;
    float cy = r.y + r.height / 2;
    /* breadcrumb */
    const char *crumbs[4]; int nc = 0; Page target[4];
    if (A.page == PG_SETTINGS) { crumbs[nc] = "Settings"; target[nc++] = PG_SETTINGS; }
    else if (A.page == PG_COUPLING) { crumbs[nc] = "Coupling"; target[nc++] = PG_COUPLING; }
    else if (A.page == PG_QUEUE) { crumbs[nc] = "Queue"; target[nc++] = PG_QUEUE; }
    else if (A.page == PG_STUDIES) { crumbs[nc] = "Studies"; target[nc++] = PG_STUDIES; }
    else if (A.page == PG_NEW) { crumbs[nc] = "Cases"; target[nc++] = PG_CASES; crumbs[nc] = "New case"; target[nc++] = PG_NEW; }
    else {
        crumbs[nc] = "Cases"; target[nc++] = PG_CASES;
        if (participant_open() && A.page >= PG_OVERVIEW) { crumbs[nc] = pl_basename(A.cplBack); target[nc++] = PG_COUNT; }   /* back to its coupled run */
        if (A.cs && A.page >= PG_OVERVIEW) {
            crumbs[nc] = crumb_name(A.cs->s.name, F_MED, 14, fmaxf(160, r.width * 0.3f)); target[nc++] = PG_OVERVIEW;
            if (A.page != PG_OVERVIEW) { crumbs[nc] = page_label(A.page); target[nc++] = A.page; }
        }
    }
    for (int i = 0; i < nc; i++) {
        bool last = i == nc - 1;
        const char *C = ui_cap_words(crumbs[i]);
        float w = ui_text_w(last ? F_MED : F_REG, 14, C);
        Rectangle cr = { x - 4, cy - 14, w + 8, 28 };
        bool hov = !last && ui_hovered(cr);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        ui_text(last ? F_MED : F_REG, 14, C, x, cy - 9, last ? TH.text : (hov ? TH.text : TH.text3));
        if (A.cs && target[i] == PG_OVERVIEW && strstr(crumbs[i], "…")) ui_tooltip(cr, A.cs->s.name);
        if (!last && ui_clicked(cr)) crumb_go(target[i]);
        x += w + 10;
        if (!last) { ui_icon(ICON_CHEVRON_RIGHT, x + 4, cy, 14, TH.text3); x += 18; }
    }

    /* actions, right to left */
    float rx = r.x + r.width - PAGE_PAD;
    if (A.cs && A.page >= PG_OVERVIEW && A.page <= PG_HISTORY) {
        bool mine = (A.cj->active && !strcmp(A.cj->casePath, A.cs->s.path)) || coupled_running();
        if (mine) {
            float w = ui_button_width(ICON_SQUARE, "Stop");
            rx -= w;
            if (ui_button(ui_id("top-stop"), (Rectangle){ rx, cy - 17, w, 34 }, ICON_SQUARE, "Stop", BTN_DANGER)) {
                if (coupled_running()) coupling_stop(A.cpl, GetTime()); else job_stop(A.cj, GetTime());
            }
            if (app_can_pause() && !A.cj->pausing) {
                w = ui_button_width(ICON_PAUSE, "Pause");
                rx -= w + 10;
                if (ui_button(ui_id("top-pause"), (Rectangle){ rx, cy - 17, w, 34 }, ICON_PAUSE, "Pause", BTN_SECONDARY)) app_pause_run();
                ui_tooltip((Rectangle){ rx, cy - 17, w, 34 }, "Write the time step it is on and stop; Resume continues from there");
            }
        } else {
            bool paused = app_resumable(NULL);
            const char *rl = paused ? "Resume" : A.cs->s.prog ? term(T_RUN_GO) : "Run case";
            float w = ui_button_width(ICON_PLAY, rl);
            rx -= w;
            ui_disable(app_jobs_active() >= MAX_JOBS);
            if (ui_button(ui_id("top-run"), (Rectangle){ rx, cy - 17, w, 34 }, ICON_PLAY, rl, BTN_PRIMARY)) {
                if (paused) { app_resume_run(); app_go(PG_RUN); }
                else if (A.page == PG_RUN) A.runPending = true;   /* already here: run, as the button below does (same checks) */
                else app_go(PG_RUN);
            }
            ui_disable(false);
            if (app_jobs_active() >= MAX_JOBS) ui_tooltip((Rectangle){ rx, cy - 17, w, 34 }, "The most runs at once are going");
        }
        rx -= 10;
        float w = ui_button_width(ICON_EXTERNAL_LINK, "ParaView");
        rx -= w;
        if (ui_button(ui_id("top-pv"), (Rectangle){ rx, cy - 17, w, 34 }, ICON_EXTERNAL_LINK, "ParaView", BTN_SECONDARY)) app_open_paraview();
        ui_tooltip((Rectangle){ rx, cy - 17, w, 34 }, paraview_description(&A.env, &A.set));
        rx -= 10;
        if (!A.cs->s.prog) {
            w = ui_button_width(ICON_SPARKLES, "ParaView + Claude");
            rx -= w;
            if (ui_button(ui_id("top-pvc"), (Rectangle){ rx, cy - 17, w, 34 }, ICON_SPARKLES, "ParaView + Claude", BTN_SECONDARY)) { A.modal = MD_PV_CLAUDE; A.mText2[0] = 0; }
            ui_tooltip((Rectangle){ rx, cy - 17, w, 34 }, "Claude Code builds a ParaView view for this case");
            rx -= 10;
        }
        int dirty = case_dirty_count(A.cs) + (prog_setup_dirty() ? 1 : 0);
        char lab[48]; snprintf(lab, sizeof lab, dirty ? "Save %d" : "Saved", dirty);
        w = ui_button_width(ICON_SAVE, lab);
        rx -= w;
        ui_disable(dirty == 0);
        if (ui_button(ui_id("top-save"), (Rectangle){ rx, cy - 17, w, 34 }, ICON_SAVE, lab, BTN_SECONDARY)) app_save_request();
        ui_disable(false);
        if (dirty) ui_tooltip((Rectangle){ rx, cy - 17, w, 34 }, "Save all edited dictionaries (Ctrl+S)");
        rx -= 6;
        for (int k = 1; k >= 0; k--) {                    /* redo, then undo to its left */
            const char *what = undo_label(k);
            rx -= 34;
            Rectangle ub = { rx, cy - 17, 34, 34 };
            ui_disable(!what);
            if (ui_button(ui_id(k ? "top-redo" : "top-undo"), ub, k ? ICON_REFRESH_CW : ICON_UNDO_2, NULL, BTN_ICON)) undo_apply(k);
            ui_disable(false);
            char tip[300]; pl_fmt_ui(tip, sizeof tip, "%s%s%s (%s)", k ? "Redo" : "Undo", what ? ": " : "", what ? what : "", k ? "Ctrl+Y" : "Ctrl+Z");
            ui_tooltip(ub, tip);
            rx -= 2;
        }
        rx -= 8;
        w = ui_button_width(ICON_TRASH_2, "Clean");
        rx -= w;
        ui_disable(mine);
        if (ui_button(ui_id("top-clean"), (Rectangle){ rx, cy - 17, w, 34 }, ICON_TRASH_2, "Clean", BTN_GHOST)) { A.modal = A.cs->s.prog ? MD_PROG_CLEAN : MD_CLEAN; A.mFlag = A.mFlag2 = false; }
        ui_disable(false);
        ui_tooltip((Rectangle){ rx, cy - 17, w, 34 }, mine ? "Stop the run first" : "Delete the results and logs (asks first)");
    }
    DrawRectangleRec((Rectangle){ r.x, r.y + r.height - 1, r.width, 1 }, TH.lineSoft);
}


/* ============================================================================ LCARS-style shell */
#define LC_M  8.0f      /* outer margin                    */
#define LC_CW 172.0f    /* column width                    */
#define LC_HH 116.0f    /* header frame height             */

static void lc_round_bl(Rectangle r, float R, Color c)      /* block whose bottom-left corner is rounded */
{
    if (R > r.height) R = r.height;
    DrawRectangleRec((Rectangle){ r.x, r.y, r.width, r.height - R }, c);
    DrawCircleSector((Vector2){ r.x + R, r.y + r.height - R }, R, 90, 180, 32, c);
    DrawRectangleRec((Rectangle){ r.x + R, r.y + r.height - R, r.width - R, R }, c);
}

/* a column block: label in black capitals at the bottom right, optional small code at the top left */
static bool lc_block(Rectangle r, Color c, const char *label, const char *code, bool sel, bool blink, bool interactive)
{
    if (interactive && label) ui_register(nav_id(label), r, label);
    bool hov = interactive && ui_hovered(r);
    if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    float h = interactive ? ui_anim(ui_idn(ui_id("lcb"), (int)r.y), hov ? 1.f : 0.f, 10.f) : 0;
    Color bg = ui_mix(c, WHITE, sel ? 0.42f : 0.18f * h);
    if (blink && fmod(ui_time(), 1.0) < 0.5) bg = TH.accent;
    DrawRectangleRec(r, bg);
    if (label && *label) {
        float fs = r.height >= 40 ? 17 : 15.5f;
        const char *L = ui_cap_words(label);
        float tw = ui_text_w(F_MED, fs, L);
        if (tw > r.width - 14) ui_text_clip(F_MED, fs, L, r.x + 8, r.y + r.height - fs - 7, r.width - 16, BLACK);
        else ui_text(F_MED, fs, L, r.x + r.width - 10 - tw, r.y + r.height - fs - 7, BLACK);
    }
    if (code && *code) ui_text_clip(F_MED, 12, code, r.x + 8, r.y + 5, r.width - 16, ui_alpha(BLACK, 0.75f));
    if (sel) ui_pill((Rectangle){ r.x + r.width + 6, r.y + 2, 8, r.height - 4 }, TH.accent);    /* "you are here" */
    return interactive && ui_clicked(r);
}

static void lcars_header(float W)
{
    const FoamInstall *fi = env_active(&A.env, &A.set);
    float x0 = LC_M, y0 = LC_M, h = LC_HH - LC_M;
    ui_elbow(x0, y0, LC_CW, h, W - 2 * LC_M, 16, 44, 18, true, TH.lc[0]);
    /* bar segments at the right end */
    float bx = W - LC_M, by = y0 + h - 16;
    DrawRectangleRec((Rectangle){ bx - 300, by, 4, 16 }, BLACK);
    DrawRectangleRec((Rectangle){ bx - 296, by, 120, 16 }, TH.lc[1]);
    DrawRectangleRec((Rectangle){ bx - 176, by, 4, 16 }, BLACK);
    DrawRectangleRec((Rectangle){ bx - 172, by, 40, 16 }, TH.lc[5]);
    DrawRectangleRec((Rectangle){ bx - 132, by, 4, 16 }, BLACK);
    DrawRectangleRec((Rectangle){ bx - 128, by, 128, 16 }, TH.lc[2]);
    /* column text: the OpenFOAM build in use */
    char ver[64]; snprintf(ver, sizeof ver, "%s", fi ? fi->version : "NO OPENFOAM");
    ui_text(F_MED, 12, ui_upper(A.env.wsl ? "wsl" : "linux"), x0 + 10, y0 + 8, ui_alpha(BLACK, 0.75f));
    const char *V = ui_upper(ver);
    ui_text(F_SEMI, 20, V, x0 + LC_CW - 12 - ui_text_w(F_SEMI, 20, V), y0 + h - 44, BLACK);
    /* title */
    const char *T = "Q-CONTINUUM";
    float tw = ui_text_w(F_SEMI, 44, T);
    ui_text(F_SEMI, 44, T, W - LC_M - 6 - tw, y0 + 2, TH.accent);
    draw_logo(W - LC_M - 6 - tw - 50, y0 + 6, 40);

    /* breadcrumb row */
    float x = x0 + LC_CW + 30, cy = y0 + 66;
    const char *crumbs[4]; int nc = 0; Page target[4];
    if (A.page == PG_SETTINGS) { crumbs[nc] = "Settings"; target[nc++] = PG_SETTINGS; }
    else if (A.page == PG_COUPLING) { crumbs[nc] = "Coupling"; target[nc++] = PG_COUPLING; }
    else if (A.page == PG_QUEUE) { crumbs[nc] = "Queue"; target[nc++] = PG_QUEUE; }
    else if (A.page == PG_STUDIES) { crumbs[nc] = "Studies"; target[nc++] = PG_STUDIES; }
    else if (A.page == PG_NEW) { crumbs[nc] = "Cases"; target[nc++] = PG_CASES; crumbs[nc] = "New case"; target[nc++] = PG_NEW; }
    else {
        crumbs[nc] = "Cases"; target[nc++] = PG_CASES;
        if (participant_open() && A.page >= PG_OVERVIEW) { crumbs[nc] = pl_basename(A.cplBack); target[nc++] = PG_COUNT; }   /* back to its coupled run */
        if (A.cs && A.page >= PG_OVERVIEW) {
            crumbs[nc] = crumb_name(A.cs->s.name, F_MED, 20, fmaxf(160, (W - LC_CW) * 0.28f)); target[nc++] = PG_OVERVIEW;
            if (A.page != PG_OVERVIEW) { crumbs[nc] = page_label(A.page); target[nc++] = A.page; }
        }
    }
    for (int i = 0; i < nc; i++) {
        bool last = i == nc - 1;
        const char *C = ui_cap_words(crumbs[i]);
        float w = ui_text_w(F_MED, 20, C);
        Rectangle cr = { x - 4, cy - 16, w + 8, 32 };
        bool hov = !last && ui_hovered(cr);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        ui_text(F_MED, 20, C, x, cy - 11, last ? TH.text : (hov ? TH.accent : TH.text2));
        if (A.cs && target[i] == PG_OVERVIEW && strstr(crumbs[i], "…")) ui_tooltip(cr, A.cs->s.name);
        if (!last && ui_clicked(cr)) crumb_go(target[i]);
        x += w + 10;
        if (!last) { ui_text(F_MED, 20, "/", x, cy - 11, TH.text3); x += 18; }
    }

    /* actions, right to left, under the title */
    float rx = W - LC_M;
    if (A.cs && A.page >= PG_OVERVIEW && A.page <= PG_HISTORY) {
        bool mine = (A.cj->active && !strcmp(A.cj->casePath, A.cs->s.path)) || coupled_running();
        float bh = 32, byy = cy - bh / 2;
        if (mine) {
            float w = ui_button_width(NULL, "Stop run");
            rx -= w;
            if (ui_button(ui_id("top-stop"), (Rectangle){ rx, byy, w, bh }, NULL, "Stop run", BTN_DANGER)) {
                if (coupled_running()) coupling_stop(A.cpl, GetTime()); else job_stop(A.cj, GetTime());
            }
            if (app_can_pause() && !A.cj->pausing) {
                w = ui_button_width(NULL, "Pause");
                rx -= w + 8;
                if (ui_button(ui_id("top-pause"), (Rectangle){ rx, byy, w, bh }, NULL, "Pause", BTN_SECONDARY)) app_pause_run();
                ui_tooltip((Rectangle){ rx, byy, w, bh }, "Write the time step it is on and stop; Resume continues from there");
            }
        } else {
            bool paused = app_resumable(NULL);
            const char *rl = paused ? "Resume" : A.cs->s.prog ? term(T_RUN_GO) : "Run case";
            float w = ui_button_width(NULL, rl);
            rx -= w;
            ui_disable(app_jobs_active() >= MAX_JOBS);
            if (ui_button(ui_id("top-run"), (Rectangle){ rx, byy, w, bh }, NULL, rl, BTN_PRIMARY)) {
                if (paused) { app_resume_run(); app_go(PG_RUN); }
                else if (A.page == PG_RUN) A.runPending = true;
                else app_go(PG_RUN);
            }
            ui_disable(false);
            if (app_jobs_active() >= MAX_JOBS) ui_tooltip((Rectangle){ rx, byy, w, bh }, "The most runs at once are going");
        }
        rx -= 8;
        float w = ui_button_width(NULL, "ParaView");
        rx -= w;
        if (ui_button(ui_id("top-pv"), (Rectangle){ rx, byy, w, bh }, NULL, "ParaView", BTN_SECONDARY)) app_open_paraview();
        ui_tooltip((Rectangle){ rx, byy, w, bh }, paraview_description(&A.env, &A.set));
        rx -= 8;
        if (!A.cs->s.prog) {
            w = ui_button_width(NULL, "ParaView + Claude");
            rx -= w;
            if (ui_button(ui_id("top-pvc"), (Rectangle){ rx, byy, w, bh }, NULL, "ParaView + Claude", BTN_SECONDARY)) { A.modal = MD_PV_CLAUDE; A.mText2[0] = 0; }
            ui_tooltip((Rectangle){ rx, byy, w, bh }, "Claude Code builds a ParaView view for this case");
            rx -= 8;
        }
        int dirty = case_dirty_count(A.cs) + (prog_setup_dirty() ? 1 : 0);
        char lab[48]; snprintf(lab, sizeof lab, dirty ? "Save %d" : "Saved", dirty);
        w = ui_button_width(NULL, lab) + 6;
        rx -= w;
        ui_disable(dirty == 0);
        if (ui_button(ui_id("top-save"), (Rectangle){ rx, byy, w, bh }, NULL, lab, dirty ? BTN_PRIMARY : BTN_GHOST)) app_save_request();
        ui_disable(false);
        if (dirty) ui_tooltip((Rectangle){ rx, byy, w, bh }, "Save all edited dictionaries (Ctrl+S)");
        rx -= 8;
        for (int k = 1; k >= 0; k--) {
            const char *what = undo_label(k);
            const char *lb = k ? "Redo" : "Undo";
            w = ui_button_width(NULL, lb);
            rx -= w;
            ui_disable(!what);
            if (ui_button(ui_id(k ? "top-redo" : "top-undo"), (Rectangle){ rx, byy, w, bh }, NULL, lb, BTN_GHOST)) undo_apply(k);
            ui_disable(false);
            char tip[300]; pl_fmt_ui(tip, sizeof tip, "%s%s%s (%s)", lb, what ? ": " : "", what ? what : "", k ? "Ctrl+Y" : "Ctrl+Z");
            ui_tooltip((Rectangle){ rx, byy, w, bh }, tip);
            rx -= 8;
        }
        w = ui_button_width(NULL, "Clean");
        rx -= w;
        ui_disable(mine);
        if (ui_button(ui_id("top-clean"), (Rectangle){ rx, byy, w, bh }, NULL, "Clean", BTN_GHOST)) { A.modal = A.cs->s.prog ? MD_PROG_CLEAN : MD_CLEAN; A.mFlag = A.mFlag2 = false; }
        ui_disable(false);
        ui_tooltip((Rectangle){ rx, byy, w, bh }, mine ? "Stop the run first" : "Delete the results and logs (asks first)");
    }
}

#define LC_NAV 30.0f    /* thickness of the bar over the content: the general pages are buttons in it */

static void lcars_column(float W, float H)
{
    float x = LC_M, y0 = LC_HH + 6, cw = LC_CW;
    /* main frame elbow: the column into the bar along the top of the content */
    float eh = 72;
    ui_elbow(x, y0, cw, eh, W - 2 * LC_M, LC_NAV, 40, 18, false, TH.lc[1]);
    const char *Cd = A.cs ? "CASE" : "NO CASE";
    ui_text(F_MED, 15.5f, Cd, x + cw - 10 - ui_text_w(F_MED, 15.5f, Cd), y0 + eh - 22, BLACK);
    /* the general pages: buttons along the bar, Settings at its far end */
    float bx = x + cw + 34, gap = 4;
    for (int i = 0; i <= N_NAV; i++) {
        bool settings = i == N_NAV;
        Page p = settings ? PG_SETTINGS : NAV[i].p;
        const char *label = settings ? "Settings" : NAV[i].label;
        float w = ui_text_w(F_MED, 15.5f, ui_upper(label)) + 40;
        if (settings) bx = W - LC_M - w;
        DrawRectangleRec((Rectangle){ bx - gap, y0, gap, LC_NAV }, BLACK);
        if (lc_block((Rectangle){ bx, y0, w, LC_NAV }, TH.lc[(i * 3 + 2) % 8], label, NULL, false, nav_busy(p), true)) nav_go(p);
        if (A.page == p) DrawRectangleRec((Rectangle){ bx, y0 + LC_NAV + 3, w, 4 }, TH.accent);   /* "you are here" */
        bx += w + gap;
        if (i == N_NAV - 1) DrawRectangleRec((Rectangle){ bx - gap, y0, gap, LC_NAV }, BLACK);    /* the last one ends */
    }
    DrawRectangleRec((Rectangle){ W - LC_M - ui_text_w(F_MED, 15.5f, "SETTINGS") - 40 - gap, y0, gap, LC_NAV }, BLACK);

    /* the column: the open case and its pages */
    struct Item { Page p; const char *label; Color c; float h; };
    struct Item foamPages[] = {
        { PG_OVERVIEW, "Overview", TH.lc[1], 38 }, { PG_MESH, "Mesh", TH.lc[5], 38 }, { PG_BC, "Boundaries", TH.lc[2], 38 },
        { PG_PHYSICS, "Physics", TH.lc[7], 38 }, { PG_NUMERICS, "Numerics", TH.lc[1], 38 }, { PG_RUN, "Run", TH.lc[0], 38 },
        { PG_FILES, "Files", TH.lc[6], 38 }, { PG_HISTORY, "History", TH.lc[3], 38 } };
    struct Item progPages[] = {
        { PG_OVERVIEW, "Overview", TH.lc[1], 38 }, { PG_SETUP, "Setup", TH.lc[7], 38 }, { PG_RUN, "Run", TH.lc[0], 38 },
        { PG_RESULTS, "Results", TH.lc[5], 38 }, { PG_FILES, "Files", TH.lc[6], 38 }, { PG_HISTORY, "History", TH.lc[3], 38 } };
    struct Item cplPages[] = {
        { PG_OVERVIEW, "Overview", TH.lc[1], 38 }, { PG_SETUP, "Coupling", TH.lc[7], 38 }, { PG_RESULTS, "Participants", TH.lc[5], 38 },
        { PG_RUN, "Run", TH.lc[0], 38 }, { PG_FILES, "Files", TH.lc[6], 38 }, { PG_HISTORY, "History", TH.lc[3], 38 } };
    bool otherProg = A.cs && A.cs->s.prog;
    struct Item *pages = coupled_open() ? cplPages : otherProg ? progPages : foamPages;
    const int nPages = coupled_open() ? (int)(sizeof cplPages / sizeof cplPages[0]) :
                       otherProg ? (int)(sizeof progPages / sizeof progPages[0]) : (int)(sizeof foamPages / sizeof foamPages[0]);
    Job *oj[3]; int no = other_jobs(oj, 3);
    float bottomH = 58 + no * (48 + gap);
    float need = A.cs ? 62 + gap + nPages * (38 + gap) + 16 : 0;
    float avail = (H - LC_M - bottomH - gap) - (y0 + eh + gap);
    float k = need > avail ? fmaxf(0.62f, avail / need) : 1.0f;
    float y = y0 + eh + gap;
    if (A.cs) {
        float h = roundf(62 * k);
        Color cc = cat_color(A.cs->s.cat);
        char nm[300]; snprintf(nm, sizeof nm, "`%s`", A.cs->s.name);              /* a folder name keeps its case */
        lc_block((Rectangle){ x, y, cw, h }, cc, nm, A.cs->s.prog ? prog_info(A.cs->s.prog)->name : A.cs->s.app[0] ? A.cs->s.app : "NO SOLVER", false, false, false);
        y += h + gap;
        for (int i = 0; i < nPages; i++) {
            float hh = roundf(pages[i].h * k);
            bool running = pages[i].p == PG_RUN && (coupled_open() ? coupled_running() : A.cj->active && !strcmp(A.cj->casePath, A.cs->s.path));
            ui_register(nav_id(page_label(pages[i].p)), (Rectangle){ x, y, cw, hh }, page_label(pages[i].p));   /* same id in every theme */
            Badge bd = nav_badge(pages[i].p);
            char code[48] = "";
            if (bd.text) snprintf(code, sizeof code, "%s%s", bd.kind == TOAST_INFO ? "" : "! ", bd.text);   /* Antonio has no x-mark glyph */
            if (lc_block((Rectangle){ x, y, cw, hh }, pages[i].c, pages[i].label, code[0] ? ui_upper(code) : NULL, A.page == pages[i].p, running, true)) app_go(pages[i].p);
            y += hh + gap;
        }
    } else {
        if (lc_block((Rectangle){ x, y, cw, 62 }, TH.lc[2], "Open a case", "NONE OPEN", false, false, true)) nav_go(PG_CASES);
        y += 62 + gap;
    }
    /* filler down to the bottom group */
    float by = H - LC_M - bottomH;
    float fh = by - gap - y;
    if (fh > 120) {                                   /* two filler blocks, like the column on a bridge console */
        DrawRectangleRec((Rectangle){ x, y, cw, fh - 64 - gap }, TH.lc[3]);
        DrawRectangleRec((Rectangle){ x, y + fh - 64, cw, 64 }, TH.lc[4]);
    } else if (fh > 6) DrawRectangleRec((Rectangle){ x, y, cw, fh }, TH.lc[3]);
    y = by;
    for (int i = 0; i < no; i++) {
        if (lc_block((Rectangle){ x, y, cw, 48 }, TH.accent, pl_basename(oj[i]->casePath), "RUNNING", false, false, true)) {
            if (case_is_project(oj[i]->casePath)) app_open_case(oj[i]->casePath, PG_RUN);
        }
        y += 48 + gap;
    }
    const FoamInstall *fi = env_active(&A.env, &A.set);
    Rectangle env = { x, y, cw, H - LC_M - y };
    lc_round_bl(env, 30, fi ? TH.lc[6] : TH.err);
    const char *E = fi ? "ENVIRONMENT READY" : "SET UP OPENFOAM";
    ui_text(F_MED, 15, E, env.x + cw - 10 - ui_text_w(F_MED, 15, E), env.y + env.height - 22, BLACK);
    ui_register(ui_id("env-status"), env, E);
    if (ui_hovered(env)) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    if (ui_clicked(env)) A.modal = MD_ENV;
}

/* ============================================================================ frame */
static bool run_short_of_end(const char *path)
{
    CaseSummary cs; case_summarize(path, &cs);
    char lg[96]; snprintf(lg, sizeof lg, "log.%s", cs.app);
    double last; bool ended, conv;
    return cs.app[0] && log_tail_state(path, lg, &last, &ended, &conv) && last >= 0 && !conv && cs.endTime > 0 && last < cs.endTime * (1 - 1e-6);
}

/* a continued run set startFrom latestTime for itself; put the case's own value back (H-16). Only when it still reads
   latestTime (an edit made meanwhile wins) and nothing else in the file is waiting to be saved. */
static void restore_start_from(const char *casePath, bool open)
{
    char mk[PL_PATH]; pl_join(mk, sizeof mk, casePath, ".qc-startfrom");
    char *was = pl_read_file(mk, NULL);
    if (!was) return;
    pl_trim_inplace(was);
    char cdp[PL_PATH]; pl_join(cdp, sizeof cdp, casePath, "system/controlDict");
    FDFile *cd = open ? case_dict(A.cs, "system/controlDict") : fd_load(cdp);
    char cur[64] = "";
    if (cd && fd_get(cd, "startFrom", cur, sizeof cur) && !strcmp(cur, "latestTime") && !(open && cd->dirty)) {
        bool ok = !strcmp(was, "-") ? fd_remove(cd, "startFrom") : fd_put(cd, "startFrom", was);
        if (ok) ok = fd_save(cd);
        if (ok) unlink(mk);
    } else if (cd && strcmp(cur, "latestTime")) unlink(mk);
    if (!open && cd) fd_free(cd);
    free(was);
}

static void on_job_finished(Job *j)
{
    bool here = A.cs && !strcmp(A.cs->s.path, j->casePath);
    char who[96] = "";                                    /* another case finished: say which */
    if (!here) snprintf(who, sizeof who, "%s: ", pl_basename(j->casePath));
    app_sound(j->failed ? SND_ALERT : j->stopped ? SND_STOPPED : SND_DONE);   /* before the toasts: it stands for them */
    if (j->failed) {
        const char *lab = j->cur < j->n ? j->steps[j->cur].label : "Run";
        ui_toast(TOAST_ERR, "%s%s failed. The log on the Run page shows why.", who, lab);
        if (j->isRun || here) flavour_event(FEV_FAILED, j->casePath, lab);
    } else if (j->stopped) ui_toast(TOAST_WARN, "%sRun stopped", who);
    else if (!case_pause_pending(j->casePath) && !(j->isRun && run_short_of_end(j->casePath))) {
        ui_toast(TOAST_OK, "%s%s finished in %.0f s", who, j->title, j->finishedAt - j->startedAt);
        if (j->isRun) flavour_event(FEV_DONE, j->casePath, NULL);
    }
    bool paused = false;
    if (case_pause_pending(j->casePath)) {                 /* it ended by Pause: put stopAt back, remember where */
        CaseSummary cs; case_summarize(j->casePath, &cs);
        paused = !j->failed && !j->stopped && !j->convStop;   /* converged: done, not paused */
        case_pause_settle(j->casePath, paused, case_latest_result(&cs));
        if (paused) ui_toast(TOAST_INFO, "%sPaused at t = %s. Resume continues from there.", who, case_latest_result(&cs));
        else if (j->convStop && !j->failed) ui_toast(TOAST_OK, "%sConverged: stopped at t = %s", who, case_latest_result(&cs));
    }
    /* ended without an error but short of the end time (stopAt writeNow/nextWrite, a function object that ends it): say so */
    bool early = false;
    if (j->isRun && !j->failed && !j->stopped && !paused && !j->convStop) {
        CaseSummary cs; case_summarize(j->casePath, &cs);
        char lg[96]; snprintf(lg, sizeof lg, "log.%s", cs.app);
        double last; bool ended, conv;
        if (cs.app[0] && log_tail_state(j->casePath, lg, &last, &ended, &conv) && last >= 0 && !conv && cs.endTime > 0 &&
            last < cs.endTime * (1 - 1e-6)) {
            early = true;
            char p[PL_PATH], stopAt[64] = "endTime"; pl_join(p, sizeof p, j->casePath, "system/controlDict");
            FDFile *cd = fd_load(p);
            if (cd) { fd_get(cd, "stopAt", stopAt, sizeof stopAt); fd_free(cd); }
            ui_toast(TOAST_WARN, "%s%s stopped at t = %g of %g without an error%s%s%s", who, cs.app, last, cs.endTime,
                     strcmp(stopAt, "endTime") ? " (stopAt is " : "", strcmp(stopAt, "endTime") ? stopAt : "", strcmp(stopAt, "endTime") ? " in system/controlDict)" : "");
        }
    }
    const char *outcome = j->failed ? "failed" : j->stopped ? "stopped" : paused ? "paused" : early ? "early" : j->convStop ? "converged" : "finished";
    if (j->isRun && A.set.history) { hist_run_ended(j->casePath, !strcmp(outcome, "converged") ? "finished" : outcome); history_invalidate(); }
    if (j->isRun && access(j->casePath, W_OK) == 0) {     /* 3.7 CAPTAIN'S LOG: the run's record, kept in the case */
        static const char *const stName[] = { "pending", "running", "done", "failed", "skipped", "stopped" };
        RunEnd re = { .casePath = j->casePath, .outcome = outcome, .version = "OpenFOAM", .procs = j->procs,
                      .wall = j->finishedAt - j->startedAt, .ended = time(NULL) };
        for (int i = 0; i < j->n && re.nSteps < RUNLOG_MAX_STEPS; i++) {
            const JobStep *st = &j->steps[i];
            if (st->solver && st->log[0] && !re.solverLog) re.solverLog = st->log;
            re.stepLabel[re.nSteps] = st->label;
            re.stepState[re.nSteps] = st->st >= 0 && st->st <= JS_STOPPED ? stName[st->st] : "";
            re.stepSec[re.nSteps++] = st->t1 > st->t0 ? st->t1 - st->t0 : 0;
        }
        for (int i = 0; i < A.env.nInst; i++) if (!strcmp(A.env.inst[i].bashrc, j->bashrc)) re.version = A.env.inst[i].version;
        if (!strcmp(re.version, "OpenFOAM") && A.env.nInst > 0) re.version = A.env.inst[0].version;
        runlog_write_bg(&re);
    }
    restore_start_from(j->casePath, here);
    if (here) { case_rescan(A.cs); case_reload_changed(A.cs); }
    app_movie_finished(j);
    app_rescan_cases(true);
    /* a desktop notification for a long run, when the window isn't in front (or always, by the setting) */
    double took = j->finishedAt - j->startedAt;
    if (j->isRun && A.set.notify && took >= A.set.notifyMinSec && (A.set.notify == 2 || !IsWindowFocused()) && !g_ui_no_input) {
        char t[160], b[300], d[32];
        pl_duration(d, sizeof d, took);
        const char *name = pl_basename(j->casePath);
        if (j->failed) { snprintf(t, sizeof t, "%s failed", name); snprintf(b, sizeof b, "%s stopped with an error after %s. Q-Continuum's Run page shows why.", j->cur < j->n ? j->steps[j->cur].label : "The run", d); }
        else if (paused) { snprintf(t, sizeof t, "%s paused", name); snprintf(b, sizeof b, "Paused after %s. Resume continues from the last write.", d); }
        else if (j->stopped) { snprintf(t, sizeof t, "%s stopped", name); snprintf(b, sizeof b, "The run was stopped after %s.", d); }
        else if (j->convStop) { snprintf(t, sizeof t, "%s converged", name); snprintf(b, sizeof b, "The monitored value settled after %s; the run stopped and wrote its last time step.", d); }
        else { snprintf(t, sizeof t, "%s finished", name); snprintf(b, sizeof b, "%s finished in %s.%s", j->title, d, app_queue_count() ? " The queue starts the next run." : ""); }
        notify_desktop(&A.env, t, b, A.set.notifySound);
    }
}

/* "Stop when converged", for every run whichever case is open: each running case's settings, its monitor file
   read every couple of seconds, and the same write-and-stop as Pause once the value has settled */
static struct { char path[PL_PATH]; MonSet ms; double at; } CW[MAX_JOBS + 1];

static void converge_watch(int k, Job *j, double now)
{
    if (!j->active || !j->isRun || j->pausing || j->convStop || now - CW[k].at < 2.0) return;
    CW[k].at = now;
    CaseCfg cfg; casecfg_load(j->casePath, &cfg);
    if (!cfg.convOn || !cfg.convFile[0] || cfg.convWin < 2) return;
    if (strcmp(CW[k].path, j->casePath)) { monset_open(&CW[k].ms, j->casePath); pl_strlcpy(CW[k].path, j->casePath, sizeof CW[k].path); }
    else monset_poll(&CW[k].ms, now);
    int fi = monset_find(&CW[k].ms, cfg.convFile);
    if (fi < 0) return;
    MonFile *f = &CW[k].ms.f[fi];
    int ci = -1; for (int i = 0; i < f->nCols; i++) if (!strcmp(f->col[i], cfg.convCol)) ci = i;
    double ch;
    if (!mon_converged(f, ci, cfg.convWin, cfg.convTol, &ch)) return;
    char err[300];
    CaseSummary cs; case_summarize(j->casePath, &cs);
    if (case_pause_request(j->casePath, cs.app, err, sizeof err)) {
        j->convStop = true;
        ui_toast(TOAST_OK, "%s: %s has settled (%.2g %% over the last %d samples). Stopping: the solver writes this time step.",
                 pl_basename(j->casePath), cfg.convCol, ch * 100, cfg.convWin);
    } else ui_toast(TOAST_WARN, "%s: converged, but %s", pl_basename(j->casePath), err);
}

/* the chosen interface size, unless the window is too small to lay the pages out at that size */
float app_fit_scale(void)
{
    float fit = fminf(GetScreenWidth() / 1100.0f, GetScreenHeight() / 700.0f);
    return fmaxf(0.75f, fminf(A.set.uiScale, fit));
}

double g_frame_work_ms;                                  /* the last frame's own work, without the wait for vsync */

/* QC_FRAME_PROF=1: a frame over 300 ms prints how long each part took (stderr), to find what froze it */
static double profT[16]; static const char *profN[16]; static int profK;
#define PROF(name) do { if (profOn) { profT[profK] = GetTime(); profN[profK] = name; if (profK < 15) profK++; } } while (0)
static int profOn = -1;

static void frame(void)
{
    double now = GetTime();
    if (profOn < 0) profOn = getenv("QC_FRAME_PROF") != NULL;
    profK = 0; PROF("start");
    ui_set_scale(app_fit_scale());
    /* the window and desktop icon follow the theme: a few frames after a switch, not on the switch's own (slow) frame;
       hidden test runs leave the desktop alone */
    static int iconTheme = -1, iconWait = 0;
    if (ui_theme_id() != iconTheme && (iconTheme < 0 || ++iconWait >= 4)) {
        iconTheme = ui_theme_id(); iconWait = 0;
        logo_set_icon(iconTheme, !IsWindowState(FLAG_WINDOW_HIDDEN));
    }
    for (int k = 0; k <= MAX_JOBS; k++) {                 /* every run, whichever case is open */
        Job *j = &A.jobs[k];
        bool was = j->active;
        job_poll(j, now);
        /* a step just started: its log file was truncated, so read it again from the start */
        if (j->launches != A.jobSeen[k]) {
            A.jobSeen[k] = j->launches;
            if (A.cs && j->active && j->cur < j->n && !strcmp(A.cs->s.path, j->casePath))
                logset_restart(&A.logs, j->steps[j->cur].log);
        }
        if (was && !j->active && j->external && !j->stopped) {   /* no exit code: the log says how it ended */
            CaseSummary cs; case_summarize(j->casePath, &cs);
            if (cs.status == CS_FAILED) j->failed = true;
            else if (cs.status != CS_DONE && !case_pause_pending(j->casePath)) j->stopped = true;
        }
        if (was && !j->active) on_job_finished(j);
        converge_watch(k, j, now);
    }
    PROF("jobs");
    coupling_frame(now);
    PROF("coupling");
    queue_tick(now);
    PROF("queue");
    deme_pair_tick(now);
    PROF("deme");
    if (A.cs) {
        g_log_budget_ms = ui_awake() ? 8.0 : 25.0;           /* read big logs faster while nobody is interacting */
        logset_poll(&A.logs, now, false);
        static double monAt;
        if (now - monAt > 0.5) { monAt = now; monset_poll(&A.mons, now); }
        if (now - A.lastDiskCheck > 1.5) {
            A.lastDiskCheck = now;
            /* 2.7: say when a file changed outside the app, unless a run in the case is the one writing (Allrun scripts edit
               their dictionaries) */
            char what[600];
            bool ownRun = A.cj->active && !strcmp(A.cj->casePath, A.cs->s.path);
            if (case_reload_report(A.cs, what, sizeof what) > 0) {
                if (!ownRun) ui_toast(TOAST_INFO, "Changed outside Q-Continuum, now shown here: %s", what);
                case_rescan(A.cs);                           /* the summary (end time, time step, solver) too */
            }
            if (case_conflicts(A.cs, NULL) && A.modal == MD_NONE) A.modal = MD_CONFLICT;
            if (A.cj->active) case_rescan(A.cs);
        }
    }

    PROF("logs+disk");
    /* keyboard shortcuts */
    red_alert_tick(now);
    /* LCARS sounds: find out in the background whether a sound device works, before the first key beep needs it */
    if (A.set.sounds && app_sound_pack() >= 0 && !g_ui_no_input && !getenv("QC_TEST_SOUND_LOG")) sound_prepare();
    actions_keys();                                          /* the registry's shortcuts (actions.c) */
    session_note();
    if (A.copyPending) { A.copyPending = false; app_copy_case_to_workspace(); }   /* not mid-draw: it swaps A.cs */

    PROF("keys");
    BeginDrawing();
    ui_crt_begin();                                          /* Alien, Pip-Boy, Severance: through a tube */
    ClearBackground(TH.bg);
    ui_begin_frame();
    stlmesh_tick();                                         /* the STL file dialog, whichever page is open */
    ui_backdrop(ui_width(), ui_height());
    float W = ui_width(), H = ui_height();
    float sw = 248;
    Rectangle side = { 0, 0, sw, H };
    Rectangle nav = { sw, 0, W - sw, 52 };
    Rectangle top = { sw, 52, W - sw, 60 };
    Rectangle body = { sw, 112, W - sw, H - 112 };
    if (TH.lcars) {
        float by = LC_HH + 6 + LC_NAV + 4 + 14;      /* black under the frame (and its "you are here" marks): scrolled text is clipped clear */
        body = (Rectangle){ LC_M + LC_CW + 4, by, W - (LC_M + LC_CW + 4) - LC_M, H - by - LC_M };
    }
    if (!TH.glass && !TH.hud && TH.retro != RETRO_HALFLIFE) ui_rect(body, 0, TH.bg);     /* glass, JARVIS, Half-Life: the backdrop shows through */
    switch (A.page) {
    case PG_CASES: page_cases(body); break;
    case PG_NEW: page_new(body); break;
    case PG_OVERVIEW: if (coupled_open()) page_cpl_case(body, 0); else if (A.cs && A.cs->s.prog) page_prog_overview(body); else page_overview(body); break;
    case PG_MESH: page_mesh(body); break;
    case PG_BC: page_bc(body); break;
    case PG_PHYSICS: page_physics(body); break;
    case PG_NUMERICS: page_numerics(body); break;
    case PG_RUN: if (coupled_open()) page_cpl_case(body, 3); else if (A.cs && A.cs->s.prog) page_prog_run(body); else page_run(body); break;
    case PG_FILES: page_files(body); break;
    case PG_HISTORY: page_history(body); break;
    case PG_QUEUE: page_queue(body); break;
    case PG_STUDIES: page_studies(body); break;
    case PG_SETUP: if (coupled_open()) page_cpl_case(body, 1); else page_prog_setup(body); break;
    case PG_RESULTS: if (coupled_open()) page_cpl_case(body, 2); else page_prog_results(body); break;
    case PG_SETTINGS: page_settings(body); break;
    case PG_COUPLING: page_coupling(body); break;
    default: break;
    }
    if (TH.lcars) {
        DrawRectangleRec((Rectangle){ 0, 0, W, body.y }, TH.bg);           /* keep scrolled content under the frame */
        DrawRectangleRec((Rectangle){ 0, 0, body.x, H }, TH.bg);
        lcars_header(W);
        lcars_column(W, H);
    } else {
        ui_rect(top, 0, TH.glass || TH.hud ? TH.sidebar : TH.bg);
        draw_topbar(top);
        nav_bar(nav);
        draw_sidebar(side);
    }
    PROF("page+chrome");
    draw_modals();
    PROF("modals");
    ui_end_frame();
    rlDrawRenderBatchActive();                               /* the GPU work counts: drawing is part of the frame */
    g_frame_work_ms = (GetTime() - now) * 1000.0;
    ui_crt_end();
    EndDrawing();
    PROF("end");
    if (profOn && profT[profK - 1] - profT[0] > 0.3) {
        fprintf(stderr, "slow frame %.0f ms:", (profT[profK - 1] - profT[0]) * 1000);
        for (int k = 1; k < profK; k++) fprintf(stderr, " %s %.0f", profN[k], (profT[k] - profT[k - 1]) * 1000);
        fprintf(stderr, "\n");
    }
}

bool g_autorun = false;

/* can this process open a GL window with the current environment? Tried in a child, which may crash doing so */
static bool gl_window_works(void)
{
    pid_t pid = fork();
    if (pid < 0) return true;                                  /* can't tell: go ahead */
    if (pid == 0) {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        SetTraceLogLevel(LOG_NONE);
        SetConfigFlags(FLAG_WINDOW_HIDDEN);
        InitWindow(64, 64, "");
        _exit(IsWindowReady() ? 0 : 1);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return true;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* glibc copies large blocks with rep movsb, which on AMD Zen writes into GPU-mapped memory at ~10 MB/s: under WSLg
   that is the d3d12 driver's glBufferSubData, ~97 % of every frame (Overview 100+ ms). The tunable keeps memcpy on
   vector stores, and glibc reads it only at startup, so the app starts itself again once with it set. The second
   start gives back the environment it was given, so solvers and tools launched from the app don't inherit it. */
static void fast_gpu_copies(char **argv)
{
#if defined(__linux__) && defined(__x86_64__)
    static const char TUNE[] = "glibc.cpu.x86_rep_movsb_threshold=4294967295";
    const char *t = getenv("GLIBC_TUNABLES");
    if (getenv("QC_TUNED")) {
        const char *orig = getenv("QC_TUNABLES_ORIG");
        if (orig && *orig) setenv("GLIBC_TUNABLES", orig, 1); else unsetenv("GLIBC_TUNABLES");
        unsetenv("QC_TUNABLES_ORIG"); unsetenv("QC_TUNED");
        return;
    }
    const char *pre = getenv("LD_PRELOAD");                        /* under Valgrind /proc/self/exe is Valgrind */
    if (getenv("QC_NO_TUNE") || (t && strstr(t, "x86_rep_movsb_threshold")) || (pre && strstr(pre, "vgpreload"))) return;
    char v[2048];
    if (snprintf(v, sizeof v, "%s%s%s", t ? t : "", t && *t ? ":" : "", TUNE) >= (int)sizeof v) return;
    setenv("QC_TUNABLES_ORIG", t ? t : "", 1); setenv("QC_TUNED", "1", 1); setenv("GLIBC_TUNABLES", v, 1);
    execv("/proc/self/exe", argv);
    if (t) setenv("GLIBC_TUNABLES", t, 1); else unsetenv("GLIBC_TUNABLES");   /* no exec: run as we are */
    unsetenv("QC_TUNABLES_ORIG"); unsetenv("QC_TUNED");
#else
    (void)argv;
#endif
}

int main(int argc, char **argv)
{
    fast_gpu_copies(argv);
    const char *shot = NULL, *casePath = NULL, *pageName = NULL;
    int frames = 3, theme = -1, bcF = -1, bcP = -1;
    bool testModal = false, waitJob = false;
    float testScroll = 0;
    bool testCont = false;
    const char *testFile = NULL, *testCpl = NULL, *scriptPath = NULL;
    int testTab = 0;
    int sweep = 0, closeAt = -1;
    bool hidden = false;
    int nInj = 0, injF[32]; float injX[32], injY[32], injX2[32], injY2[32]; const char *injT[32]; bool injD[32] = { 0 };
    int testProcs = 0;
    float scale = 0;
    const char *exportLogos = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--case") && i + 1 < argc) casePath = argv[++i];
        else if (!strcmp(argv[i], "--page") && i + 1 < argc) pageName = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--theme") && i + 1 < argc) theme = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bc") && i + 2 < argc) { bcF = atoi(argv[++i]); bcP = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--autorun")) g_autorun = true;
        else if (!strcmp(argv[i], "--procs") && i + 1 < argc) testProcs = atoi(argv[++i]);   /* testing */
        else if (!strcmp(argv[i], "--modal-clean")) testModal = true;        /* testing: show a dialog */
        else if (!strcmp(argv[i], "--wait-job")) waitJob = true;
        else if (!strcmp(argv[i], "--click") && i + 1 < argc && nInj < 32) { injF[nInj] = 0; sscanf(argv[++i], "%d:%f:%f", &injF[nInj], &injX[nInj], &injY[nInj]); injT[nInj++] = NULL; }   /* testing: frame:x:y */
        else if (!strcmp(argv[i], "--sweep") && i + 1 < argc) sweep = atoi(argv[++i]);                       /* testing: every page and tab, N frames each, then quit */
        else if (!strcmp(argv[i], "--close-at") && i + 1 < argc) closeAt = atoi(argv[++i]);             /* testing: as if the window were closed at frame N */
        else if (!strcmp(argv[i], "--drag") && i + 1 < argc && nInj < 32) { sscanf(argv[++i], "%d:%f:%f:%f:%f", &injF[nInj], &injX[nInj], &injY[nInj], &injX2[nInj], &injY2[nInj]); injD[nInj] = true; injT[nInj++] = NULL; }   /* testing: frame:x0:y0:x1:y1 */
        else if (!strcmp(argv[i], "--type") && i + 1 < argc && nInj < 32) { char *q = strchr(argv[++i], ':'); injF[nInj] = atoi(argv[i]); injT[nInj++] = q ? q + 1 : ""; }        /* testing: frame:text */
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) testFile = argv[++i];               /* testing: Files page selection */
        else if (!strcmp(argv[i], "--raw")) A.filesRaw = true;                                    /* testing: Files page text mode */
        else if (!strcmp(argv[i], "--coupling") && i + 1 < argc) testCpl = argv[++i];           /* testing: open a coupled run */
        else if (!strcmp(argv[i], "--tab") && i + 1 < argc) testTab = atoi(argv[++i]);            /* testing: Coupling page tab */
        else if (!strcmp(argv[i], "--builtin")) { extern bool g_force_builtin; g_force_builtin = true; }   /* testing: built-in steps, not Allrun */
        else if (!strcmp(argv[i], "--continue")) testCont = true;          /* testing: tick "Continue from the latest time" */
        else if (!strcmp(argv[i], "--scroll") && i + 1 < argc) testScroll = (float)atof(argv[++i]);   /* testing: scroll every page */             /* testing: screenshot once the started job has ended */
        else if (!strcmp(argv[i], "--hidden")) hidden = true;
        else if (!strcmp(argv[i], "--export-logos") && i + 1 < argc) { exportLogos = argv[++i]; hidden = true; }   /* every theme's icon as a PNG */
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) scriptPath = argv[++i];      /* testing: replay a script */                                     /* testing: no window on the desktop */
        else if (!strcmp(argv[i], "--version")) { printf("Q-Continuum %s\n", QC_VERSION); return 0; }
        else if (!strcmp(argv[i], "--play-sounds") && i + 1 < argc) {                           /* every cue of a pack, for real */
            SetTraceLogLevel(LOG_WARNING);
            const char *pk = argv[++i];
            static const char *const PK[] = { "lcars", "stardew", "pokemon", "jarvis", "severance", "alien", "pipboy", "nerv", "portal",
                                              "souls", "factorio", "win95", "starcitizen", "halflife" };
            int p = SOUND_LCARS;
            for (int k = 0; k < SOUND__N; k++) if (!strcmp(pk, PK[k])) p = k;
            printf("played %d sounds\n", sound_play_all(p, 0.45f));
            return 0;
        }
        else if (!strcmp(argv[i], "--export-sounds") && i + 1 < argc) {                          /* the LCARS cues as WAV files */
            SetTraceLogLevel(LOG_WARNING);
            int n = sound_export(argv[++i]);
            printf("wrote %d sounds to %s\n", n, argv[i]);
            return n == SND__N * SOUND__N ? 0 : 1;
        }
        else if (!strcmp(argv[i], "--list-actions")) {                                            /* tests: the registry */
            for (int k = 0; k < actions_count(); k++) {
                const Action *a = actions_get(k);
                printf("%s\t%s\t%s\n", a->id, a->widget ? a->widget : "-", a->shortcut ? a->shortcut : "-");
            }
            return 0;
        }
        else if (!strcmp(argv[i], "--notify-test")) {                                             /* testing: one desktop notification */
            env_detect(&A.env);
            notify_desktop(&A.env, "Q-Continuum test", "cavity's run finished in 2m 05s. (A test notification: it's working.)", true);
            sleep(3);
            return 0;
        }
        else if (argv[i][0] != '-' && !casePath) casePath = argv[i];
    }
    env_detect(&A.env);
    settings_load(&A.set, &A.env);
    undo_init();
    if (shot || sweep > 0 || scriptPath) g_settings_readonly = g_ui_no_input = true;
    if (scriptPath && !script_load(scriptPath)) return 2;
    if (theme >= 0) A.set.theme = theme;
    if (scale > 0) A.set.uiScale = scale;

    SetTraceLogLevel(LOG_WARNING);
    /* WSLg: Mesa defaults to llvmpipe (software), which spent 2-3 cores redrawing an idle window; the d3d12 driver draws
       on the GPU. raylib 5.5 crashes inside InitWindow when the window can't be created (it asks the missing window for
       its monitor), so the retry further down is never reached: test each choice in a throwaway process first. */
    if (pl_is_wsl() && !getenv("GALLIUM_DRIVER") && !getenv("QC_SOFTWARE_GL") &&
        (pl_is_file("/usr/lib/x86_64-linux-gnu/dri/d3d12_dri.so") || pl_is_file("/usr/lib/aarch64-linux-gnu/dri/d3d12_dri.so"))) {
        setenv("GALLIUM_DRIVER", "d3d12", 1);
        if (!gl_window_works()) unsetenv("GALLIUM_DRIVER");
    }
    if (!getenv("GALLIUM_DRIVER") && !getenv("LIBGL_ALWAYS_SOFTWARE") && !gl_window_works()) {
        setenv("QC_SOFTWARE_GL", "1", 1);
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    }
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | (hidden ? FLAG_WINDOW_HIDDEN : 0));
    InitWindow(1440, 900, "Q-Continuum");
    if (!IsWindowReady()) {
        /* WSL / VM GPUs sometimes cannot create a GL context: retry once with Mesa's software renderer */
        if (!getenv("QC_SOFTWARE_GL")) {
            setenv("QC_SOFTWARE_GL", "1", 1);
            setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
            unsetenv("GALLIUM_DRIVER");
            execv("/proc/self/exe", argv);         // NOLINT(clang-analyzer-optin.taint.GenericTaint): re-runs itself with its own argv
        }
        fprintf(stderr, "Q-Continuum: could not open a window. On WSL, update with 'wsl --update' (PowerShell) so WSLg is available.\n");
        return 1;
    }
    if (A.set.fullscreen && !hidden && !shot && !scriptPath) app_fullscreen(true);   /* F11 was on when it closed */
    else A.set.fullscreen = false;                     /* test runs open as a window (and never write settings.ini) */
    if (exportLogos) { int n = logo_export(exportLogos); printf("wrote %d logos to %s\n", n, exportLogos); CloseWindow(); return n == THEME__N ? 0 : 1; }
    SetWindowMinSize(1100, 700);
    SetExitKey(0);
    SetTargetFPS(60);
    float dpi = GetWindowScaleDPI().x;
    if (scale <= 0 && dpi > 1.01f && A.set.uiScale == 1.0f) A.set.uiScale = dpi;
    ui_init(A.set.uiScale, A.set.theme);
    ui_sound_hook = ui_sound;

    A.cj = &A.jobs[MAX_JOBS];
    for (int i = 0; i <= MAX_JOBS; i++) A.jobs[i].pid = -1;
    A.bcField = A.bcPatch = -1;
    A.runLog = -1;
    A.runFollow = true;
    app_rescan_cases(true);
    if (casePath) {
        char real[PL_PATH];
        if (!realpath(casePath, real)) pl_strlcpy(real, casePath, sizeof real);
        app_open_case(real, PG_OVERVIEW);
    } else if (!shot && sweep <= 0) session_restore();       /* no case asked for: back where you left off */
    if (pageName) for (int p = 0; p < PG_COUNT; p++) if (!strcasecmp(pageName, PAGE_TITLES[p]) || (pageName[0] && !strncasecmp(pageName, PAGE_TITLES[p], strlen(pageName)))) { app_go((Page)p); break; }
    if (bcF >= 0) { A.bcField = bcF; A.bcPatch = bcP; }
    if (testModal && A.cs) { A.modal = A.cs->s.prog ? MD_PROG_CLEAN : MD_CLEAN; A.mFlag = A.mFlag2 = false; }
    if (testProcs > 0) A.runProcs = testProcs;
    if (testCont) A.continueRun = true;
    if (testFile) snprintf(A.filesSel, sizeof A.filesSel, "%s", testFile);
    if (testCpl) { A.cpl = coupling_open(testCpl); A.cplValidateOk = -1; A.cplTab = testTab; A.page = PG_COUPLING; }

    if (!script_active() && !g_settings_readonly && A.set.whatsNewSeen < WHATS_NEW_REV && A.modal == MD_NONE) A.modal = MD_WHATS_NEW;
    int n = 0;
    while (!A.quit) {
        for (int k = 0; k < nInj; k++) if (injF[k] == n) { if (injT[k]) ui_inject_text(injT[k]); else if (injD[k]) ui_inject_drag(injX[k], injY[k], injX2[k], injY2[k]); else ui_inject_click(injX[k], injY[k]); }
        if (testScroll > 0 && n == 2) for (int k = 0; k < 24; k++) A.scr[k].target = testScroll;
        if (sweep > 0 && n % sweep == 0) {
            /* (page, tab) stops; each is drawn at the top, then scrolled to the end */
            static const int stops[][2] = { {PG_CASES,0}, {PG_NEW,0}, {PG_OVERVIEW,0}, {PG_MESH,0}, {PG_MESH,1}, {PG_MESH,2}, {PG_MESH,3}, {PG_BC,0}, {PG_BC,1},
                                            {PG_PHYSICS,0}, {PG_PHYSICS,1}, {PG_NUMERICS,0}, {PG_NUMERICS,1},
                                            {PG_NUMERICS,2}, {PG_RUN,0}, {PG_FILES,0}, {PG_HISTORY,0}, {PG_HISTORY,1},
                                            {PG_HISTORY,2}, {PG_SETTINGS,0}, {PG_COUPLING,0}, {PG_QUEUE,0}, {PG_STUDIES,0} };
            int nst = (int)(sizeof stops / sizeof stops[0]), k = n / sweep;
            if (k >= 2 * nst) break;
            int s = k / 2;
            if (k % 2 == 0) {
                if (A.cs || stops[s][0] < PG_OVERVIEW || stops[s][0] >= PG_SETTINGS) app_go((Page)stops[s][0]);
                A.meshTab = A.physicsTab = A.numericsTab = A.settingsTab = A.histTab = stops[s][1];
                if (stops[s][0] == PG_BC) { A.bcField = stops[s][1] ? 0 : -1; A.bcPatch = stops[s][1] ? 0 : -1; }
                for (int q = 0; q < 24; q++) A.scr[q].target = A.scr[q].y = 0;
                if (stops[s][0] == PG_FILES && A.cs) snprintf(A.filesSel, sizeof A.filesSel, "system/controlDict");
            } else for (int q = 0; q < 24; q++) A.scr[q].target = 1e6f;
        }
        if (WindowShouldClose() || n == closeAt) {
            if (app_jobs_active()) A.modal = MD_QUIT_RUNNING;
            else if (A.cs && case_dirty_count(A.cs)) { A.modal = MD_UNSAVED; A.pendingOpen[0] = 0; A.mFlag = true; }
            else A.quit = true;
        }
        script_tick(n);
        frame();
        script_after_frame(n);
        n++;
        if (script_active() && script_done()) break;
        /* full rate while something changes; otherwise a few frames a second keep logs and jobs polled. Test modes
           count frames, so they stay at 60. */
        if (!g_ui_no_input && sweep <= 0) {   /* test modes (screenshots, scripts) stay at 60 */
            static int fps = 60;
            bool busy = app_jobs_active() || (A.cpl && A.cpl->running);
            int want = ui_awake() ? 60 : busy ? 12 : 5;
            if (want != fps) { fps = want; SetTargetFPS(fps); }
        }
        static int doneAt = -1;                                   /* --wait-job: let the monitor read the last output */
        static bool sawCpl = false;
        if (A.cpl && A.cpl->running) sawCpl = true;
        if (waitJob && doneAt < 0 && ((!A.cj->active && A.cj->finished) || (sawCpl && A.cpl && !A.cpl->running))) doneAt = n;
        if (waitJob && doneAt < 0 && !A.cj->active && !A.cj->finished && !sawCpl && n > frames + 600) {
            fprintf(stderr, "--wait-job: no job started (a missed click?); taking the screenshot anyway\n");
            doneAt = n - 30;
        }
        if (A.quit) break;
        if (shot && n >= frames && (!waitJob || (doneAt >= 0 && n >= doneAt + 30))) { Image im = LoadImageFromScreen(); ExportImage(im, shot); UnloadImage(im); break; }
    }
    settings_save(&A.set);
    app_close_case();
    ui_shutdown();
    sound_shutdown();
    CloseWindow();
    return script_active() ? script_exit_code() : 0;
}
