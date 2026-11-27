/* report.c -- results written up as Markdown and SVG (see report.h; improvement plan A3) */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "report.h"
#include "foamcase.h"
#include "monitor.h"
#include "pl_util.h"
#include "runlog.h"
#include "runner.h"

static const char *const PAL[RPT_MAX_SERIES] = { "#2563eb", "#dc2626", "#16a34a", "#d97706", "#7c3aed", "#0891b2",
                                                 "#db2777", "#65a30d", "#ea580c", "#4f46e5", "#0d9488", "#9333ea" };

static void xml_text(Str *b, const char *t)
{
    for (; *t; t++) {
        if (*t == '<') str_cat(b, "&lt;"); else if (*t == '>') str_cat(b, "&gt;"); else if (*t == '&') str_cat(b, "&amp;");
        else { char c[2] = { *t, 0 }; str_cat(b, c); }
    }
}

static double nice_step(double range)
{
    double p = pow(10, floor(log10(range > 0 ? range : 1))), f = range / p;
    return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * p;   /* 1, 2, 5 or 10 times a power of ten, near range */
}

bool rpt_svg(const char *path, const char *title, const char *xLabel, const char *yLabel, const RptSeries *s, int ns, bool logY)
{
    if (ns > RPT_MAX_SERIES) ns = RPT_MAX_SERIES;
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    long pts = 0;
    for (int k = 0; k < ns; k++)
        for (int i = 0; i < s[k].n; i++) {
            double x = s[k].x[i], y = s[k].y[i];
            if (!isfinite(x) || !isfinite(y) || (logY && !(y > 0))) continue;
            if (logY) y = log10(y);
            x0 = fmin(x0, x); x1 = fmax(x1, x); y0 = fmin(y0, y); y1 = fmax(y1, y); pts++;
        }
    if (!pts) return false;
    if (x1 - x0 < 1e-300) { x0 -= 0.5; x1 += 0.5; }
    if (y1 - y0 < 1e-300) { double m = fabs(y1) > 0 ? fabs(y1) * 0.1 : 1; y0 -= m; y1 += m; }
    if (logY) { y0 = floor(y0); y1 = ceil(y1); }
    else { double p = (y1 - y0) * 0.06; y0 -= p; y1 += p; }
    const double W = 760, H = 380, L = 74, R = 170, T = 40, B = 52, pw = W - L - R, ph = H - T - B;
    Str b; str_init(&b);
    str_catf(&b, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%g\" height=\"%g\" viewBox=\"0 0 %g %g\" font-family=\"sans-serif\" font-size=\"12\">\n"
                 "<rect width=\"100%%\" height=\"100%%\" fill=\"#ffffff\"/>\n<text x=\"%g\" y=\"24\" font-size=\"15\" font-weight=\"600\" fill=\"#111827\">", W, H, W, H, L);
    xml_text(&b, title);
    str_cat(&b, "</text>\n");
    /* grid and ticks */
    if (logY) {
        for (double e = y0; e <= y1 + 1e-9; e += 1) {
            double py = T + ph - (e - y0) / (y1 - y0) * ph;
            str_catf(&b, "<line x1=\"%g\" y1=\"%.1f\" x2=\"%g\" y2=\"%.1f\" stroke=\"#e5e7eb\"/><text x=\"%g\" y=\"%.1f\" text-anchor=\"end\" fill=\"#4b5563\">1e%d</text>\n",
                     L, py, L + pw, py, L - 6, py + 4, (int)e);
        }
    } else {
        double st = nice_step((y1 - y0) / 5);
        for (double v = ceil(y0 / st) * st; v <= y1 + 1e-12; v += st) {
            double py = T + ph - (v - y0) / (y1 - y0) * ph;
            str_catf(&b, "<line x1=\"%g\" y1=\"%.1f\" x2=\"%g\" y2=\"%.1f\" stroke=\"#e5e7eb\"/><text x=\"%g\" y=\"%.1f\" text-anchor=\"end\" fill=\"#4b5563\">%.4g</text>\n",
                     L, py, L + pw, py, L - 6, py + 4, fabs(v) < st * 1e-6 ? 0 : v);
        }
    }
    double sx = nice_step((x1 - x0) / 6);
    for (double v = ceil(x0 / sx) * sx; v <= x1 + 1e-12; v += sx) {
        double px = L + (v - x0) / (x1 - x0) * pw;
        str_catf(&b, "<line x1=\"%.1f\" y1=\"%g\" x2=\"%.1f\" y2=\"%g\" stroke=\"#f3f4f6\"/><text x=\"%.1f\" y=\"%g\" text-anchor=\"middle\" fill=\"#4b5563\">%.4g</text>\n",
                 px, T, px, T + ph, px, T + ph + 16, fabs(v) < sx * 1e-6 ? 0 : v);
    }
    str_catf(&b, "<rect x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"none\" stroke=\"#9ca3af\"/>\n", L, T, pw, ph);
    str_catf(&b, "<text x=\"%g\" y=\"%g\" text-anchor=\"middle\" fill=\"#374151\">", L + pw / 2, H - 12); xml_text(&b, xLabel); str_cat(&b, "</text>\n");
    str_catf(&b, "<text transform=\"translate(16 %g) rotate(-90)\" text-anchor=\"middle\" fill=\"#374151\">", T + ph / 2); xml_text(&b, yLabel); str_cat(&b, "</text>\n");
    /* series: at most 2000 points each (every k-th) */
    for (int k = 0; k < ns; k++) {
        if (s[k].n <= 0) continue;
        int step = s[k].n > 2000 ? s[k].n / 2000 : 1;
        str_catf(&b, "<polyline fill=\"none\" stroke=\"%s\" stroke-width=\"1.6\" points=\"", PAL[k]);
        int drawn = 0;
        for (int i = 0; i < s[k].n; i += step) {
            double x = s[k].x[i], y = s[k].y[i];
            if (!isfinite(x) || !isfinite(y) || (logY && !(y > 0))) continue;
            if (logY) y = log10(y);
            str_catf(&b, "%.1f,%.1f ", L + (x - x0) / (x1 - x0) * pw, T + ph - (y - y0) / (y1 - y0) * ph);
            drawn++;
        }
        str_cat(&b, "\"/>\n");
        if (drawn == 1) {                                        /* one point: a dot, a line of one point shows nothing */
            double x = s[k].x[0], y = logY ? log10(s[k].y[0]) : s[k].y[0];
            str_catf(&b, "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"4\" fill=\"%s\"/>\n", L + (x - x0) / (x1 - x0) * pw, T + ph - (y - y0) / (y1 - y0) * ph, PAL[k]);
        }
        double ly = T + 10 + k * 18;
        str_catf(&b, "<line x1=\"%g\" y1=\"%g\" x2=\"%g\" y2=\"%g\" stroke=\"%s\" stroke-width=\"3\"/><text x=\"%g\" y=\"%g\" fill=\"#111827\">",
                 L + pw + 14, ly, L + pw + 34, ly, PAL[k], L + pw + 40, ly + 4);
        char nm[40]; snprintf(nm, sizeof nm, "%s", s[k].name ? s[k].name : "");
        xml_text(&b, nm);
        str_cat(&b, "</text>\n");
    }
    str_cat(&b, "</svg>\n");
    bool ok = pl_write_file(path, b.s, b.len);
    str_free(&b);
    return ok;
}

static void md_cell(Str *b, const char *t)                       /* a table cell: no pipes or newlines */
{
    for (; *t; t++) { if (*t == '|') str_cat(b, "\\|"); else if (*t == '\n') str_cat(b, " "); else { char c[2] = { *t, 0 }; str_cat(b, c); } }
}

static void read_starter(const char *casePath, char *title, int tc, char *expect, int ec)
{
    title[0] = expect[0] = 0;
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, ".qc-starter");
    char *t = pl_read_file(p, NULL);
    if (!t) return;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        if (!strncmp(l, "title = ", 8)) pl_strlcpy(title, l + 8, (size_t)tc);
        else if (!strncmp(l, "expect = ", 9)) pl_strlcpy(expect, l + 9, (size_t)ec);
    }
    free(t);
}

static int pngs_in(const char *dir, char out[][256], int max)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) if (pl_ends_with(e->d_name, ".png")) pl_strlcpy(out[n++], e->d_name, 256);
    closedir(d);
    return n;
}

static void pandoc_html(const char *md)
{
    if (system("command -v pandoc >/dev/null 2>&1") != 0) return;
    char html[PL_PATH], q1[PL_PATH + 8], q2[PL_PATH + 8], dir[PL_PATH], qd[PL_PATH + 8], cmd[PL_PATH * 3 + 80];
    pl_strlcpy(html, md, sizeof html); char *dot = strrchr(html, '.'); if (dot) strcpy(dot, ".html");
    pl_dirname(dir, sizeof dir, md);
    pl_shell_quote(q1, sizeof q1, md); pl_shell_quote(q2, sizeof q2, html); pl_shell_quote(qd, sizeof qd, dir);
    if (pl_fmt(cmd, sizeof cmd, "cd %s && pandoc -s --embed-resources %s -o %s >/dev/null 2>&1", qd, q1, q2)) (void)!system(cmd);
}

bool rpt_case(const char *casePath, char *out, int ocap, char *err, int ecap)
{
    Case *c = case_open(casePath);
    if (!c) { snprintf(err, (size_t)ecap, "Could not open the case %s", casePath); return false; }
    char dir[PL_PATH]; pl_join(dir, sizeof dir, casePath, "report");
    if (!pl_mkdirs(dir)) { case_close(c); snprintf(err, (size_t)ecap, "Could not make %s", dir); return false; }
    Str b; str_init(&b);
    char date[40]; time_t now = time(NULL); strftime(date, sizeof date, "%Y-%m-%d %H:%M", localtime(&now));
    str_catf(&b, "# %s\n\nReport written by Q-Continuum on %s from `%s`.\n\n", c->s.name, date, casePath);
    char st[200], ex[1600];
    read_starter(casePath, st, sizeof st, ex, sizeof ex);
    if (st[0]) str_catf(&b, "**Starter: %s.** Expected: %s\n\n", st, ex);
    /* the case */
    str_catf(&b, "## The case\n\n| | |\n|---|---|\n| Solver | `%s` |\n", c->s.app[0] ? c->s.app : "?");
    if (c->s.nRegions) {
        str_cat(&b, "| Regions | ");
        for (int i = 0; i < c->s.nRegions; i++) str_catf(&b, "%s%s", i ? ", " : "", c->s.regions[i]);
        str_cat(&b, " |\n");
    }
    str_catf(&b, "| Time | %g to %g (step %g) |\n| Results | %d time%s, latest %s |\n\n", c->s.startTime, c->s.endTime, c->s.deltaT, c->s.nTimes,
             c->s.nTimes == 1 ? "" : "s", case_latest_result(&c->s)[0] ? case_latest_result(&c->s) : "none");
    /* the last run */
    RunRecord rr[1];
    if (runlog_list(casePath, rr, 1) == 1) {
        RunRecord *r = &rr[0];
        str_catf(&b, "## The last run\n\n| | |\n|---|---|\n| Outcome | %s%s%s |\n| OpenFOAM | %s |\n| Cells | %ld |\n| Processes | %d |\n| Reached | %g of %g |\n"
                     "| Wall time | %.0f s (solver %.0f s) |\n", r->outcome, r->reason[0] ? ": " : "", r->reason, r->version, r->cells, r->procs, r->reached,
                 r->endTime, r->wall, r->exec);
        if (r->nonOrtho >= 0) str_catf(&b, "| Mesh | max non-orthogonality %.1f°, max skewness %.2f |\n", r->nonOrtho, r->skew);
        if (r->nRes) {
            str_cat(&b, "\n**Final initial residuals**\n\n| Equation | Residual |\n|---|---|\n");
            for (int i = 0; i < r->nRes; i++) { str_cat(&b, "| "); md_cell(&b, r->resName[i]); str_catf(&b, " | %.3e |\n", r->resVal[i]); }
        }
        str_cat(&b, "\n");
    }
    /* residual history from the solver log */
    LogSet *ls = calloc(1, sizeof *ls);
    if (ls) {
        logset_open(ls, casePath);
        for (int k = 0; k < 400; k++) logset_poll(ls, 1e9 + k, true);   /* read every log to its end (the budget is per poll) */
        int pi = logset_primary(ls, c->s.app);
        if (pi >= 0 && ls->logs[pi].nSeries) {
            LogMon *m = &ls->logs[pi];
            RptSeries rs[RPT_MAX_SERIES]; int nrs = 0;
            for (int i = 0; i < m->nSeries && nrs < RPT_MAX_SERIES; i++) if (m->series[i].n > 0)
                rs[nrs++] = (RptSeries){ m->series[i].name, m->series[i].t, m->series[i].v, m->series[i].n };
            char svg[PL_PATH]; pl_join(svg, sizeof svg, dir, "residuals.svg");
            if (rpt_svg(svg, "Initial residuals", "time (or iteration)", "residual", rs, nrs, true))
                str_catf(&b, "## Convergence\n\n![Initial residuals](residuals.svg)\n\nFrom `%s`, %d steps.\n\n", m->name, m->steps);
        }
        logset_close(ls);
        free(ls);
    }
    /* monitors: each file's columns, last values and a chart */
    MonSet *ms = calloc(1, sizeof *ms);
    if (ms) {
        monset_open(ms, casePath);
        monset_poll(ms, 1e9);
        if (ms->n) str_cat(&b, "## Monitors\n\n");
        for (int f = 0; f < ms->n; f++) {
            MonFile *mf = &ms->f[f];
            if (!mf->n || !mf->nCols) continue;
            str_cat(&b, "### "); md_cell(&b, mf->label); str_catf(&b, "\n\n| Column | Last value (t = %g) |\n|---|---|\n", mf->t[mf->n - 1]);
            RptSeries rs[RPT_MAX_SERIES]; int nrs = 0;
            for (int k = 0; k < mf->nCols; k++) {
                str_cat(&b, "| "); md_cell(&b, mf->col[k]); str_catf(&b, " | %.6g |\n", mf->v[k][mf->n - 1]);
                if (nrs < 6) rs[nrs++] = (RptSeries){ mf->col[k], mf->t, mf->v[k], mf->n };
            }
            char nm[64], svg[PL_PATH]; snprintf(nm, sizeof nm, "monitor%d.svg", f + 1);
            pl_join(svg, sizeof svg, dir, nm);
            if (mf->n > 1 && rpt_svg(svg, mf->label, "time", "value", rs, nrs, false)) str_catf(&b, "\n![%s](%s)\n", mf->label, nm);
            str_cat(&b, "\n");
        }
        monset_close(ms);
        free(ms);
    }
    /* pictures made on Results > Visualization */
    char vd[PL_PATH]; pl_join(vd, sizeof vd, casePath, "visualization");
    static char png[32][256];
    int np = pngs_in(vd, png, 32);
    if (np) {
        str_cat(&b, "## Pictures\n\n");
        for (int i = 0; i < np; i++) str_catf(&b, "![%s](../visualization/%s)\n\n", png[i], png[i]);
    }
    /* every run, oldest first (History > Captain's log) */
    RunRecord one[1];
    char *hist = runlog_list(casePath, one, 1) > 0 ? runlog_markdown(casePath, c->s.name) : NULL;
    if (hist && *hist) {
        const char *body = hist;
        if (!strncmp(body, "# ", 2)) { body = strchr(body, '\n'); body = body ? body + 1 : ""; }   /* its own title: this report has one */
        str_cat(&b, "## Run history\n\n"); str_cat(&b, body); str_cat(&b, "\n");
    }
    free(hist);
    char md[PL_PATH]; pl_join(md, sizeof md, dir, "report.md");
    bool ok = pl_write_file(md, b.s, b.len);
    str_free(&b);
    case_close(c);
    if (!ok) { snprintf(err, (size_t)ecap, "Could not write %s", md); return false; }
    pandoc_html(md);
    pl_strlcpy(out, md, (size_t)ocap);
    return true;
}

bool rpt_compare(const char *dir, const char *name, const char *const *colNames, int nCols, const RptRow *rows, int nRows,
                 const char *chartTitle, const RptSeries *s, int ns, char *out, int ocap, char *err, int ecap)
{
    if (nCols > 8) nCols = 8;
    Str b; str_init(&b);
    char date[40]; time_t now = time(NULL); strftime(date, sizeof date, "%Y-%m-%d %H:%M", localtime(&now));
    str_catf(&b, "# %s\n\nA comparison of %d cases, written by Q-Continuum on %s.\n\n| Case |", name, nRows, date);
    for (int k = 0; k < nCols; k++) { str_cat(&b, " "); md_cell(&b, colNames[k]); str_cat(&b, " |"); }
    str_cat(&b, "\n|---|");
    for (int k = 0; k < nCols; k++) str_cat(&b, "---|");
    str_cat(&b, "\n");
    for (int i = 0; i < nRows; i++) {
        str_cat(&b, "| "); md_cell(&b, rows[i].caseName); str_cat(&b, " |");
        for (int k = 0; k < nCols; k++) { if (isnan(rows[i].val[k])) str_cat(&b, " - |"); else str_catf(&b, " %.6g |", rows[i].val[k]); }
        str_cat(&b, "\n");
    }
    char base[200]; snprintf(base, sizeof base, "%s-report", name);
    for (char *q = base; *q; q++) if (*q == ' ' || *q == '/') *q = '_';
    if (ns > 0) {
        char svgName[240], svg[PL_PATH]; snprintf(svgName, sizeof svgName, "%s.svg", base);
        pl_join(svg, sizeof svg, dir, svgName);
        if (rpt_svg(svg, chartTitle ? chartTitle : name, "time (or iteration)", chartTitle ? chartTitle : "value", s, ns, false))
            str_catf(&b, "\n![%s](%s)\n", chartTitle ? chartTitle : name, svgName);
    }
    str_cat(&b, "\nEach case's own report (Results > Export report) holds its setup, convergence and pictures:\n\n");
    for (int i = 0; i < nRows; i++) str_catf(&b, "- %s: `%s/report/report.md`\n", rows[i].caseName, rows[i].casePath);
    char md[PL_PATH], fn[240]; snprintf(fn, sizeof fn, "%s.md", base);
    pl_join(md, sizeof md, dir, fn);
    bool ok = pl_write_file(md, b.s, b.len);
    str_free(&b);
    if (!ok) { snprintf(err, (size_t)ecap, "Could not write %s", md); return false; }
    pandoc_html(md);
    pl_strlcpy(out, md, (size_t)ocap);
    return true;
}
