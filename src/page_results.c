/* page_results.c -- an OpenFOAM case's Results (workflow plan W5, 2026-11-14): the last step of the workflow, "here is the
 * answer and how far to trust it".
 *
 *   1  the verdict: converged, finished, stopped or failed, and how far the residuals fell
 *   2  the numbers the case measures (its function objects: forces and coefficients, the setup's reports, any other
 *      postProcessing series), each with its last value and whether it has settled over the last fifth of the run (a
 *      transient's: how much it varies, and its mean)
 *   3  trust checks, shown when they apply: the walls' y+ against the wall treatment, the Courant number, the mass balance
 *      over the boundaries that report a flow rate
 *   4  a 2-D case's cross section; then what next: ParaView, and "Is the mesh fine enough?" (a refinement study)
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "report.h"
#include "xsection.h"

/* the solver's log: the one the Run page plots */
static LogMon *solver_log(Case *c)
{
    int k = logset_primary(&A.logs, c->s.app);
    return k >= 0 ? &A.logs.logs[k] : NULL;
}

static float last_value(const MonFile *f, int col)
{
    for (int j = f->n - 1; j >= 0; j--) if (!isnan(f->v[col][j])) return f->v[col][j];
    return NAN;
}

/* the mean and the spread (max - min) of a column over its last fifth */
static bool tail_stats(const MonFile *f, int col, double *mean, double *spread)
{
    int n0 = f->n - (f->n / 5 > 3 ? f->n / 5 : 3);
    if (n0 < 0) n0 = 0;
    double s = 0, lo = 1e300, hi = -1e300; int k = 0;
    for (int j = n0; j < f->n; j++) {
        float v = f->v[col][j];
        if (isnan(v)) continue;
        s += v; k++;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (k < 2) return false;
    *mean = s / k; *spread = hi - lo;
    return true;
}

static bool is_clock_or_meta(const char *col)
{
    return !strcmp(col, "Time") || !strcmp(col, "time") || strstr(col, "nIter") || strstr(col, "converged") || strstr(col, "_initial") ||
           strstr(col, "_final") || strstr(col, "solver");
}

static int changing;   /* W6: how many of the numbers haven't settled */
/* one number in a row: its name, its value, and whether it has settled */
static float number_row(float x, float y, float w, const char *name, const char *unit, const MonFile *f, int col, bool transient)
{
    float v = last_value(f, col);
    double mean, spread;
    bool st = tail_stats(f, col, &mean, &spread);
    char val[64], note[200] = "";
    snprintf(val, sizeof val, "%.4g%s%s", transient && st ? mean : v, unit && *unit ? " " : "", unit ? unit : "");
    int kind = TOAST_OK;
    if (st && f->n >= 6) {
        double rel = fabs(mean) > 1e-30 ? spread / fabs(mean) : spread;
        if (!transient && rel >= 0.01) changing++;
        if (transient) pl_fmt_ui(note, sizeof note, "mean of the last fifth; it varies by %.2g %% there (an unsteady flow: the mean is the answer)", rel * 100);
        else if (rel < 1e-4) pl_strlcpy(note, "settled: within 0.01 % over the last fifth of the run", sizeof note);
        else if (rel < 0.01) pl_fmt_ui(note, sizeof note, "settled: within %.2g %% over the last fifth of the run", rel * 100);
        else { pl_fmt_ui(note, sizeof note, "still changing: %.2g %% over the last fifth. Run longer before trusting it", rel * 100); kind = TOAST_WARN; }
    } else pl_strlcpy(note, "too few points yet to say whether it has settled", sizeof note);
    ui_icon(kind == TOAST_OK ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT, x + 10, y + 13, 15, kind == TOAST_OK ? TH.ok : TH.warn);
    ui_text_clip(F_MED, 14, name, x + 28, y + 4, w * 0.36f, TH.text);
    ui_text(F_MONO_MED, 15, val, x + w * 0.38f, y + 3, TH.text);
    ui_text_clip(F_REG, 12.5f, note, x + w * 0.38f + 150, y + 5, w * 0.62f - 150, kind == TOAST_OK ? TH.text3 : TH.warn);
    return 30;
}

/* the walls' y+: the mean and the greatest of the yPlus function object's "<wall> average" / "<wall> max" columns */
static const MonFile *yplus_file(double *avg, double *max)
{
    for (int i = 0; i < A.mons.n; i++) {
        const MonFile *f = &A.mons.f[i];
        if (!strstr(f->key, "yPlus")) continue;
        double sum = 0, mx = 0; int n = 0;
        for (int k = 0; k < f->nCols; k++) {
            float last = last_value(f, k);
            if (isnan(last)) continue;
            if (pl_ends_with(f->col[k], " average")) { sum += last; n++; }
            else if (pl_ends_with(f->col[k], " max") && last > mx) mx = last;
        }
        if (!n) continue;
        *avg = sum / n; *max = mx;
        return f;
    }
    return NULL;
}

/* a case made from a starter: the answer to expect, to compare with (workflow plan W8); returns the new y */
static float starter_compare(const Case *c, float x, float y, float w)
{
    char st[96], ln[400], ex[700];
    if (!app_starter_info(c->s.path, st, sizeof st, ln, sizeof ln, ex, sizeof ex)) return y;
    y += section_head(x, y, w, "Compare with", st);
    return y + ui_text_wrap(F_REG, 13.5f, ex, (Rectangle){ x, y, w, 120 }, 20, TH.text2, true) + 22;
}

void page_foam_results(Rectangle r)
{
    Case *c = A.cs;
    UiScroll *sc = &A.scr[11];
    float top = ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = fminf(r.width - 2 * PAGE_PAD, CONTENT_MAX);
    float y = top + (A.resultsTab == 1 ? 14 : 30);
    y += page_header(x, y, w, "Results", A.resultsTab == 1 ? NULL : "What the run produced, and how far to trust it. ParaView has the whole flow field.");
    if (A.resultsTab != 1) y += case_steps(c, x, y, w);
    {
        static const char *tabs[2] = { "Summary", "Visualization" };
        int shown = A.resultsTab == 1 ? 1 : 0;
        float th;
        if (ui_tabs(ui_id("res-tabs"), x, y, tabs, 2, &shown, &th)) A.resultsTab = shown;
        ui_divider(x, y + th, w);
        y += th + 18;
    }
    if (A.resultsTab == 1) {                                  /* the 3-D results (docs/VISUALIZATION_PLAN.md): the page's full width */
        float vx = r.x + PAGE_PAD, vw = r.width - 2 * PAGE_PAD;
        float vh = fmaxf(400, r.height - (y - top) - 24);
        y += viz_page(c, vx, y, vw, vh);
        ui_scroll_end(sc, y - top + 20);
        return;
    }
    bool running = A.cj->active && !strcmp(A.cj->casePath, c->s.path);
    bool steady = c->s.app[0] && (strstr(c->s.app, "simple") || strstr(c->s.app, "Simple"));
    LogMon *m = solver_log(c);

    /* ---- 1 the verdict */
    if (!m && !c->s.nTimes && !running) {
        const char *t = "No results yet", *d = "Run the case: what it measures and how far to trust it appear here as it runs.";
        float ch = callout_height(w - 140, t, d);
        callout((Rectangle){ x, y, w - 140, ch }, TOAST_INFO, ICON_CHART_LINE, t, d);
        if (ui_button(ui_id("res-run"), (Rectangle){ x + w - 124, y + (ch - 36) / 2, 124, 36 }, ICON_PLAY, "Run", BTN_SECONDARY)) app_go(PG_RUN);
        y += ch + 26;
        y = starter_compare(c, x, y, w);
        ui_scroll_end(sc, y + 40 - top);
        return;
    }
    char t[200], d[600];
    int kind = TOAST_INFO;
    if (running) { pl_fmt_ui(t, sizeof t, "Running: t = %g", m ? m->time : 0); pl_strlcpy(d, "The numbers below follow the run as it writes them.", sizeof d); }
    else if (m && m->fatal) { kind = TOAST_ERR; pl_strlcpy(t, "The last run failed", sizeof t); pl_fmt_ui(d, sizeof d, "%s The numbers below are from before it stopped.", m->fatalMsg[0] ? m->fatalMsg : "The log on the Run page says why."); }
    else if (m && m->converged) { kind = TOAST_OK; pl_fmt_ui(t, sizeof t, "Converged at %s %g", steady ? "iteration" : "t =", m->time); pl_strlcpy(d, "Every residual fell under its target: the equations are solved to the accuracy asked for.", sizeof d); }
    else if (m && m->ended && steady) { kind = TOAST_WARN; pl_fmt_ui(t, sizeof t, "Ran all %g iterations without converging", m->time);
        pl_strlcpy(d, "The residuals never met their targets. The answer may still be changing: check below that the numbers have settled, or run longer.", sizeof d); }
    else if (m && m->ended) { kind = TOAST_OK; pl_fmt_ui(t, sizeof t, "Finished at t = %g", m->time); pl_strlcpy(d, "A transient run: the flow at each written time is in ParaView; the numbers below are over its last fifth.", sizeof d); }
    else { kind = TOAST_WARN; pl_fmt_ui(t, sizeof t, "Stopped at %s %g", steady ? "iteration" : "t =", m ? m->time : atof(c->s.latest)); pl_strlcpy(d, "The run didn't reach its end: the numbers below are where it stopped.", sizeof d); }
    /* how far the residuals fell: their first value against their last */
    if (m && m->nSeries) {
        double lo = 1e9, hi = -1e9;
        for (int i = 0; i < m->nSeries; i++) {
            const Series *s = &m->series[i];
            if (s->n < 2 || !(s->v[0] > 0) || !(s->v[s->n - 1] > 0)) continue;
            double o = log10(s->v[0] / s->v[s->n - 1]);
            if (o < lo) lo = o;
            if (o > hi) hi = o;
        }
        if (hi > -1e9) {
            size_t n = strlen(d);
            pl_fmt_ui(d + n, sizeof d - n, lo >= 3 ? " The residuals fell %.1f to %.1f orders of magnitude." : " The residuals fell only %.1f to %.1f orders of magnitude (three or more is usual).", lo, hi);
            if (lo < 3 && kind == TOAST_OK && steady) kind = TOAST_WARN;
        }
    }
    float ch = callout_height(w, t, d);
    callout((Rectangle){ x, y, w, ch }, kind, kind == TOAST_OK ? ICON_CIRCLE_CHECK : kind == TOAST_ERR ? ICON_CIRCLE_X : kind == TOAST_WARN ? ICON_TRIANGLE_ALERT : ICON_ACTIVITY, t, d);
    y += ch + 26;

    /* ---- 2 the numbers */
    int shown = 0, have = 0;
    changing = 0;
    for (int i = 0; i < A.mons.n; i++)
        if (!strstr(A.mons.f[i].key, "yPlus") && !strstr(A.mons.f[i].key, "solverInfo") && !strstr(A.mons.f[i].key, "residuals") && A.mons.f[i].n) have++;
    y += section_head(x, y, w, "The numbers", have ? "What the case measures as it runs (its function objects), at the last write." : NULL);
    for (int i = 0; i < A.mons.n; i++) {
        const MonFile *f = &A.mons.f[i];
        if (strstr(f->key, "yPlus") || strstr(f->key, "solverInfo") || strstr(f->key, "residuals") || f->n == 0) continue;
        bool coeffs = strstr(f->key, "forceCoeffs") || strstr(f->key, "coefficient");
        char name[64]; snprintf(name, sizeof name, "%.*s", (int)strcspn(f->key, "/"), f->key);
        char desc[200] = "", unit[24] = "";
        bool rep = setup_report_info(c, name, desc, sizeof desc, unit, sizeof unit);
        ui_text_clip(F_SEMI, 14, rep ? desc : f->label, x, y, w, TH.text2);
        y += 26;
        int ncol = 0, cd = -1, cl = -1;
        for (int k = 0; k < f->nCols && ncol < 8; k++) {
            if (is_clock_or_meta(f->col[k])) continue;
            if (coeffs && strcmp(f->col[k], "Cd") && strcmp(f->col[k], "Cl") && strncmp(f->col[k], "Cm", 2)) continue;
            if (!strcmp(f->col[k], "Cd")) cd = k;
            if (!strcmp(f->col[k], "Cl")) cl = k;
            bool flow = !strcmp(f->col[k], "sum(phi)"), mflow = !strcmp(f->col[k], "sum(rhoPhi)");
            const char *label = coeffs ? (!strcmp(f->col[k], "Cd") ? "Drag coefficient Cd" : !strcmp(f->col[k], "Cl") ? "Lift coefficient Cl" : "Moment coefficient Cm")
                              : flow ? "Flow rate (out is +)" : mflow ? "Mass flow rate (out is +)" : f->col[k];
            y += number_row(x, y, w, label, rep ? unit : flow ? "m³/s" : mflow ? "kg/s" : "", f, k, !steady);
            ncol++; shown++;
        }
        if (cd >= 0 && cl >= 0) {                           /* lift over drag */
            float vd = last_value(f, cd), vl = last_value(f, cl);
            if (fabsf(vd) > 1e-9f) {
                char ld[64]; snprintf(ld, sizeof ld, "%.3g", vl / vd);
                ui_text(F_MED, 14, "Lift over drag L/D", x + 28, y + 4, TH.text);
                ui_text(F_MONO_MED, 15, ld, x + w * 0.38f, y + 3, TH.text);
                y += 30;
            }
        }
        y += 12;
    }
    if (!shown) {
        y += ui_text_wrap(F_REG, 13, "The case measures nothing as it runs (no function objects with a time series). Add reports on Physics > Setup > Reports "
                          "(a pressure drop, a flow rate, the drag), or open the results in ParaView.", (Rectangle){ x, y, w, 80 }, 19, TH.text3, true) + 20;
    }

    /* ---- 3 the trust checks */
    y += section_head(x, y, w, "How far to trust it", "Checks that apply to this case. Each says what it means for the answer.");
    int nChecks = 0;
    double yAvg = 0, yMax = 0;
    if (yplus_file(&yAvg, &yMax)) {
        char adv[400] = "";
        int stt = setup_yplus_advice(c, yAvg, yMax, adv, sizeof adv);
        char tt[120]; pl_fmt_ui(tt, sizeof tt, "Walls: y+ %.3g on average, %.3g at most", yAvg, yMax);
        if (!adv[0]) pl_strlcpy(adv, "y+ is the first cell's distance from the wall in viscous units: wall functions want 30 to 300, a resolved wall about 1.", sizeof adv);
        float h2 = callout_height(w, tt, adv);
        callout((Rectangle){ x, y, w, h2 }, stt == 1 ? TOAST_WARN : TOAST_OK, stt == 1 ? ICON_TRIANGLE_ALERT : ICON_CIRCLE_CHECK, tt, adv);
        y += h2 + 10; nChecks++;
    }
    if (m && m->courant.n > 0 && !steady) {
        double cmax = 0;
        for (int j = m->courant.n / 2; j < m->courant.n; j++) if (m->courant.v[j] > cmax) cmax = m->courant.v[j];
        char tt[120]; pl_fmt_ui(tt, sizeof tt, "Courant number up to %.3g", cmax);
        const char *dd = cmax <= 1 ? "Under 1: the flow moves less than a cell per time step, as PISO and most transient runs need."
                       : cmax <= 5 ? "Above 1: fine for PIMPLE with outer correctors; for PISO, halve the time step."
                                   : "Well above 1: the time step is too big for the mesh. Make it smaller on the Run page (or let it adjust).";
        float h2 = callout_height(w, tt, dd);
        callout((Rectangle){ x, y, w, h2 }, cmax <= 1 ? TOAST_OK : cmax <= 5 ? TOAST_INFO : TOAST_WARN, cmax <= 5 ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT, tt, dd);
        y += h2 + 10; nChecks++;
    }
    {   /* the mass balance: the flow rates the boundaries report (sum(phi), positive out), added up */
        double net = 0, out = 0; int nb = 0;
        for (int i = 0; i < A.mons.n; i++) {
            const MonFile *f = &A.mons.f[i];
            for (int k = 0; k < f->nCols; k++) {
                if (strcmp(f->col[k], "sum(phi)") && strcmp(f->col[k], "sum(rhoPhi)")) continue;
                float v = last_value(f, k);
                if (isnan(v)) continue;
                net += v; if (v > 0) out += v; nb++;
            }
        }
        if (nb >= 2 && out > 0) {
            double imb = fabs(net) / out * 100;
            char tt[120]; pl_fmt_ui(tt, sizeof tt, "Mass balance: %.2g %% %s", imb, net > 0 ? "more out than in" : "more in than out");
            const char *dd = imb < 0.5 ? "What comes in goes out: the flow rates of the reported boundaries agree."
                                       : "In and out don't agree yet: the run isn't converged, or a boundary isn't reported. Run longer, then check again.";
            float h2 = callout_height(w, tt, dd);
            callout((Rectangle){ x, y, w, h2 }, imb < 0.5 ? TOAST_OK : TOAST_WARN, imb < 0.5 ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT, tt, dd);
            y += h2 + 10; nChecks++;
        }
    }
    /* W6: checks that appear only when they fail, each with the fix */
    {
        double Lr = 0; bool lam = false;
        double Re = setup_reynolds(c, &Lr, &lam);
        if (lam && Re > 1e4) {
            char tt[120], dd[300]; pl_fmt_ui(tt, sizeof tt, "A laminar model on a turbulent flow: Re about %.3g", Re);
            pl_fmt_ui(dd, sizeof dd, "Re from the inflow speed over %.3g m. Above about 2300 in a pipe, or 5e5 along a plate, the flow is turbulent: pick RANS "
                      "(k-omega SST) on Physics > Setup, Models, or it won't settle.", Lr);
            float h2 = callout_height(w, tt, dd);
            callout((Rectangle){ x, y, w, h2 }, TOAST_WARN, ICON_TRIANGLE_ALERT, tt, dd);
            y += h2 + 10; nChecks++;
        }
        double Lf, Uf;
        if (!steady && m && m->ended && setup_flow_through(c, &Lf, &Uf) && Lf > 0) {
            double ft = m->time * Uf / Lf;
            if (ft < 1) {
                char tt[120], dd[300]; pl_fmt_ui(tt, sizeof tt, "The run covers %.2g flow-throughs", ft);
                pl_fmt_ui(dd, sizeof dd, "The flow hasn't crossed the domain once (%.3g m at %.3g m/s takes %.3g s): the start still shapes the answer. "
                          "Run to two or three flow-throughs, more before averaging.", Lf, Uf, Lf / Uf);
                float h2 = callout_height(w, tt, dd);
                callout((Rectangle){ x, y, w, h2 }, TOAST_WARN, ICON_TRIANGLE_ALERT, tt, dd);
                y += h2 + 10; nChecks++;
            }
        }
        if (steady && changing > 0) {
            char tt[120]; pl_fmt_ui(tt, sizeof tt, "%d of the numbers above %s still changing", changing, changing == 1 ? "is" : "are");
            const char *dd = "A steady answer is one that stopped changing: run more iterations (the Run page continues from the last result) until it settles.";
            float h2 = callout_height(w, tt, dd);
            callout((Rectangle){ x, y, w, h2 }, TOAST_WARN, ICON_TRIANGLE_ALERT, tt, dd);
            y += h2 + 10; nChecks++;
        }
    }
    if (!nChecks) y += ui_text_wrap(F_REG, 13, "None apply yet: they need the walls' y+ (written by Physics > Setup's cases), a transient run's Courant "
                                    "number, or flow-rate reports on the inlets and outlets.", (Rectangle){ x, y, w, 80 }, 19, TH.text3, true) + 10;
    y += 20;

    y = starter_compare(c, x, y, w);   /* a starter's answer to compare with (W8) */
    /* ---- 4 a 2-D case's picture, then what next */
    if (A.set.meshView && c->s.meshed && c->s.nRegions == 0 && xs_is_2d(c->s.path)) {
        y += section_head(x, y, w, "Cross section", "The mesh and the latest results across the 2-D plane.");
        y += xsection_panel((Rectangle){ x, y, w, 380 }, c, false) + 20;
    }
    y += section_head(x, y, w, "What next", NULL);
    float bx = x;
    float b1 = ui_button_width(ICON_EXTERNAL_LINK, "Open in ParaView");
    if (ui_button(ui_id("res-pv"), (Rectangle){ bx, y, b1, 38 }, ICON_EXTERNAL_LINK, "Open in ParaView", BTN_SECONDARY)) app_open_paraview();
    bx += b1 + 10;
    float b2 = ui_button_width(ICON_GRID_3X3, "Is the mesh fine enough?");
    bool canRefine = study_can_refine(c);
    ui_disable(!canRefine || running);
    if (ui_button(ui_id("res-meshstudy"), (Rectangle){ bx, y, b2, 38 }, ICON_GRID_3X3, "Is the mesh fine enough?", BTN_SECONDARY)) study_new_mesh();
    ui_disable(false);
    ui_tooltip((Rectangle){ bx, y, b2, 38 }, canRefine ? "Runs the case on a coarser and a finer mesh (0.7, 1 and 1.4 times the cells along each edge) and compares the numbers: "
                                                         "if they barely change, the mesh is fine enough"
                                                       : "A refinement study needs a blockMesh case; for a mesh from the STL mesher, make a finer one there (Mesh settings: base size) and compare");
    bx += b2 + 10;
    /* plan A3: the results written up, report/report.md with its charts (and report.html with pandoc) */
    float b3 = ui_button_width(ICON_FILE_TEXT, "Export report");
    if (ui_button(ui_id("res-report"), (Rectangle){ bx, y, b3, 38 }, ICON_FILE_TEXT, "Export report", BTN_SECONDARY)) {
        char md[PL_PATH], err[300], dir[PL_PATH];
        if (rpt_case(c->s.path, md, sizeof md, err, sizeof err)) {
            ui_toast(TOAST_OK, "Report written: %s", md);
            pl_dirname(dir, sizeof dir, md);
            launch_folder(&A.env, dir);
        } else ui_toast(TOAST_ERR, "No report: %s", err);
    }
    ui_tooltip((Rectangle){ bx, y, b3, 38 }, "Writes report/report.md in the case: the solver, the last run and its final residuals, a residual chart, every "
                                             "monitor's last values and chart, the Visualization pictures and the run history");
    y += 52;
    y += ui_text_wrap(F_REG, 13, "A mesh is fine enough when a finer one gives the same answer: the numbers above changing by a percent or two between "
                      "meshes is usual for engineering work.", (Rectangle){ x, y, w, 80 }, 19, TH.text3, true) + 20;
    ui_scroll_end(sc, y - top + 20);
}
