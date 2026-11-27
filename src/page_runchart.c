/* page_runchart.c -- the Run page's charts (split from page_run.c, improvement plan A5, 2026-11-26): the time ruler, the
 * residual chart, the monitors chart with its readouts (force coefficients, the walls' heat, Nusselt numbers, y+) and the
 * area that holds them. page_run.c draws the page around them; page_run_int.h is what the two share. */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "monitor.h"
#include "simsetup.h"
#include "program.h"
#include "page_run_int.h"

/* ============================================================================ time ruler */
static double nice_step(double range, int target)
{
    double raw = range / target, mag = pow(10, floor(log10(raw)));
    double n = raw / mag;
    return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag;
}

void time_ruler(Rectangle r, double t0, double t1, double t, Color fill)
{
    if (!(t1 > t0)) t1 = t0 + 1;
    double frac = (t - t0) / (t1 - t0);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    /* a short window (the user's 1.5 scale at 1440 x 900, plan A4): the time and the share beside the bar, one line */
    bool slim = r.height < 50;
    char cur[64], pct[48];
    snprintf(cur, sizeof cur, "t = %g", t);
    snprintf(pct, sizeof pct, slim ? "%.0f%%" : "%.0f%% of end time", frac * 100);
    float cw = slim ? ui_text_w(F_SEMI, 14, cur) + 14 : 0, pw = slim ? ui_text_w(F_REG, 12.5f, pct) + 12 : 0;
    float bx = r.x + cw, bw = r.width - cw - pw, by = slim ? r.y + 6 : r.y + 34;
    /* track and fill */
    ui_rect((Rectangle){ bx, by, bw, 10 }, 2, TH.lineSoft);
    ui_rect((Rectangle){ bx, by, (float)(bw * frac), 10 }, 2, fill);
    /* scale ticks */
    double st = nice_step(t1 - t0, 8);
    double minor = st / 5;
    for (double v = ceil(t0 / minor) * minor; v <= t1 + minor * 0.01; v += minor) {
        float px = bx + (float)((v - t0) / (t1 - t0)) * bw;
        bool major = fabs(fmod(v + st * 1e-9, st)) < minor * 0.5 || fabs(fmod(v, st) - st) < minor * 0.5;
        DrawRectangleRec((Rectangle){ px, by + 12, 1, major ? 9.f : 5.f }, major ? TH.text3 : TH.line);
        if (major) {
            char lab[32]; snprintf(lab, sizeof lab, "%g", fabs(v) < st * 1e-6 ? 0.0 : v);
            float lw = ui_text_w(F_REG, 11.5f, lab);
            ui_text(F_REG, 11.5f, lab, fminf(fmaxf(px - lw / 2, bx), bx + bw - lw), by + 24, TH.text3);
        }
    }
    /* marker */
    float mx = bx + (float)(bw * frac);
    DrawTriangle((Vector2){ mx - 6, by - 9 }, (Vector2){ mx, by - 1 }, (Vector2){ mx + 6, by - 9 }, fill);
    if (slim) {
        ui_text(F_SEMI, 14, cur, r.x, by - 3, TH.text);
        ui_text(F_REG, 12.5f, pct, r.x + r.width - pw + 12, by - 2, TH.text3);
        return;
    }
    ui_text(F_SEMI, 14, cur, bx, r.y + 4, TH.text);
    ui_text(F_REG, 12.5f, pct, bx + bw - ui_text_w(F_REG, 12.5f, pct), r.y + 6, TH.text3);
}

/* ============================================================================ residual chart */
/* "h (topAir)" -> "topAir"; "I (rays)" and plain names have no region */
static bool series_region(const char *name, char *out, int cap)
{
    const char *o = strrchr(name, '(');
    size_t n = strlen(name);
    if (!o || o == name || o[-1] != ' ' || name[n - 1] != ')' || !strcmp(o, "(rays)")) { out[0] = 0; return false; }
    snprintf(out, (size_t)cap, "%.*s", (int)(name + n - 1 - (o + 1)), o + 1);
    return true;
}

/* region the chart shows: the user's pick, or ("-" = not picked) the first region when the series crowd the chart.
   Resolved every frame, because series appear one by one while the first time step is parsed. */
static char chartRegion[64];

static bool series_shown(const Series *s)
{
    if (s->hidden) return false;
    if (!chartRegion[0]) return true;
    char rg[64];
    return series_region(s->name, rg, sizeof rg) && !strcmp(rg, chartRegion);
}

/* the chart's Expand / Restore: the chart takes the whole page (the log's Expand does the same for the log) */
float chart_max_button(float right, float y)
{
    const char *xl = A.chartMax ? "Restore" : "Expand", *xi = A.chartMax ? ICON_CHEVRON_DOWN : ICON_CHEVRON_UP;
    float xw = ui_button_width(xi, xl);
    Rectangle b = { right - xw, y, xw, 28 };
    if (ui_button(ui_id("chart-max"), b, xi, xl, BTN_GHOST)) { A.chartMax = !A.chartMax; A.logMax = false; }
    ui_tooltip(b, A.chartMax ? "Back to the normal layout (Esc)" : "Give the chart the whole page");
    if (A.chartMax && inp_key_pressed(KEY_ESCAPE) && !ui_any_input_focused() && A.modal == MD_NONE) A.chartMax = false;
    return xw;
}

void run_chart(Rectangle r, LogMon *m)
{
    ui_rect(r, 12, TH.chartBg);
    ui_rect_lines(r, 12, 1, TH.lineSoft);
    float xw = m && m->nSeries ? chart_max_button(r.x + r.width - 10, r.y + 10) + 8 : 0;   /* the legend wraps before it */
    if (!m || m->nSeries == 0) {
        /* a run that ended with nothing to plot: a linear step (CalculiX *STATIC) is solved in one go, no iterations to show
           (W0, 2026-11-14: it said residuals would appear, after the run) */
        bool done = m && m->ended && !m->fatal;
        ui_icon(done ? ICON_CIRCLE_CHECK : ICON_CHART_LINE, r.x + r.width / 2, r.y + r.height / 2 - 18, 28, TH.text3);
        const char *t = !done ? "Residuals appear here while a solver runs."
                      : A.cs && A.cs->s.prog == PROG_CALCULIX ? "Solved in one go: a linear step has no iterations to plot. The results are on the Results page."
                      : "The run ended without iterations to plot. The results are on the Results page.";
        ui_text_center(F_REG, 13.5f, t, (Rectangle){ r.x, r.y + r.height / 2 + 2, r.width, 22 }, TH.text3);
        return;
    }
    /* multi-region logs: one row of region pills; with many series start on the first region */
    char regions[16][64]; int nReg = 0;
    for (int i = 0; i < m->nSeries && nReg < 16; i++) {
        char rg[64];
        if (!series_region(m->series[i].name, rg, sizeof rg)) continue;
        bool have = false;
        for (int k = 0; k < nReg; k++) if (!strcmp(regions[k], rg)) have = true;
        if (!have) pl_strlcpy(regions[nReg++], rg, 64);
    }
    if (A.chartRegion[0] == '-') pl_strlcpy(chartRegion, nReg && m->nSeries > 8 ? regions[0] : "", sizeof chartRegion);
    else {
        bool ok = !A.chartRegion[0];
        for (int k = 0; k < nReg; k++) if (!strcmp(regions[k], A.chartRegion)) ok = true;
        pl_strlcpy(chartRegion, ok ? A.chartRegion : "", sizeof chartRegion);
    }
    float lx = r.x + 16, ly = r.y + 12;
    if (nReg > 1) {
        for (int k = -1; k < nReg; k++) {
            const char *lab = k < 0 ? ui_cap("All regions") : regions[k];          /* region names keep their case */
            bool on = k < 0 ? !chartRegion[0] : !strcmp(chartRegion, regions[k]);
            float w = ui_text_w(F_MED, 12.5f, lab) + 24;
            if (lx + w > r.x + r.width - 12 - xw) { lx = r.x + 16; ly += 30; }
            Rectangle pill = { lx, ly, w, 24 };
            bool hov = ui_hovered(pill);
            if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
            ui_rect(pill, 12, on ? (TH.lcars ? TH.lc[k < 0 ? 0 : (k + 1) % 4] : TH.accent) : hov ? TH.hover : BLANK);
            if (!on) ui_rect_lines(pill, 12, 1, TH.line);
            ui_text(F_MED, 12.5f, lab, lx + 12, ly + 4, on ? TH.textOnAccent : TH.text2);
            if (ui_clicked(pill)) pl_strlcpy(A.chartRegion, k < 0 ? "" : regions[k], sizeof A.chartRegion);
            lx += w + 6;
        }
        lx = r.x + 16; ly += 34;
    }
    /* legend: in a short chart one row, the rest behind a "+N" chip (their names in its tooltip) so the plot keeps its room */
    bool oneRow = r.height < 200;
    float legendY = ly;
    for (int i = 0; i < m->nSeries; i++) {
        Series *s = &m->series[i];
        if (chartRegion[0]) { char rg[64]; if (!series_region(s->name, rg, sizeof rg) || strcmp(rg, chartRegion)) continue; }
        Color c = TH.series[i % 10];
        char lab[96], nm[48];
        pl_strlcpy(nm, s->name, sizeof nm);
        if (chartRegion[0]) { char *o = strrchr(nm, '('); if (o && o > nm) o[-1] = 0; }     /* region is in the pill */
        snprintf(lab, sizeof lab, "%s  %.1e", nm, s->n ? s->v[s->n - 1] : 0.0);
        float w = ui_text_w(F_MED, 12.5f, lab) + 30;
        if (oneRow && ly == legendY && lx + w > r.x + r.width - 12 - xw - (i + 1 < m->nSeries ? 48 : 0)) {
            int more = 0; char names[400] = "";
            for (int j = i; j < m->nSeries; j++) {
                if (chartRegion[0]) { char rg[64]; if (!series_region(m->series[j].name, rg, sizeof rg) || strcmp(rg, chartRegion)) continue; }
                more++;
                snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s %.1e", strlen(names) ? "\n" : "", m->series[j].name,
                         m->series[j].n ? m->series[j].v[m->series[j].n - 1] : 0.0);
            }
            char t[16]; snprintf(t, sizeof t, "+%d", more);
            Rectangle chip = { lx, ly, ui_text_w(F_MED, 12.5f, t) + 20, 24 };
            ui_rect(chip, 12, ui_alpha(TH.text3, 0.15f));
            ui_text(F_MED, 12.5f, t, lx + 10, ly + 4, TH.text2);
            ui_tooltip(chip, TextFormat("%s\n(Expand shows them all)", names));
            break;
        }
        if (lx + w > r.x + r.width - 12 - xw) { lx = r.x + 16; ly += 28; }
        Rectangle chip = { lx, ly, w, 24 };
        bool hov = ui_hovered(chip);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (ui_clicked(chip)) s->hidden = !s->hidden;
        ui_rect(chip, 12, s->hidden ? BLANK : ui_alpha(c, 0.13f));
        if (s->hidden || hov) ui_rect_lines(chip, 12, 1, ui_alpha(c, 0.4f));
        DrawCircleV((Vector2){ lx + 12, ly + 12 }, 4, s->hidden ? ui_alpha(c, 0.35f) : c);
        ui_text(F_MED, 12.5f, lab, lx + 22, ly + 4, s->hidden ? TH.text3 : TH.text);
        lx += w + 8;
    }
    bool shortBox = r.height < 200;                              /* large UI scale, small window: tighter margins */
    float pTop = ly + (shortBox ? 30 : 40);
    Rectangle p = { r.x + 58, pTop, r.width - 58 - 20, r.y + r.height - pTop - (shortBox ? 26 : 34) };
    if (p.height < 40) {
        if (m->nSeries > 0 && r.y + r.height - (ly + 30) > 16)
            ui_text_clip(F_REG, 12, "Too short to plot: Expand, or drag the line under the chart down.", r.x + 16, ly + 32, r.width - 32, TH.text3);
        return;
    }
    /* ranges. The y range ignores zeros and the lowest 1% of points (a 1e-14 first Uy residual would
       otherwise stretch the axis over a dozen empty decades) and spans at most 12 decades. */
    enum { DLO = -40, DHI = 20 };
    int hist[DHI - DLO + 1] = { 0 }, total = 0;
    float tmin = 1e30f, tmax = -1e30f;
    for (int i = 0; i < m->nSeries; i++) {
        Series *s = &m->series[i];
        if (!series_shown(s)) continue;
        for (int k = 0; k < s->n; k++) {
            tmin = fminf(tmin, s->t[k]); tmax = fmaxf(tmax, s->t[k]);
            if (!(s->v[k] > 0)) continue;
            int d = (int)floorf(log10f(s->v[k]));
            hist[(d < DLO ? DLO : d > DHI ? DHI : d) - DLO]++;
            total++;
        }
    }
    if (tmax < tmin) return;
    if (tmax - tmin < 1e-12f) { tmax = tmin + 1; }
    int d0 = -6, d1 = 0;
    if (total) {
        int lo = 0, hi = DHI - DLO, cum = 0, cut = total / 100;
        while (lo < hi && cum + hist[lo] <= cut) cum += hist[lo++];
        while (hi > lo && !hist[hi]) hi--;
        d0 = lo + DLO; d1 = hi + DLO + 1;
    }
    if (d1 > 3) d1 = 3;
    if (d0 < d1 - 12) d0 = d1 - 12;
    if (d1 <= d0) d1 = d0 + 1;
    /* grid: a line per decade, labels only as often as the height allows */
    float perDec = p.height / (float)(d1 - d0);
    int every = (int)ceilf(18.f / fmaxf(perDec, 1.f));
    if (every < 1) every = 1;
    for (int d = d0; d <= d1; d++) {
        float py = p.y + p.height - (float)(d - d0) / (d1 - d0) * p.height;
        bool lab = (d1 - d) % every == 0;
        DrawRectangleRec((Rectangle){ p.x, py, p.width, 1 }, lab ? TH.grid : ui_alpha(TH.grid, 0.45f));
        if (!lab) continue;
        char t[16]; snprintf(t, sizeof t, "1e%d", d);
        ui_text(F_MONO, 11.5f, t, p.x - 10 - ui_text_w(F_MONO, 11.5f, t), py - 7, TH.text3);
    }
    double st = nice_step(tmax - tmin, 6);
    for (double v = ceil(tmin / st) * st; v <= tmax + st * 1e-6; v += st) {
        float px = p.x + (float)((v - tmin) / (tmax - tmin)) * p.width;
        DrawRectangleRec((Rectangle){ px, p.y, 1, p.height }, ui_alpha(TH.grid, 0.6f));
        char lab[32]; snprintf(lab, sizeof lab, "%g", v);
        float lw = ui_text_w(F_REG, 11.5f, lab);
        ui_text(F_REG, 11.5f, lab, px - lw / 2, p.y + p.height + 8, TH.text3);
    }
    /* series */
    ui_clip_push(ui_inset(p, -2, -2));
    for (int i = 0; i < m->nSeries; i++) {
        Series *s = &m->series[i];
        if (!series_shown(s) || s->n == 0) continue;
        Color c = TH.series[i % 10];
        int step = s->n > (int)(p.width * 2) ? s->n / (int)(p.width * 2) : 1;
        Vector2 prev = { 0 };
        bool have = false;
        for (int k = 0; k < s->n; k += step) {
            float v = s->v[k];
            if (step > 1) for (int q = k; q < k + step && q < s->n; q++) if (s->v[q] > v) v = s->v[q];
            if (!(v > 0)) { have = false; continue; }                 /* no residual this step: break the line */
            float lv = fminf(fmaxf(log10f(v), (float)d0), (float)d1);
            Vector2 pt = { p.x + (s->t[k] - tmin) / (tmax - tmin) * p.width, p.y + p.height - (lv - d0) / (d1 - d0) * p.height };
            if (have) DrawLineEx(prev, pt, 1.7f, c);
            prev = pt; have = true;
        }
        if (s->n == 1) DrawCircleV(prev, 3, c);
    }
    ui_clip_pop();
    /* hover read-out */
    Vector2 mp = ui_mouse();
    if (ui_hovered(p)) {
        float t = tmin + (mp.x - p.x) / p.width * (tmax - tmin);
        DrawRectangleRec((Rectangle){ mp.x, p.y, 1, p.height }, ui_alpha(TH.text3, 0.6f));
        char lines[MAX_SERIES][64]; int nl = 0; float tw = 0;
        char head[48]; snprintf(head, sizeof head, "t = %.5g", t);
        for (int i = 0; i < m->nSeries; i++) {
            Series *s = &m->series[i];
            if (!series_shown(s) || !s->n) continue;
            int lo = 0, hi = s->n - 1;
            while (lo < hi) { int mid = (lo + hi) / 2; if (s->t[mid] < t) lo = mid + 1; else hi = mid; }
            pl_fmt_ui(lines[nl], 64, "%s  %.3e", s->name, s->v[lo]);
            tw = fmaxf(tw, ui_text_w(F_MONO, 12, lines[nl]));
            float lv = fminf(fmaxf(log10f(s->v[lo] > 0 ? s->v[lo] : 1e-30f), (float)d0), (float)d1);
            DrawCircleV((Vector2){ p.x + (s->t[lo] - tmin) / (tmax - tmin) * p.width, p.y + p.height - (lv - d0) / (d1 - d0) * p.height }, 3.5f, TH.series[i % 10]);
            nl++;
        }
        float bw = tw + 40, bh = 30 + nl * 18;
        float bx = mp.x + 14 + bw > p.x + p.width ? mp.x - 14 - bw : mp.x + 14;
        Rectangle box = { bx, p.y + 8, bw, bh };
        ui_rect(box, 8, ui_alpha(TH.raised, 0.97f));
        ui_rect_lines(box, 8, 1, TH.line);
        ui_text(F_MED, 12.5f, head, box.x + 12, box.y + 8, TH.text);
        for (int i = 0, k = 0; i < m->nSeries; i++) {
            if (!series_shown(&m->series[i]) || !m->series[i].n) continue;
            DrawCircleV((Vector2){ box.x + 16, box.y + 34 + k * 18 }, 3.5f, TH.series[i % 10]);
            ui_text(F_MONO, 12, lines[k], box.x + 26, box.y + 27 + k * 18, TH.text2);
            k++;
        }
    }
}

/* ============================================================================ monitors */
static bool mon_hidden(const Case *c, const char *col)
{
    char key[80]; snprintf(key, sizeof key, "|%s|", col);
    char hay[600]; snprintf(hay, sizeof hay, "|%s|", c->cfg.monHide);
    return strstr(hay, key) != NULL;
}

static void mon_toggle(Case *c, const char *col)
{
    char hay[600]; snprintf(hay, sizeof hay, "|%s|", c->cfg.monHide);
    char key[80]; snprintf(key, sizeof key, "|%s|", col);
    char *at = strstr(hay, key);
    if (at) memmove(at, at + strlen(key) - 1, strlen(at + strlen(key) - 1) + 1);
    else { size_t L = strlen(hay); snprintf(hay + L, sizeof hay - L, "%s|", col); }
    /* back to "a|b|c" without the outer bars */
    char *b = hay; while (*b == '|') b++;
    size_t L = strlen(b); while (L && b[L - 1] == '|') b[--L] = 0;
    pl_strlcpy(c->cfg.monHide, b, sizeof c->cfg.monHide);
    casecfg_save(c->s.path, &c->cfg);
}

void mon_select(Case *c, MonFile *f);

/* the file shown: the remembered one, else forces/coefficients first, else the first */
MonFile *mon_current(Case *c)
{
    if (A.mons.n == 0) return NULL;
    int k = c->cfg.monFile[0] ? monset_find(&A.mons, c->cfg.monFile) : -1;
    if (k < 0) {
        for (int i = 0; i < A.mons.n && k < 0; i++) if (strstr(A.mons.f[i].key, "coefficient")) k = i;
        for (int i = 0; i < A.mons.n && k < 0; i++) if (strstr(A.mons.f[i].key, "force")) k = i;
        if (k < 0) k = 0;
        else if (strstr(A.mons.f[k].key, "coefficient") && A.mons.f[k].nCols) mon_select(c, &A.mons.f[k]);   /* remembered, with Cd, Cl, Cm */
    }
    return &A.mons.f[k];
}

/* the force coefficients (forceCoeffs' coefficient.dat), if the case writes them */
static MonFile *mon_coeffs(int *cd, int *cl, int *cm)
{
    for (int i = 0; i < A.mons.n; i++) {
        MonFile *f = &A.mons.f[i];
        if (!strstr(f->key, "coefficient")) continue;
        *cd = *cl = *cm = -1;
        for (int k = 0; k < f->nCols; k++) {
            if (!strcmp(f->col[k], "Cd")) *cd = k;
            else if (!strcmp(f->col[k], "Cl")) *cl = k;
            else if (!strcmp(f->col[k], "CmPitch")) *cm = k;
        }
        if (*cd >= 0 && *cl >= 0) return f;
    }
    return NULL;
}

/* the heat through the walls (wallHeatFlux, Physics > Setup with heat on): its "<wall> integral" columns, W */
static MonFile *mon_heat(void)
{
    for (int i = 0; i < A.mons.n; i++) if (strstr(A.mons.f[i].key, "wallHeatFlux")) return &A.mons.f[i];
    return NULL;
}

/* a heated wall's Nusselt number (Nu_<wall>, Physics > Setup with heat on): its one column */
static bool nu_file(const MonFile *f) { return pl_starts_with(f->key, "Nu_") && f->nCols >= 1; }

static bool heat_col(const MonFile *f, int k) { return pl_ends_with(f->col[k], " integral"); }

void mon_select(Case *c, MonFile *f)
{
    pl_strlcpy(c->cfg.monFile, f->key, sizeof c->cfg.monFile);
    c->cfg.monHide[0] = 0;
    if (strstr(f->key, "wallHeatFlux")) {           /* each wall's heat (W), not its least and greatest flux */
        for (int i = 0; i < f->nCols; i++) {
            if (heat_col(f, i)) continue;
            size_t L = strlen(c->cfg.monHide); snprintf(c->cfg.monHide + L, sizeof c->cfg.monHide - L, "%s%s", L ? "|" : "", f->col[i]);
        }
        casecfg_save(c->s.path, &c->cfg);
        return;
    }
    if (strstr(f->key, "yPlus")) {                  /* each wall's mean y+, not its least and greatest */
        for (int i = 0; i < f->nCols; i++) {
            if (pl_ends_with(f->col[i], " average")) continue;
            size_t L = strlen(c->cfg.monHide); snprintf(c->cfg.monHide + L, sizeof c->cfg.monHide - L, "%s%s", L ? "|" : "", f->col[i]);
        }
        casecfg_save(c->s.path, &c->cfg);
        return;
    }
    if (strstr(f->key, "coefficient")) {            /* the force coefficients: Cd, Cl and the pitching moment, not their front/rear parts */
        for (int i = 0; i < f->nCols; i++) {
            if (!strcmp(f->col[i], "Cd") || !strcmp(f->col[i], "Cl") || !strcmp(f->col[i], "CmPitch")) continue;
            size_t L = strlen(c->cfg.monHide); snprintf(c->cfg.monHide + L, sizeof c->cfg.monHide - L, "%s%s", L ? "|" : "", f->col[i]);
        }
        casecfg_save(c->s.path, &c->cfg);
        return;
    }
    /* many columns (solver info, min/max with locations): start with the first six */
    for (int i = 6; i < f->nCols; i++) { size_t L = strlen(c->cfg.monHide); snprintf(c->cfg.monHide + L, sizeof c->cfg.monHide - L, "%s%s", L ? "|" : "", f->col[i]); }
    casecfg_save(c->s.path, &c->cfg);
}

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

static const char *mon_chip_label(const MonFile *f, int i)   /* "name  latest" */
{
    float last = NAN;
    for (int k = f->n - 1; k >= 0 && isnan(last); k--) last = f->v[i][k];
    return isnan(last) ? f->col[i] : TextFormat("%s  %.4g", f->col[i], last);
}

static void mon_chart(Rectangle r, Case *c)
{
    ui_rect(r, 12, TH.chartBg);
    ui_rect_lines(r, 12, 1, TH.lineSoft);
    MonFile *f = mon_current(c);
    if (!f) return;
    /* file picker */
    const char *labels[MON_MAX_FILES];
    for (int i = 0; i < A.mons.n; i++) labels[i] = A.mons.f[i].label;
    char pick[200] = "";
    /* a short chart (large UI scale, small window): the legend goes on the picker's row, a narrower picker, no sample count */
    bool compact = r.height - 48 - 40 - 34 < 90;
    float pw = compact ? fminf(240, r.width * 0.3f) : fminf(360, r.width * 0.45f);
    if (ui_dropdown(ui_id("mon-file"), (Rectangle){ r.x + 14, r.y + 10, pw, 30 }, f->label, labels, A.mons.n, 0, pick, sizeof pick, "file")) {
        for (int i = 0; i < A.mons.n; i++) if (!strcmp(A.mons.f[i].label, pick)) { mon_select(c, &A.mons.f[i]); f = &A.mons.f[i]; }
    }
    char info[96]; snprintf(info, sizeof info, "%d samples%s", f->n, f->n ? "" : " yet");
    if (!compact) ui_text(F_REG, 12, info, r.x + 26 + pw, r.y + 17, TH.text3);
    /* legend: one chip per column, click to show/hide. When the chips would leave the plot under 90 px (nine wall-heat
       columns at a large UI scale), names only; still too many, the ones that fit and a "+N" chip that expands the chart */
    float right = r.x + r.width - 12, moreW = ui_text_w(F_MED, 12, "+48") + 24;
    float x0 = compact ? r.x + 14 + pw + 12 : r.x + 16, y0 = compact ? r.y + 13 : r.y + 48;
    int maxRows = compact ? 1 : 1 + (int)fmaxf(0, floorf((r.height - 48 - 40 - 34 - 90) / 28));
    bool names = false; int shown = f->nCols;
    for (int pass = 0; pass < 2; pass++, names = true) {
        float lx = x0, endX[MON_MAX_COLS]; int row = 1;
        shown = f->nCols;
        for (int i = 0; i < f->nCols; i++) {
            float w = ui_text_w(F_MED, 12, names ? f->col[i] : mon_chip_label(f, i)) + 30;
            if (lx + w > right) {
                if (row == maxRows) { shown = i; break; }
                lx = r.x + 16; row++;
            }
            lx += w + 8; endX[i] = lx;
        }
        if (shown == f->nCols) break;
        if (pass == 1) while (shown > 0 && endX[shown - 1] + moreW > right) shown--;   /* the "+N" chip ends the last row */
    }
    float lx = x0, ly = y0;
    for (int i = 0; i < shown; i++) {
        Color col = TH.series[i % 10];
        bool hid = mon_hidden(c, f->col[i]);
        const char *lab = names ? f->col[i] : mon_chip_label(f, i);
        float w = ui_text_w(F_MED, 12, lab) + 30;
        if (lx + w > r.x + r.width - 12) { lx = r.x + 16; ly += 28; }
        Rectangle chip = { lx, ly, w, 24 };
        bool hov = ui_hovered(chip);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (ui_clicked(chip)) mon_toggle(c, f->col[i]);
        ui_rect(chip, 12, hid ? BLANK : ui_alpha(col, 0.13f));
        if (hid || hov) ui_rect_lines(chip, 12, 1, ui_alpha(col, 0.4f));
        DrawCircleV((Vector2){ lx + 12, ly + 12 }, 4, hid ? ui_alpha(col, 0.35f) : col);
        ui_text(F_MED, 12, lab, lx + 22, ly + 5, hid ? TH.text3 : TH.text);
        lx += w + 8;
    }
    if (shown < f->nCols) {
        char more[16]; snprintf(more, sizeof more, "+%d", f->nCols - shown);
        Rectangle chip = { lx, ly, ui_text_w(F_MED, 12, more) + 24, 24 };
        bool hov = ui_hovered(chip);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (ui_clicked(chip)) { A.chartMax = !A.chartMax; A.logMax = false; }
        ui_rect_lines(chip, 12, 1, hov ? TH.text2 : TH.lineSoft);
        ui_text(F_MED, 12, more, chip.x + 12, ly + 5, TH.text2);
        ui_tooltip(chip, A.chartMax ? "More columns than fit: make the window larger to see them all."
                                    : "More columns than fit here. Click to expand the chart and see them all.");
    }
    float pTop = ly + (compact ? 30 : 40), pBot = compact ? 26 : 34;
    Rectangle p = { r.x + 70, pTop, r.width - 70 - 20, r.y + r.height - pTop - pBot };
    if (p.height < 40 && f->n > 0 && r.y + r.height - (ly + 30) > 16)
        ui_text_clip(F_REG, 12, "Too short to plot: Expand, or drag the line under the chart down.", r.x + 16, ly + 32, r.width - 32, TH.text3);
    if (p.height < 40 || f->n == 0) return;
    float tmin = f->t[0], tmax = f->t[f->n - 1];
    /* the y range leaves out the extreme 2 % at either end: a start-up spike (the first iterations' forces, often 100x the
       settled value) would flatten the rest into a line. Clipped lines run off the chart. */
    static float *vals; static int cap;
    int nv = 0, stride = 1, tot = 0;
    for (int i = 0; i < f->nCols; i++) if (!mon_hidden(c, f->col[i])) tot += f->n;
    if (tot > 40000) stride = tot / 40000 + 1;
    for (int i = 0; i < f->nCols; i++) {
        if (mon_hidden(c, f->col[i])) continue;
        for (int k = 0; k < f->n; k += stride) {
            float v = f->v[i][k]; if (isnan(v)) continue;
            if (nv == cap) {
                int nc = cap ? cap * 2 : 4096;
                float *nvv = realloc(vals, sizeof(float) * (size_t)nc);
                if (!nvv) goto collected;
                vals = nvv; cap = nc;
            }
            vals[nv++] = v;
        }
    }
collected:                                                       /* out of memory: chart what fitted */
    if (nv == 0) return;
    qsort(vals, (size_t)nv, sizeof(float), cmpf);
    double vmin = vals[(int)(nv * 0.02)], vmax = vals[(int)(nv * 0.98) < nv ? (int)(nv * 0.98) : nv - 1];
    if (nv < 50) { vmin = vals[0]; vmax = vals[nv - 1]; }
    if (tmax - tmin < 1e-12f) tmax = tmin + 1;
    if (vmax - vmin < 1e-30) { double m = fabs(vmax) > 0 ? fabs(vmax) * 0.05 : 1; vmin -= m; vmax += m; }
    double pad = (vmax - vmin) * 0.06; vmin -= pad; vmax += pad;
    /* about one label per 22 px of height, so a short chart's labels don't run into each other */
    double st = nice_step(vmax - vmin, (int)fmaxf(2, fminf(6, p.height / 22)));
    for (double v = ceil(vmin / st) * st; v <= vmax; v += st) {
        float py = p.y + p.height - (float)((v - vmin) / (vmax - vmin)) * p.height;
        DrawRectangleRec((Rectangle){ p.x, py, p.width, 1 }, TH.grid);
        char t[32]; snprintf(t, sizeof t, "%.4g", fabs(v) < st * 1e-6 ? 0.0 : v);
        ui_text(F_MONO, 11.5f, t, p.x - 10 - ui_text_w(F_MONO, 11.5f, t), py - 7, TH.text3);
    }
    double xs = nice_step(tmax - tmin, 6);
    for (double v = ceil(tmin / xs) * xs; v <= tmax + xs * 1e-6; v += xs) {
        float px = p.x + (float)((v - tmin) / (tmax - tmin)) * p.width;
        DrawRectangleRec((Rectangle){ px, p.y, 1, p.height }, ui_alpha(TH.grid, 0.6f));
        char lab[32]; snprintf(lab, sizeof lab, "%g", v);
        ui_text(F_REG, 11.5f, lab, px - ui_text_w(F_REG, 11.5f, lab) / 2, p.y + p.height + 8, TH.text3);
    }
    ui_clip_push(ui_inset(p, -2, -2));
    for (int i = 0; i < f->nCols; i++) {
        if (mon_hidden(c, f->col[i])) continue;
        Color col = TH.series[i % 10];
        int step = f->n > (int)(p.width * 2) ? f->n / (int)(p.width * 2) : 1;
        Vector2 prev = { 0 }; bool have = false;
        for (int k = 0; k < f->n; k += step) {
            float v = f->v[i][k];
            if (isnan(v)) { int q = k; while (q < k + step && q < f->n && isnan(f->v[i][q])) q++; if (q < f->n && q < k + step) v = f->v[i][q]; }
            if (isnan(v)) continue;
            Vector2 pt = { p.x + (f->t[k] - tmin) / (tmax - tmin) * p.width, p.y + p.height - (float)((v - vmin) / (vmax - vmin)) * p.height };
            if (have) DrawLineEx(prev, pt, 1.7f, col);
            prev = pt; have = true;
        }
    }
    ui_clip_pop();
    if (ui_hovered(p)) {                                       /* read-out at the mouse */
        Vector2 mp = ui_mouse();
        float t = tmin + (mp.x - p.x) / p.width * (tmax - tmin);
        DrawRectangleRec((Rectangle){ mp.x, p.y, 1, p.height }, ui_alpha(TH.text3, 0.6f));
        int lo = 0, hi = f->n - 1;
        while (lo < hi) { int mid = (lo + hi) / 2; if (f->t[mid] < t) lo = mid + 1; else hi = mid; }
        char lines[MON_MAX_COLS][80]; int cols[MON_MAX_COLS]; int nl = 0; float tw = 0;
        for (int i = 0; i < f->nCols && nl < MON_MAX_COLS; i++) {
            if (mon_hidden(c, f->col[i])) continue;
            float v = NAN;
            for (int k = lo; k >= 0 && k > lo - 50 && isnan(v); k--) v = f->v[i][k];
            if (isnan(v)) continue;
            snprintf(lines[nl], 80, "%s  %.5g", f->col[i], v);
            tw = fmaxf(tw, ui_text_w(F_MONO, 12, lines[nl]));
            cols[nl++] = i;
        }
        char head[48]; snprintf(head, sizeof head, "t = %.5g", f->t[lo]);
        float bw = fmaxf(tw, ui_text_w(F_MED, 12.5f, head)) + 40, bh = 30 + nl * 18;
        float bx = mp.x + 14 + bw > p.x + p.width ? mp.x - 14 - bw : mp.x + 14;
        Rectangle box = { bx, p.y + 8, bw, bh };
        ui_rect(box, 8, ui_alpha(TH.raised, 0.97f));
        ui_rect_lines(box, 8, 1, TH.line);
        ui_text(F_MED, 12.5f, head, box.x + 12, box.y + 8, TH.text);
        for (int k = 0; k < nl; k++) {
            DrawCircleV((Vector2){ box.x + 16, box.y + 34 + k * 18 }, 3.5f, TH.series[cols[k] % 10]);
            ui_text(F_MONO, 12, lines[k], box.x + 26, box.y + 27 + k * 18, TH.text2);
        }
    }
}

/* watts with a prefix: 1.1 mW, 250 W, 3.2 kW */
static void fmt_watts(char *o, size_t cap, double w)
{
    double a = fabs(w);
    if (a >= 1e6) snprintf(o, cap, "%.3g MW", w / 1e6);
    else if (a >= 1e3) snprintf(o, cap, "%.3g kW", w / 1e3);
    else if (a >= 1 || a == 0) snprintf(o, cap, "%.3g W", w);
    else if (a >= 1e-3) snprintf(o, cap, "%.3g mW", w * 1e3);
    else snprintf(o, cap, "%.3g µW", w * 1e6);
}

/* a value as it is now over the chart (the force coefficients, the walls' heat): a click plots its file; returns the room
   it took */
static float readout_chip_c(Case *c, MonFile *f, UiId id, const char *t, float x, float y, float room, const char *tip, Color tint)
{
    if (room < 80) return 0;
    Rectangle tr = { x, y, fminf(ui_text_w(F_MONO_MED, 13, t) + 24, room), 30 };
    bool plain = !memcmp(&tint, &TH.accent, sizeof tint);
    ui_rect(tr, 8, ui_alpha(tint, plain ? 0.12f : 0.18f));
    if (!plain) ui_rect_lines(tr, 8, 1, ui_alpha(tint, 0.6f));     /* a verdict (the y+): its colour shows at a glance */
    ui_text_clip(F_MONO_MED, 13, t, tr.x + 12, tr.y + 8, tr.width - 20, plain ? TH.text : tint);
    ui_register(id, tr, t);
    if (ui_hovered(tr)) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    if (ui_clicked(tr)) { A.chartTab = 1; mon_select(c, f); }
    /* cut short (a short chart keeps its chips on one row): the whole value in the tooltip too */
    ui_tooltip(tr, tr.width < ui_text_w(F_MONO_MED, 13, t) + 24 ? TextFormat("%s\n%s", t, tip) : tip);
    return tr.width + 8;
}

static float readout_chip(Case *c, MonFile *f, UiId id, const char *t, float x, float y, float room, const char *tip)
{
    return readout_chip_c(c, f, id, t, x, y, room, tip, TH.accent);
}

/* a short chart's readout (plan A4): plain text, no box, so three or four fit whole beside the switch; a click plots it */
static float readout_text(Case *c, MonFile *f, UiId id, const char *t, float x, float y, float room, float fs, const char *tip, Color tint)
{
    if (room < 40) return 0;
    float tw = ui_text_w(F_MONO_MED, fs, t), w = fminf(tw, room);
    Rectangle tr = { x, y + 4, w, 22 };
    bool hov = ui_hovered(tr);
    ui_text_clip(F_MONO_MED, fs, t, x, y + 8, w, hov ? TH.accent : tint);
    if (hov) { ui_cursor(MOUSE_CURSOR_POINTING_HAND); DrawRectangleRec((Rectangle){ x, y + 26, w, 1 }, TH.accent); }
    ui_register(id, tr, t);
    if (ui_clicked(tr)) { A.chartTab = 1; mon_select(c, f); }
    ui_tooltip(tr, w < tw ? TextFormat("%s\n%s", t, tip) : tip);
    return w;
}

/* the walls' y+ (the setup's yPlus function object, plan step 5): its "<wall> average" and "<wall> max" columns */
static MonFile *mon_yplus(double *avg, double *max)
{
    for (int i = 0; i < A.mons.n; i++) {
        MonFile *f = &A.mons.f[i];
        if (!strstr(f->key, "yPlus")) continue;
        double sum = 0, mx = 0; int n = 0;
        for (int k = 0; k < f->nCols; k++) {
            float last = NAN;
            for (int j = f->n - 1; j >= 0 && isnan(last); j--) last = f->v[k][j];
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

/* residuals, or (when the case writes function-object output) the monitors, on one switch */
void chart_area(Rectangle r, Case *c, LogMon *pm)
{
    if (A.mons.n == 0) {
        run_chart(r, pm);
        /* nothing to plot and nothing running: the next action is to run it */
        if ((!pm || pm->nSeries == 0) && !A.cj->active && !c->s.runningLog[0]) {
            const char *lab = A.continueRun ? "Continue run" : "Run case";
            float bw = ui_button_width(ICON_PLAY, lab);
            if (ui_button(ui_id("chart-run"), (Rectangle){ r.x + (r.width - bw) / 2, r.y + r.height / 2 + 34, bw, 36 }, ICON_PLAY, lab, BTN_PRIMARY)) start_run();
        }
        return;
    }
    const char *items[] = { "Residuals", "Monitors" };
    char m[32]; snprintf(m, sizeof m, "Monitors (%d)", A.mons.n);
    items[1] = m;
    /* a short chart (a large UI scale in a small window) has no line to spare under the switch: the switch narrows so the
       readouts fit beside it */
    bool tight = r.height < 306;
    float sw = tight ? 190 : 300;
    ui_segmented(ui_id("chart-tab"), (Rectangle){ r.x + r.width - sw, r.y, sw, 30 }, items, 2, &A.chartTab);
    /* the values as they are now, beside the switch (and the Monitors tab's Expand); short of room, on a line of their
       own under it. A click shows their chart. */
    char ct[160] = "", ht[240] = "", nt[200] = "", yt[64] = "", yAdv[600] = "";
    double yAvg = 0, yMax = 0;
    MonFile *yf = mon_yplus(&yAvg, &yMax);
    int yState = yf ? setup_yplus_advice(c, yAvg, yMax, yAdv, sizeof yAdv) : -1;
    bool shortChart = r.height < 306;                       /* see the switch below: no room for words in the chip */
    if (yf) snprintf(yt, sizeof yt, yState == 1 && !shortChart ? "y+ %.3g: change the walls" : "y+ %.3g", yAvg);
    MonFile *nf = NULL;
    MonFile *cf = NULL, *hf = mon_heat();
    {   /* lift and drag */
        int cd, cl, cm; MonFile *f = mon_coeffs(&cd, &cl, &cm);
        float last[3] = { NAN, NAN, NAN }; int col[3] = { cd, cl, cm };
        if (f) for (int j = 0; j < 3; j++) for (int k = f->n - 1; k >= 0 && col[j] >= 0 && isnan(last[j]); k--) last[j] = f->v[col[j]][k];
        if (f && !isnan(last[0])) {
            cf = f;
            int n = snprintf(ct, sizeof ct, "Cd %.4g   Cl %.4g", last[0], last[1]);
            if (!isnan(last[2])) n += snprintf(ct + n, sizeof ct - (size_t)n, "   Cm %.4g", last[2]);
            if (fabsf(last[0]) > 1e-12f) snprintf(ct + n, sizeof ct - (size_t)n, "   L/D %.3g", last[1] / last[0]);
        }
    }
    {   /* the heat into the fluid through each wall (negative: out of it) */
        int n = 0;
        for (int k = 0; hf && k < hf->nCols && n < (int)sizeof ht - 40; k++) {
            if (!heat_col(hf, k)) continue;
            float last = NAN;
            for (int i = hf->n - 1; i >= 0 && isnan(last); i--) last = hf->v[k][i];
            if (isnan(last) || last == 0) continue;               /* an adiabatic wall passes none */
            char w[48]; fmt_watts(w, sizeof w, last);
            n += snprintf(ht + n, sizeof ht - (size_t)n, "%s%.*s %s", n ? "   " : "Heat in: ", (int)(strlen(hf->col[k]) - 9), hf->col[k], w);
        }
    }
    {   /* each heated wall's Nusselt number */
        int n = 0;
        for (int i = 0; i < A.mons.n && n < (int)sizeof nt - 40; i++) {
            MonFile *f = &A.mons.f[i];
            if (!nu_file(f)) continue;
            float last = NAN;
            for (int k = f->n - 1; k >= 0 && isnan(last); k--) last = f->v[0][k];
            if (isnan(last)) continue;
            const char *w = f->key + 3, *sl = strchr(w, '/');
            /* the size: a cooled wall's flux (so its Nu_ file) is negative, the heat out of the fluid */
            n += snprintf(nt + n, sizeof nt - (size_t)n, "%s%.*s %.3g", n ? "  " : "Nu ", (int)(sl ? sl - w : (long)strlen(w)), w, fabsf(last));
            if (!nf) nf = f;
        }
    }
    float right = r.x + r.width - sw - 8 - (A.chartTab == 1 ? chart_max_button(r.x + r.width - sw - 10, r.y + 1) + 8 : 0);
    float cw = ct[0] ? ui_text_w(F_MONO_MED, 13, ct) + 24 : 0, hw = ht[0] ? ui_text_w(F_MONO_MED, 13, ht) + 24 : 0,
          nw = nt[0] ? ui_text_w(F_MONO_MED, 13, nt) + 24 : 0, yw = yt[0] ? ui_text_w(F_MONO_MED, 13, yt) + 24 : 0;
    int nChip = (cw > 0) + (hw > 0) + (nw > 0) + (yw > 0);
    bool below = r.x + cw + hw + nw + yw + 8 * (nChip - 1) > right;
    /* a short chart (a large UI scale in a small window: step 3's open bug, 2026-11-01) can't spare a line for them:
       they stay beside the switch, sharing its room, cut short with the whole value in their tooltips */
    float share = nChip ? (right - r.x) / nChip : 0;
    if (below && tight && share >= 88) below = false;
    else share = 1e9f;
    /* short: plain text with dots between instead of boxes (24 px of padding each); when one line can't hold them, two
       lines of a size down in the switch's row (plan A4) */
    if (tight && !below && nChip) {
        const char *T[4] = { ct, ht, nt, yt };
        MonFile *F4[4] = { cf, hf, nf, yf };
        const char *ID[4] = { "run-coeffs", "run-heat", "run-nu", "run-yplus" };
        const char *TIP[4] = { "The latest force coefficients (Physics > Setup, Lift and drag). Click to plot them.",
                               "The heat each wall passes into the fluid (Physics > Setup, Reports); negative: out of it. Click to plot it.",
                               "Each heated or cooled wall's Nusselt number, |q| L / (k ΔT). Click to plot it.",
                               yAdv[0] ? yAdv : "The walls' mean y+ at the last write. Click to plot it." };
        float room = right - r.x, fs = 13, sep = ui_text_w(F_REG, 13, "  ·  "), tot = 0; int k = 0;
        for (int i = 0; i < 4; i++) if (T[i][0]) { tot += ui_text_w(F_MONO_MED, 13, T[i]); k++; }
        bool two = tot + sep * (k - 1) > room;
        if (two) { fs = 11.5f; sep = ui_text_w(F_REG, fs, "  ·  "); }
        float rx = r.x, ry = two ? r.y - 6 : r.y; int onLine = 0;
        for (int i = 0; i < 4; i++) {
            if (!T[i][0]) continue;
            float tw = ui_text_w(F_MONO_MED, fs, T[i]);
            if (two && onLine && rx + sep + tw > r.x + room && ry < r.y) { rx = r.x; ry = r.y + 11; onLine = 0; }   /* the second line */
            if (onLine) { ui_text(F_REG, fs, "  ·  ", rx, ry + 8, TH.text3); rx += sep; }
            Color tint = i == 3 ? (yState == 1 ? TH.warn : yState == 0 ? TH.ok : TH.text) : TH.text;
            rx += readout_text(c, F4[i], ui_id(ID[i]), T[i], rx, ry, r.x + room - rx, fs, TIP[i], tint);
            onLine++;
        }
        Rectangle body = { r.x, r.y + 38, r.width, r.height - 38 };
        if (A.chartTab == 1) mon_chart(body, c); else run_chart(body, pm);
        return;
    }
    float rx = r.x, ry = below ? r.y + 38 : r.y, rr = below ? r.x + r.width : right;
    if (ct[0]) rx += readout_chip(c, cf, ui_id("run-coeffs"), ct, rx, ry, fminf(share - 8, rr - rx), "The latest force coefficients (Physics > Setup, Lift and drag). Click to plot them.");
    if (ht[0]) rx += readout_chip(c, hf, ui_id("run-heat"), ht, rx, ry, fminf(share - 8, rr - rx), "The heat each wall passes into the fluid (Physics > Setup, Reports); negative: out of it. Click to plot it.");
    if (nt[0]) rx += readout_chip(c, nf, ui_id("run-nu"), nt, rx, ry, fminf(share - 8, rr - rx), "Each heated or cooled wall's Nusselt number, |q| L / (k ΔT), with the length and ΔT of "
                                                                         "Physics > Setup, Reports (the plot keeps the sign: negative, heat out). Click to plot it.");
    float yx = rx, ycw = 0;
    if (yt[0]) ycw = readout_chip_c(c, yf, ui_id("run-yplus"), yt, rx, ry, fminf(share - 8, rr - rx), yAdv[0] ? yAdv : "The walls' mean y+ at the last write. Click to plot it.",
                                    yState == 1 ? TH.warn : yState == 0 ? TH.ok : TH.accent);
    float top = below && (ct[0] || ht[0] || nt[0] || yt[0]) ? 76 : 38;
    /* the walls don't suit their treatment: what to change, on a line of its own (whole in its tooltip) while the chart
       keeps 150 px under it; else the warning chip says it in its tooltip */
    if (yState == 1 && r.height - top < 172 && ycw > 0) ui_register(ui_id("run-yplus-advice"), (Rectangle){ yx, ry, ycw - 8, 30 }, yAdv);
    if (yState == 1 && r.height - top >= 172) {
        Rectangle ar = { r.x, r.y + top - 4, r.width, 22 };
        ui_icon(ICON_TRIANGLE_ALERT, ar.x + 8, ar.y + 10, 14, TH.warn);
        ui_text_clip(F_REG, 12.5f, yAdv, ar.x + 22, ar.y + 3, ar.width - 22, TH.warn);
        ui_register(ui_id("run-yplus-advice"), ar, yAdv);
        ui_tooltip(ar, yAdv);
        top += 22;
    }
    Rectangle body = { r.x, r.y + top, r.width, r.height - top };
    if (A.chartTab == 1) mon_chart(body, c); else run_chart(body, pm);
}

