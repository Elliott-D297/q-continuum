/* gci.c -- Richardson extrapolation and the Grid Convergence Index (see gci.h) */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gci.h"

const char *const GCI_KIND_NAME[GCI__KINDS] = { "monotonic", "oscillating", "diverging", "unchanged" };

double gci_h(double N, int D) { return N > 0 ? pow(1.0 / N, 1.0 / (D == 2 ? 2 : 3)) : 0; }

static const char *num(double v) { static char b[8][40]; static int k; k = (k + 1) & 7; wk_num(b[k], sizeof b[k], v); return b[k]; }
static const char *pct(double v) { static char b[8][40]; static int k; k = (k + 1) & 7; snprintf(b[k], sizeof b[k], "%.3g %%", v * 100); return b[k]; }

bool gci_compute(const GciIn *in, GciOut *o, Working *wk)
{
    Working local; if (!wk) wk = &local;
    memset(o, 0, sizeof *o);
    wk_init(wk, "Mesh independence: Richardson extrapolation and the Grid Convergence Index");
    double pf = in->formalOrder > 0 ? in->formalOrder : 2;
    /* the meshes with a value, fine to coarse (h rising); one per size */
    int idx[GCI_MAX], n = 0;
    for (int i = 0; i < in->n && i < GCI_MAX; i++) {
        if (!(in->h[i] > 0) || !isfinite(in->f[i])) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) if (fabs(in->h[idx[k]] - in->h[i]) <= 1e-9 * in->h[i]) dup = true;
        if (!dup) idx[n++] = i;
    }
    for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) if (in->h[idx[b]] < in->h[idx[a]]) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
    if (n < 2) {
        snprintf(o->why, sizeof o->why, "Needs the quantity on at least two meshes of different sizes (%d so far)", n);
        wk_note(wk, "%s.", o->why);
        return false;
    }
    o->used = n >= 3 ? 3 : 2;
    for (int k = 0; k < o->used; k++) o->idx[k] = idx[k];
    double h1 = in->h[idx[0]], h2 = in->h[idx[1]], f1 = in->f[idx[0]], f2 = in->f[idx[1]];
    double h3 = o->used == 3 ? in->h[idx[2]] : 0, f3 = o->used == 3 ? in->f[idx[2]] : 0;
    o->r21 = h2 / h1; o->r32 = o->used == 3 ? h3 / h2 : 0;
    const char *nm[3] = { "fine", "medium", "coarse" };
    for (int k = 0; k < o->used; k++) {
        char s[8], w2[64]; snprintf(s, sizeof s, "f%d", k + 1); snprintf(w2, sizeof w2, "the quantity on the %s mesh", nm[k]);
        wk_given(wk, s, in->f[idx[k]], "", w2);
    }
    for (int k = 0; k < o->used; k++) {
        char s[8], w2[96]; snprintf(s, sizeof s, "h%d", k + 1); snprintf(w2, sizeof w2, "the %s mesh's cell size, (1/N)^(1/D) or 1/refinement", nm[k]);
        wk_given(wk, s, in->h[idx[k]], "", w2);
    }
    wk_eq(wk, "r21 = h2 / h1,  r32 = h3 / h2", "refinement ratios (above 1.3 for the change to stand out from round-off)");
    double e21 = f2 - f1, e32 = o->used == 3 ? f3 - f2 : 0;
    wk_step(wk, "r21 = %s%s%s", num(o->r21), o->used == 3 ? ",  r32 = " : "", o->used == 3 ? num(o->r32) : "");
    double scale = fmax(fabs(f1), fmax(fabs(f2), fabs(f3)));
    double tiny = 1e-12 * (scale > 0 ? scale : 1);
    if (fabs(e21) <= tiny && (o->used == 2 || fabs(e32) <= tiny)) {
        o->ok = true; o->kind = GCI_SAME; o->fExt = f1; o->p = pf; o->pAssumed = true;
        o->verdict = 0;
        snprintf(o->verdictText, sizeof o->verdictText, "The quantity is the same on every mesh: it doesn't depend on the mesh at all (or the "
                 "meshes didn't change; check they differ).");
        wk_note(wk, "f1 = f2%s: no change between the meshes.", o->used == 3 ? " = f3" : "");
        return true;
    }
    bool assumed = o->used == 2;
    double p = pf;
    if (o->used == 3) {
        o->R = fabs(e32) > tiny ? e21 / e32 : 1e300;
        o->kind = o->R < 0 ? GCI_OSCILLATING : o->R >= 1 ? GCI_DIVERGING : GCI_MONOTONIC;
        wk_eq(wk, "R = e21 / e32 = (f2 - f1) / (f3 - f2)", "0 < R < 1: monotonic convergence; R < 0: oscillating; R > 1: diverging");
        wk_step(wk, "R = %s / %s = %s: %s", num(e21), num(e32), num(o->R), GCI_KIND_NAME[o->kind]);
        if (o->kind == GCI_MONOTONIC || o->kind == GCI_OSCILLATING) {
            double s = e32 / e21 >= 0 ? 1 : -1, lr = log(o->r21);
            p = 1;
            bool conv = false;
            for (int it = 0; it < 200; it++) {
                double q = log((pow(o->r21, p) - s) / (pow(o->r32, p) - s));
                double pn = fabs(log(fabs(e32 / e21)) + q) / lr;
                if (!isfinite(pn)) break;
                if (fabs(pn - p) < 1e-10) { p = pn; conv = true; break; }
                p = pn;
            }
            wk_eq(wk, "p = | ln|e32/e21| + q(p) | / ln r21,  q(p) = ln((r21^p - s) / (r32^p - s)),  s = sign(e32/e21)",
                  "the apparent order: how fast the error falls as the mesh is refined (by fixed-point iteration)");
            if (!conv || !(p > 0.05) || p > 10) {
                wk_step(wk, "p doesn't settle to a sensible value (%s): the formal order %s is used instead", num(p), num(pf));
                p = pf; assumed = true;
            } else wk_step(wk, "p = %s", num(p));
        } else {
            wk_step(wk, "Diverging: no apparent order; the formal order %s is used for an estimate only", num(pf));
            assumed = true;
        }
    } else {
        o->kind = GCI_MONOTONIC;
        wk_note(wk, "Two meshes: the apparent order can't be found; the schemes' formal order p = %s is assumed, with a safety factor of 3 instead of 1.25.", num(pf));
    }
    o->p = p; o->pAssumed = assumed;
    double rp = pow(o->r21, p);
    o->fExt = (rp * f1 - f2) / (rp - 1);
    o->ea = fabs(f1) > 0 ? fabs((f1 - f2) / f1) : fabs(f1 - f2);
    o->eext = fabs(o->fExt) > 0 ? fabs((o->fExt - f1) / o->fExt) : 0;
    double Fs = assumed ? 3 : 1.25;
    o->gci = Fs * o->ea / (rp - 1);
    wk_eq(wk, "f_ext = (r21^p f1 - f2) / (r21^p - 1)", "Richardson extrapolation: the value on an infinitely fine mesh");
    wk_step(wk, "f_ext = (%s × %s - %s) / (%s - 1) = %s", num(rp), num(f1), num(f2), num(rp), num(o->fExt));
    wk_eq(wk, "e_a = |(f1 - f2) / f1|,  e_ext = |(f_ext - f1) / f_ext|", "the change from the medium mesh, and from the extrapolated value");
    wk_step(wk, "e_a = %s,  e_ext = %s", pct(o->ea), pct(o->eext));
    wk_eq(wk, assumed ? "GCI = 3 e_a / (r21^p - 1)" : "GCI = 1.25 e_a / (r21^p - 1)", "Roache's Grid Convergence Index: a band round the fine mesh's value that the "
          "mesh-independent one lies in (with about 95 % confidence)");
    wk_step(wk, "GCI = %s × %s / (%s - 1) = %s", assumed ? "3" : "1.25", pct(o->ea), num(rp), pct(o->gci));
    if (o->used == 3 && !assumed) {
        double ea32 = fabs(f2) > 0 ? fabs((f2 - f3) / f2) : fabs(f2 - f3);
        o->gci32 = 1.25 * ea32 / (pow(o->r32, p) - 1);
        o->asym = o->gci > 0 ? o->gci32 / (rp * o->gci) : 0;
        wk_eq(wk, "GCI32 / (r21^p GCI21) ≈ 1", "in the asymptotic range the error falls as the order says");
        wk_step(wk, "GCI32 = %s;  %s / (%s × %s) = %s", pct(o->gci32), pct(o->gci32), num(rp), pct(o->gci), num(o->asym));
    }
    wk_result(wk, "f_ext", o->fExt, "", "the mesh-independent estimate");
    wk_result_s(wk, "GCI", pct(o->gci), "", "the fine mesh's uncertainty from the mesh");
    if (assumed) wk_result(wk, "p", p, "", "assumed: the formal order");
    else wk_result(wk, "p", p, "", "apparent order");
    /* the verdict, in words */
    char band[80]; snprintf(band, sizeof band, "%s ± %s", num(f1), pct(o->gci));
    if (o->kind == GCI_DIVERGING) {
        o->verdict = 2;
        snprintf(o->verdictText, sizeof o->verdictText, "Diverging: the change grows as the mesh gets finer (R = %s). The meshes aren't fine enough to "
                 "see the trend yet, or the runs haven't converged: check the residuals, then refine further.", num(o->R));
    } else if (o->gci <= 0.03 && (o->used == 2 || assumed || fabs(o->asym - 1) < 0.1)) {
        o->verdict = 0;
        snprintf(o->verdictText, sizeof o->verdictText, "Fine enough: the fine mesh's value is within %s of the mesh-independent one (%s); "
                 "%s.", pct(o->gci), band, o->used == 2 ? "two meshes only, so a third would confirm it" : "the medium mesh would do for further runs if it is within what you need");
    } else if (o->gci <= 0.1) {
        o->verdict = 1;
        snprintf(o->verdictText, sizeof o->verdictText, "Close: the fine mesh is within %s of the mesh-independent value (%s). Good for trends and "
                 "comparisons; refine once more for a number to quote%s.", pct(o->gci), band,
                 o->used == 3 && !assumed && fabs(o->asym - 1) >= 0.1 ? ", and the meshes aren't yet in the asymptotic range" : "");
    } else {
        o->verdict = 2;
        snprintf(o->verdictText, sizeof o->verdictText, "Not yet: the mesh changes the answer by up to %s (%s). Refine (1.5 times each way) and run "
                 "again%s.", pct(o->gci), band, o->kind == GCI_OSCILLATING ? "; it oscillates, so watch more than two meshes" : "");
    }
    o->ok = true;
    return true;
}
