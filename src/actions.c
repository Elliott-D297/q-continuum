/* actions.c -- the action registry (ROADMAP 2.1): every app-level command once, with its label, keyboard shortcut,
 * the button that does the same, an equivalent shell command where there is one, a line of help, when it can't run
 * (and why), and what it does. The keyboard shortcuts, the shortcut sheet (F1) and the "action <id>" script command
 * all go through it, and tests/actions_covered.sh fails for an action no journey exercises.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "actions.h"
#include "palette.h"
#include "undo.h"
#include "safety.h"

enum { M_CTRL = 1, M_SHIFT = 2, M_ALT = 4 };

/* ---------------------------------------------------------------- when they can't run */
static const char *no_case(void) { return A.cs ? NULL : "No case is open"; }
static const char *b_save(void) { return !A.cs ? "No case is open" : case_dirty_count(A.cs) == 0 ? "Nothing to save" : NULL; }
static const char *b_undo(void) { return !A.cs ? "No case is open" : undo_label(false) ? NULL : "Nothing to undo"; }
static const char *b_redo(void) { return !A.cs ? "No case is open" : undo_label(true) ? NULL : "Nothing to redo"; }
static bool running_here(void) { return A.cs && A.cj->active && !strcmp(A.cj->casePath, A.cs->s.path); }
static const char *b_run(void)
{
    if (!A.cs) return "No case is open";
    if (running_here()) return "It is running";
    if (app_jobs_active() >= MAX_JOBS) return "The most runs at once are going";
    return NULL;
}
static const char *b_stop(void) { return running_here() ? NULL : "Nothing is running in this case"; }
static const char *b_pause(void) { return !running_here() ? "Nothing is running in this case" : app_can_pause() ? NULL : "Only the solver step can pause"; }
static const char *b_resume(void) { return !A.cs ? "No case is open" : running_here() ? "It is running" : app_resumable(NULL) ? NULL : "The last run finished or never started"; }
static const char *b_clean(void) { return !A.cs ? "No case is open" : running_here() ? "Stop the run first" : NULL; }
static const char *b_mesh(void)
{
    if (!A.cs) return "No case is open";
    if (running_here()) return "Something is running in this case";
    char p[PL_PATH]; pl_join(p, sizeof p, A.cs->s.path, "system/blockMeshDict");
    return pl_is_file(p) ? NULL : "This case has no system/blockMeshDict";
}
static const char *b_log(void) { return !A.cs ? "No case is open" : A.page == PG_RUN ? NULL : "On the Run page"; }
static const char *b_hist(void) { return !A.cs ? "No case is open" : A.set.history ? NULL : "History is off in Settings"; }

/* ---------------------------------------------------------------- what they do */
static void a_save(void) { app_save_request(); }
static void a_undo(void) { undo_apply(false); }
static void a_redo(void) { undo_apply(true); }
static void a_run(void) { if (A.page == PG_RUN) A.runPending = true; else { app_go(PG_RUN); A.runPending = true; } }
static void a_stop(void) { job_stop(A.cj, GetTime()); }
static void a_pause(void) { app_pause_run(); }
static void a_resume(void) { app_resume_run(); app_go(PG_RUN); }
static void a_clean(void) { A.modal = MD_CLEAN; A.mFlag = A.mFlag2 = false; }
static void a_mesh(void) { app_generate_mesh(); }
static void a_away(void) { A.modal = MD_AWAY; }
static const char *b_away(void) { return !A.cs ? "No case is open" : A.cs->s.prog ? "Only OpenFOAM cases go to aerofly" : NULL; }
static void a_pv(void) { app_open_paraview(); }
static void a_pvc(void) { A.modal = MD_PV_CLAUDE; A.mText2[0] = 0; }
static void a_rescan(void) { app_rescan_cases(true); if (A.cs) case_rescan(A.cs); }
static void a_log(void) { A.logMax = !A.logMax; A.chartMax = false; }
static void a_chart(void) { A.chartMax = !A.chartMax; A.logMax = false; }
static void a_hist(void)
{
    app_save_all(true);
    if (hist_commit(A.cs->s.path, "Recorded by hand")) ui_toast(TOAST_OK, "Recorded the setup");
    else ui_toast(TOAST_INFO, "Nothing changed since the last record");
}
static void a_keys(void) { A.modal = MD_SHORTCUTS; }
static void a_computer(void) { palette_open(); }
static void a_news(void) { A.modal = MD_WHATS_NEW; }
/* F11: the window over the whole screen and back. Real full screen (_NET_WM_STATE_FULLSCREEN), which WSLg hands on to
   Windows: no caption, taskbar covered. Borderless windowed was not enough there, because WSLg drew its own title bar over it.
   The window takes the monitor's size first so XWayland has no mode to change (or scale), and gets its old size back after */
void app_fullscreen(bool on)
{
    static int ww = 1440, wh = 900;
    if (IsWindowHidden()) { A.set.fullscreen = on; return; }   /* --hidden test runs: the state only, never over the user's screen */
    if (on == IsWindowFullscreen()) return;
    int m = GetCurrentMonitor();
    if (on) {
        ww = GetScreenWidth(); wh = GetScreenHeight();
        SetWindowSize(GetMonitorWidth(m), GetMonitorHeight(m));
        ToggleFullscreen();
    } else {
        ToggleFullscreen();
        SetWindowSize(ww, wh);
    }
    A.set.fullscreen = IsWindowFullscreen();
    if (getenv("QC_DEBUG_FULLSCREEN"))
        fprintf(stderr, "fullscreen=%d monitor %d: %dx%d at %.0f,%.0f\n", A.set.fullscreen, m, GetMonitorWidth(m), GetMonitorHeight(m),
                GetMonitorPosition(m).x, GetMonitorPosition(m).y);
}
static void a_fullscreen(void) { app_fullscreen(!A.set.fullscreen); settings_save(&A.set); }
static void a_theme(void) { A.set.theme = A.set.theme == 0 ? 1 : 0; ui_set_theme(A.set.theme); settings_save(&A.set); }

#define GO(fn, pg) static void fn(void) { if (pg == PG_CASES) app_rescan_cases(true); app_go(pg); }
GO(g_cases, PG_CASES) GO(g_new, PG_NEW) GO(g_queue, PG_QUEUE) GO(g_studies, PG_STUDIES) GO(g_coupling, PG_COUPLING)
GO(g_settings, PG_SETTINGS) GO(g_overview, PG_OVERVIEW) GO(g_mesh, PG_MESH) GO(g_bc, PG_BC)
GO(g_physics, PG_PHYSICS) GO(g_numerics, PG_NUMERICS) GO(g_runpg, PG_RUN) GO(g_files, PG_FILES) GO(g_history, PG_HISTORY)
#undef GO

static const Action ACTIONS[] = {
    /* id               label                     shortcut         key            mods            widget          shell                      help */
    { "case.save",      "Save",                   "Ctrl+S",        KEY_S,         M_CTRL,         "top-save",     NULL,
      "Write every edited dictionary to disk", b_save, a_save, true },
    { "edit.undo",      "Undo",                   "Ctrl+Z",        KEY_Z,         M_CTRL,         "top-undo",     NULL,
      "Take back the last dictionary edit", b_undo, a_undo, false },
    { "edit.redo",      "Redo",                   "Ctrl+Y",        KEY_Y,         M_CTRL,         "top-redo",     NULL,
      "Do the undone edit again (also Ctrl+Shift+Z)", b_redo, a_redo, false },
    { "edit.redo2",     NULL,                     "Ctrl+Shift+Z",  KEY_Z,         M_CTRL | M_SHIFT, "top-redo",   NULL,
      NULL, b_redo, a_redo, false },
    { "run.start",      "Run case",               "Ctrl+R",        KEY_R,         M_CTRL,         "run-go",       "./Allrun, or the steps listed under Commands",
      "Run the case with the steps on the Run page (asks first when there are results)", b_run, a_run, false },
    { "run.pause",      "Pause run",              NULL,            0,             0,              "run-pause",    NULL,
      "The solver writes the time step it is on and stops; Resume continues", b_pause, a_pause, false },
    { "run.resume",     "Resume run",             NULL,            0,             0,              "run-resume",   NULL,
      "Continue a paused or interrupted run from its last written time", b_resume, a_resume, false },
    { "run.stop",       "Stop run",               NULL,            0,             0,              "run-stop",     "kill -TERM -<process group>",
      "Stop the run now, without writing", b_stop, a_stop, false },
    { "mesh.generate",  "Generate mesh",          "Ctrl+M",        KEY_M,         M_CTRL,         "mesh-gen",     "blockMesh",
      "Build the mesh from system/blockMeshDict", b_mesh, a_mesh, false },
    { "case.clean",     "Clean results",          NULL,            0,             0,              "top-clean",    "foamListTimes -rm; rm log.*",
      "Move the results and logs to the trash (asks first)", b_clean, a_clean, false },
    { "case.away",      "Prepare for the cluster", NULL,           0,             0,              "run-away",     "tar czf case.tar.gz; scp; sbatch",
      "Pack the case for aerofly (v2412, Slurm): checked with v2412 here, a batch script, the commands to copy", b_away, a_away, false },
    { "view.paraview",  "Open in ParaView",       "Ctrl+P",        KEY_P,         M_CTRL,         "top-pv",       "paraFoam",
      "Open the case in ParaView", no_case, a_pv, false },
    { "view.claude",    "ParaView with Claude",   NULL,            0,             0,              "top-pvc",      NULL,
      "Claude Code builds a ParaView view of the case for you", no_case, a_pvc, false },
    { "log.expand",     "Expand the log",         "Ctrl+L",        KEY_L,         M_CTRL,         "log-max",      NULL,
      "Give the log the whole page, or put it back", b_log, a_log, false },
    { "chart.expand",   "Expand the residuals",   "Ctrl+Shift+L",  KEY_L,         M_CTRL | M_SHIFT, "chart-max",  NULL,
      "Give the residuals chart the whole page, or put it back", b_log, a_chart, false },
    { "history.record", "Record the setup",       NULL,            0,             0,              "h-rec",        "git -C .qc-history commit",
      "Record the case setup in its history now", b_hist, a_hist, false },
    { "cases.rescan",   "Look for cases again",   "F5",            KEY_F5,        0,              NULL,           NULL,
      "Read the workspace and the open case from disk again", NULL, a_rescan, true },
    { "view.fullscreen", "Full screen",           "F11",           KEY_F11,       0,              NULL,           NULL,
      "The window over the whole screen, or back to a window (F11 again)", NULL, a_fullscreen, false },
    { "view.theme",     "Switch style",           NULL,            0,             0,              NULL,           NULL,
      "Between the LCARS and the classic look (Settings has all three)", NULL, a_theme, false },
    { "help.computer",  "Computer",               "Ctrl+K",        KEY_K,         M_CTRL,         NULL,           NULL,
      "Search every command, page, case, patch, field and keyword; \"deltaT = 0.001\" sets a value", NULL, a_computer, true },
    { "help.keys",      "Keyboard shortcuts",     "F1",            KEY_F1,        0,              NULL,           NULL,
      "This list", NULL, a_keys, true },
    { "help.whatsnew",  "What's new",             NULL,            0,             0,              NULL,           NULL,
      "What changed in this update", NULL, a_news, false },
    { "go.cases",       "Cases",                  "Ctrl+0",        KEY_ZERO,      M_CTRL,         "nav:cases",    NULL, "Your cases and the tutorials", NULL, g_cases, false },
    { "go.overview",    "Overview",               "Ctrl+1",        KEY_ONE,       M_CTRL,         "nav:overview", NULL, "The open case at a glance", no_case, g_overview, false },
    { "go.mesh",        "Mesh",                   "Ctrl+2",        KEY_TWO,       M_CTRL,         "nav:mesh",     NULL, "Mesh dictionaries and generation", no_case, g_mesh, false },
    { "go.bc",          "Boundary conditions",    "Ctrl+3",        KEY_THREE,     M_CTRL,         "nav:boundary conditions", NULL, "Every field on every patch", no_case, g_bc, false },
    { "go.physics",     "Physics",                "Ctrl+4",        KEY_FOUR,      M_CTRL,         "nav:physics",  NULL, "Models and properties", no_case, g_physics, false },
    { "go.numerics",    "Numerics",               "Ctrl+5",        KEY_FIVE,      M_CTRL,         "nav:numerics", NULL, "Schemes and solvers", no_case, g_numerics, false },
    { "go.run",         "Run",                    "Ctrl+6",        KEY_SIX,       M_CTRL,         "nav:run",      NULL, "Run, watch and stop", no_case, g_runpg, false },
    { "go.files",       "Files",                  "Ctrl+7",        KEY_SEVEN,     M_CTRL,         "nav:files",    NULL, "Every file of the case, with an editor", no_case, g_files, false },
    { "go.history",     "History",                "Ctrl+8",        KEY_EIGHT,     M_CTRL,         "nav:history",  NULL, "Earlier setups, snapshots and the trash", no_case, g_history, false },
    { "go.new",         "New case",               "Ctrl+N",        KEY_N,         M_CTRL,         "nav:new case", NULL, "Start from a tutorial", NULL, g_new, false },
    { "go.queue",       "Queue",                  NULL,            0,             0,              "nav:queue",    NULL, "Runs waiting for free cores", NULL, g_queue, false },
    { "go.studies",     "Studies",                NULL,            0,             0,              "nav:studies",  NULL, "Variants of a case, compared", NULL, g_studies, false },
    { "go.coupling",    "Coupling",               NULL,            0,             0,              "nav:coupling", NULL, "preCICE coupled runs", NULL, g_coupling, false },
    { "go.settings",    "Settings",               "Ctrl+,",        KEY_COMMA,     M_CTRL,         "nav:settings", NULL, "Environment, style, sounds", NULL, g_settings, false },
};
#define N_ACTIONS ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

int actions_count(void) { return N_ACTIONS; }
const Action *actions_get(int i) { return i >= 0 && i < N_ACTIONS ? &ACTIONS[i] : NULL; }
const Action *actions_find(const char *id)
{
    for (int i = 0; i < N_ACTIONS; i++) if (!strcmp(ACTIONS[i].id, id)) return &ACTIONS[i];
    return NULL;
}
const char *action_blocked(const Action *a) { return a->blocked ? a->blocked() : NULL; }

bool action_run(const Action *a, bool quiet)
{
    const char *why = action_blocked(a);
    if (why) {
        if (!quiet) { ui_toast(TOAST_INFO, "%s: %s", a->label ? a->label : a->id, why); app_sound(SND_DENIED); }
        return false;
    }
    a->run();
    return true;
}

void actions_keys(void)
{
    if (A.modal != MD_NONE && A.modal != MD_SHORTCUTS) return;
    int mods = (inp_key_down(KEY_LEFT_CONTROL) || inp_key_down(KEY_RIGHT_CONTROL) ? M_CTRL : 0) |
               (inp_key_down(KEY_LEFT_SHIFT) || inp_key_down(KEY_RIGHT_SHIFT) ? M_SHIFT : 0) |
               (inp_key_down(KEY_LEFT_ALT) || inp_key_down(KEY_RIGHT_ALT) ? M_ALT : 0);
    bool typing = ui_any_input_focused() || text_editor_focused();
    for (int i = 0; i < N_ACTIONS; i++) {
        const Action *a = &ACTIONS[i];
        if (!a->key || a->mods != mods || !inp_key_pressed(a->key)) continue;
        if (typing && !a->whileTyping) continue;               /* the text field or editor has the keys */
        if (A.modal == MD_SHORTCUTS && a->run != a_keys) A.modal = MD_NONE;
        action_run(a, false);
        return;
    }
}

/* the F1 sheet */
void actions_sheet(bool *close)
{
    Rectangle b = ui_modal_begin("Keyboard shortcuts", "Every command of the app; greyed ones can't run now, and say why.", 760, 600, close);
    static UiScroll sc;
    float top = ui_scroll_begin(&sc, b);
    float y = top + 4;
    for (int i = 0; i < N_ACTIONS; i++) {
        const Action *a = &ACTIONS[i];
        if (!a->label) continue;                               /* a second shortcut of the action before */
        const char *why = action_blocked(a);
        char keys[48] = "";
        if (a->shortcut) pl_strlcpy(keys, a->shortcut, sizeof keys);
        if (i + 1 < N_ACTIONS && !ACTIONS[i + 1].label && ACTIONS[i + 1].shortcut) {
            size_t L = strlen(keys); snprintf(keys + L, sizeof keys - L, "%s%s", L ? ", " : "", ACTIONS[i + 1].shortcut);
        }
        Rectangle row = { b.x, y, b.width - 12, 40 };
        bool hov = ui_hovered(row);
        if (hov) ui_rect(row, 7, TH.hover);
        if (keys[0]) {
            float kw = ui_text_w(F_MONO, 12, keys) + 14;
            ui_rect((Rectangle){ b.x + 6, y + 9, kw, 22 }, 5, TH.surface2);
            ui_text(F_MONO, 12, keys, b.x + 13, y + 13, why ? TH.text3 : TH.text);
        }
        ui_text(F_MED, 13, strcmp(a->id, "help.computer") ? a->label : term(T_PALETTE), b.x + 160, y + 4, why ? TH.text3 : TH.text);
        char sub[300];
        pl_fmt_ui(sub, sizeof sub, "%s%s%s", why ? why : (a->help ? a->help : ""), a->shell ? "  ·  shell: " : "", a->shell ? a->shell : "");
        ui_text_clip(F_REG, 12, sub, b.x + 160, y + 22, b.width - 190, why ? TH.text3 : TH.text2);
        if (!why && ui_clicked(row)) { A.modal = MD_NONE; action_run(a, false); }   /* it may open a dialog of its own */
        y += 42;
    }
    ui_scroll_end(&sc, y - top + 8);
}
