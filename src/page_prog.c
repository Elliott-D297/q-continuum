/* page_prog.c -- the pages of a project of another program (DEME, Chrono, CalculiX, code_aster, code_saturne, MOOSE,
 * Elmer, SPARTA): the same workflow as an OpenFOAM case. Overview (status, checks, a look at the results), Setup (the
 * values in its input file, edited in place: params.c), Run (input, processes, checks, live progress, the chart of
 * its residual-like values and its log), Results (ParaView, cgx, the particle viewer, tables). Files and History are
 * the OpenFOAM pages, which work on any folder. What each program is and does is program.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "params.h"
#include "program.h"
#include "safety.h"

#define MAX_EDIT 1024

static struct {
    char      forPath[PL_PATH];
    NameList  inputs; int sel; double inputsAt;
    int       procs;
    bool      rebuild;
    ProgCheck ck[24]; int nck; double ckAt; char ckFor[160];
    ProgResult res[96]; int nres; double resAt;
    /* setup */
    NameList  setupFiles; int setupSel; char setupFor[160];
    ParamFile pf;
    char      edit[MAX_EDIT][256];
    int       mode;                           /* 0 Basic, 1 All */
    char      search[96];
    double    pfCheckAt;
    /* results */
    int       resSel;
    /* table viewer (a CSV result) */
    char      tblPath[PL_PATH]; long long tblStamp;
    char      tblCols[16][48]; int nCols;
    float    *tbl; int tblRows;
    int       tblX; bool tblOn[16];
} P = { .sel = 0, .resSel = -1 };

static int prog(void) { return A.cs ? A.cs->s.prog : 0; }
static const ProgInfo *info(void) { return prog_info(prog()); }
static const char *input(void) { return P.sel >= 0 && P.sel < P.inputs.n ? P.inputs.names[P.sel] : ""; }

static void edits_from_params(void)
{
    for (int i = 0; i < P.pf.n && i < MAX_EDIT; i++) pl_strlcpy(P.edit[i], P.pf.p[i].value, sizeof P.edit[i]);
}

void prog_case_closed(void)
{
    params_free(&P.pf);
    names_free(&P.inputs); names_free(&P.setupFiles);
    free(P.tbl); P.tbl = NULL; P.tblRows = 0; P.tblPath[0] = 0;
    P.forPath[0] = 0; P.setupFor[0] = 0; P.ckFor[0] = 0;
    P.sel = 0; P.setupSel = 0; P.resSel = -1; P.nres = 0; P.nck = 0;
}

/* ============================================================================ state that follows the open project */
static void sync(void)
{
    if (!A.cs) return;
    double now = GetTime();
    if (strcmp(P.forPath, A.cs->s.path)) {
        prog_case_closed();
        pl_strlcpy(P.forPath, A.cs->s.path, sizeof P.forPath);
        P.procs = info()->mpi || info()->threads ? (A.env.nCpu >= 4 ? 4 : A.env.nCpu) : 1;
        if (prog() == PROG_ELMER || prog() == PROG_ASTER) P.procs = 1;
        P.inputsAt = -100; P.resAt = -100; P.ckAt = -100;
    }
    if (now - P.inputsAt > 3) {
        char keep[160]; pl_strlcpy(keep, input(), sizeof keep);
        prog_inputs(prog(), A.cs->s.path, &P.inputs);
        P.sel = 0;
        for (int i = 0; i < P.inputs.n; i++) if (!strcmp(P.inputs.names[i], keep)) P.sel = i;
        P.inputsAt = now;
    }
    if (now - P.ckAt > 2 || strcmp(P.ckFor, input())) {
        P.nck = prog_checks(prog(), A.cs->s.path, input(), P.ck, 24);
        pl_strlcpy(P.ckFor, input(), sizeof P.ckFor);
        P.ckAt = now;
    }
    static double seenEnd;                                    /* a run just ended: its results are new */
    if (A.cj->finishedAt != seenEnd) { seenEnd = A.cj->finishedAt; P.resAt = -100; P.ckAt = -100; }
    if (now - P.resAt > (A.cj->active ? 2 : 5)) { P.nres = prog_results(prog(), A.cs->s.path, P.res, 96); P.resAt = now; }
}

static bool running(void) { return A.cs && A.cj->active && !strcmp(A.cj->casePath, A.cs->s.path); }

static int bad_checks(void) { int n = 0; for (int i = 0; i < P.nck; i++) if (P.ck[i].sev >= 2) n++; return n; }

/* the log of the run of the selected input (log.<program>.<input>), else the newest */
static LogMon *run_log(void)
{
    char ln[96]; prog_log_name(prog(), input(), ln, sizeof ln);
    int k = logset_find(&A.logs, ln);
    if (k < 0) k = logset_primary(&A.logs, NULL);
    return k >= 0 ? &A.logs.logs[k] : NULL;
}

/* ============================================================================ starting a run */
bool prog_start_run(const char *path, const char *in, int procs, bool quiet)
{
    if (app_jobs_active() >= MAX_JOBS) { ui_toast(TOAST_WARN, "%d runs are already going: wait for one to finish", MAX_JOBS); return false; }
    Job *j = job_slot_for(path);
    if (j == &A.jobs[MAX_JOBS] || j->active) { ui_toast(TOAST_WARN, "Something is already running there"); return false; }
    int pg = prog_detect(path);
    if (pg <= PROG_OPENFOAM) return false;
    bool here = A.cs && !strcmp(A.cs->s.path, path);
    if (here && prog_setup_dirty() && !prog_setup_save()) return false;
    ProgStep st[4]; char err[400];
    int n = prog_run_steps(pg, path, in, procs, here && P.rebuild, st, 4, err, sizeof err);
    if (n <= 0) { ui_toast(TOAST_ERR, "%s", err); return false; }
    char title[96]; snprintf(title, sizeof title, "%s %s", prog_info(pg)->name, in);
    job_reset(j, title);
    for (int i = 0; i < n; i++) {
        job_add(j, st[i].id, st[i].label, st[i].cmd, st[i].log, st[i].solver);
        if (here) logset_restart(&A.logs, st[i].log);
    }
    if (!job_start(j, path, "", GetTime())) { ui_toast(TOAST_ERR, "Could not start %s", title); return false; }
    j->procs = procs < 1 ? 1 : procs;
    j->isRun = true;
    if (A.set.history) {
        char what[160]; snprintf(what, sizeof what, "%s %s, %d %s", prog_info(pg)->name, in, j->procs,
                                 prog_info(pg)->threads ? (j->procs == 1 ? "thread" : "threads") : (j->procs == 1 ? "process" : "processes"));
        hist_run_started_bg(path, what);
        history_invalidate();
    }
    if (here) { A.runLog = -1; A.runFollow = true; P.rebuild = false; P.resAt = -100; }
    if (!quiet) ui_toast(TOAST_INFO, "%s started%s", title, n > 1 ? " (it builds first)" : "");
    return true;
}

static void start_here(void)
{
    if (!A.cs || running()) return;
    if (app_review_needed()) { review_open(1); return; }
    if (bad_checks()) {
        for (int i = 0; i < P.nck; i++) if (P.ck[i].sev >= 2) { ui_toast(TOAST_ERR, "%s", P.ck[i].text); break; }
        return;
    }
    prog_start_run(A.cs->s.path, input(), P.procs, false);
}

static bool start_both(const char *foam)
{
    CaseSummary cs; case_summarize(foam, &cs);
    if (!run_start_queued(foam, -1, A.set.nProcs, case_has_results(&cs), false)) return false;
    return prog_start_run(A.cs->s.path, input(), P.procs, false);
}

/* ============================================================================ shared bits */
static float checks_list(float x, float y, float w, bool fixes)
{
    float y0 = y;
    for (int i = 0; i < P.nck; i++) {
        ProgCheck *c = &P.ck[i];
        Color col = c->sev >= 2 ? TH.err : TH.warn;
        ui_icon(c->sev >= 2 ? ICON_CIRCLE_X : ICON_TRIANGLE_ALERT, x + 8, y + 10, 15, col);
        float fw = fixes && c->fix ? ui_button_width(NULL, c->fixLabel) : 0;
        float th = ui_text_wrap(F_REG, 13, c->text, (Rectangle){ x + 24, y + 1, w - 30 - fw, 80 }, 19, TH.text, true);
        ui_register(ui_idn(ui_id("pcheck"), i), (Rectangle){ x, y, w, th }, c->text);
        if (fw > 0) {
            ui_disable(running());
            if (ui_button(ui_idn(ui_id("pcheck-fix"), i), (Rectangle){ x + w - fw, y - 4, fw, 28 }, NULL, c->fixLabel, BTN_SECONDARY)) {
                if (c->fix == PFIX_BUILD) { P.rebuild = true; start_here(); }
                else {
                    char msg[300];
                    if (prog_apply_fix(prog(), A.cs->s.path, input(), c->fix, msg, sizeof msg)) ui_toast(TOAST_OK, "%s", msg);
                    else ui_toast(TOAST_ERR, "%s", msg);
                    P.ckAt = -100; P.inputsAt = -100;
                }
            }
            ui_disable(false);
        }
        y += fmaxf(th, 19) + 10;
    }
    if (!P.nck) {
        ui_icon(ICON_CIRCLE_CHECK, x + 8, y + 10, 15, TH.ok);
        char t[200]; pl_fmt_ui(t, sizeof t, "%s: %s", term(T_SWEEP), "nothing in the way of a run");
        ui_text(F_REG, 13, t, x + 24, y + 1, TH.text2);
        y += 30;
    }
    return y - y0;
}

static const char *status_text(Color *c, const char **icon)
{
    LogMon *m = run_log();
    *c = TH.text2; *icon = ICON_CIRCLE_DOT;
    if (!A.cs) return "Ready";
    if (running()) { *c = TH.accent; *icon = NULL; return "Running"; }
    if (A.cj->n && !strcmp(A.cj->casePath, A.cs->s.path)) {
        if (A.cj->failed) { *c = TH.err; *icon = ICON_CIRCLE_X; return "Failed"; }
        if (A.cj->stopped) { *c = TH.warn; *icon = ICON_OCTAGON_X; return "Stopped"; }
        if (A.cj->finished) { *c = TH.ok; *icon = ICON_CIRCLE_CHECK; return "Finished"; }
    }
    if (m && m->fatal) { *c = TH.err; *icon = ICON_CIRCLE_X; return "Last run failed"; }
    if (m && (m->ended || m->exitCode == 0)) { *c = TH.ok; *icon = ICON_CIRCLE_CHECK; return "Last run finished"; }
    if (A.cs->s.runningLog[0]) { *c = TH.accent; *icon = ICON_CIRCLE_DOT; return "Running in another process"; }
    if (m && m->exitCode == 143) { *c = TH.warn; *icon = ICON_OCTAGON_X; return "Last run stopped"; }
    if (m && m->steps > 0) { *c = TH.warn; *icon = ICON_OCTAGON_X; return "Last run stopped"; }
    *c = TH.text2; *icon = ICON_CIRCLE_DOT; return "Ready";
}

/* the particle frames among the results: folder and prefix ("" for CSV folders) */
static bool particles_result(char *dir, int cap, char *prefix, int pcap)
{
    for (int i = 0; i < P.nres; i++) {
        if (P.res[i].kind != RK_PARTICLES) continue;
        const char *rel = P.res[i].rel;
        const char *star = strstr(rel, ".*");
        prefix[0] = 0;
        if (star) {                                            /* "dump.flow.*" or "out/dump.*" */
            char r2[256]; pl_strlcpy(r2, rel, sizeof r2); r2[star - rel] = 0;
            char *sl = strrchr(r2, '/');
            if (sl) { *sl = 0; pl_join(dir, (size_t)cap, A.cs->s.path, r2); pl_strlcpy(prefix, sl + 1, (size_t)pcap); }
            else { pl_strlcpy(dir, A.cs->s.path, (size_t)cap); pl_strlcpy(prefix, r2, (size_t)pcap); }
        } else if (!strcmp(rel, ".")) pl_strlcpy(dir, A.cs->s.path, (size_t)cap);
        else pl_join(dir, (size_t)cap, A.cs->s.path, rel);
        return true;
    }
    return false;
}

static void tool_row(float x, float y, float w)
{
    float tx = x + w;
    struct { const char *id, *icon, *label; } tools[] = { { "p-term", ICON_TERMINAL, "Terminal" }, { "p-folder", ICON_FOLDER_OPEN, "Open folder" } };
    for (int i = 0; i < 2; i++) {
        float tw = ui_button_width(tools[i].icon, tools[i].label);
        tx -= tw;
        if (ui_button(ui_id(tools[i].id), (Rectangle){ tx, y, tw, 32 }, tools[i].icon, tools[i].label, BTN_GHOST)) {
            if (i == 0) launch_terminal(&A.env, &A.set, A.cs->s.path); else launch_folder(&A.env, A.cs->s.path);
        }
        tx -= 6;
    }
    /* the program's own GUI, to build or change what the form here doesn't reach (meshes, physics models) */
    {
        const char *lab = NULL; char cmd[PL_PATH * 2] = "", qd[PL_PATH + 8], qi[300];
        pl_shell_quote(qd, sizeof qd, A.cs->s.path); pl_shell_quote(qi, sizeof qi, input());
        char exe[PL_PATH];
        if (prog() == PROG_SATURNE && pl_which("code_saturne", exe, sizeof exe)) { lab = "code_saturne GUI"; snprintf(cmd, sizeof cmd, "cd %s/DATA && code_saturne gui setup.xml", qd); }
        else if (prog() == PROG_CALCULIX && pl_which("cgx", exe, sizeof exe) && input()[0]) { lab = "Open in cgx"; snprintf(cmd, sizeof cmd, "cd %s && cgx -c %s", qd, qi); }
        else if (prog() == PROG_ASTER && pl_which("salome_meca", exe, sizeof exe)) { lab = "salome_meca"; snprintf(cmd, sizeof cmd, "cd %s && salome_meca", qd); }
        else if (prog() == PROG_ELMER && pl_which("ElmerGUI", exe, sizeof exe)) { lab = "ElmerGUI"; snprintf(cmd, sizeof cmd, "cd %s && ElmerGUI", qd); }
        if (lab) {
            float tw = ui_button_width(ICON_MONITOR, lab);
            tx -= tw;
            if (ui_button(ui_id("p-gui"), (Rectangle){ tx, y, tw, 32 }, ICON_MONITOR, lab, BTN_GHOST)) { pl_spawn_detached(cmd); ui_toast(TOAST_INFO, "Opening %s", lab); }
            tx -= 6;
        }
    }
    if (prog() == PROG_DEME || prog() == PROG_CHRONO) {
        char drv[PL_PATH]; bool has = deme_driver_source(A.cs->s.path, input(), drv, sizeof drv);
        float tw = ui_button_width(ICON_FILE_CODE, "Edit driver");
        tx -= tw;
        ui_disable(!has);
        if (ui_button(ui_id("p-edit"), (Rectangle){ tx, y, tw, 32 }, ICON_FILE_CODE, "Edit driver", BTN_GHOST)) {
            launch_editor(&A.env, &A.set, drv);
            ui_toast(TOAST_INFO, "Opening %s. The next run rebuilds it after you save.", pl_basename(drv));
        }
        ui_disable(false);
    }
}

/* ============================================================================ Overview */
void page_prog_overview(Rectangle r)
{
    if (!A.cs) return;
    sync();
    Case *c = A.cs;
    UiScroll *sc = &A.scr[2];
    float top = ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = fminf(r.width - 2 * PAGE_PAD, CONTENT_MAX + 120), y = top + 30;
    char sub[PL_PATH + 200];
    pl_fmt_ui(sub, sizeof sub, "%s project  ·  %s", info()->name, info()->what);
    y += page_header(x, y, w, c->s.name, sub);
    ui_text_clip(F_MONO, 12, c->s.path, x, y + 9, w - 300, TH.text3);
    tool_row(x, y, w);
    y += 48;

    /* tiles */
    Color sc2; const char *sic; const char *st = status_text(&sc2, &sic);
    char exe[PL_PATH], why[300]; bool have = prog_exe(prog(), exe, sizeof exe, why, sizeof why);
    float gap = 14, tw = (w - 3 * gap) / 4;
    char v1[64], v2[64], v3[64], s3[96], s4[96];
    snprintf(v1, sizeof v1, "%d", P.inputs.n);
    snprintf(v2, sizeof v2, "%d", P.nres);
    char ago[48] = "never"; LogMon *m = run_log();
    if (m && m->mtime) pl_time_ago(ago, sizeof ago, m->mtime);
    snprintf(v3, sizeof v3, "%s", ago);
    snprintf(s3, sizeof s3, "%s", input()[0] ? input() : "nothing to run yet");
    snprintf(s4, sizeof s4, "%s", have ? pl_basename(exe) : "not installed");
    stat_tile((Rectangle){ x, y, tw, 96 }, ICON_CPU, info()->name, have ? "Installed" : "Missing", s4);
    stat_tile((Rectangle){ x + (tw + gap), y, tw, 96 }, ICON_ACTIVITY, "Status", st, v3);
    stat_tile((Rectangle){ x + 2 * (tw + gap), y, tw, 96 }, ICON_FILE_TEXT, info()->inputWord, v1, s3);
    stat_tile((Rectangle){ x + 3 * (tw + gap), y, tw, 96 }, ICON_BOXES, "Results", v2, P.nres ? P.res[0].rel : "none yet");
    y += 116;

    /* what to do next */
    float bx = x;
    {
        bool run = running();
        const char *lab = run ? "Watch the run" : "Run";
        float bw = ui_button_width(run ? ICON_ACTIVITY : ICON_PLAY, lab) + 20;
        if (ui_button(ui_id("po-run"), (Rectangle){ bx, y, bw, 40 }, run ? ICON_ACTIVITY : ICON_PLAY, lab, BTN_PRIMARY)) app_go(PG_RUN);
        bx += bw + 10;
        bw = ui_button_width(ICON_SLIDERS_HORIZONTAL, "Setup") + 20;
        if (ui_button(ui_id("po-setup"), (Rectangle){ bx, y, bw, 40 }, ICON_SLIDERS_HORIZONTAL, "Setup", BTN_SECONDARY)) app_go(PG_SETUP);
        bx += bw + 10;
        bw = ui_button_width(ICON_BOXES, "Results") + 20;
        ui_disable(!P.nres);
        if (ui_button(ui_id("po-results"), (Rectangle){ bx, y, bw, 40 }, ICON_BOXES, "Results", BTN_SECONDARY)) app_go(PG_RESULTS);
        ui_disable(false);
    }
    y += 62;

    /* the program's pre-run check */
    char head[96]; pl_fmt_ui(head, sizeof head, "%s", term(T_SWEEP));
    y += section_head(x, y, w, head, "What would stop a run, checked from the files: the program, the files the input names, the build.");
    y += checks_list(x, y, w, true) + 14;

    /* a look at the particles, for DEME and SPARTA */
    char pd[PL_PATH], pre[128];
    if (particles_result(pd, sizeof pd, pre, sizeof pre)) {
        y += section_head(x, y, w, "Particles", "The newest frame. Results has the full viewer.");
        y += deme_particles_panel(pd, pre, x, y, w, 360, running(), true) + 12;
    }

    /* inputs */
    char ih[96]; pl_fmt_ui(ih, sizeof ih, "%s%s", ui_cap_words(info()->inputWord), P.inputs.n == 1 ? "" : "s");
    y += section_head(x, y, w, ih, prog() == PROG_DEME || prog() == PROG_CHRONO ? "The programs its CMakeLists.txt builds." : "Click one to edit it on the Files page.");
    for (int i = 0; i < P.inputs.n; i++) {
        Rectangle row = { x, y, w, 34 };
        if (ui_row(ui_idn(ui_id("po-input"), i), row, i == P.sel)) {
            P.sel = i;
            if (prog() != PROG_DEME && prog() != PROG_CHRONO) { pl_strlcpy(A.filesSel, P.inputs.names[i], sizeof A.filesSel); app_go(PG_FILES); }
        }
        ui_register(ui_idn(ui_id("po-input"), i), row, P.inputs.names[i]);
        ui_icon(ICON_FILE_TEXT, row.x + 18, row.y + 17, 15, TH.text3);
        ui_text_clip(F_MONO_MED, 13, P.inputs.names[i], row.x + 36, row.y + 9, w - 50, TH.text);
        y += 36;
    }
    if (!P.inputs.n) { ui_text(F_REG, 13, "None found in this folder.", x, y, TH.text3); y += 24; }
    ui_scroll_end(sc, y - top + 40);
}

/* ============================================================================ Setup */
static void setup_files_for(const char *in)
{
    names_free(&P.setupFiles);
    const char *dir = A.cs->s.path;
    if (prog() == PROG_DEME) return;
    if (prog() == PROG_CHRONO) { char p[PL_PATH]; pl_join(p, sizeof p, dir, "params.ini"); if (pl_is_file(p)) names_add(&P.setupFiles, "params.ini"); return; }
    if (prog() == PROG_SATURNE) {
        names_add(&P.setupFiles, "DATA/setup.xml");
        char p[PL_PATH]; pl_join(p, sizeof p, dir, "DATA/run.cfg"); if (pl_is_file(p)) names_add(&P.setupFiles, "DATA/run.cfg");
        return;
    }
    if (prog() == PROG_ASTER && pl_ends_with(in, ".export")) {
        /* the .comm it runs first (where the physics is), then the job's own settings */
        char p[PL_PATH]; pl_join(p, sizeof p, dir, in);
        char *t = pl_read_head(p, 16384);
        for (char *l = t; l && *l; ) {
            char a[8], k[16], path[PL_PATH];
            if (sscanf(l, "%7s %15s %1023s", a, k, path) == 3 && !strcmp(a, "F") && !strcmp(k, "comm")) {
                const char *b = pl_basename(path);
                char here[PL_PATH]; pl_join(here, sizeof here, dir, b);
                if (pl_is_file(here)) names_add(&P.setupFiles, b);
            }
            l = strchr(l, '\n'); if (l) l++;
        }
        free(t);
    }
    if (in[0]) names_add(&P.setupFiles, in);
}

static const char *setup_file(void) { return P.setupSel >= 0 && P.setupSel < P.setupFiles.n ? P.setupFiles.names[P.setupSel] : ""; }

static void load_setup(void)
{
    char p[PL_PATH]; pl_join(p, sizeof p, A.cs->s.path, setup_file());
    if (!setup_file()[0] || !params_load(&P.pf, prog(), p)) params_free(&P.pf);
    edits_from_params();
    pl_strlcpy(P.setupFor, setup_file(), sizeof P.setupFor);
}

bool prog_setup_dirty(void) { return A.cs && A.cs->s.prog && P.pf.dirty && !strcmp(P.forPath, A.cs->s.path); }

bool prog_setup_pending(const char **name, const char **before, const char **after)
{
    if (!prog_setup_dirty()) return false;
    *name = setup_file(); *before = P.pf.orig ? P.pf.orig : ""; *after = P.pf.text ? P.pf.text : "";
    return true;
}

void prog_setup_revert(void) { if (prog_setup_dirty()) load_setup(); }

bool prog_setup_save(void)
{
    if (!P.pf.dirty) return true;
    if (!params_save(&P.pf)) { ui_toast(TOAST_ERR, "Could not write %s", P.pf.path); return false; }
    edits_from_params();
    char msg[300]; pl_fmt_ui(msg, sizeof msg, "Setup: %s", pl_basename(P.pf.path));
    if (A.set.history) { hist_commit(A.cs->s.path, msg); history_invalidate(); }
    P.ckAt = -100;
    return true;
}

static bool shown(const Param *p)
{
    if (P.mode == 0 && p->importance < 2) return false;
    if (P.search[0] && !pl_contains_ci(p->key, P.search) && !pl_contains_ci(p->group, P.search) && !pl_contains_ci(p->value, P.search)) return false;
    return true;
}

void page_prog_setup(Rectangle r)
{
    if (!A.cs) return;
    sync();
    Case *c = A.cs;
    if (strcmp(P.setupFor, "") == 0 || P.setupFiles.n == 0 || strcmp(P.ckFor, input())) {}
    static char setupInput[160];
    if (strcmp(setupInput, input()) || (P.setupFiles.n == 0 && prog() != PROG_DEME)) {
        pl_strlcpy(setupInput, input(), sizeof setupInput);
        setup_files_for(input());
        P.setupSel = 0; P.setupFor[0] = 0;
    }
    if (strcmp(P.setupFor, setup_file()) || !P.pf.path[0]) load_setup();
    if (GetTime() - P.pfCheckAt > 1) {                          /* changed outside (an editor, the Files page): take it in */
        P.pfCheckAt = GetTime();
        if (P.pf.path[0] && !P.pf.dirty && params_changed_on_disk(&P.pf)) load_setup();
    }
    UiScroll *sc = &A.scr[4];
    float top = ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = fminf(r.width - 2 * PAGE_PAD, CONTENT_MAX + 120), y = top + 30;
    char sub[400];
    if (prog() == PROG_DEME) pl_fmt_ui(sub, sizeof sub, "The driver reads params.ini when it starts: a change takes effect on the next run, with no rebuild.");
    else pl_fmt_ui(sub, sizeof sub, "The values in %s's input, read from the file and written back in place: every other line, comment and "
                   "space stays as it was.", info()->name);
    y += page_header(x, y, w, "Setup", sub);
    if (P.inputs.n > 1) {                                       /* which input */
        const char *items[32]; int n = 0;
        for (int i = 0; i < P.inputs.n && n < 32; i++) items[n++] = P.inputs.names[i];
        ui_text(F_MED, 13, ui_cap_words(info()->inputWord), x, y + 9, TH.text2);
        ui_select(ui_id("ps-input"), (Rectangle){ x + 120, y, 320, 34 }, items, n, &P.sel);
        y += 48;
    }
    if (prog() == PROG_DEME) {
        y += deme_setup_panel(c->s.path, x, y, w);
        ui_scroll_end(sc, y - top + 40);
        return;
    }
    if (!P.setupFiles.n) {
        ui_text_wrap(F_REG, 13, prog() == PROG_CHRONO ? "This Chrono project keeps its settings in its C++ source: edit the driver (Overview or Run), "
                     "or add a params.ini of key = value lines and read it in the driver." : "Nothing to set up: the project has no input file.",
                     (Rectangle){ x, y, w, 60 }, 19, TH.text3, true);
        ui_scroll_end(sc, y - top + 80);
        return;
    }
    /* file pills */
    float lx = x;
    for (int i = 0; i < P.setupFiles.n; i++) {
        const char *n = P.setupFiles.names[i];
        float pw = ui_text_w(F_MED, 13, n) + 30;
        Rectangle pr = { lx, y, pw, 30 };
        bool on = i == P.setupSel, hov = ui_hovered(pr);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        ui_rect(pr, 15, on ? TH.accent : hov ? TH.hover : TH.surface);
        if (!on) ui_rect_lines(pr, 15, 1, TH.lineSoft);
        ui_text(F_MED, 13, n, lx + 15, y + 7, on ? TH.textOnAccent : TH.text2);
        ui_register(ui_idn(ui_id("ps-file"), i), pr, n);
        if (ui_clicked(pr) && i != P.setupSel) {
            if (P.pf.dirty && !prog_setup_save()) break;
            P.setupSel = i;
        }
        lx += pw + 8;
    }
    /* Save / Revert on the right */
    float rx = x + w;
    float sw = ui_button_width(ICON_SAVE, term(T_REVIEW_GO));
    rx -= sw;
    ui_disable(!P.pf.dirty);
    if (ui_button(ui_id("ps-save"), (Rectangle){ rx, y - 3, sw, 36 }, ICON_SAVE, term(T_REVIEW_GO), BTN_PRIMARY)) {
        if (app_save_request()) ui_toast(TOAST_OK, "Saved %s", setup_file());
    }
    float rw = ui_button_width(ICON_ROTATE_CCW, "Revert");
    rx -= rw + 8;
    if (ui_button(ui_id("ps-revert"), (Rectangle){ rx, y - 3, rw, 36 }, ICON_ROTATE_CCW, "Revert", BTN_GHOST)) load_setup();
    ui_disable(false);
    if (P.pf.dirty) { float bw = ui_badge_width(ICON_PENCIL, "Edited, not saved"); ui_badge(rx - bw - 12, y + 4, ICON_PENCIL, "Edited, not saved", TH.accent, TH.accentSoft); }
    y += 48;
    /* Basic / All, search */
    static const char *modes[] = { "Basic", "All values" };
    ui_segmented(ui_id("ps-mode"), (Rectangle){ x, y, 220, 34 }, modes, 2, &P.mode);
    ui_input(ui_id("ps-search"), (Rectangle){ x + 232, y, fminf(320, w - 232), 34 }, P.search, sizeof P.search, "Find a value", INPUT_LIVE | INPUT_SEARCH);
    y += 50;
    if (!P.pf.path[0]) { ui_text(F_REG, 13, "The file could not be read.", x, y, TH.err); ui_scroll_end(sc, y - top + 60); return; }
    int nShown = 0;
    for (int i = 0; i < P.pf.n; i++) if (shown(&P.pf.p[i])) nShown++;
    if (!nShown) {
        ui_text_wrap(F_REG, 13, P.mode == 0 ? "No well-known values in this file: All values lists everything Q-Continuum could read in it."
                                            : "Nothing matches.", (Rectangle){ x, y, w, 40 }, 19, TH.text3, true);
        y += 40;
    }
    /* rows, grouped as in the file */
    const char *grp = NULL;
    float lab = fminf(300, w * 0.34f), vw = w - lab - 150;
    for (int i = 0; i < P.pf.n && i < MAX_EDIT; i++) {
        Param *p = &P.pf.p[i];
        if (!shown(p)) continue;
        if (!grp || strcmp(grp, p->group)) {
            grp = p->group;
            y += 8;
            ui_text_clip(F_SEMI, 14, grp[0] ? grp : "(top)", x, y, w, head_color());
            y += 28;
        }
        Rectangle row = { x, y, w, 40 };
        ui_text_clip(F_MED, 13, p->key, x + 8, y + 10, lab - 16, TH.text);
        if (p->hint) ui_tooltip((Rectangle){ x, y, lab, 40 }, p->hint);
        UiId id = ui_idn(ui_id("ps-val"), i);
        char reg[200]; pl_fmt_ui(reg, sizeof reg, "%s/%s", p->group, p->key);
        if (p->editable) {
            if (ui_input(id, (Rectangle){ x + lab, y + 2, vw, 34 }, P.edit[i], sizeof P.edit[i], p->value, INPUT_MONO | (p->numeric ? INPUT_NUMERIC : 0))) {
                char v[256]; pl_strlcpy(v, P.edit[i], sizeof v); pl_trim_inplace(v);
                if (v[0] && params_set(&P.pf, i, v)) edits_from_params();
                else pl_strlcpy(P.edit[i], p->value, sizeof P.edit[i]);
            }
        } else ui_text_clip(F_MONO, 12.5f, p->value, x + lab + 8, y + 11, vw - 16, TH.text3);
        ui_register(id, (Rectangle){ x + lab, y + 2, vw, 34 }, reg);
        if (p->unit) ui_text_clip(F_REG, 12.5f, p->unit, x + lab + vw + 10, y + 11, 90, TH.text3);
        char ln[24]; snprintf(ln, sizeof ln, "line %d", p->line);
        Rectangle lr = { x + w - 52, y + 8, 52, 24 };
        bool lh = ui_hovered(lr);
        if (lh) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        ui_text(F_REG, 12, ln, lr.x, lr.y + 3, lh ? TH.accent : TH.text3);
        if (ui_clicked(lr)) {
            if (P.pf.dirty) prog_setup_save();
            pl_strlcpy(A.filesSel, setup_file(), sizeof A.filesSel);
            A.filesRaw = true;
            text_editor_goto(p->line);
            app_go(PG_FILES);
        }
        (void)row;
        y += 42;
    }
    ui_scroll_end(sc, y - top + 60);
}

/* ============================================================================ Run */
static void kpi(Rectangle r, const char *label, const char *value, Color vc)
{
    ui_text(F_REG, 12, ui_cap(label), r.x, r.y, TH.text3);
    ui_text_clip(F_SEMI, 18, value, r.x, r.y + 18, r.width - 8, vc);
}

void page_prog_run(Rectangle r)
{
    if (!A.cs) return;
    sync();
    if (A.runPending && A.modal == MD_NONE) { A.runPending = false; start_here(); }   /* the header's Run */
    Case *c = A.cs;
    if (!c) return;
    bool mine = running();
    LogMon *pm = run_log();
    if (A.chartMax || A.logMax) {                               /* the chart or the log takes the whole page */
        float x = r.x + 32, y = r.y + 22, w = r.width - 64;
        char t[300]; snprintf(t, sizeof t, "%s  ·  %s", c->s.name, A.chartMax ? "progress" : "log");
        ui_text(F_SEMI, 16, t, x, y, TH.text);
        if (mine && ui_button(ui_id("max-stop"), (Rectangle){ x + w - 110, y - 6, 110, 32 }, ICON_SQUARE, "Stop", BTN_DANGER)) job_stop(A.cj, GetTime());
        Rectangle body = { x, y + 38, w, r.y + r.height - y - 38 - 24 };
        if (A.chartMax) { run_chart(body, pm); if (!pm || !pm->nSeries) chart_max_button(x + w, y + 44); }
        else run_console(body, &A.logs, pm ? (int)(pm - A.logs.logs) : logset_newest(&A.logs));
        return;
    }
    float lw = 400;
    Rectangle left = { r.x, r.y, lw, r.height };
    DrawRectangleRec((Rectangle){ r.x + lw - 1, r.y, 1, r.height }, TH.lineSoft);
    UiScroll *sc = &A.scr[13];
    float top = ui_scroll_begin(sc, left);
    float x = left.x + 28, w = lw - 56, y = top + 28;
    ui_text(F_SEMI, TH.lcars ? 26 : 20, ui_cap("Run"), x, y - (TH.lcars ? 3 : 0), head_color()); y += 40;
    ui_disable(mine);
    if (P.inputs.n) {
        const char *items[32] = { 0 }; int n = 0;
        for (int i = 0; i < P.inputs.n && n < 32; i++) items[n++] = P.inputs.names[i];
        ui_text(F_MED, 13, ui_cap_words(info()->inputWord), x, y, TH.text2); y += 22;
        ui_select(ui_id("pr-input"), (Rectangle){ x, y, w, 36 }, items, n, &P.sel);
        y += 50;
    }
    if (info()->mpi || info()->threads) {
        ui_text(F_MED, 13.5f, info()->threads ? "Threads" : "Processes", x, y + 8, TH.text);
        char np[16]; snprintf(np, sizeof np, "%d", P.procs);
        Rectangle pr = { x + w - 132, y, 132, 34 };
        if (ui_button(ui_id("np-dec"), (Rectangle){ pr.x, pr.y, 34, 34 }, ICON_CHEVRON_LEFT, NULL, BTN_SECONDARY) && P.procs > 1) P.procs--;
        if (ui_input(ui_id("np-in"), (Rectangle){ pr.x + 40, pr.y, 52, 34 }, np, sizeof np, NULL, INPUT_NUMERIC)) { int v = atoi(np); if (v >= 1 && v <= 1024) P.procs = v; }
        if (ui_button(ui_id("np-inc"), (Rectangle){ pr.x + 98, pr.y, 34, 34 }, ICON_CHEVRON_RIGHT, NULL, BTN_SECONDARY) && P.procs < 1024) P.procs++;
        y += 40;
        char pinfo[160];
        if (info()->threads) pl_fmt_ui(pinfo, sizeof pinfo, "OpenMP threads (OMP_NUM_THREADS), %d cores here", A.env.nCpu);
        else if (P.procs > 1) pl_fmt_ui(pinfo, sizeof pinfo, "Parallel with MPI on %d of %d cores", P.procs, A.env.nCpu);
        else pl_fmt_ui(pinfo, sizeof pinfo, "Serial run (%d cores available)", A.env.nCpu);
        ui_text(F_REG, 12.5f, pinfo, x, y, TH.text3);
        y += 30;
    }
    if (info()->build) {
        ui_checkbox(ui_id("pr-rebuild"), (Rectangle){ x, y, w, 24 }, &P.rebuild, "Rebuild before running");
        ui_tooltip((Rectangle){ x, y, w, 24 }, "It rebuilds by itself when the source is newer than the program");
        y += 36;
    }
    ui_disable(false);
    ui_divider(x, y, w); y += 20;
    ui_text(F_MED, 13.5f, term(T_SWEEP), x, y, TH.text); y += 28;
    y += checks_list(x, y, w, true) + 6;
    ui_divider(x, y, w); y += 20;
    if (mine) {
        if (ui_button(ui_id("run-stop"), (Rectangle){ x, y, w, 42 }, ICON_SQUARE, "Stop run", BTN_DANGER)) job_stop(A.cj, GetTime());
    } else {
        ui_disable(A.cj->active || !input()[0]);
        char lab[96]; pl_fmt_ui(lab, sizeof lab, "%s %s", term(T_RUN_GO), input());
        if (ui_button(ui_id("run-go"), (Rectangle){ x, y, w, 42 }, ICON_PLAY, lab, BTN_PRIMARY)) start_here();
        ui_disable(false);
    }
    y += 54;
    if (!mine) {
        bool queued = app_queue_has(c->s.path);
        ui_disable(queued);
        if (ui_button(ui_id("run-queue"), (Rectangle){ x, y - 4, w, 34 }, ICON_LIST, queued ? "In the queue" : "Add to queue", BTN_GHOST))
            app_queue_add(c->s.path, -1, P.procs, false, false);
        ui_disable(false);
        ui_tooltip((Rectangle){ x, y - 4, w, 34 }, "Run it when cores are free, after the runs queued before it (Queue page)");
        y += 42;
    }
    float hb = (w - 8) / 2;
    ui_disable(!P.nres);
    if (ui_button(ui_id("pr-results"), (Rectangle){ x, y, hb, 34 }, ICON_BOXES, "Results", BTN_SECONDARY)) app_go(PG_RESULTS);
    ui_disable(mine);
    if (ui_button(ui_id("run-clean"), (Rectangle){ x + hb + 8, y, hb, 34 }, ICON_TRASH_2, "Clean results", BTN_GHOST)) { A.modal = MD_PROG_CLEAN; }
    ui_disable(false);
    y += 46;
    if (prog() == PROG_DEME) { y += 10; y += deme_pair_panel(x, y, w, mine, start_both); }
    ui_scroll_end(sc, y - top + 20);

    /* ---------------- right: progress */
    Rectangle mon = { r.x + lw, r.y, r.width - lw, r.height };
    float mx = mon.x + 32, mw = mon.width - 64, my = mon.y + 26;
    Color stC; const char *stIc; const char *stTxt = status_text(&stC, &stIc);
    if (stIc) ui_icon(stIc, mx + 10, my + 13, 20, stC); else ui_spinner(mx + 10, my + 13, 9, stC);
    ui_text(F_SEMI, TH.lcars ? 24 : 20, ui_cap(stTxt), mx + 30, my - (TH.lcars ? 2 : 0), TH.lcars ? stC : TH.text);
    float sx = mx + 40 + ui_text_w(F_SEMI, TH.lcars ? 24 : 20, ui_cap(stTxt));
    if (mine && A.cj->cur < A.cj->n) {
        char t[200]; pl_fmt_ui(t, sizeof t, "Step %d of %d: %s", A.cj->cur + 1, A.cj->n, A.cj->steps[A.cj->cur].label);
        ui_text(F_REG, 13.5f, t, sx, my + 5, TH.text2);
    } else if (pm) { char t[160]; pl_fmt_ui(t, sizeof t, "Showing %s", pm->name); ui_text(F_REG, 13.5f, t, sx, my + 5, TH.text3); }
    my += 44;
    /* progress bar */
    bool isSteps; double inEnd = prog_end(prog(), c->s.path, input(), &isSteps);
    double done = 0, total = 0; bool steps = false;
    bool known = pm && prog_progress(prog(), pm, inEnd, isSteps, &done, &total, &steps);
    bool failed = pm && pm->fatal;
    Color barC = mine ? TH.accent : failed ? TH.err : TH.ok;
    ui_progress((Rectangle){ mx, my, mw, 10 }, known && total > 0 ? (float)(done / total) : (pm && (pm->ended || pm->exitCode == 0) ? 1.f : 0.f), barC);
    my += 20;
    char t[300];
    if (known) pl_fmt_ui(t, sizeof t, steps ? "step %.0f of %.0f" : "t = %.4g of %.4g s", done, total);
    else if (pm && pm->steps) pl_fmt_ui(t, sizeof t, "%d step%s, t = %.4g", pm->steps, pm->steps == 1 ? "" : "s", pm->time);
    else pl_fmt_ui(t, sizeof t, mine ? "Starting..." : "No run yet");
    ui_text(F_MED, 13, t, mx, my, TH.text2);
    my += 30;
    /* kpis */
    float kw = mw / 5;
    char v[64];
    snprintf(v, sizeof v, "%d", pm ? pm->steps : 0);
    kpi((Rectangle){ mx, my, kw, 50 }, steps ? "Steps" : "Steps / increments", v, TH.text);
    snprintf(v, sizeof v, pm && pm->steps ? "%.4g" : "-", pm ? pm->time : 0);
    kpi((Rectangle){ mx + kw, my, kw, 50 }, prog() == PROG_SPARTA ? "Step number" : "Simulated time", v, TH.text);
    double el = mine ? GetTime() - A.cj->startedAt : (A.cj->n && !strcmp(A.cj->casePath, c->s.path) ? A.cj->finishedAt - A.cj->startedAt : -1);
    pl_duration(v, sizeof v, el);
    kpi((Rectangle){ mx + 2 * kw, my, kw, 50 }, "Wall time", el > 0 ? v : "-", TH.text);
    double eta = -1;
    if (mine && known && el > 5 && done > 0 && total > done) eta = el * (total - done) / done;
    pl_duration(v, sizeof v, eta);
    kpi((Rectangle){ mx + 3 * kw, my, kw, 50 }, "Remaining", eta >= 0 ? v : "-", TH.text);
    snprintf(v, sizeof v, "%d", pm ? pm->warnings : 0);
    kpi((Rectangle){ mx + 4 * kw, my, kw, 50 }, "Warnings", v, pm && pm->warnings ? TH.warn : TH.text);
    my += 62;
    /* what went wrong, in words */
    if (pm && pm->fatal && !mine && pm->fatalMsg[0]) {
        char title[96]; pl_fmt_ui(title, sizeof title, "%s stopped with an error", info()->name);
        float ch = callout_height(mw, title, pm->fatalMsg);
        callout((Rectangle){ mx, my, mw, ch }, TOAST_ERR, ICON_CIRCLE_X, title, pm->fatalMsg);
        float bw = ui_button_width(ICON_SPARKLES, term(T_ASK));
        if (A.env.claude[0] && ui_button(ui_id("pr-claude"), (Rectangle){ mx + mw - bw - 12, my + 10, bw, 30 }, ICON_SPARKLES, term(T_ASK), BTN_SECONDARY))
            app_claude_error(pm->name, pm->fatalMsg);
        my += ch + 12;
    }
    if (prog() == PROG_DEME) {
        char lp[PL_PATH]; char ln[96]; prog_log_name(prog(), input(), ln, sizeof ln); pl_join(lp, sizeof lp, c->s.path, ln);
        my += deme_run_panel(c->s.path, lp, mx, my, mw, mine);
    }
    if (prog() == PROG_DEME || prog() == PROG_CHRONO) {         /* a failed build: its first error */
        if (A.cj->failed && !strcmp(A.cj->casePath, c->s.path) && A.cj->cur < A.cj->n && !strcmp(A.cj->steps[A.cj->cur].id, "build")) {
            char e[300]; deme_build_error(c->s.path, e, sizeof e);
            if (e[0]) { ui_icon(ICON_CIRCLE_X, mx + 8, my + 8, 14, TH.err); ui_text_clip(F_MONO, 12, e, mx + 22, my + 1, mw - 30, TH.err); my += 28; }
        }
    }
    /* chart and log share the rest */
    float rest = mon.y + mon.height - my - 20;
    float chH = fmaxf(180, rest * 0.45f);
    run_chart((Rectangle){ mx, my, mw, chH }, pm);
    my += chH + 14;
    run_console((Rectangle){ mx, my, mw, mon.y + mon.height - my - 20 }, &A.logs, pm ? (int)(pm - A.logs.logs) : logset_newest(&A.logs));
    if (mine) ui_request_frames(1);
}

/* ============================================================================ Results */
static void table_load(const char *path)
{
    long long st = params_stamp(path);
    if (!strcmp(P.tblPath, path) && st == P.tblStamp) return;
    pl_strlcpy(P.tblPath, path, sizeof P.tblPath); P.tblStamp = st;
    free(P.tbl); P.tbl = NULL; P.tblRows = 0; P.nCols = 0;
    char *t = pl_read_file(path, NULL);
    if (!t) return;
    char *nl = strchr(t, '\n');
    if (!nl) { free(t); return; }
    *nl = 0;
    for (char *c = strtok(t, ",\r"); c && P.nCols < 16; c = strtok(NULL, ",\r")) { pl_strlcpy(P.tblCols[P.nCols], c, 48); pl_trim_inplace(P.tblCols[P.nCols]); P.nCols++; }
    int cap = 256;
    P.tbl = malloc(sizeof(float) * (size_t)cap * 16);
    for (char *s = nl + 1; *s && P.tbl; ) {
        char *e = strchr(s, '\n'); if (e) *e = 0;
        float row[16] = { 0 }; int k = 0; char *q = s;
        for (; k < P.nCols; k++) { char *end; row[k] = strtof(q, &end); if (end == q) break; q = end; while (*q == ',' || *q == ' ') q++; }
        if (k == P.nCols) {
            if (P.tblRows == cap) { cap *= 2; float *nt = realloc(P.tbl, sizeof(float) * (size_t)cap * 16); if (!nt) break; P.tbl = nt; }
            memcpy(P.tbl + (size_t)P.tblRows * 16, row, sizeof row);
            P.tblRows++;
        }
        if (!e) break;
        s = e + 1;
    }
    free(t);
    P.tblX = 0;
    for (int i = 0; i < 16; i++) P.tblOn[i] = i > 0 && i < 4;
}

static void table_chart(Rectangle r)
{
    ui_rect(r, 10, TH.chartBg);
    ui_rect_lines(r, 10, 1, TH.lineSoft);
    if (P.tblRows < 2 || P.nCols < 2) { ui_text(F_REG, 13, "Not enough rows of numbers to draw.", r.x + 16, r.y + 16, TH.text3); return; }
    float lo = 1e30f, hi = -1e30f;
    float x0 = P.tbl[P.tblX], x1 = P.tbl[(size_t)(P.tblRows - 1) * 16 + P.tblX];
    for (int k = 0; k < P.nCols; k++) if (P.tblOn[k] && k != P.tblX)
        for (int i = 0; i < P.tblRows; i++) { float v = P.tbl[(size_t)i * 16 + k]; if (v < lo) lo = v; if (v > hi) hi = v; }
    for (int i = 0; i < P.tblRows; i++) { float v = P.tbl[(size_t)i * 16 + P.tblX]; if (v < x0) x0 = v; if (v > x1) x1 = v; }
    if (lo > hi) return;
    if (hi - lo < 1e-12f) { hi += 1; lo -= 1; }
    if (x1 - x0 < 1e-12f) x1 = x0 + 1;
    Rectangle in = { r.x + 64, r.y + 16, r.width - 84, r.height - 44 };
    for (int g = 0; g <= 4; g++) {
        float yy = in.y + in.height * g / 4;
        ui_line(in.x, yy, in.x + in.width, yy, 1, TH.grid);
        char b[24]; snprintf(b, sizeof b, "%.4g", hi - (hi - lo) * g / 4);
        ui_text(F_MONO, 11, b, r.x + 6, yy - 6, TH.text3);
    }
    int ci = 0;
    for (int k = 0; k < P.nCols; k++) {
        if (!P.tblOn[k] || k == P.tblX) continue;
        Color col = TH.series[ci++ % 10];
        Vector2 prev = { 0 };
        for (int i = 0; i < P.tblRows; i++) {
            float xv = P.tbl[(size_t)i * 16 + P.tblX], yv = P.tbl[(size_t)i * 16 + k];
            Vector2 q = { in.x + in.width * (xv - x0) / (x1 - x0), in.y + in.height * (1 - (yv - lo) / (hi - lo)) };
            if (i) ui_line(prev.x, prev.y, q.x, q.y, 2, col);
            prev = q;
        }
    }
    char b[96]; snprintf(b, sizeof b, "%s  %.4g ... %.4g", P.tblCols[P.tblX], x0, x1);
    ui_text(F_MONO, 11, b, in.x, r.y + r.height - 20, TH.text3);
}

static void open_result(const ProgResult *res)
{
    char full[PL_PATH]; pl_join(full, sizeof full, A.cs->s.path, res->rel);
    char err[300] = "";
    switch (res->kind) {
    case RK_PARAVIEW:
        if (launch_paraview(&A.env, &A.set, full, NULL, err, sizeof err)) ui_toast(TOAST_OK, "Opening %s in ParaView", res->rel);
        else ui_toast(TOAST_ERR, "%s", err);
        break;
    case RK_CGX: {
        char exe[PL_PATH];
        if (!pl_which("cgx", exe, sizeof exe)) { ui_toast(TOAST_ERR, "cgx (CalculiX GraphiX) is not installed"); break; }
        char q[PL_PATH + 8], qd[PL_PATH + 8], cmd[PL_PATH * 3];
        pl_shell_quote(q, sizeof q, pl_basename(full)); pl_shell_quote(qd, sizeof qd, A.cs->s.path);
        snprintf(cmd, sizeof cmd, "cd %s && cgx -v %s", qd, q);
        pl_spawn_detached(cmd);
        ui_toast(TOAST_OK, "Opening %s in cgx", res->rel);
        break;
    }
    case RK_MED: {
        char exe[PL_PATH];
        if (pl_which("salome_meca", exe, sizeof exe)) {
            char q[PL_PATH + 8], cmd[PL_PATH * 2]; pl_shell_quote(q, sizeof q, full);
            snprintf(cmd, sizeof cmd, "salome_meca %s", q);
            pl_spawn_detached(cmd);
            ui_toast(TOAST_OK, "Opening %s in salome_meca (ParaVis reads MED)", res->rel);
        } else ui_toast(TOAST_ERR, "MED results open in SALOME (salome_meca), which was not found");
        break;
    }
    case RK_TEXT: case RK_TABLE:
        pl_strlcpy(A.filesSel, res->rel, sizeof A.filesSel); A.filesRaw = true; app_go(PG_FILES);
        break;
    case RK_IMAGE: case RK_FOLDER: default: {
        char d[PL_PATH]; pl_dirname(d, sizeof d, full);
        launch_folder(&A.env, res->kind == RK_FOLDER ? full : d);
        break;
    }
    }
}

void page_prog_results(Rectangle r)
{
    if (!A.cs) return;
    sync();
    Case *c = A.cs;
    UiScroll *sc = &A.scr[16];
    float top = ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = fminf(r.width - 2 * PAGE_PAD, CONTENT_MAX + 200), y = top + 30;
    float cbw = ui_button_width(ICON_TRASH_2, "Clean results");
    y += page_header(x, y, w - cbw - 20, "Results", "What the runs wrote, newest first: particles in the viewer here, fields in ParaView, CalculiX "
                     "results in cgx, tables as a chart.");
    ui_disable(running() || !P.nres);
    if (ui_button(ui_id("pres-clean"), (Rectangle){ x + w - cbw, top + 32, cbw, 36 }, ICON_TRASH_2, "Clean results", BTN_GHOST)) A.modal = MD_PROG_CLEAN;
    ui_disable(false);
    if (!P.nres) {
        ui_text(F_REG, 13.5f, "No results yet: run the project.", x, y, TH.text3);
        if (ui_button(ui_id("pres-run"), (Rectangle){ x, y + 30, 120, 36 }, ICON_PLAY, "Run", BTN_PRIMARY)) app_go(PG_RUN);
        ui_scroll_end(sc, y - top + 100);
        return;
    }
    /* the particle viewer first */
    char pd[PL_PATH], pre[128];
    if (particles_result(pd, sizeof pd, pre, sizeof pre)) {
        y += section_head(x, y, w, "Particles", pre[0] ? "SPARTA's particle dumps, drawn here: drag along the bar or press Play."
                                                       : "The frames the driver wrote, drawn here: drag along the bar or press Play. ParaView has the full 3-D view.");
        y += deme_particles_panel(pd, pre, x, y, w, 560, running(), false) + 14;
    }
    /* the files; a table opens as a chart when nothing else is chosen */
    if (P.resSel < 0) for (int i = 0; i < P.nres; i++) if (P.res[i].kind == RK_TABLE) { P.resSel = i; break; }
    y += section_head(x, y, w, "Files", NULL);
    for (int i = 0; i < P.nres; i++) {
        ProgResult *res = &P.res[i];
        Rectangle row = { x, y, w, 42 };
        bool sel = i == P.resSel;
        if (ui_row(ui_idn(ui_id("pres-row"), i), row, sel)) P.resSel = sel ? -1 : i;
        ui_register(ui_idn(ui_id("pres-row"), i), row, res->rel);
        const char *ic = res->kind == RK_PARTICLES ? ICON_BOXES : res->kind == RK_TABLE ? ICON_TABLE : res->kind == RK_TEXT ? ICON_FILE_TEXT :
                         res->kind == RK_IMAGE ? ICON_MONITOR : ICON_LAYERS;
        ui_icon(ic, row.x + 18, row.y + 21, 15, TH.text3);
        ui_text_clip(F_MONO_MED, 13, res->rel, row.x + 38, row.y + 12, w * 0.45f, TH.text);
        char meta[96], sz[32], ago[48];
        pl_human_size(sz, sizeof sz, res->size); pl_time_ago(ago, sizeof ago, res->mtime);
        if (res->count) pl_fmt_ui(meta, sizeof meta, "%s  ·  %d frame%s  ·  %s", prog_result_kind_name(res->kind), res->count, res->count == 1 ? "" : "s", ago);
        else pl_fmt_ui(meta, sizeof meta, "%s  ·  %s  ·  %s", prog_result_kind_name(res->kind), sz, ago);
        ui_text_clip(F_REG, 12.5f, meta, row.x + 38 + w * 0.45f + 10, row.y + 13, w * 0.55f - 190, TH.text3);
        const char *bl = res->kind == RK_PARAVIEW ? "ParaView" : res->kind == RK_CGX ? "cgx" : res->kind == RK_MED ? "SALOME" :
                         res->kind == RK_TABLE ? "Chart" : res->kind == RK_TEXT ? "Read" : res->kind == RK_PARTICLES ? "Viewer" : "Folder";
        float bw = ui_button_width(ICON_EXTERNAL_LINK, bl);
        if (ui_button(ui_idn(ui_id("pres-open"), i), (Rectangle){ x + w - bw - 8, y + 5, bw, 32 }, ICON_EXTERNAL_LINK, bl, BTN_SECONDARY)) {
            if (res->kind == RK_TABLE) P.resSel = i;
            else if (res->kind == RK_PARTICLES) sc->target = 0;
            else open_result(res);
        }
        y += 44;
        if (sel && res->kind == RK_TABLE) {                    /* a CSV: its columns as a chart */
            char full[PL_PATH]; pl_join(full, sizeof full, c->s.path, res->rel);
            table_load(full);
            float cx = x + 8;
            if (P.nCols > 1) {
                const char *cols[16]; for (int k = 0; k < P.nCols; k++) cols[k] = P.tblCols[k];
                ui_text(F_MED, 12.5f, "x axis", cx, y + 8, TH.text2);
                ui_select(ui_id("pres-x"), (Rectangle){ cx + 54, y, 180, 30 }, cols, P.nCols, &P.tblX);
                cx += 250;
                for (int k = 0; k < P.nCols; k++) {
                    if (k == P.tblX) continue;
                    float cw = ui_text_w(F_REG, 12.5f, P.tblCols[k]) + 36;
                    if (cx + cw > x + w) { cx = x + 8; y += 32; }
                    ui_checkbox(ui_idn(ui_id("pres-col"), k), (Rectangle){ cx, y + 5, cw, 20 }, &P.tblOn[k], P.tblCols[k]);
                    cx += cw + 6;
                }
                y += 40;
            }
            table_chart((Rectangle){ x, y, w, 300 });
            y += 314;
        }
    }
    ui_scroll_end(sc, y - top + 40);
}

/* ============================================================================ Clean */
void prog_clean_modal(bool *close)
{
    Rectangle b = ui_modal_begin("Clean results", "The run's output and logs go to the project's trash (.qc-trash): restore them from the "
                                 "History page. Inputs and the setup stay.", 640, 420, close);
    if (!A.cs) { *close = true; return; }
    float y = b.y;
    const char *rels[128]; int n = 0;
    static char buf[128][256];
    for (int i = 0; i < P.nres && n < 120; i++) {
        const char *rel = P.res[i].rel;
        if (P.res[i].kind == RK_PARTICLES) {                  /* a frames folder, or the dumps by name */
            if (strstr(rel, ".*")) {
                char d[PL_PATH], pre[128]; particles_result(d, sizeof d, pre, sizeof pre);
                NameList l = { 0 }; pl_list_dir(d, &l, false, true);
                for (int k = 0; k < l.n && n < 120; k++) if (pl_starts_with(l.names[k], pre) && l.names[k][strlen(pre)] == '.') {
                    const char *sub = d + strlen(A.cs->s.path); if (*sub == '/') sub++;
                    if (*sub) pl_fmt_ui(buf[n], 256, "%s/%s", sub, l.names[k]); else pl_strlcpy(buf[n], l.names[k], 256);
                    rels[n] = buf[n]; n++;
                }
                names_free(&l);
                continue;
            }
            if (!strcmp(rel, ".")) continue;
        }
        pl_strlcpy(buf[n], rel, 256); rels[n] = buf[n]; n++;
    }
    NameList all = { 0 }; pl_list_dir(A.cs->s.path, &all, false, true);
    for (int i = 0; i < all.n && n < 127; i++) if (pl_starts_with(all.names[i], "log.")) { pl_strlcpy(buf[n], all.names[i], 256); rels[n] = buf[n]; n++; }
    names_free(&all);
    if (prog() == PROG_SATURNE) { char p[PL_PATH]; pl_join(p, sizeof p, A.cs->s.path, "RESU"); if (pl_is_dir(p) && n < 128) { pl_strlcpy(buf[n], "RESU", 256); rels[n] = buf[n]; n++; } }
    char t[200]; pl_fmt_ui(t, sizeof t, "%d item%s:", n, n == 1 ? "" : "s");
    ui_text(F_MED, 13, t, b.x, y, TH.text); y += 26;
    for (int i = 0; i < n && y < b.y + b.height - 70; i++) { ui_text_clip(F_MONO, 12.5f, rels[i], b.x + 12, y, b.width - 12, TH.text2); y += 20; }
    if (n && y >= b.y + b.height - 70) ui_text(F_REG, 12.5f, "...", b.x + 12, y, TH.text3);
    if (ui_button(ui_id("pc-cancel"), (Rectangle){ b.x, b.y + b.height - 38, 100, 38 }, NULL, "Cancel", BTN_GHOST)) *close = true;
    ui_disable(!n || running());
    if (ui_button(ui_id("pc-go"), (Rectangle){ b.x + b.width - 150, b.y + b.height - 38, 150, 38 }, ICON_TRASH_2, "Move to trash", BTN_DANGER)) {
        int moved = trash_items(A.cs->s.path, "Clean", NULL, rels, n);
        if (moved > 0) ui_toast(TOAST_OK, "Moved %d item%s to %s/.qc-trash", moved, moved == 1 ? "" : "s", A.cs->s.name);
        else ui_toast(TOAST_ERR, "Nothing could be moved");
        P.resAt = -100; logset_open(&A.logs, A.cs->s.path); case_rescan(A.cs); history_invalidate();
        *close = true;
    }
    ui_disable(false);
}
