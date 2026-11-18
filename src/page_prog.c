/* page_prog.c -- the pages of a project of another program (DEME, Chrono, CalculiX, code_aster, code_saturne, MOOSE,
 * Elmer, SPARTA): the same workflow as an OpenFOAM case. Overview (status, checks, a look at the results), Setup (the
 * values in its input file, edited in place: params.c), Run (input, processes, checks, live progress, the chart of
 * its residual-like values and its log), Results (ParaView, cgx, the particle viewer, tables). Files and History are
 * the OpenFOAM pages, which work on any folder. What each program is and does is program.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "ccxmodel.h"
#include "gpu.h"
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
        if (prog() == PROG_CALCULIX && fea_is_study(A.cs->s.path)) for (int i = 0; i < P.inputs.n; i++) if (!strcmp(P.inputs.names[i], "study.inp")) P.sel = i;
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
    if (pg == PROG_CALCULIX && fea_is_study(path)) {            /* a part study: study.inp written from Setup first */
        if (!fea_prepare_run(path, err, sizeof err)) { ui_toast(TOAST_ERR, "Not started: %s", err); return false; }
        in = "study.inp";
    }
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
    y += app_starter_callout(c->s.path, x, y, w);              /* a starter: what it teaches, what to expect (C4) */

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


/* ============================================================================ CalculiX in engineering words (workflow plan C1)
 * Above the values: the unit system the numbers imply (and a warning when the model's size or density disagree), each
 * material with a library to pick from, each support as the directions it holds, each load as a force along a direction
 * with its total (a *CLOAD acts on every node of its set). Edits go into the input's own data lines through params.c. */
static int param_at(int line, const char *key)
{
    for (int i = 0; i < P.pf.n; i++) if (P.pf.p[i].line == line && !strcmp(P.pf.p[i].key, key)) return i;
    return -1;
}
static void put_param(int line, const char *key, const char *value)
{
    int i = param_at(line, key);
    if (i >= 0 && params_set(&P.pf, i, value)) edits_from_params();
}
static const char *fmt_si(double v, const char *unit)    /* 210000000000 Pa -> "210 GPa" */
{
    static char b[4][48]; static int k; char *o = b[k = (k + 1) % 4];
    double a = fabs(v);
    if (a >= 1e9) snprintf(o, 48, "%.4g G%s", v / 1e9, unit); else if (a >= 1e6) snprintf(o, 48, "%.4g M%s", v / 1e6, unit);
    else if (a >= 1e3) snprintf(o, 48, "%.4g k%s", v / 1e3, unit); else snprintf(o, 48, "%.4g %s", v, unit);
    return o;
}
static const char *const HOLD_NAME[] = { "x only", "y only", "z only", "x and y", "y and z", "x, y and z (fixed)" };
static const int HOLD_D0[] = { 1, 2, 3, 1, 2, 1 }, HOLD_D1[] = { 1, 2, 3, 2, 3, 3 };
static const char *const DIR_NAME[] = { "+x", "-x", "+y", "-y", "+z", "-z" };

static float ccx_model_view(Case *c, float x, float y, float w);
static float ccx_model_panel(Case *c, float x, float y, float w)
{
    float y0 = y;
    CcxModel m;
    ccx_model_load_text(c->s.path, setup_file(), P.pf.text, &m);
    char um[400]; int u = ccx_units(&m, um, sizeof um);
    bool mm = u == CCX_UNITS_MM;
    char sub[300];
    pl_fmt_ui(sub, sizeof sub, "%s · %d nodes, %ld %s elements · units: %s", ccx_analysis_name(m.analysis), m.nNodes, m.nElems, m.elType[0] ? m.elType : "",
              u == CCX_UNITS_SI ? "metres, newtons, pascals" : mm ? "millimetres, newtons, megapascals" : "not clear from the numbers");
    y += section_head(x, y, w, "The model", sub);
    if (um[0]) {
        float ch = callout_height(w, "The units don't agree", um);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "The units don't agree", um);
        y += ch + 12;
    }
    /* materials */
    for (int i = 0; i < m.nMat; i++) {
        const CcxMat *mt = &m.mat[i];
        int lib = ccx_lib_match(mt, u);
        char line1[300] = "";
        double E = mm ? mt->E * 1e6 : mt->E, rho = mm ? mt->rho * 1e12 : mt->rho;
        if (mt->hasE) pl_fmt_ui(line1, sizeof line1, "E %s, Poisson's ratio %.3g", fmt_si(E, "Pa"), mt->nu);
        if (mt->hasRho) { size_t n = strlen(line1); pl_fmt_ui(line1 + n, sizeof line1 - n, "%sdensity %.4g kg/m³", n ? ", " : "", rho); }
        if (mt->hasK) { size_t n = strlen(line1); pl_fmt_ui(line1 + n, sizeof line1 - n, "%sconductivity %.4g W/(m K)", n ? ", " : "", mt->k); }
        ui_icon(ICON_BOXES, x + 10, y + 13, 15, TH.text2);
        char nm[120]; pl_fmt_ui(nm, sizeof nm, "Material %s%s%s", mt->name, lib >= 0 ? " · " : "", lib >= 0 ? CCX_LIB[lib].name : "");
        ui_text_clip(F_MED, 14, nm, x + 28, y + 3, w * 0.5f, TH.text);
        ui_text_clip(F_REG, 12.5f, line1, x + 28, y + 24, w - 260, TH.text3);
        if (mt->lineE || mt->lineRho) {
            static const char *items[8]; int n = 0;
            items[n++] = "Use a material...";
            for (int k = 0; k < CCX_LIB_N && n < 8; k++) items[n++] = CCX_LIB[k].name;
            int pick = 0;
            if (ui_select(ui_idn(ui_id("ccx-mat"), i), (Rectangle){ x + w - 230, y + 4, 230, 34 }, items, n, &pick) && pick > 0) {
                const CcxLibMat *L = &CCX_LIB[pick - 1];
                char v[64];
                if (mt->lineE) {
                    snprintf(v, sizeof v, "%.6g", mm ? L->E / 1e6 : L->E); put_param(mt->lineE, "E", v);
                    snprintf(v, sizeof v, "%.4g", L->nu); put_param(mt->lineE, "nu", v);
                }
                if (mt->lineRho) { snprintf(v, sizeof v, "%.6g", mm ? L->rho * 1e-12 : L->rho); put_param(mt->lineRho, "rho", v); }
                ui_toast(TOAST_OK, "%s: E %s, density %.4g kg/m³ (in the model's %s). Save to keep it", L->name, fmt_si(L->E, "Pa"), L->rho, mm ? "millimetres" : "metres");
            }
        }
        y += 50;
    }
    /* supports */
    for (int i = 0; i < m.nBc; i++) {
        const CcxBc *b = &m.bc[i];
        int nn = ccx_set_nodes(&m, b->set);
        char t[200], nodes[48];
        pl_fmt_ui(nodes, sizeof nodes, nn > 0 ? "%d node%s" : "", nn, nn == 1 ? "" : "s");
        ui_icon(ICON_SQUARE, x + 10, y + 13, 15, TH.text2);
        if (b->d0 == 11) {
            pl_fmt_ui(t, sizeof t, "%s%s%s: temperature held at %.4g K", b->set, nodes[0] ? " · " : "", nodes, b->value);
            ui_text_clip(F_MED, 14, t, x + 28, y + 9, w - 40, TH.text);
            y += 40; continue;
        }
        pl_fmt_ui(t, sizeof t, "Support %s%s%s", b->set, nodes[0] ? " · " : "", nodes);
        ui_text_clip(F_MED, 14, t, x + 28, y + 3, w * 0.45f, TH.text);
        char what[200];
        if (b->hasValue && b->value != 0) pl_fmt_ui(what, sizeof what, "moved %.4g %s along %s", b->value, mm ? "mm" : "m", ccx_dof_name(b->d0));
        else if (b->d0 >= 1 && b->d1 <= 3) pl_fmt_ui(what, sizeof what, "held in %s%s%s", ccx_dof_name(b->d0), b->d1 > b->d0 ? " to " : "", b->d1 > b->d0 ? ccx_dof_name(b->d1) : "");
        else pl_fmt_ui(what, sizeof what, "degrees of freedom %d to %d held", b->d0, b->d1);
        ui_text_clip(F_REG, 12.5f, what, x + 28, y + 24, w * 0.45f, TH.text3);
        int sel = -1;
        for (int k = 0; k < 6; k++) if (HOLD_D0[k] == b->d0 && HOLD_D1[k] == b->d1) sel = k;
        if (b->line && sel >= 0 && !(b->hasValue && b->value != 0)) {
            int s0 = sel;
            if (ui_select(ui_idn(ui_id("ccx-hold"), i), (Rectangle){ x + w - 230, y + 4, 230, 34 }, HOLD_NAME, 6, &sel) && sel != s0) {
                char v[160]; snprintf(v, sizeof v, "%s, %d, %d", b->set, HOLD_D0[sel], HOLD_D1[sel]);
                put_param(b->line, "boundary", v);
            }
        }
        y += 50;
    }
    /* loads */
    for (int i = 0; i < m.nLoad; i++) {
        const CcxLoad *ld = &m.load[i];
        int nn = ccx_set_nodes(&m, ld->set);
        ui_icon(ICON_ZAP, x + 10, y + 13, 15, TH.text2);
        char t[240], d[260];
        if (ld->dof == 11) { pl_fmt_ui(t, sizeof t, "Heat into %s: %.4g W per node", ld->set, ld->mag); ui_text_clip(F_MED, 14, t, x + 28, y + 9, w - 40, TH.text); y += 40; continue; }
        pl_fmt_ui(t, sizeof t, "Force on %s%s", ld->set, nn > 1 ? TextFormat(" · %d nodes", nn) : "");
        ui_text_clip(F_MED, 14, t, x + 28, y + 3, w * 0.4f, TH.text);
        if (nn > 1) pl_fmt_ui(d, sizeof d, "%.4g N on each node along %s%s: %.4g N in total", fabs(ld->mag), ld->mag < 0 ? "-" : "+", ccx_dof_name(ld->dof), fabs(ld->mag) * nn);
        else pl_fmt_ui(d, sizeof d, "%.4g N along %s%s", fabs(ld->mag), ld->mag < 0 ? "-" : "+", ccx_dof_name(ld->dof));
        ui_text_clip(F_REG, 12.5f, d, x + 28, y + 24, w * 0.4f, TH.text3);
        if (ld->line && ld->dof >= 1 && ld->dof <= 3) {
            int dir = (ld->dof - 1) * 2 + (ld->mag < 0 ? 1 : 0), d0 = dir;
            double total = fabs(ld->mag) * (nn > 0 ? nn : 1), t0 = total;
            float fx = x + w - 380;
            ui_select(ui_idn(ui_id("ccx-dir"), i), (Rectangle){ fx, y + 4, 90, 34 }, DIR_NAME, 6, &dir);
            char tb[32]; snprintf(tb, sizeof tb, "%.6g", total);
            static char edit[CCX_MAX_BC][32];
            if (!ui_input_focused(ui_idn(ui_id("ccx-total"), i))) pl_strlcpy(edit[i], tb, sizeof edit[i]);
            if (ui_input(ui_idn(ui_id("ccx-total"), i), (Rectangle){ fx + 100, y + 4, 150, 34 }, edit[i], sizeof edit[i], NULL, INPUT_MONO | INPUT_NUMERIC)) total = atof(edit[i]);
            ui_text(F_REG, 12.5f, "N in total", fx + 258, y + 13, TH.text3);
            if (dir != d0 || (total != t0 && total > 0)) {
                double per = total / (nn > 0 ? nn : 1) * (dir % 2 ? -1 : 1);
                char v[160]; snprintf(v, sizeof v, "%s, %d, %.6g", ld->set, dir / 2 + 1, per);
                put_param(ld->line, "load", v);
            }
        }
        y += 50;
    }
    for (int i = 0; i < m.nDl; i++) {
        const CcxDload *dl = &m.dl[i];
        char t[240];
        if (!strncasecmp(dl->kind, "GRAV", 4)) pl_fmt_ui(t, sizeof t, "Gravity on %s: %.4g %s along (%g %g %g)", dl->set, dl->mag, mm ? "mm/s²" : "m/s²", dl->dir[0], dl->dir[1], dl->dir[2]);
        else if (dl->kind[0] == 'P' || dl->kind[0] == 'p') pl_fmt_ui(t, sizeof t, "Pressure on %s (face %s): %s", dl->set, dl->kind + 1, mm ? TextFormat("%.4g MPa", dl->mag) : fmt_si(dl->mag, "Pa"));
        else pl_fmt_ui(t, sizeof t, "Distributed load on %s: %s %.4g", dl->set, dl->kind, dl->mag);
        ui_icon(ICON_ZAP, x + 10, y + 11, 15, TH.text2);
        ui_text_clip(F_MED, 14, t, x + 28, y + 3, w - 40, TH.text);
        y += 36;
    }
    if (!m.nMat && !m.nBc && !m.nLoad && !m.nDl) y += ui_text_wrap(F_REG, 13, "No materials, supports or loads found in this input.", (Rectangle){ x, y, w, 40 }, 19, TH.text3, true) + 8;
    y += ui_text_wrap(F_REG, 12.5f, "Below: every value of the input as it is written, with its line (Basic: the usual ones; All values: everything).",
                      (Rectangle){ x, y + 4, w, 40 }, 18, TH.text3, true) + 18;
    ui_divider(x, y, w);
    return y + 18 - y0;
}

void page_prog_setup(Rectangle r)
{
    if (!A.cs) return;
    sync();
    if (prog() == PROG_CALCULIX && fea_is_study(A.cs->s.path)) { fea_setup_page(r); return; }   /* a part study (FEA_PLAN) */
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
    if (prog() == PROG_CALCULIX && P.pf.path[0] && P.pf.text) y += ccx_model_panel(c, x, y, w);   /* in engineering words (C1) */
    if (prog() == PROG_CALCULIX && P.pf.path[0]) y += ccx_model_view(c, x, y, w);   /* the wireframe, locking (C4) */
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

static void kpi(Rectangle r, const char *label, const char *value, Color vc)
{
    ui_text(F_REG, 12, ui_cap(label), r.x, r.y, TH.text3);
    ui_text_clip(F_SEMI, 18, value, r.x, r.y + 18, r.width - 8, vc);
}

/* ============================================================================ CalculiX: what the run did (workflow plan C3)
   By analysis type: a linear static run is one solve; a nonlinear, dynamic or heat run is increments of Newton iterations
   with cutbacks; a frequency run is its list of modes (against a cantilever's hand calculation when the model is one). */
static struct { char path[PL_PATH]; time_t sig; double at; CcxRun r; CcxModel m; } R;
static void cpu_text(char *v, size_t cap, double s)   /* a solver time; under a minute to two figures */
{
    if (s < 0) pl_strlcpy(v, "-", cap);
    else if (s < 60) pl_fmt_ui(v, cap, "%.2g s", s);
    else pl_duration(v, cap, s);
}
/* the run's files, read again when they change */
static const CcxRun *ccx_run_get(Case *c, bool mine)
{
    char key[PL_PATH]; pl_join(key, sizeof key, c->s.path, input());
    char job[256]; pl_strlcpy(job, input(), sizeof job);
    char *dot = strrchr(job, '.'); if (dot && !strcasecmp(dot, ".inp")) *dot = 0;
    char ln[96], lp[PL_PATH], f[300], fp[PL_PATH]; prog_log_name(prog(), input(), ln, sizeof ln); pl_join(lp, sizeof lp, c->s.path, ln);
    /* the files it reads, so a finished run shows at once; mtimes are whole seconds, so files under 3 s old are read again
       every half second (a header and the rows written within one second) */
    time_t mt[4] = { pl_mtime(lp), pl_mtime(key), 0, 0 };
    snprintf(f, sizeof f, "%s.sta", job); pl_join(fp, sizeof fp, c->s.path, f); mt[2] = pl_mtime(fp);
    snprintf(f, sizeof f, "%s.dat", job); pl_join(fp, sizeof fp, c->s.path, f); mt[3] = pl_mtime(fp);
    time_t sig = 0, newest = 0; for (int i = 0; i < 4; i++) { sig += mt[i]; if (mt[i] > newest) newest = mt[i]; }
    bool fresh = time(NULL) - newest < 3;
    if (strcmp(R.path, key) || sig != R.sig || ((mine || fresh) && GetTime() - R.at > 0.5)) {
        pl_strlcpy(R.path, key, sizeof R.path); R.sig = sig; R.at = GetTime();
        ccx_run_load(c->s.path, job, lp, &R.r);
        ccx_model_load(c->s.path, input(), &R.m);
    }
    return &R.r;
}

static float ccx_run_panel(Case *c, float x, float y, float w, bool mine)
{
    ccx_run_get(c, mine);
    const CcxRun *r = &R.r; const CcxModel *m = &R.m;
    if (!r->hasSta && !r->nModes) return 0;
    float y0 = y, kw = w / 4;
    char v[64], t[200], d[900];
    if (r->nModes) {                                                   /* ---- frequency */
        pl_fmt_ui(d, sizeof d, "%d natural frequencies, lowest first, in cycles per unit time (Hz with times in seconds, as in both m-N-Pa and mm-N-MPa)."
                  " Each is a shape the part vibrates in by itself.", r->nModes);
        y += section_head(x, y, w, "The modes", d);
        snprintf(v, sizeof v, "%d", r->nModes); kpi((Rectangle){ x, y, kw, 50 }, "Modes", v, TH.text);
        pl_fmt_ui(v, sizeof v, "%.4g Hz", r->hz[0]); kpi((Rectangle){ x + kw, y, kw, 50 }, "Lowest", v, TH.text);
        pl_fmt_ui(v, sizeof v, "%.4g Hz", r->hz[r->nModes - 1]); kpi((Rectangle){ x + 2 * kw, y, kw, 50 }, "Highest", v, TH.text);
        cpu_text(v, sizeof v, r->cpu); kpi((Rectangle){ x + 3 * kw, y, kw, 50 }, "Solver time", v, TH.text);
        y += 58;
        bool radC = w >= 3 * 250.f;                                 /* rad/s too when three columns of both fit */
        int cols = radC || w > 480 ? 3 : 2, rows = (r->nModes + cols - 1) / cols;
        float cw = w / cols;
        for (int i = 0; i < r->nModes; i++) {
            float cx = x + (float)(i / rows) * cw, cy = y + (float)(i % rows) * 22;
            pl_fmt_ui(t, sizeof t, "%d", i + 1); ui_text(F_MONO, 12.5f, t, cx, cy, TH.text3);
            if (radC) pl_fmt_ui(t, sizeof t, "%10.4g Hz  %9.4g rad/s", r->hz[i], r->rad[i]); else pl_fmt_ui(t, sizeof t, "%10.4g Hz", r->hz[i]); ui_text_clip(F_MONO, 12.5f, t, cx + 28, cy, cw - 36, TH.text);
        }
        y += (float)rows * 22 + 10;
        if (r->nNeg) {
            pl_fmt_ui(t, sizeof t, "%d negative eigenvalue%s, shown as 0 Hz", r->nNeg, r->nNeg == 1 ? "" : "s");
            pl_fmt_ui(d, sizeof d, "No real vibration has a negative eigenvalue: either the part isn't held, or the eigensolver returned a spurious one "
                      "(it can with several threads). If the supports are right, run it again, with 1 thread to be sure.");
            float ch = callout_height(w, t, d); callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, t, d); y += ch + 10;
        }
        int rigid = 0; for (int i = 0; i < r->nModes; i++) if (r->hz[i] < 1e-3 * r->hz[r->nModes - 1] && r->hz[i] < 1) rigid++;
        rigid -= r->nNeg;
        if (rigid) {
            pl_fmt_ui(t, sizeof t, "%d mode%s at almost zero: the part can move freely", rigid, rigid == 1 ? "" : "s");
            pl_fmt_ui(d, sizeof d, "A frequency near zero is a rigid-body motion, not a vibration: the supports don't hold the part in every direction. "
                      "Hold it (*BOUNDARY, Setup > Supports) unless it really is free, as in a part in flight.");
            float ch = callout_height(w, t, d); callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, t, d); y += ch + 10;
        }
        for (int i = 0; i + 1 < r->nModes; i++)
            if (r->hz[i] > 0 && fabs(r->hz[i + 1] - r->hz[i]) < 0.01 * r->hz[i]) {
                pl_fmt_ui(d, sizeof d, "Modes %d and %d have the same frequency (within 1 %%): a symmetric section bends the same way in two directions, so the "
                          "pair is one shape turned 90°.", i + 1, i + 2);
                ui_icon(ICON_INFO, x + 8, y + 9, 14, TH.text3);
                y += ui_text_wrap(F_REG, 13, d, (Rectangle){ x + 24, y, w - 24, 80 }, 19, TH.text2, true) + 8;
                break;
            }
        double f1 = ccx_beam_f1(m);
        if (f1 > 0 && !r->nNeg && rigid <= 0) {
            double err = (r->hz[0] - f1) / f1 * 100;
            pl_fmt_ui(d, sizeof d, "By hand, a cantilever's first bending mode: f1 = 1.875² / (2 pi) x sqrt(E I / (ρ A L^4)) = %.4g Hz. CalculiX: %.4g Hz, %+.1f %%. "
                      "Higher modes of a real beam depart from the formula as it gets stubby.", f1, r->hz[0], err);
            ui_icon(fabs(err) < 5 ? ICON_CIRCLE_CHECK : ICON_INFO, x + 8, y + 9, 14, fabs(err) < 5 ? TH.ok : TH.text3);
            y += ui_text_wrap(F_REG, 13, d, (Rectangle){ x + 24, y, w - 24, 80 }, 19, TH.text2, true) + 8;
        }
        return y - y0 + 10;
    }
    bool linear = m->analysis == CCX_STATIC && !m->nonlinear && r->nInc <= 1 && r->itersMax <= 1;
    if (linear) {                                                      /* ---- linear static */
        y += section_head(x, y, w, "What CalculiX did", r->nInc ? "Solved in one increment" : "Solving");
        char du[32] = ""; if (r->cpu >= 0) { char dd[24]; cpu_text(dd, sizeof dd, r->cpu); pl_fmt_ui(du, sizeof du, "Solver time %s. ", dd); }
        pl_fmt_ui(d, sizeof d, "%sA linear static analysis is one solve of K u = F: no time steps and nothing to converge, so it takes one increment. "
                  "Large deflections (NLGEOM), contact or plasticity make it nonlinear: then it goes in increments, each iterated until the forces balance.", du);
        y += ui_text_wrap(F_REG, 13, d, (Rectangle){ x, y, w, 120 }, 19, TH.text2, true) + 10;
        return y - y0 + 10;
    }
    /* ---- nonlinear static, dynamic, heat transfer: increments */
    pl_fmt_ui(t, sizeof t, "%s in increments", m->analysis == CCX_STATIC ? (m->nonlinear ? "Nonlinear static" : "Static") : ccx_analysis_name(m->analysis));
    y += section_head(x, y, w, "What CalculiX did", t);
    snprintf(v, sizeof v, "%d", r->nInc); kpi((Rectangle){ x, y, kw, 50 }, "Increments", v, TH.text);
    pl_fmt_ui(v, sizeof v, "%.1f / inc", r->nInc ? (double)r->itersTotal / r->nInc : 0); kpi((Rectangle){ x + kw, y, kw, 50 }, "Iterations", v, TH.text);
    snprintf(v, sizeof v, "%d", r->cutbacks); kpi((Rectangle){ x + 2 * kw, y, kw, 50 }, "Cutbacks", v, r->cutbacks ? TH.warn : TH.text);
    cpu_text(v, sizeof v, r->cpu); kpi((Rectangle){ x + 3 * kw, y, kw, 50 }, "Solver time", v, TH.text);
    y += 58;
    bool fixed = r->nInc > 1 && fabs(r->incMax - r->incMin) < 1e-9 * fmax(1e-30, r->incMax);
    if (fixed) pl_fmt_ui(d, sizeof d, "%d increments of %.4g, a fixed size (DIRECT on the step line): no automatic control, so a too-large increment fails instead of shrinking. "
                         "Step time reached %.4g.", r->nInc, r->incMax, r->stepTime);
    else pl_fmt_ui(d, sizeof d, "Step time reached %.4g in increments from %.4g to %.4g (the first %.4g). Each increment is solved by Newton iterations until the forces "
                   "(or heat flows) balance; at most %d in one. An increment that converges easily lets the next grow; one that doesn't is cut back and tried smaller.",
                   r->stepTime, r->incMin, r->incMax, r->incFirst, r->itersMax);
    y += ui_text_wrap(F_REG, 13, d, (Rectangle){ x, y, w, 140 }, 19, TH.text2, true) + 10;
    if (r->cutbacks >= 3 || (r->cutbacks && r->cutbacks * 2 >= r->nInc)) {
        pl_fmt_ui(t, sizeof t, "The increment was cut back %d time%s", r->cutbacks, r->cutbacks == 1 ? "" : "s");
        pl_fmt_ui(d, sizeof d, "Increments that didn't converge were halved and tried again. A few is normal; many slow the run and can end it. "
                  "Start smaller (the first number on the *STATIC or *DYNAMIC line), and check the supports and contact: a part that isn't held, "
                  "or contact that opens and closes, are the usual causes.");
        float ch = callout_height(w, t, d); callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, t, d); y += ch + 10;
    }
    return y - y0 + 10;
}

/* ============================================================================ DEME: the GPU (workflow plan D3)
   What nvidia-smi says (polled off the frame), and the run's size against the free memory, learned from this project's own
   runs: while one goes the GPU memory it adds is watched, and <project>/.qc-gpu keeps "particles peak_mb" for the next. */
static long deme_max_particles(const char *proj)
{
    char p[PL_PATH]; pl_join(p, sizeof p, proj, "params.ini");
    char *t = pl_read_file(p, NULL); long n = 0;
    for (char *l = t; l && *l; ) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        while (*l == ' ' || *l == '\t') l++;
        if (!strncmp(l, "max_particles", 13)) { char *q = strchr(l, '='); if (q) n = atol(q + 1); }
        l = e ? e + 1 : NULL;
    }
    free(t);
    return n;
}

static float deme_gpu_row(Case *c, float x, float y, float w, bool mine)
{
    static struct { char path[PL_PATH]; bool on; int base, peak; long parts; } W;   /* the run being watched */
    GpuInfo g = gpu_get();
    gpu_poll_fast(mine);
    if (mine && !W.on && g.ok) { W.on = true; pl_strlcpy(W.path, c->s.path, sizeof W.path); W.base = g.usedMB; W.peak = 0; W.parts = deme_max_particles(c->s.path); }
    if (W.on && g.ok && g.usedMB - W.base > W.peak) W.peak = g.usedMB - W.base;
    if (W.on && !mine) {                                         /* it ended: keep what it took, when it took something */
        W.on = false;
        if (W.peak > 20 && W.parts > 0) {
            char p[PL_PATH]; pl_join(p, sizeof p, W.path, ".qc-gpu");
            char b[96]; int n = snprintf(b, sizeof b, "%ld %d\n", W.parts, W.peak);
            pl_write_file(p, b, (size_t)n);
        }
    }
    if (!g.probed) return 0;
    float y0 = y;
    char t[300];
    if (!g.ok) {
        ui_icon(ICON_CIRCLE_X, x + 8, y + 9, 14, TH.err);
        y += ui_text_wrap(F_REG, 13, "No NVIDIA GPU answers (nvidia-smi): DEME runs on CUDA only. Under WSL the GPU comes from the Windows NVIDIA driver; "
                          "update it, then nvidia-smi in a terminal should list the card.", (Rectangle){ x + 24, y, w - 24, 100 }, 19, TH.err, true) + 8;
        return y - y0;
    }
    int freeMB = g.totalMB - g.usedMB;
    pl_fmt_ui(t, sizeof t, "GPU: %s, %.1f GB free of %.1f GB", g.name, freeMB / 1024.0, g.totalMB / 1024.0);
    bool low = freeMB < 1024;
    ui_icon(low ? ICON_TRIANGLE_ALERT : ICON_CIRCLE_CHECK, x + 8, y + 9, 14, low ? TH.warn : TH.ok);
    y += ui_text_wrap(F_REG, 13, t, (Rectangle){ x + 24, y, w - 24, 60 }, 19, TH.text2, true) + 4;
    if (low) y += ui_text_wrap(F_REG, 12.5f, "Under 1 GB free: another program holds the GPU's memory. Close it, or the run may fail to start.",
                               (Rectangle){ x + 24, y, w - 24, 60 }, 18, TH.warn, true) + 4;
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, ".qc-gpu");
    char *h = pl_read_file(p, NULL); long hp = 0; int hm = 0;
    long want = deme_max_particles(c->s.path);
    if (h && sscanf(h, "%ld %d", &hp, &hm) == 2 && hp > 0 && hm > 0 && want > 0 && !mine) {
        double per = (double)hm / hp, need = per * want;
        bool tight = need > 0.9 * freeMB;
        pl_fmt_ui(t, sizeof t, "The last run took %d MB for up to %ld particles (%.2g kB each); this one allows %ld: about %.0f MB%s",
                  hm, hp, per * 1024, want, need, tight ? ", more than is free" : "");
        ui_icon(tight ? ICON_TRIANGLE_ALERT : ICON_INFO, x + 8, y + 9, 14, tight ? TH.warn : TH.text3);
        y += ui_text_wrap(F_REG, 12.5f, t, (Rectangle){ x + 24, y, w - 24, 80 }, 18, tight ? TH.warn : TH.text3, true) + 4;
    }
    free(h);
    return y - y0 + 6;
}

/* ============================================================================ Run */

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
    static bool adv;                                             /* D3: the rebuild switch and the coupled run, behind Advanced */
    if (info()->build) {
        y += ui_text_wrap(F_REG, 12.5f, "It builds by itself before running when the driver's source or CMakeLists.txt changed since the last build.",
                          (Rectangle){ x, y, w, 60 }, 18, TH.text3, true) + 8;
        ui_disable(false);
        ui_switch(ui_id("pr-advanced"), x, y, &adv);
        ui_register(ui_id("pr-advanced"), (Rectangle){ x, y, 44, 26 }, "Advanced");
        ui_text(F_MED, 13.5f, prog() == PROG_DEME ? "Advanced: rebuild, run with OpenFOAM" : "Advanced: rebuild", x + 56, y + 4, TH.text);
        y += 36;
        ui_disable(mine);
        if (adv) {
            ui_checkbox(ui_id("pr-rebuild"), (Rectangle){ x, y, w, 24 }, &P.rebuild, "Rebuild anyway, from scratch");
            ui_tooltip((Rectangle){ x, y, w, 24 }, "For a change it can't see: a library updated underneath, or a header outside the project");
            y += 36;
        }
    }
    ui_disable(false);
    ui_divider(x, y, w); y += 20;
    ui_text(F_MED, 13.5f, term(T_SWEEP), x, y, TH.text); y += 28;
    y += checks_list(x, y, w, true) + 6;
    if (prog() == PROG_DEME) y += deme_gpu_row(c, x, y, w, mine);   /* the GPU against the run's size (D3) */
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
    if (prog() == PROG_DEME && adv) { y += 10; y += deme_pair_panel(x, y, w, mine, start_both); }
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
    const CcxRun *cr = prog() == PROG_CALCULIX ? ccx_run_get(c, mine) : NULL;   /* C3: its increments, not the log's steps */
    if (cr && cr->nModes) pl_fmt_ui(t, sizeof t, "%d modes", cr->nModes);
    else if (known) pl_fmt_ui(t, sizeof t, steps ? "step %.0f of %.0f" : "t = %.4g of %.4g s", done, total);
    else if (pm && pm->steps) pl_fmt_ui(t, sizeof t, "%d step%s, t = %.4g", pm->steps, pm->steps == 1 ? "" : "s", pm->time);
    else pl_fmt_ui(t, sizeof t, mine ? "Starting..." : pm && (pm->ended || pm->exitCode == 0) ? "Ended" : "No run yet");
    ui_text(F_MED, 13, t, mx, my, TH.text2);
    my += 30;
    if (!(cr && cr->nModes)) {   /* a modal run: the modes panel has its own figures */
        /* kpis */
        float kw = mw / 5;
        char v[64];
        snprintf(v, sizeof v, "%d", cr && cr->hasSta ? cr->nInc : pm ? pm->steps : 0);
        kpi((Rectangle){ mx, my, kw, 50 }, cr ? "Increments" : steps ? "Steps" : "Steps / increments", v, TH.text);
        if (cr && cr->nInc) snprintf(v, sizeof v, "%.4g", cr->tot);
        else snprintf(v, sizeof v, pm && pm->steps ? "%.4g" : "-", pm ? pm->time : 0);
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
    }
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
    if (prog() == PROG_CALCULIX) my += ccx_run_panel(c, mx, my, mw, mine);   /* by analysis type (C3) */
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
    /* the first column across; up against it the first three that aren't another clock (W0, 2026-11-14: a DEME history
       plotted t_phys against t_sim, a straight line that dwarfed the temperatures) */
    P.tblX = 0;
    int on = 0;
    for (int i = 0; i < 16; i++) {
        const char *n = i < P.nCols ? P.tblCols[i] : "";
        bool clock = !strcmp(n, "t") || !strcasecmp(n, "time") || !strcasecmp(n, "step") || !strcasecmp(n, "frame") ||
                     !strncmp(n, "t_", 2) || !strncasecmp(n, "time_", 5);
        P.tblOn[i] = i > 0 && i < P.nCols && !clock && on < 3;
        if (P.tblOn[i]) on++;
    }
    if (!on) for (int i = 1; i < 4 && i < P.nCols; i++) P.tblOn[i] = true;   /* only clocks: show them rather than nothing */
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


/* ============================================================================ CalculiX: the model before running (workflow plan C4)
   A wireframe of the input's mesh, turned by dragging, with the nodes the supports hold and the point loads push (arrows
   along the force); and the note when plain C3D8 bricks with few layers through the thickness carry bending (shear locking). */
static struct { char path[PL_PATH]; time_t mt; double at; CcxMesh g; CcxModel m; bool ok; float yaw, pitch; bool drag; } MV = { .yaw = -0.6f, .pitch = 0.45f };

static float ccx_model_view(Case *c, float x, float y, float w)
{
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, setup_file());
    time_t mt = pl_mtime(p);
    if (strcmp(MV.path, p) || mt != MV.mt || GetTime() - MV.at > 5) {          /* an included mesh file may change too */
        if (MV.ok) ccx_mesh_free(&MV.g);
        pl_strlcpy(MV.path, p, sizeof MV.path); MV.mt = mt; MV.at = GetTime();
        ccx_model_load(c->s.path, setup_file(), &MV.m);
        MV.ok = ccx_mesh_load(c->s.path, setup_file(), &MV.m, &MV.g);
    }
    if (!MV.ok || !MV.g.nEdges) return 0;
    const CcxMesh *g = &MV.g; const CcxModel *m = &MV.m;
    float y0 = y;
    char d[600], t[200];
    pl_fmt_ui(d, sizeof d, "%d nodes, %d elements. Drag to turn it. Check the supports and loads sit where you mean before running.", g->n, g->nElems);
    y += section_head(x, y, w, "The mesh", d);
    Rectangle box = { x, y, w, fminf(360, fmaxf(240, w * 0.42f)) };
    ui_rect(box, 10, TH.chartBg); ui_rect_lines(box, 10, 1, TH.lineSoft);
    /* turning */
    if (ui_hovered(box) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) MV.drag = true;
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) MV.drag = false;
    if (MV.drag) {
        Vector2 dm = GetMouseDelta();
        MV.yaw += dm.x / ui_scale() * 0.01f;
        MV.pitch = fmaxf(-1.5f, fminf(1.5f, MV.pitch + dm.y / ui_scale() * 0.01f));
        ui_cursor(MOUSE_CURSOR_RESIZE_ALL);
    } else if (ui_hovered(box)) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    float cyw = cosf(MV.yaw), syw = sinf(MV.yaw), cp = cosf(MV.pitch), sp = sinf(MV.pitch);
    float ctr[3];
    for (int k = 0; k < 3; k++) ctr[k] = (float)(m->lo[k] + m->hi[k]) / 2;
    /* fitted to the outline it has from this side: the nodes' projected extent */
    float plo[2] = { 1e30f, 1e30f }, phi[2] = { -1e30f, -1e30f };
    for (int i = 0; i < g->n; i++) {
        float X = g->x[3 * i] - ctr[0], Y = g->x[3 * i + 1] - ctr[1], Z = g->x[3 * i + 2] - ctr[2];
        float u = X * cyw - Y * syw, y1 = X * syw + Y * cyw, v = Z * cp - y1 * sp;
        plo[0] = fminf(plo[0], u); phi[0] = fmaxf(phi[0], u);
        plo[1] = fminf(plo[1], v); phi[1] = fmaxf(phi[1], v);
    }
    float aw = box.width - 60, ah = box.height - 70;
    float kk = fminf(aw / fmaxf(phi[0] - plo[0], 1e-12f), ah / fmaxf(phi[1] - plo[1], 1e-12f));
    float ox = box.x + box.width / 2 - kk * (plo[0] + phi[0]) / 2, oy = box.y + 26 + (box.height - 30) / 2 + kk * (plo[1] + phi[1]) / 2;
    #define PROJ(X_, Y_, Z_, OUT_) do { float x1_ = (X_) * cyw - (Y_) * syw, y1_ = (X_) * syw + (Y_) * cyw, z1_ = (Z_); \
                                        (OUT_) = (Vector2){ ox + kk * x1_, oy - kk * (z1_ * cp - y1_ * sp) }; } while (0)
    Color edgeC = ui_alpha(TH.text2, g->nEdges > 20000 ? 0.18f : 0.4f);
    for (int e = 0; e < g->nEdges; e++) {
        const float *a = &g->x[3 * g->edge[2 * e]], *b = &g->x[3 * g->edge[2 * e + 1]];
        Vector2 pa, pb;
        PROJ(a[0] - ctr[0], a[1] - ctr[1], a[2] - ctr[2], pa);
        PROJ(b[0] - ctr[0], b[1] - ctr[1], b[2] - ctr[2], pb);
        DrawLineEx(pa, pb, 1, edgeC);
    }
    /* held nodes coloured by what they can't do: x red, y green, z blue (the axes' colours), all three teal, temperature orange */
    static const Color AXC[3] = { { 239, 68, 68, 255 }, { 34, 197, 94, 255 }, { 59, 130, 246, 255 } };
    Color allC = { 45, 212, 191, 255 }, tempC = { 249, 115, 22, 255 }, loadC = TH.warn;
    int nHeld = 0, nLoaded = 0, kinds[6] = { 0 };   /* x y z all temperature mixed */
    for (int i = 0; i < g->n; i++) {
        int r = g->role[i] & 7;
        if (r || (g->role[i] & 8)) nHeld++;
        if (g->role[i] & 16) nLoaded++;
        if (r == 1) kinds[0]++; else if (r == 2) kinds[1]++; else if (r == 4) kinds[2]++; else if (r == 7) kinds[3]++; else if (r) kinds[5]++;
        if (g->role[i] & 8) kinds[4]++;
    }
    float hs = nHeld > 200 ? 1.8f : 3;
    for (int i = 0; i < g->n; i++) {
        int r = g->role[i] & 7;
        if (!r && !(g->role[i] & 8)) continue;
        Color col = r == 1 ? AXC[0] : r == 2 ? AXC[1] : r == 4 ? AXC[2] : r == 7 ? allC : r ? ui_mix(allC, TH.text, 0.3f) : tempC;
        Vector2 q; PROJ(g->x[3 * i] - ctr[0], g->x[3 * i + 1] - ctr[1], g->x[3 * i + 2] - ctr[2], q);
        DrawRectangleRec((Rectangle){ q.x - hs, q.y - hs, 2 * hs, 2 * hs }, col);
    }
    if (nLoaded) {                                      /* an arrow along each point load's force, ending on its node */
        float dir[3] = { 0, 0, 0 };
        for (int l = 0; l < m->nLoad; l++) if (m->load[l].dof >= 1 && m->load[l].dof <= 3) dir[m->load[l].dof - 1] += m->load[l].mag >= 0 ? 1.f : -1.f;
        float dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        Vector2 o0, o1; PROJ(0, 0, 0, o0); PROJ(dir[0], dir[1], dir[2], o1);
        Vector2 sd = { o1.x - o0.x, o1.y - o0.y }; float sl = sqrtf(sd.x * sd.x + sd.y * sd.y);
        int stride = nLoaded > 60 ? nLoaded / 60 : 1, seen = 0;
        for (int i = 0; i < g->n; i++) if (g->role[i] & 16) {
            if (seen++ % stride) continue;
            Vector2 q; PROJ(g->x[3 * i] - ctr[0], g->x[3 * i + 1] - ctr[1], g->x[3 * i + 2] - ctr[2], q);
            if (dl > 0 && sl > 1e-6f) {
                float L = 26; Vector2 u = { sd.x / sl, sd.y / sl };
                Vector2 tail = { q.x - u.x * L, q.y - u.y * L };
                DrawLineEx(tail, q, 1.6f, loadC);
                Vector2 n = { -u.y, u.x }; float hl = 7, hw = 4;
                DrawTriangle(q, (Vector2){ q.x - u.x * hl - n.x * hw, q.y - u.y * hl - n.y * hw }, (Vector2){ q.x - u.x * hl + n.x * hw, q.y - u.y * hl + n.y * hw }, loadC);
                DrawTriangle(q, (Vector2){ q.x - u.x * hl + n.x * hw, q.y - u.y * hl + n.y * hw }, (Vector2){ q.x - u.x * hl - n.x * hw, q.y - u.y * hl - n.y * hw }, loadC);
            } else DrawCircleV(q, 3, loadC);
        }
    }
    /* the axes, bottom left, in the held colours */
    {
        static const char *const AXN[3] = { "x", "y", "z" };
        Vector2 o; PROJ(0, 0, 0, o);
        float L = 22 / kk;
        Vector2 base = { box.x + 26, box.y + box.height - 26 };
        for (int k = 0; k < 3; k++) {
            float v[3] = { 0, 0, 0 }; v[k] = L;
            Vector2 q; PROJ(v[0], v[1], v[2], q);
            Vector2 e = { base.x + q.x - o.x, base.y + q.y - o.y };
            DrawLineEx(base, e, 1.6f, AXC[k]);
            ui_text(F_REG, 11, AXN[k], e.x + 2, e.y - 7, AXC[k]);
        }
    }
    #undef PROJ
    /* legend */
    float lx = box.x + 12, ly = box.y + 8;
    static const char *const KN[6] = { "held in x", "held in y", "held in z", "held in x, y and z", "temperature held", "held in two directions" };
    for (int k = 0; k < 6; k++) if (kinds[k]) {
        Color col = k < 3 ? AXC[k] : k == 3 ? allC : k == 4 ? tempC : ui_mix(allC, TH.text, 0.3f);
        ui_rect((Rectangle){ lx, ly + 4, 8, 8 }, 1, col); lx += 13;
        pl_fmt_ui(t, sizeof t, "%s: %d", KN[k], kinds[k]); ui_text(F_REG, 12, t, lx, ly, TH.text2); lx += ui_text_w(F_REG, 12, t) + 14;
    }
    if (nLoaded) {
        ui_rect((Rectangle){ lx, ly + 7, 12, 2 }, 1, loadC); lx += 16;
        pl_fmt_ui(t, sizeof t, "point loads: %d node%s", nLoaded, nLoaded == 1 ? "" : "s"); ui_text(F_REG, 12, t, lx, ly, TH.text2); lx += ui_text_w(F_REG, 12, t) + 14;
    }
    if (m->nDl) { pl_fmt_ui(t, sizeof t, "%d pressure / gravity load%s (*DLOAD) not drawn", m->nDl, m->nDl == 1 ? "" : "s"); ui_text_clip(F_REG, 12, t, lx, ly, box.x + box.width - lx - 12, TH.text3); }
    y += box.height + 14;
    /* shear locking */
    int layers = ccx_locking_risk(m, g);
    if (layers) {
        pl_fmt_ui(t, sizeof t, layers > 0 ? "Plain C3D8 bricks, %d through the thickness, will be too stiff in bending" : "Plain C3D8 bricks will be too stiff in bending", layers);
        pl_fmt_ui(d, sizeof d, "Fully integrated linear bricks can't bend without shearing, so they lock: in a test cantilever, one brick through the thickness "
                  "came out 35 %% too stiff, two 12 %%, four 3.5 %%. C3D8I (incompatible modes) bends correctly with one; C3D20R (quadratic) does too.");
        float bw = ui_button_width(NULL, "Use C3D8I"), cw = w - bw - 16;
        float ch = callout_height(cw, t, d);
        callout((Rectangle){ x, y, cw, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, t, d);
        if (ui_button(ui_id("ccx-c3d8i"), (Rectangle){ x + w - bw, y + (ch - 34) / 2, bw, 34 }, NULL, "Use C3D8I", BTN_SECONDARY)) {
            if (!P.pf.dirty || prog_setup_save()) {
                int nchg = ccx_set_element_type(c->s.path, setup_file(), "C3D8", "C3D8I");
                ui_toast(nchg ? TOAST_OK : TOAST_ERR, nchg ? "The elements are C3D8I now (%d *ELEMENT line%s)" : "No *ELEMENT, TYPE=C3D8 line found", nchg, nchg == 1 ? "" : "s");
                MV.path[0] = 0;
                load_setup();
            }
        }
        y += ch + 14;
    }
    return y - y0;
}

/* ============================================================================ CalculiX results in the app (workflow plan C2)
 * The newest .frd of the input: the largest displacement and von Mises stress and where, the safety factor against the
 * material's yield (when it is a library material), the reactions at the supports against the loads, the Euler-Bernoulli
 * hand calculation beside a slender cantilever's, a heat run's temperature range, and a side view of the deformed part
 * coloured by stress. */
static struct { char path[PL_PATH]; long long stamp; CcxFrd r; bool ok; } F;
static const CcxFrd *frd_for(Case *c)
{
    char stem[160]; pl_strlcpy(stem, input(), sizeof stem);
    char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
    char rel[200]; snprintf(rel, sizeof rel, "%s.frd", stem);
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, rel);
    long long st = params_stamp(p);
    if (strcmp(F.path, p) || st != F.stamp) {
        if (F.ok) ccx_frd_free(&F.r);
        pl_strlcpy(F.path, p, sizeof F.path); F.stamp = st;
        F.ok = st > 0 && ccx_frd_load(p, &F.r);
    }
    return F.ok ? &F.r : NULL;
}

static Color stress_color(float t)                       /* blue (low) to red (high) */
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t < 0.5f ? ui_mix((Color){ 59, 130, 246, 255 }, (Color){ 34, 197, 94, 255 }, t * 2) : ui_mix((Color){ 34, 197, 94, 255 }, (Color){ 239, 68, 68, 255 }, (t - 0.5f) * 2);
}

static float deformed_view(const CcxFrd *r, int ax, int ay, Rectangle box, bool mm)
{
    ui_rect(box, 10, TH.chartBg); ui_rect_lines(box, 10, 1, TH.lineSoft);
    if (!r->nEdges || !r->u) return box.height;
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
    for (int i = 0; i < r->n; i++) {
        float p[2] = { r->x[3 * i + ax], r->x[3 * i + ay] };
        for (int k = 0; k < 2; k++) { if (p[k] < lo[k]) lo[k] = p[k]; if (p[k] > hi[k]) hi[k] = p[k]; }
    }
    float ext = fmaxf(hi[0] - lo[0], hi[1] - lo[1]);
    float scale = r->uMax > 0 ? 0.12f * ext / (float)r->uMax : 1;      /* the largest displacement drawn at 12 % of the part */
    float pad = 30, sx = (box.width - 2 * pad) / fmaxf(hi[0] - lo[0], 1e-12f), sy = (box.height - 2 * pad) / fmaxf(hi[1] - lo[1] + 0.25f * ext, 1e-12f);
    float k = fminf(sx, sy);
    float cx = box.x + box.width / 2 - k * (lo[0] + hi[0]) / 2, cy = box.y + box.height / 2 + k * (lo[1] + hi[1]) / 2;
    for (int e = 0; e < r->nEdges; e++) {
        int a = r->edge[2 * e], b = r->edge[2 * e + 1];
        Vector2 pa = { cx + k * r->x[3 * a + ax], cy - k * r->x[3 * a + ay] }, pb = { cx + k * r->x[3 * b + ax], cy - k * r->x[3 * b + ay] };
        DrawLineEx(pa, pb, 1, ui_alpha(TH.text3, 0.25f));
    }
    for (int e = 0; e < r->nEdges; e++) {
        int a = r->edge[2 * e], b = r->edge[2 * e + 1];
        Vector2 pa = { cx + k * (r->x[3 * a + ax] + scale * r->u[3 * a + ax]), cy - k * (r->x[3 * a + ay] + scale * r->u[3 * a + ay]) };
        Vector2 pb = { cx + k * (r->x[3 * b + ax] + scale * r->u[3 * b + ax]), cy - k * (r->x[3 * b + ay] + scale * r->u[3 * b + ay]) };
        float t = r->vm && r->vmMax > 0 ? (r->vm[a] + r->vm[b]) / 2 / (float)r->vmMax : 0.5f;
        DrawLineEx(pa, pb, 1.4f, stress_color(t));
    }
    static const char *const AX[3] = { "x", "y", "z" };
    char lab[160]; pl_fmt_ui(lab, sizeof lab, "Side view (%s across, %s up) · displacements drawn %.3g times larger · colour: von Mises stress, blue low, red high",
                             AX[ax], AX[ay], scale);
    ui_text_clip(F_REG, 12, lab, box.x + 12, box.y + 8, box.width - 24, TH.text3);
    (void)mm;
    return box.height;
}

static float ccx_results_panel(Case *c, float x, float y, float w)
{
    const CcxFrd *r = frd_for(c);
    if (!r) return 0;
    float y0 = y;
    CcxModel m; ccx_model_load(c->s.path, input(), &m);
    int u = ccx_units(&m, NULL, 0);
    bool mm = u == CCX_UNITS_MM;
    y += section_head(x, y, w, "The answer", ccx_analysis_name(m.analysis));
    char t[300], d[400];
    #define ROW(IC_, COL_, TT_, DD_) do { ui_icon(IC_, x + 10, y + 13, 15, COL_); ui_text_clip(F_MED, 14, TT_, x + 28, y + 3, w - 40, TH.text); \
                                           if ((DD_)[0]) ui_text_clip(F_REG, 12.5f, DD_, x + 28, y + 24, w - 40, TH.text3); \
                                           y += (DD_)[0] ? 48 : 32; } while (0)
    bool modal = m.analysis == CCX_FREQUENCY;
    if (modal) {                                             /* a modal run writes mode shapes: their size means nothing (C4) */
        const CcxRun *ru = ccx_run_get(c, false);
        if (ru->nModes) {
            pl_fmt_ui(t, sizeof t, "Lowest natural frequency %.4g Hz", ru->hz[0]);
            pl_fmt_ui(d, sizeof d, "%d modes, up to %.4g Hz; the Run page lists them. The displacements written are the mode shapes, scaled arbitrarily: only their shape means anything",
                      ru->nModes, ru->hz[ru->nModes - 1]);
            ROW(ICON_ACTIVITY, TH.accent, t, d);
        }
    }
    if (r->u && !modal) {
        int i = r->uMaxAt;
        double um = mm ? r->uMax : r->uMax * 1e3;
        pl_fmt_ui(t, sizeof t, "Largest displacement %.4g mm", um);
        if (i >= 0) pl_fmt_ui(d, sizeof d, "at node %d (%.4g, %.4g, %.4g) %s", r->id[i], r->x[3 * i], r->x[3 * i + 1], r->x[3 * i + 2], mm ? "mm" : "m"); else d[0] = 0;
        ROW(ICON_ARROW_RIGHT, TH.accent, t, d);
    }
    if (r->vm && !modal) {
        double sm = mm ? r->vmMax : r->vmMax / 1e6;
        int i = r->vmMaxAt;
        pl_fmt_ui(t, sizeof t, "Largest von Mises stress %.4g MPa", sm);
        if (i >= 0) pl_fmt_ui(d, sizeof d, "at node %d (%.4g, %.4g, %.4g) %s; corners and point loads concentrate stress, so look at where it is", r->id[i],
                              r->x[3 * i], r->x[3 * i + 1], r->x[3 * i + 2], mm ? "mm" : "m"); else d[0] = 0;
        ROW(ICON_GAUGE, TH.accent, t, d);
        int lib = m.nMat ? ccx_lib_match(&m.mat[0], u) : -1;
        if (lib >= 0 && sm > 0) {
            double sf = CCX_LIB[lib].yield / 1e6 / sm;
            pl_fmt_ui(t, sizeof t, "Safety factor %.3g against yielding", sf);
            pl_fmt_ui(d, sizeof d, "%s yields at %.0f MPa; %s", CCX_LIB[lib].name, CCX_LIB[lib].yield / 1e6,
                      sf < 1 ? "the part yields: the linear answer no longer holds there" : sf < 1.5 ? "little margin for a structure (1.5 to 2 is usual)" : "the stress is well within the material's strength");
            ROW(sf < 1.5 ? ICON_TRIANGLE_ALERT : ICON_CIRCLE_CHECK, sf < 1.5 ? TH.warn : TH.ok, t, d);
        }
    }
    if (r->T) { pl_fmt_ui(t, sizeof t, "Temperature from %.4g to %.4g K at t = %.4g", r->tMin, r->tMax, r->time); ROW(ICON_THERMOMETER, TH.accent, t, ""); }
    /* the reactions against the loads */
    if (m.analysis == CCX_STATIC || m.analysis == CCX_COUPLED) {
        double applied[3] = { 0, 0, 0 };
        for (int i = 0; i < m.nLoad; i++) { int nn = ccx_set_nodes(&m, m.load[i].set); if (m.load[i].dof >= 1 && m.load[i].dof <= 3 && nn > 0) applied[m.load[i].dof - 1] += m.load[i].mag * nn; }
        if (r->rf) {
            double res = sqrt((r->rfSum[0] + applied[0]) * (r->rfSum[0] + applied[0]) + (r->rfSum[1] + applied[1]) * (r->rfSum[1] + applied[1]) + (r->rfSum[2] + applied[2]) * (r->rfSum[2] + applied[2]));
            double amag = sqrt(applied[0] * applied[0] + applied[1] * applied[1] + applied[2] * applied[2]);
            bool ok = amag > 0 ? res < 0.01 * amag : true;
            pl_fmt_ui(t, sizeof t, "Reactions at the supports (%.4g, %.4g, %.4g) N", r->rfSum[0], r->rfSum[1], r->rfSum[2]);
            pl_fmt_ui(d, sizeof d, ok ? "They balance the loads (%.4g, %.4g, %.4g) N: the model is in equilibrium" : "They don't balance the loads (%.4g, %.4g, %.4g) N: check for loads on held nodes or a pressure not counted here",
                      applied[0], applied[1], applied[2]);
            ROW(ok ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT, ok ? TH.ok : TH.warn, t, d);
        } else if (m.nBc) {
            ROW(ICON_INFO, TH.info, "Reactions not written", "Add RF to the node output to see the support forces and check they balance the loads.");
            float bw = ui_button_width(ICON_PLUS, "Add reaction forces to the output");
            if (ui_button(ui_id("ccx-add-rf"), (Rectangle){ x + 28, y, bw, 32 }, ICON_PLUS, "Add reaction forces to the output", BTN_SECONDARY)) {
                char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, input());
                char *txt = pl_read_file(p, NULL);
                char *k = txt ? strcasestr(txt, "*NODE FILE") : NULL;
                char *nl = k ? strchr(k, '\n') : NULL, *e2 = nl ? strchr(nl + 1, '\n') : NULL;
                if (nl && e2) {
                    Str o; str_init(&o);
                    str_catf(&o, "%.*s, RF%s", (int)(e2 - txt), txt, e2);
                    if (pl_write_file(p, o.s, o.len)) { ui_toast(TOAST_OK, "RF added to *NODE FILE in %s: run again to get the reactions", input()); load_setup(); }
                    str_free(&o);
                } else ui_toast(TOAST_ERR, "No *NODE FILE line with outputs in %s: add \"*NODE FILE\" and \"U, RF\" before *END STEP", input());
                free(txt);
            }
            y += 44;
        }
        CcxBeam bm;
        if (r->u && ccx_beam_check(&m, &bm)) {
            double fe = mm ? r->uMax : r->uMax * 1e3, hc = mm ? bm.delta : bm.delta * 1e3;
            pl_fmt_ui(t, sizeof t, "Hand calculation: tip deflection %.4g mm (FE %.4g mm, %+.1f %%)", hc, fe, (fe - hc) / hc * 100);
            pl_fmt_ui(d, sizeof d, "Euler-Bernoulli cantilever: delta = F L^3 / (3 E I), I = b h^3 / 12. Root stress M c / I = %.4g MPa (FE %.4g MPa: a root corner's is higher)",
                      mm ? bm.sigma : bm.sigma / 1e6, r->vm ? (mm ? r->vmMax : r->vmMax / 1e6) : 0.0);
            ROW(ICON_SIGMA, fabs(fe - hc) < 0.05 * hc ? TH.ok : TH.warn, t, d);
            static bool work;
            if (ui_button(ui_id("ccx-working"), (Rectangle){ x + 28, y, ui_button_width(work ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, "Show the working"), 30 },
                          work ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, "Show the working", BTN_GHOST)) work = !work;
            y += 36;
            if (work) {
                const char *lu = mm ? "mm" : "m", *su = mm ? "MPa" : "Pa";
                char wk[900];
                pl_fmt_ui(wk, sizeof wk, "F = %.4g N (the load on each node times the %s set's nodes), L = %.4g %s, b = %.4g %s, h = %.4g %s (the side the load bends), E = %.4g %s.\n"
                          "I = b h^3 / 12 = %.4g %s^4.\ndelta = F L^3 / (3 E I) = %.4g %s.\nsigma = M c / I = F L (h / 2) / I = %.4g %s.\n"
                          "Beam theory ignores shear and the fixed end's local restraint: within a few percent for a slender beam (L / h above 10).",
                          bm.F, m.load[0].set, bm.L, lu, bm.b, lu, bm.h, lu, bm.E, su, bm.I, lu, bm.delta, lu, bm.sigma, su);
                y += ui_text_wrap(F_MONO, 12.5f, wk, (Rectangle){ x + 28, y, w - 40, 200 }, 19, TH.text2, true) + 10;
            }
        }
    }
    {   /* a starter's promised answer against this run's (C4): .qc-starter "check = <umax|vmmax|f1> <SI value>" */
        char sp[PL_PATH]; pl_join(sp, sizeof sp, c->s.path, ".qc-starter");
        char *note = pl_read_file(sp, NULL), *ck = note ? strstr(note, "check = ") : NULL;
        char kind[16] = ""; double want = 0, got = -1;
        if (ck && sscanf(ck + 8, "%15s %lf", kind, &want) == 2 && want != 0) {
            const char *what = "", *unit = ""; double sc = 1;
            if (!strcmp(kind, "umax") && r->u && !modal) { got = mm ? r->uMax / 1e3 : r->uMax; what = "largest displacement"; unit = "mm"; sc = 1e3; }
            else if (!strcmp(kind, "vmmax") && r->vm && !modal) { got = mm ? r->vmMax * 1e6 : r->vmMax; what = "largest von Mises stress"; unit = "MPa"; sc = 1e-6; }
            else if (!strcmp(kind, "f1")) { const CcxRun *ru = ccx_run_get(c, false); if (ru->nModes) { got = ru->hz[0]; what = "first natural frequency"; unit = "Hz"; } }
            if (got >= 0) {
                double err = (got - want) / want * 100;
                pl_fmt_ui(t, sizeof t, "Compare with: the %s should be about %.4g %s; this run %.4g %s (%+.1f %%)", what, want * sc, unit, got * sc, unit, err);
                char *ex = strstr(note, "expect = "), *nl = ex ? strchr(ex, '\n') : NULL;
                if (ex) pl_fmt_ui(d, sizeof d, "%.*s", (int)(nl ? nl - ex - 9 : (long)strlen(ex + 9)), ex + 9); else d[0] = 0;
                ui_icon(fabs(err) < 5 ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT, x + 10, y + 13, 15, fabs(err) < 5 ? TH.ok : TH.warn);
                ui_text_clip(F_MED, 14, t, x + 28, y + 3, w - 40, TH.text);
                y += 26;
                if (d[0]) y += ui_text_wrap(F_REG, 12.5f, d, (Rectangle){ x + 28, y, w - 40, 100 }, 18, TH.text3, true);
                y += 12;
            }
        }
        free(note);
    }
    #undef ROW
    if (r->u && r->nEdges && !modal) {                       /* the deformed part, side on: across the long side, up the way it bends most */
        double ext[3]; for (int k = 0; k < 3; k++) ext[k] = m.hi[k] - m.lo[k];
        int ax = 0; for (int k = 1; k < 3; k++) if (ext[k] > ext[ax]) ax = k;
        int ay = ax == 0 ? 2 : 0;
        if (r->uMaxAt >= 0) { double best = -1; for (int k = 0; k < 3; k++) if (k != ax && fabs(r->u[3 * r->uMaxAt + k]) > best) { best = fabs(r->u[3 * r->uMaxAt + k]); ay = k; } }
        y += 8;
        y += deformed_view(r, ax, ay, (Rectangle){ x, y, w, fminf(320, w * 0.4f) }, mm) + 16;
    }
    ui_divider(x, y, w);
    return y + 16 - y0;
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
    if (prog() == PROG_CALCULIX) y += fea_results_panel(x, y, w);     /* a part study: the deformed part in 3-D (FEA_PLAN F3) */
    if (prog() == PROG_CALCULIX) y += ccx_results_panel(c, x, y, w);   /* the answer, in the app (C2) */
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
