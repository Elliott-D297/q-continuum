/* page_cases.c -- case list and "new case from tutorial" */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "plume.h"
#include "program.h"

static void fmt_int(char *out, size_t cap, long v)
{
    if (v < 0) { snprintf(out, cap, "-"); return; }
    char tmp[32]; snprintf(tmp, sizeof tmp, "%ld", v);
    int n = (int)strlen(tmp), k = 0;
    char res[48];
    for (int i = 0; i < n; i++) {
        res[k++] = tmp[i];
        if ((n - i - 1) % 3 == 0 && i < n - 1) res[k++] = ',';
    }
    res[k] = 0;
    snprintf(out, cap, "%s", res);
}

static void status_chip(float x, float y, CaseStatus s, bool right, bool program)
{
    const char *t = program && s == CS_NEW ? "Not run yet" : status_name(s);
    Color c = status_color(s);
    if (TH.lcars) {
        float bw = ui_badge_width(NULL, t);
        if (right) x -= bw;
        float pulse = s == CS_RUNNING ? 0.6f + 0.4f * sinf((float)ui_time() * 5) : 1.0f;
        ui_badge(x, y, NULL, t, ui_alpha(c, pulse), BLANK);
        return;
    }
    float w = ui_text_w(F_MED, 12, t) + 24;
    if (right) x -= w;
    ui_rect((Rectangle){ x, y, w, 22 }, 11, ui_alpha(c, 0.14f));
    float pulse = s == CS_RUNNING ? 0.5f + 0.5f * sinf((float)ui_time() * 5) : 1.0f;
    DrawCircleV((Vector2){ x + 10, y + 11 }, 3.5f, ui_alpha(c, pulse));
    ui_text(F_MED, 12, t, x + 18, y + 4, c);
}

/* ============================================================================ case list */
static void case_card(Rectangle r, const CaseSummary *s, int idx)
{
    UiId id = ui_idn(ui_id("case-card"), idx);
    bool hov = ui_hovered(r);
    float h = ui_anim(id, hov ? 1.f : 0.f, 7.f);
    bool other = s->prog != PROG_OPENFOAM;
    Color cc = other ? prog_color(s->prog) : cat_color(s->cat);
    const char *ic = other ? prog_icon(s->prog) : cat_icon(s->cat);
    if (TH.lcars) {
        lc_panel(r, cc, h * 0.6f, false);
        r.x += 16; r.width -= 16;
        ui_icon(ic, r.x + 36, r.y + 36, 20, cc);
    } else {
        if (h > 0.01f) ui_shadow(r, 12, h);
        ui_rect(r, 12, TH.surface);
        ui_rect_lines(r, 12, 1, ui_mix(TH.lineSoft, TH.line, h));
        Rectangle ib = { r.x + 18, r.y + 18, 36, 36 };
        ui_rect(ib, 9, ui_alpha(cc, 0.16f));
        ui_icon(ic, ib.x + 18, ib.y + 18, 18, cc);
    }
    status_chip(r.x + r.width - 16, r.y + 20, s->status, true, s->prog != PROG_OPENFOAM);
    ui_text_clip(F_SEMI, TH.lcars ? 18 : 15.5f, s->name, r.x + 66, r.y + (TH.lcars ? 16 : 17), r.width - 66 - 110, TH.lcars ? cc : TH.text);
    char line[160];
    if (other) pl_fmt_ui(line, sizeof line, "%s%s%s", prog_info(s->prog)->name, s->input[0] ? "  ·  " : "", s->input);
    else pl_fmt_ui(line, sizeof line, "%s%s", s->app[0] ? s->app : "No application set", "  ·  OpenFOAM");
    if (flavour_case_tag(s->path)) { char t2[160]; pl_fmt_ui(t2, sizeof t2, "%s  ·  %s", flavour_case_tag(s->path), line); pl_strlcpy(line, t2, sizeof line); }
    ui_text_clip(F_REG, 13, line, r.x + 66, r.y + 38, r.width - 80, TH.text3);

    char cells[48];
    float y = r.y + 76;
    float mx = r.x + r.width * 0.5f;
    if (other) {
        ui_icon(ICON_FILE_TEXT, r.x + 26, y + 9, 14, TH.text3);
        pl_fmt_ui(line, sizeof line, "%d %s%s", s->nInputs, prog_info(s->prog)->inputWord, s->nInputs == 1 ? "" : "s");
        ui_text_clip(F_REG, 13, line, r.x + 40, y, r.width * 0.5f - 50, TH.text2);
        ui_icon(ICON_BOXES, mx + 8, y + 9, 14, TH.text3);
        pl_fmt_ui(line, sizeof line, s->nTimes ? "%d result%s" : "No results yet", s->nTimes, s->nTimes == 1 ? "" : "s");
        ui_text_clip(F_REG, 13, line, mx + 22, y, r.width * 0.5f - 34, TH.text2);
    } else {
        fmt_int(cells, sizeof cells, s->nCells);
        ui_icon(ICON_GRID_3X3, r.x + 26, y + 9, 14, TH.text3);
        if (s->meshed) snprintf(line, sizeof line, s->nCells >= 0 ? "%s cells" : "Meshed (decomposed)", cells);
        else snprintf(line, sizeof line, "No mesh yet");
        ui_text(F_REG, 13, line, r.x + 40, y, TH.text2);
        ui_icon(ICON_CLOCK, mx + 8, y + 9, 14, TH.text3);
        if (s->latest[0]) snprintf(line, sizeof line, "t = %s of %g", s->latest, s->endTime);
        else snprintf(line, sizeof line, "End time %g", s->endTime);
        ui_text_clip(F_REG, 13, line, mx + 22, y, r.width * 0.5f - 34, TH.text2);
    }

    ui_divider(r.x + 18, r.y + r.height - 42, r.width - 36);
    char ago[48]; pl_time_ago(ago, sizeof ago, s->activity);
    char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
    const char *shown = pl_starts_with(s->path, ws) ? s->path + strlen(ws) + 1 : s->path;
    ui_text_clip(F_REG, 12.5f, shown, r.x + 18, r.y + r.height - 30, r.width - 36 - ui_text_w(F_REG, 12.5f, ago) - 16, TH.text3);
    ui_text(F_REG, 12.5f, ago, r.x + r.width - 18 - ui_text_w(F_REG, 12.5f, ago), r.y + r.height - 30, TH.text3);
    Rectangle del = { r.x + r.width - 46, r.y + 44, 30, 30 };
    bool delClicked = false;
    if (hov) {                                                  /* delete: only while hovering, never by a stray click on the card */
        delClicked = ui_button(ui_idn(ui_id("case-del"), idx), del, ICON_TRASH_2, NULL, BTN_ICON);
        ui_tooltip(del, "Delete this case...");
    }
    if (delClicked) app_ask_delete_case(s->path);
    else if (ui_clicked(r) && !ui_hovered(del)) app_open_case(s->path, PG_OVERVIEW);
    if (hov && !ui_hovered(del)) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
}

static bool cat_matches(const CaseSummary *s)
{
    if (A.catFilter > 0 && s->prog != A.catFilter - 1) return false;
    if (s->prog != PROG_OPENFOAM || A.catFilter != 1 || A.physFilter == 0) return true;
    if (A.physFilter == 4) return s->cat == CAT_COMP || s->cat == CAT_OTHER;
    return (int)s->cat == A.physFilter - 1;
}

void page_cases(Rectangle r)
{
    app_rescan_cases(false);
    UiScroll *sc = &A.scr[0];
    float top = ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = r.width - 2 * PAGE_PAD;
    float y = top + 30;

    char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
    char sub[PL_PATH + 400];
    pl_fmt_ui(sub, sizeof sub, "OpenFOAM cases in %s, and the projects of the other programs (DEME, CalculiX, MOOSE, code_aster, "
             "code_saturne, Elmer, SPARTA, Chrono) found there and in your home folder. Open one to set it up, run it and watch it.", ws);
    float bw1 = ui_button_width(ICON_FOLDER_PLUS, "New case"), bw2 = ui_button_width(ICON_FOLDER_OPEN, "Open folder");
    float bw3 = ui_button_width(ICON_BOX, "Mesh an STL");
    float hh = page_header(x, y, w - bw1 - bw2 - bw3 - 40, "Cases", sub);
    if (ui_button(ui_id("cases-stl"), (Rectangle){ x + w - bw1 - bw2 - bw3 - 20, y + 2, bw3, 36 }, ICON_BOX, "Mesh an STL", BTN_SECONDARY)) stlmesh_new_case();
    if (ui_button(ui_id("cases-new"), (Rectangle){ x + w - bw1, y + 2, bw1, 36 }, ICON_FOLDER_PLUS, "New case", BTN_PRIMARY)) app_go(PG_NEW);
    if (ui_button(ui_id("cases-open"), (Rectangle){ x + w - bw1 - bw2 - 10, y + 2, bw2, 36 }, ICON_FOLDER_OPEN, "Open folder", BTN_SECONDARY)) {
        A.modal = MD_OPEN_FOLDER; A.mText[0] = 0; A.mErr[0] = 0;
    }
    y += hh;

    if (!env_active(&A.env, &A.set)) {
        const char *msg = "Q-Continuum looks for OpenFOAM in /usr/lib/openfoam. Install v2606 with the commands in Settings, "
                          "or point Settings at another etc/bashrc.";
        float ch = callout_height(w, "OpenFOAM is not installed", msg);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "OpenFOAM is not installed", msg);
        Rectangle b = { x + w - 150, y + 12, 136, 30 };
        if (ui_button(ui_id("cases-setup"), b, ICON_SETTINGS, "Open Settings", BTN_SECONDARY)) app_go(PG_SETTINGS);
        y += ch + 20;
    }

    ui_input(ui_id("cases-search"), (Rectangle){ x, y, 300, 36 }, A.search, sizeof A.search, "Search cases", INPUT_LIVE | INPUT_SEARCH);
    /* a pill per program that has projects here (and always OpenFOAM) */
    {
        int count[PROG_COUNT] = { 0 };
        for (int i = 0; i < A.nList; i++) if (A.list[i].prog >= 0 && A.list[i].prog < PROG_COUNT) count[A.list[i].prog]++;
        float px = x + 316;
        for (int k = -1; k < PROG_COUNT; k++) {
            if (k > 0 && !count[k] && A.catFilter != k + 1) continue;
            char lab[64];
            if (k < 0) pl_fmt_ui(lab, sizeof lab, "All  %d", A.nList);
            else pl_fmt_ui(lab, sizeof lab, "%s  %d", prog_info(k)->name, count[k]);
            float pw = ui_text_w(F_MED, 13, lab) + (k < 0 ? 28 : 46);
            if (px + pw > x + w) { px = x + 316; y += 42; }
            Rectangle pr = { px, y + 2, pw, 32 };
            bool on = A.catFilter == k + 1, hov = ui_hovered(pr);
            if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
            Color pc = k < 0 ? TH.accent : (k == PROG_OPENFOAM ? TH.accent : prog_color(k));
            ui_rect(pr, 16, on ? pc : hov ? TH.hover : TH.surface);
            if (!on) ui_rect_lines(pr, 16, 1, TH.lineSoft);
            if (k >= 0) ui_icon(k == PROG_OPENFOAM ? ICON_DROPLETS : prog_icon(k), px + 18, y + 18, 14, on ? TH.textOnAccent : pc);
            ui_text(F_MED, 13, lab, px + (k < 0 ? 14 : 32), y + 9, on ? TH.textOnAccent : TH.text2);
            char rid[48]; pl_fmt_ui(rid, sizeof rid, "cases-prog:%s", k < 0 ? "all" : prog_info(k)->key);
            ui_register(ui_id(rid), pr, lab);
            if (ui_clicked(pr)) { A.catFilter = k + 1; A.physFilter = 0; }
            px += pw + 8;
        }
    }
    y += 50;
    if (A.catFilter == 1) {                                     /* OpenFOAM: by physics too */
        static const char *cats[] = { "All physics", "Incompressible", "Heat transfer", "Multiphase", "Other" };
        ui_segmented(ui_id("cases-cat"), (Rectangle){ x, y, fminf(620, w), 34 }, cats, 5, &A.physFilter);
        y += 48;
    }
    y += 6;

    /* nothing at all: say what to do first */
    if (A.nList == 0) {
        Rectangle e = { x, y, w, 230 };
        ui_rect(e, 14, TH.surface);
        ui_rect_lines(e, 14, 1, TH.lineSoft);
        ui_icon(ICON_FOLDER_OPEN, e.x + e.width / 2, e.y + 56, 34, TH.text3);
        ui_text_center(F_SEMI, 17, ui_cap_words("No cases here yet"), (Rectangle){ e.x, e.y + 86, e.width, 24 }, TH.text);
        float dw = fminf(560, e.width - 40);
        ui_text_wrap(F_REG, 13.5f, "Start from an OpenFOAM tutorial (the lid-driven cavity is a good first case) or a starter for DEME, "
                     "CalculiX, MOOSE, Elmer, SPARTA or code_aster, or open a folder you already have.",
                     (Rectangle){ e.x + (e.width - dw) / 2, e.y + 118, dw, 60 }, 20, TH.text2, true);
        float b1 = ui_button_width(ICON_LIBRARY, "Start a new case"), b2 = ui_button_width(ICON_FOLDER_OPEN, "Open a case folder");
        float bx = e.x + (e.width - b1 - b2 - 12) / 2;
        if (ui_button(ui_id("empty-new"), (Rectangle){ bx, e.y + 168, b1, 36 }, ICON_LIBRARY, "Start a new case", BTN_PRIMARY)) app_go(PG_NEW);
        if (ui_button(ui_id("empty-open"), (Rectangle){ bx + b1 + 12, e.y + 168, b2, 36 }, ICON_FOLDER_OPEN, "Open a case folder", BTN_SECONDARY)) {
            A.modal = MD_OPEN_FOLDER; A.mText[0] = 0; A.mErr[0] = 0;
        }
        y += 250;
    }

    int shown = 0;
    float gap = 18, minW = 330;
    int cols = (int)((w + gap) / (minW + gap));
    if (cols < 1) cols = 1;
    float cw = (w - gap * (cols - 1)) / cols, ch = 152;
    for (int i = 0; i < A.nList; i++) {
        const CaseSummary *s = &A.list[i];
        if (!cat_matches(s)) continue;
        if (A.search[0] && !pl_contains_ci(s->name, A.search) && !pl_contains_ci(s->app, A.search) && !pl_contains_ci(s->path, A.search) &&
            !pl_contains_ci(s->input, A.search)) continue;
        int col = shown % cols, row = shown / cols;
        Rectangle cr = { x + col * (cw + gap), y + row * (ch + gap), cw, ch };
        if (cr.y + ch > r.y && cr.y < r.y + r.height) case_card(cr, s, i);
        shown++;
    }
    int rows = (shown + cols - 1) / cols;
    y += rows * (ch + gap);

    if (shown == 0 && A.nList) {                          /* cases exist, none match */
        Rectangle e = { x, y, w, 194 };
        ui_rect(e, 14, TH.surface);
        ui_rect_lines(e, 14, 1, TH.lineSoft);
        ui_icon(ICON_SEARCH, e.x + e.width / 2, e.y + 50, 30, TH.text3);
        ui_text_center(F_SEMI, 17, ui_cap_words("No cases match"), (Rectangle){ e.x, e.y + 78, e.width, 24 }, TH.text);
        ui_text_center(F_REG, 13.5f, "Try another search or category.", (Rectangle){ e.x, e.y + 108, e.width, 22 }, TH.text2);
        float bw = ui_button_width(ICON_X, "Show all cases");
        if (ui_button(ui_id("cases-clear"), (Rectangle){ e.x + (e.width - bw) / 2, e.y + 140, bw, 34 }, ICON_X, "Show all cases", BTN_SECONDARY)) {
            A.search[0] = 0; A.catFilter = 0; A.physFilter = 0;
        }
        y += 214;
    }
    ui_scroll_end(sc, y - top + 40);
}

/* ============================================================================ new case */
typedef struct { const char *suffix[3]; const char *title; const char *desc; } Starter;
static const Starter STARTERS[] = {
    { { "incompressible/icoFoam/cavity/cavity", "incompressibleFluid/cavity", NULL }, "Lid-driven cavity",
      "A moving lid drives a laminar vortex in a square box. The classic first case." },
    { { "incompressible/simpleFoam/pitzDaily", "incompressibleFluid/pitzDailySteady", NULL }, "Backward-facing step",
      "Steady turbulent flow over a step, solved with the k-epsilon model." },
    { { "heatTransfer/buoyantSimpleFoam/buoyantCavity", "fluid/buoyantCavity", NULL }, "Heated cavity",
      "Natural convection in a tall cavity between a hot and a cold wall." },
    { { "heatTransfer/buoyantSimpleFoam/hotRadiationRoom", "fluid/hotRadiationRoom", NULL }, "Radiating room, P1",
      "A hot box heats a room by convection and thermal radiation, using the P1 model." },
    { { "heatTransfer/buoyantSimpleFoam/hotRadiationRoomFvDOM", "fluid/hotRadiationRoomFvDOM", NULL }, "Radiating room, fvDOM",
      "The same room with discrete-ordinates radiation: one transport equation per ray direction." },
    { { "multiphase/interFoam/laminar/damBreak/damBreak", "incompressibleVoF/damBreakLaminar/damBreak", NULL }, "Dam break",
      "A water column collapses under gravity, tracked with the volume-of-fluid method." },
};
#define N_STARTERS ((int)(sizeof STARTERS / sizeof STARTERS[0]))

static void ensure_tutorials(void)
{
    if (A.tutScanned) return;
    char dir[PL_PATH]; env_tutorials_dir(&A.env, &A.set, dir, sizeof dir);
    const char *over = getenv("QC_TUTORIALS");
    if (over && *over) pl_strlcpy(dir, over, sizeof dir);
    tutorials_scan(&A.tut, dir);
    A.tutScanned = true;
    A.tutSel = -1;
}

static void select_tutorial(int i)
{
    A.tutSel = i;
    A.cloneErr[0] = 0;
    if (i >= 0) {
        char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
        char cand[256]; snprintf(cand, sizeof cand, "%s", A.tut.t[i].name);
        for (int k = 2; k < 100; k++) {
            char p[PL_PATH]; pl_join(p, sizeof p, ws, cand);
            if (!pl_exists(p)) break;
            snprintf(cand, sizeof cand, "%s-%d", A.tut.t[i].name, k);
        }
        pl_strlcpy(A.cloneName, cand, sizeof A.cloneName);
    }
}

static void do_clone(void)
{
    if (A.tutSel < 0) return;
    const Tutorial *t = &A.tut.t[A.tutSel];
    pl_trim_inplace(A.cloneName);
    if (!A.cloneName[0] || strchr(A.cloneName, '/') || A.cloneName[0] == '.') {
        snprintf(A.cloneErr, sizeof A.cloneErr, "Use a plain folder name without '/'.");
        return;
    }
    const char *bad = foam_path_problem(A.cloneName);
    if (bad) { snprintf(A.cloneErr, sizeof A.cloneErr, "OpenFOAM can't run a case whose name has %s. Try %s with - or _ instead.", bad, "letters and digits"); return; }
    char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
    if ((bad = foam_path_problem(ws))) { snprintf(A.cloneErr, sizeof A.cloneErr, "The workspace folder's path has %s, which OpenFOAM can't run in. Choose another one in Settings.", bad); return; }
    char dst[PL_PATH]; pl_join(dst, sizeof dst, ws, A.cloneName);
    if (pl_exists(dst)) { pl_fmt_ui(A.cloneErr, sizeof A.cloneErr, "%s already exists. Pick another name.", A.cloneName); return; }
    if (!pl_mkdirs(ws)) { pl_fmt_ui(A.cloneErr, sizeof A.cloneErr, "Could not create the workspace folder %s.", ws); return; }
    char src[PL_PATH]; pl_join(src, sizeof src, A.tut.root, t->rel);
    char why[300];
    bool group = tutorial_needs_group(src, why, sizeof why);
    char from[PL_PATH];
    if (group) pl_dirname(from, sizeof from, src); else pl_strlcpy(from, src, sizeof from);
    char qs[PL_PATH + 8], qd[PL_PATH + 8], cmd[PL_PATH * 3 + 64];
    pl_shell_quote(qs, sizeof qs, from);
    pl_shell_quote(qd, sizeof qd, dst);
    if (!pl_fmt(cmd, sizeof cmd, "cp -a %s %s && chmod -R u+w %s", qs, qd, qd)) { snprintf(A.cloneErr, sizeof A.cloneErr, "The paths are too long to copy."); return; }
    if (pl_run_wait(cmd, NULL, 0) != 0) { snprintf(A.cloneErr, sizeof A.cloneErr, "Copying the tutorial failed."); return; }
    char open[PL_PATH];
    if (group) {
        pl_join(open, sizeof open, dst, pl_basename(src));
        ui_toast(TOAST_OK, "Created %s with its whole tutorial group: %s", A.cloneName, why);
    } else {
        pl_strlcpy(open, dst, sizeof open);
        ui_toast(TOAST_OK, "Created %s from %s", A.cloneName, t->rel);
    }
    app_rescan_cases(true);
    app_open_case(open, PG_OVERVIEW);
}

static void starter_card(Rectangle r, int i, int ti)
{
    const Starter *s = &STARTERS[i];
    const Tutorial *t = &A.tut.t[ti];
    UiId id = ui_idn(ui_id("starter"), i);
    bool sel = A.tutSel == ti;
    bool hov = ui_hovered(r);
    float h = ui_anim(id, hov ? 1.f : 0.f, 7.f);
    Color cc = cat_color(t->cat);
    if (TH.lcars) { lc_panel(r, cc, h * 0.6f, sel); r.x += 16; r.width -= 16; }
    else {
        ui_rect(r, 12, TH.surface);
        ui_rect_lines(r, 12, sel ? 1.5f : 1, sel ? TH.accent : ui_mix(TH.lineSoft, TH.line, h));
    }
    ui_icon(cat_icon(t->cat), r.x + 26, r.y + 28, 18, cc);
    ui_text_clip(F_SEMI, 15, s->title, r.x + 46, r.y + 18, r.width - 64, TH.text);
    ui_text_wrap(F_REG, 13, s->desc, (Rectangle){ r.x + 18, r.y + 50, r.width - 36, 60 }, 19, TH.text2, true);
    ui_badge(r.x + 18, r.y + r.height - 34, NULL, t->solver, TH.lcars ? cc : TH.text2, TH.lineSoft);
    if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    if (ui_clicked(r)) select_tutorial(ti);
}

/* ---------------------------------------------------------------- 4.4 the impinging-plume starter */
static struct { bool init; PlumeIn in; char f[12][32]; char name[96]; char err[300]; int gas; } PL;

static void plume_field(int i, const char *label, const char *unit, float x, float y, float w)
{
    ui_text(F_MED, 13, label, x, y + 9, TH.text2);
    ui_input(ui_idn(ui_id("pl-f"), i), (Rectangle){ x + w - 170, y, 120, 34 }, PL.f[i], sizeof PL.f[i], NULL, INPUT_MONO | INPUT_LIVE | INPUT_NUMERIC);
    ui_text(F_REG, 12.5f, unit, x + w - 42, y + 10, TH.text3);
}

void plume_modal(bool *close)
{
    if (!PL.init) {
        PL.init = true; plume_defaults(&PL.in);
        double v[11] = { PL.in.Me, PL.in.pe, PL.in.Te, PL.in.re, PL.in.H, PL.in.R, PL.in.pa, PL.in.Ta, PL.in.Tw, PL.in.cellsPerRadius, PL.in.maxCo };
        for (int i = 0; i < 11; i++) snprintf(PL.f[i], sizeof PL.f[i], "%g", v[i]);
        snprintf(PL.f[11], sizeof PL.f[11], "%g", PL.in.molWeight);
        snprintf(PL.name, sizeof PL.name, "plume");
    }
    PlumeIn in = PL.in;
    in.gas = PL.gas;
    double *dst[11] = { &in.Me, &in.pe, &in.Te, &in.re, &in.H, &in.R, &in.pa, &in.Ta, &in.Tw, NULL, &in.maxCo };
    for (int i = 0; i < 11; i++) if (dst[i]) *dst[i] = atof(PL.f[i]);
    in.cellsPerRadius = atoi(PL.f[9]);
    in.molWeight = atof(PL.f[11]);
    PlumeOut o; char err[300];
    bool ok = plume_compute(&in, &o, err, sizeof err);

    Rectangle b = ui_modal_begin("Plume on a surface", "An under-expanded jet from a nozzle exit onto a flat plate: axisymmetric (5-degree wedge), "
                                 "rhoCentralFoam with shock-capturing fluxes (Kurganov, vanLeer), laminar, Courant 0.2. Set from the nozzle exit state.",
                                 960, 700, close);
    float x = b.x, y = b.y + 4, cw = 400;
    static const char *gases[] = { "N2", "Air", "Other gas" };
    ui_text(F_MED, 13, "Gas", x, y + 9, TH.text2);
    ui_select(ui_id("pl-gas"), (Rectangle){ x + cw - 170, y, 160, 34 }, gases, 3, &PL.gas);
    y += 42;
    if (PL.gas == 2) { plume_field(11, "Molar mass", "kg/kmol", x, y, cw); y += 42; }
    plume_field(0, "Exit Mach number Me", "", x, y, cw); y += 42;
    plume_field(1, "Exit pressure pe", "Pa", x, y, cw); y += 42;
    plume_field(2, "Exit temperature Te", "K", x, y, cw); y += 42;
    plume_field(3, "Exit radius re", "m", x, y, cw); y += 42;
    plume_field(4, "Stand-off height H (exit to plate)", "m", x, y, cw); y += 42;
    plume_field(5, "Domain radius R", "m", x, y, cw); y += 42;
    plume_field(6, "Ambient pressure pa", "Pa", x, y, cw); y += 42;
    plume_field(7, "Ambient temperature", "K", x, y, cw); y += 42;
    plume_field(8, "Plate temperature", "K", x, y, cw); y += 42;
    plume_field(9, "Cells across the exit radius", "", x, y, cw); y += 42;
    plume_field(10, "Courant number (maxCo)", "", x, y, cw);

    /* the numbers it makes, live */
    float px = x + cw + 30, pw = b.width - cw - 30, py = b.y + 4;
    Rectangle pr = { px, py, pw, b.height - 60 };
    ui_rect(pr, 12, TH.surface); ui_rect_lines(pr, 12, 1, TH.lineSoft);
    float tx = px + 18, ty = py + 16, tw = pw - 36;
    if (!ok) ui_text_wrap(F_REG, 13.5f, err, (Rectangle){ tx, ty, tw, 200 }, 20, TH.err, true);
    else {
        char t[1600];
        double g = PL.gas == 2 ? in.gamma : 1.4;
        pl_fmt_ui(t, sizeof t,
            "a = sqrt(gamma·R·Te) = sqrt(%g × %.1f J/(kg·K) × %g K) = %.1f m/s\n"
            "Ue = Me·a = %g × %.1f = %.1f m/s\n"
            "ρe = pe/(R·Te) = %.4g kg/m³;  mdot = ρe·Ue·pi·re² = %.4g kg/s\n"
            "p0 = pe·(1 + (gamma-1)/2·Me²)^(gamma/(gamma-1)) = %.4g Pa\n"
            "pe/pa = %.3g (%s);  NPR = p0/pa = %.4g\n"
            "Mach disk (free jet, Ashkenas-Sherman): x ≈ 0.67·De·sqrt(p0/pa) = %.3g m, %s the plate (%g m)\n\n"
            "Mesh: %d × (%d + %d) = %ld cells, Δx = %.3g m at the axis\n"
            "Δt ≈ %g·Δx/(Ue + a) = %.3g s (about 7x less on the wedge near the axis)\n"
            "endTime = 2·(H + R)/Ue = %.3g s: about %ld steps, ~%.0f core-minutes",
            g, o.Rs, in.Te, o.ae, in.Me, o.ae, o.Ue, o.rhoe, o.mdot, o.p0, o.per, o.per > 1.01 ? "under-expanded" : o.per < 0.99 ? "over-expanded" : "matched",
            o.npr, o.xMach, o.xMach > in.H ? "beyond" : "before", in.H, o.nx, o.nr1, o.nr2, o.cells, o.dx, in.maxCo, o.dt0, o.endTime, o.steps, o.cpuSeconds / 60);
        ty += ui_text_wrap(F_REG, 13.5f, t, (Rectangle){ tx, ty, tw, 420 }, 21, TH.text, true) + 12;   /* the mono face has no γ, ρ, √ */
        if (o.warn[0]) ty += ui_text_wrap(F_REG, 13, o.warn, (Rectangle){ tx, ty, tw, 120 }, 19, TH.warn, true) + 8;
        ui_text_wrap(F_REG, 12.5f, "The Moon's surface is near 1e-10 Pa: a continuum solver needs a finite ambient. SENSOR SWEEP's continuum "
                     "check shows the Knudsen number where the gas is thinnest; beyond Kn 0.1 it is DSMC territory (SPARTA).",
                     (Rectangle){ tx, ty, tw, 80 }, 18, TH.text3, true);
    }
    float by = b.y + b.height - 38;
    ui_text(F_MED, 13, "Case name", x, by + 9, TH.text2);
    ui_input(ui_id("pl-name"), (Rectangle){ x + 90, by, 220, 38 }, PL.name, sizeof PL.name, "plume", INPUT_LIVE);
    if (PL.err[0]) ui_text_clip(F_REG, 12.5f, PL.err, x + 320, by + 11, b.width - 320 - 180, TH.err);
    ui_disable(!ok);
    float gw = ui_button_width(ICON_PLUS, "Create case");
    if (ui_button(ui_id("pl-go"), (Rectangle){ b.x + b.width - gw, by, gw, 38 }, ICON_PLUS, "Create case", BTN_PRIMARY)) {
        PL.err[0] = 0;
        char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
        pl_trim_inplace(PL.name);
        const char *bad = foam_path_problem(PL.name);
        char dst[PL_PATH];
        if (!PL.name[0] || strchr(PL.name, '/') || bad) snprintf(PL.err, sizeof PL.err, "Use a plain folder name (letters, digits, - and _).");
        else if (!pl_mkdirs(ws) || !pl_fmt(dst, sizeof dst, "%s/%s", ws, PL.name)) pl_fmt_ui(PL.err, sizeof PL.err, "Can't use the workspace %s", ws);
        else if (pl_exists(dst)) pl_fmt_ui(PL.err, sizeof PL.err, "%s already exists in the workspace.", PL.name);
        else if (!plume_write(dst, &in, PL.err, sizeof PL.err)) {}
        else {
            PL.in = in;
            ui_toast(TOAST_OK, "Created %s: %ld cells, rhoCentralFoam, endTime %.3g s", PL.name, o.cells, o.endTime);
            *close = true;
            app_rescan_cases(true);
            app_open_case(dst, PG_OVERVIEW);
        }
    }
    ui_disable(false);
}

/* ---------------------------------------------------------------- the other programs: starters and examples */
typedef struct { int kind; int idx; char name[96]; char note[200]; char path[PL_PATH]; } NewItem;   /* kind 0 starter, 1 example, 2 empty */

static int new_items(int prog, NewItem *out, int max)
{
    int n = 0;
    for (int i = 0; i < PROG_N_TEMPLATES && n < max; i++) {
        if (PROG_TEMPLATES[i].prog != prog) continue;
        out[n] = (NewItem){ 0, i, "", "", "" };
        pl_strlcpy(out[n].name, PROG_TEMPLATES[i].name, sizeof out[n].name);
        pl_strlcpy(out[n].note, PROG_TEMPLATES[i].note, sizeof out[n].note);
        n++;
    }
    if (prog == PROG_SATURNE && n < max) {
        out[n] = (NewItem){ 2, 0, "Empty case", "code_saturne create: DATA, SRC and RESU folders, set up in the code_saturne GUI.", "" };
        n++;
    }
    static ProgExample ex[128]; static int nex = -1;
    if (nex < 0) nex = prog_examples(ex, 128);
    for (int i = 0; i < nex && n < max; i++) {
        if (ex[i].prog != prog) continue;
        out[n] = (NewItem){ 1, i, "", "", "" };
        pl_strlcpy(out[n].name, ex[i].name, sizeof out[n].name);
        pl_fmt_ui(out[n].note, sizeof out[n].note, "Example: %s", ex[i].path);
        pl_strlcpy(out[n].path, ex[i].path, sizeof out[n].path);
        n++;
    }
    return n;
}

static void new_program_case(int prog, const NewItem *it)
{
    pl_trim_inplace(A.cloneName);
    if (!A.cloneName[0] || strchr(A.cloneName, '/') || strchr(A.cloneName, ' ') || A.cloneName[0] == '.') {
        snprintf(A.cloneErr, sizeof A.cloneErr, "Use a folder name without spaces or '/'."); return;
    }
    char ws[PL_PATH]; app_prog_dir(prog, ws, sizeof ws);
    char dst[PL_PATH]; pl_join(dst, sizeof dst, ws, A.cloneName);
    if (pl_exists(dst)) { pl_fmt_ui(A.cloneErr, sizeof A.cloneErr, "%s already exists. Pick another name.", A.cloneName); return; }
    if (!pl_mkdirs(ws)) { pl_fmt_ui(A.cloneErr, sizeof A.cloneErr, "Could not create the folder %s.", ws); return; }
    char err[300] = "";
    bool ok = false;
    if (it->kind == 0 && prog == PROG_DEME) ok = deme_write_project(dst, PROG_TEMPLATES[it->idx].id, err, sizeof err);
    else if (it->kind == 0) ok = prog_template_write(&PROG_TEMPLATES[it->idx], dst, err, sizeof err);
    else if (it->kind == 1) ok = prog_example_copy(it->path, dst, err, sizeof err); else {
        char qw[PL_PATH + 8], qn[300], cmd[PL_PATH + 400];
        pl_shell_quote(qw, sizeof qw, ws); pl_shell_quote(qn, sizeof qn, A.cloneName);
        snprintf(cmd, sizeof cmd, "cd %s && code_saturne create -c %s --noref -q 2>&1", qw, qn);
        char out[600] = "";
        ok = pl_run_wait(cmd, out, sizeof out) == 0 && pl_is_dir(dst);
        if (!ok) pl_fmt_ui(err, sizeof err, "code_saturne create failed: %s", out);
    }
    if (!ok) { pl_strlcpy(A.cloneErr, err, sizeof A.cloneErr); return; }
    pl_stat_cache_clear();
    ui_toast(TOAST_OK, "Created %s (%s)", A.cloneName, it->name);
    app_rescan_cases(true);
    app_open_case(dst, PG_OVERVIEW);
}

static void new_name_for(const char *base)
{
    char ws[PL_PATH]; app_prog_dir(A.newProg, ws, sizeof ws);
    char cand[256]; pl_strlcpy(cand, base, sizeof cand);
    for (char *c = cand; *c; c++) if (*c == ' ' || *c == '/') *c = '-'; else *c = (char)tolower((unsigned char)*c);
    char b2[256]; pl_strlcpy(b2, cand, sizeof b2);
    for (int k = 2; k < 100; k++) {
        char p[PL_PATH]; pl_join(p, sizeof p, ws, cand);
        if (!pl_exists(p)) break;
        pl_fmt_ui(cand, sizeof cand, "%s-%d", b2, k);
    }
    pl_strlcpy(A.cloneName, cand, sizeof A.cloneName);
    A.cloneErr[0] = 0;
}

static float new_program_section(float x, float y, float w, int prog)
{
    NewItem it[96]; int n = new_items(prog, it, 96);
    char exe[PL_PATH], why[300];
    if (!prog_exe(prog, exe, sizeof exe, why, sizeof why)) {
        float ch = callout_height(w, NULL, why);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, NULL, why);
        y += ch + 16;
    }
    float lw = w * 0.58f, dx = x + lw + 24, dw = w - lw - 24;
    float top = y, rowH = 64;
    y += section_head(x, y, lw, ui_cap_words("Starters and examples"), prog_info(prog)->what);
    if (!n) { ui_text(F_REG, 13.5f, "No starters for this program yet: open a folder you have (Cases > Open folder).", x, y, TH.text3); y += 30; }
    for (int i = 0; i < n; i++) {
        Rectangle rr = { x, y, lw, rowH - 6 };
        if (ui_row(ui_idn(ui_id("np-item"), i), rr, A.newSel == i)) { A.newSel = i; new_name_for(it[i].kind == 1 ? it[i].name : it[i].name); }
        ui_register(ui_idn(ui_id("np-item"), i), rr, it[i].name);
        ui_icon(it[i].kind == 1 ? ICON_BOOK_OPEN : prog_icon(prog), rr.x + 22, rr.y + rr.height / 2, 16, it[i].kind == 1 ? TH.text3 : prog_color(prog));
        ui_text_clip(F_SEMI, 14, it[i].name, rr.x + 44, rr.y + 10, rr.width - 56, TH.text);
        ui_text_clip(F_REG, 12.5f, it[i].note, rr.x + 44, rr.y + 32, rr.width - 56, TH.text3);
        y += rowH;
    }
    /* the detail */
    Rectangle dr = { dx, top, dw, 330 };
    ui_rect(dr, 12, TH.surface);
    ui_rect_lines(dr, 12, 1, TH.lineSoft);
    if (A.newSel < 0 || A.newSel >= n) {
        ui_icon(ICON_ARROW_LEFT, dr.x + 30, dr.y + 36, 18, TH.text3);
        ui_text_wrap(F_REG, 14, "Pick a starter or an example. It is written (or copied) into this program's own folder as a project of its own, "
                     "ready to set up and run.", (Rectangle){ dr.x + 24, dr.y + 60, dr.width - 48, 200 }, 21, TH.text2, true);
    } else {
        const NewItem *s = &it[A.newSel];
        float yy = dr.y + 22, xx = dr.x + 24, ww = dr.width - 48;
        ui_text_clip(F_SEMI, 18, s->name, xx, yy, ww, TH.text); yy += 32;
        yy += ui_text_wrap(F_REG, 13, s->note, (Rectangle){ xx, yy, ww, 80 }, 19, TH.text2, true) + 12;
        ui_text(F_MED, 13.5f, "Name", xx, yy, TH.text); yy += 24;
        ui_input(ui_id("clone-name"), (Rectangle){ xx, yy, ww, 36 }, A.cloneName, sizeof A.cloneName, "my-project", INPUT_LIVE);
        yy += 46;
        char ws[PL_PATH]; app_prog_dir(prog, ws, sizeof ws);
        char dest[PL_PATH + 300]; pl_fmt_ui(dest, sizeof dest, "Made in %s/%s (Settings > Workspace sets this folder)", ws, A.cloneName);
        yy += ui_text_wrap(F_REG, 12.5f, dest, (Rectangle){ xx, yy, ww, 60 }, 18, TH.text3, true) + 10;
        if (A.cloneErr[0]) ui_text_wrap(F_REG, 13, A.cloneErr, (Rectangle){ xx, yy, ww, 60 }, 19, TH.err, true);
        if (ui_button(ui_id("clone-go"), (Rectangle){ xx, dr.y + dr.height - 60, ww, 38 }, ICON_FOLDER_PLUS, "Create project", BTN_PRIMARY))
            new_program_case(prog, s);
    }
    return fmaxf(y, top + 350);
}

void page_new(Rectangle r)
{
    ensure_tutorials();
    UiScroll *sc = &A.scr[1];
    float top = ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = r.width - 2 * PAGE_PAD;
    float y = top + 30;
    y += page_header(x, y, w, "New case",
                     A.newProg == PROG_OPENFOAM ? "Copy a tutorial into your workspace and make it yours. Tutorials ship with OpenFOAM, so they match the version you run."
                                                : "Start a project of another program from a starter Q-Continuum writes, or from an example installed with it.");
    /* the program */
    {
        float px = x;
        for (int k = 0; k < PROG_COUNT; k++) {
            float pw = ui_text_w(F_MED, 13, prog_info(k)->name) + 46;
            if (px + pw > x + w) { px = x; y += 42; }
            Rectangle pr = { px, y, pw, 32 };
            bool on = A.newProg == k, hov = ui_hovered(pr);
            if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
            Color pc = k == PROG_OPENFOAM ? TH.accent : prog_color(k);
            ui_rect(pr, 16, on ? pc : hov ? TH.hover : TH.surface);
            if (!on) ui_rect_lines(pr, 16, 1, TH.lineSoft);
            ui_icon(k == PROG_OPENFOAM ? ICON_DROPLETS : prog_icon(k), px + 18, y + 16, 14, on ? TH.textOnAccent : pc);
            ui_text(F_MED, 13, prog_info(k)->name, px + 32, y + 7, on ? TH.textOnAccent : TH.text2);
            char rid[48]; pl_fmt_ui(rid, sizeof rid, "new-prog:%s", prog_info(k)->key);
            ui_register(ui_id(rid), pr, prog_info(k)->name);
            if (ui_clicked(pr) && A.newProg != k) { A.newProg = k; A.newSel = -1; A.cloneErr[0] = 0; }
            px += pw + 8;
        }
        y += 52;
    }
    if (A.newProg != PROG_OPENFOAM) {
        y = new_program_section(x, y, w, A.newProg);
        ui_scroll_end(sc, y - top + 40);
        return;
    }

    if (A.tut.n == 0) {
        const char *msg = "No tutorials were found. They are installed with OpenFOAM (the openfoam2606-default package includes them). "
                          "Check the OpenFOAM installation in Settings.";
        float ch = callout_height(w, "Tutorials not found", msg);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "Tutorials not found", msg);
        y += ch + 20;
        ui_scroll_end(sc, y - top + 40);
        return;
    }

    /* starters */
    int idx[N_STARTERS], ns = 0, st[N_STARTERS];
    for (int i = 0; i < N_STARTERS; i++) {
        int f = -1;
        for (int k = 0; k < 3 && STARTERS[i].suffix[k] && f < 0; k++) f = tutorials_find(&A.tut, STARTERS[i].suffix[k]);
        if (f >= 0) { idx[ns] = f; st[ns] = i; ns++; }
    }
    if (ns) {
        y += section_head(x, y, w, "Good places to start", "One case for each kind of physics this app is set up for.");
        float gap = 16;
        int cols = w > 1100 ? 3 : 2;
        float cw = (w - gap * (cols - 1)) / cols, chh = 150;
        for (int i = 0; i < ns; i++) {
            // NOLINTNEXTLINE(bugprone-integer-division): row and column indices
            Rectangle cr = { x + (float)(i % cols) * (cw + gap), y + (float)(i / cols) * (chh + gap), cw, chh };   /* column, row */
            starter_card(cr, st[i], idx[i]);
        }
        /* 4.4: a case written from the nozzle exit state, not a tutorial */
        // NOLINTNEXTLINE(bugprone-integer-division): row and column indices
        Rectangle pr = { x + (float)(ns % cols) * (cw + gap), y + (float)(ns / cols) * (chh + gap), cw, chh };
        {
            UiId id = ui_id("starter-plume");
            bool hov = ui_hovered(pr);
            float h = ui_anim(id, hov ? 1.f : 0.f, 7.f);
            Color cc = TH.cat[1];
            Rectangle r = pr;
            if (TH.lcars) { lc_panel(r, cc, h * 0.6f, false); r.x += 16; r.width -= 16; }
            else { ui_rect(r, 12, TH.surface); ui_rect_lines(r, 12, 1, ui_mix(TH.lineSoft, TH.line, h)); }
            ui_icon(ICON_GAUGE, r.x + 26, r.y + 28, 18, cc);
            ui_text_clip(F_SEMI, 15, "Plume on a surface", r.x + 46, r.y + 18, r.width - 64, TH.text);
            ui_text_wrap(F_REG, 13, "An under-expanded jet hits a flat plate: set from nozzle exit Mach, p, T and stand-off height.",
                         (Rectangle){ r.x + 18, r.y + 50, r.width - 36, 60 }, 19, TH.text2, true);
            ui_badge(r.x + 18, r.y + r.height - 34, NULL, "rhoCentralFoam", TH.lcars ? cc : TH.text2, TH.lineSoft);
            ui_register(id, pr, "Plume on a surface");
            if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
            if (ui_clicked(pr)) { A.modal = MD_PLUME; }
        }
        ns++;
        /* a coupled case from two of the user's cases (preCICE CHT) */
        // NOLINTNEXTLINE(bugprone-integer-division): row and column indices
        Rectangle qr = { x + (float)(ns % cols) * (cw + gap), y + (float)(ns / cols) * (chh + gap), cw, chh };
        {
            UiId id = ui_id("starter-couple");
            bool hov = ui_hovered(qr);
            float h = ui_anim(id, hov ? 1.f : 0.f, 7.f);
            Color cc = TH.cat[3];
            Rectangle r = qr;
            if (TH.lcars) { lc_panel(r, cc, h * 0.6f, false); r.x += 16; r.width -= 16; }
            else { ui_rect(r, 12, TH.surface); ui_rect_lines(r, 12, 1, ui_mix(TH.lineSoft, TH.line, h)); }
            ui_icon(ICON_GIT_FORK, r.x + 26, r.y + 28, 18, cc);
            ui_text_clip(F_SEMI, 15, "Couple two cases", r.x + 46, r.y + 18, r.width - 64, TH.text);
            ui_text_wrap(F_REG, 13, "A fluid case and a solid case made into one conjugate-heat-transfer run through preCICE.",
                         (Rectangle){ r.x + 18, r.y + 50, r.width - 36, 60 }, 19, TH.text2, true);
            ui_badge(r.x + 18, r.y + r.height - 34, NULL, "preCICE", TH.lcars ? cc : TH.text2, TH.lineSoft);
            ui_register(id, qr, "Couple two cases");
            if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
            if (ui_clicked(qr)) A.modal = MD_CPL_BUILD;
        }
        ns++;
        // NOLINTNEXTLINE(bugprone-integer-division): whole rows
        y += (float)((ns + cols - 1) / cols) * (chh + gap) + 18;
    }

    /* browser + detail */
    char head[96]; snprintf(head, sizeof head, "All tutorials");
    char desc[PL_PATH + 64]; snprintf(desc, sizeof desc, "%d cases in %s", A.tut.n, A.tut.root);
    y += section_head(x, y, w, head, desc);
    float lw = w * 0.58f, dx = x + lw + 24, dw = w - lw - 24;
    ui_input(ui_id("tut-search"), (Rectangle){ x, y, lw * 0.6f, 36 }, A.tutSearch, sizeof A.tutSearch, "Search by name, solver or path", INPUT_LIVE | INPUT_SEARCH);
    /* group filter */
    static const char *groupsBuf[64];
    static char groupNames[64][64];
    int ng = 0;
    groupsBuf[ng++] = "All groups";
    for (int i = 0; i < A.tut.n && ng < 64; i++) {
        bool seen = false;
        for (int k = 1; k < ng; k++) if (!strcmp(groupNames[k], A.tut.t[i].group)) seen = true;
        if (!seen) { snprintf(groupNames[ng], 64, "%s", A.tut.t[i].group); groupsBuf[ng] = groupNames[ng]; ng++; }
    }
    if (A.tutGroup >= ng) A.tutGroup = 0;
    ui_select(ui_id("tut-group"), (Rectangle){ x + lw * 0.6f + 10, y, lw * 0.4f - 10, 36 }, groupsBuf, ng, &A.tutGroup);
    float listTop = y + 48, listH = 460;
    Rectangle lr = { x, listTop, lw, listH };
    ui_rect(lr, 12, TH.surface);
    ui_rect_lines(lr, 12, 1, TH.lineSoft);
    UiScroll *ls = &A.scr[2];
    float ly = ui_scroll_begin(ls, ui_inset(lr, 4, 4)) + 4;
    int nShown = 0;
    float rowH = 50;
    for (int i = 0; i < A.tut.n; i++) {
        const Tutorial *t = &A.tut.t[i];
        if (A.tutGroup > 0 && strcmp(t->group, groupsBuf[A.tutGroup])) continue;
        if (A.tutSearch[0] && !pl_contains_ci(t->rel, A.tutSearch) && !pl_contains_ci(t->solver, A.tutSearch)) continue;
        Rectangle rr = { lr.x + 8, ly + nShown * rowH, lr.width - 24, rowH - 4 };
        nShown++;
        if (rr.y + rowH < lr.y || rr.y > lr.y + lr.height) continue;
        if (ui_row(ui_idn(ui_id("tut-row"), i), rr, A.tutSel == i)) select_tutorial(i);
        DrawCircleV((Vector2){ rr.x + 16, rr.y + rr.height / 2 }, 4, cat_color(t->cat));
        float sw = ui_text_w(F_MONO, 12, t->solver);
        ui_text_clip(F_MED, 13.5f, t->name, rr.x + 32, rr.y + 7, rr.width - sw - 60, TH.text);
        ui_text_clip(F_REG, 12, t->rel, rr.x + 32, rr.y + 26, rr.width - sw - 60, TH.text3);
        ui_text(F_MONO, 12, t->solver, rr.x + rr.width - sw - 12, rr.y + 15, TH.text2);
    }
    if (!nShown) ui_text(F_REG, 13.5f, "No tutorials match.", lr.x + 20, lr.y + 20, TH.text3);
    ui_scroll_end(ls, nShown * rowH + 8);

    /* detail */
    Rectangle dr = { dx, listTop, dw, listH };
    ui_rect(dr, 12, TH.surface);
    ui_rect_lines(dr, 12, 1, TH.lineSoft);
    if (A.tutSel < 0 || A.tutSel >= A.tut.n) {
        ui_icon(ICON_ARROW_LEFT, dr.x + 30, dr.y + 36, 18, TH.text3);
        ui_text_wrap(F_REG, 14, "Pick a tutorial from the list or a starter above. It is copied into your workspace, "
                     "so the original stays untouched.", (Rectangle){ dr.x + 24, dr.y + 60, dr.width - 48, 200 }, 21, TH.text2, true);
    } else {
        const Tutorial *t = &A.tut.t[A.tutSel];
        float yy = dr.y + 22, xx = dr.x + 24, ww = dr.width - 48;
        ui_text_clip(F_SEMI, 18, t->name, xx, yy, ww, TH.text); yy += 32;
        float bx = xx;
        bx += ui_badge(bx, yy, cat_icon(t->cat), category_name(t->cat), cat_color(t->cat), ui_alpha(cat_color(t->cat), 0.14f)) + 6;
        bx += ui_badge(bx, yy, NULL, t->solver, TH.text2, TH.lineSoft) + 6;
        if (t->hasAllrun) ui_badge(bx, yy, ICON_WORKFLOW, "Allrun script", TH.text2, TH.lineSoft);
        yy += 38;
        ui_text(F_MED, 12.5f, "Source", xx, yy, TH.text3); yy += 19;
        ui_text_wrap(F_MONO, 12.5f, t->rel, (Rectangle){ xx, yy, ww, 60 }, 18, TH.text2, true);
        yy += 44;
        ui_text(F_MED, 13.5f, "Case name", xx, yy, TH.text); yy += 24;
        ui_input(ui_id("clone-name"), (Rectangle){ xx, yy, ww, 36 }, A.cloneName, sizeof A.cloneName, "my-case", INPUT_LIVE);
        yy += 46;
        char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
        char dest[PL_PATH + 300]; pl_fmt_ui(dest, sizeof dest, "Copied to %s/%s", ws, A.cloneName);
        yy += ui_text_wrap(F_REG, 12.5f, dest, (Rectangle){ xx, yy, ww, 60 }, 18, TH.text3, true) + 10;
        if (A.cloneErr[0]) ui_text_wrap(F_REG, 13, A.cloneErr, (Rectangle){ xx, yy, ww, 60 }, 19, TH.err, true);
        if (ui_button(ui_id("clone-go"), (Rectangle){ xx, dr.y + dr.height - 60, ww, 38 }, ICON_GIT_FORK, "Create case", BTN_PRIMARY)) do_clone();
    }
    y = listTop + listH + 30;
    ui_scroll_end(sc, y - top + 20);
}
