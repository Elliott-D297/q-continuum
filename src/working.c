/* working.c -- "show the working" (see working.h) */
#define _GNU_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "glossary.h"
#include "working.h"

static WkLine *add(Working *w, int kind)
{
    if (w->n >= WK_MAX) return NULL;
    WkLine *l = &w->l[w->n++];
    memset(l, 0, sizeof *l);
    l->kind = (unsigned char)kind;
    return l;
}

void wk_init(Working *w, const char *title)
{
    memset(w, 0, sizeof *w);
    snprintf(w->title, sizeof w->title, "%s", title ? title : "");
}

void wk_num(char *b, size_t cap, double v)
{
    double a = fabs(v);
    if (v == 0) snprintf(b, cap, "0");
    else if (!isfinite(v)) snprintf(b, cap, "%s", isnan(v) ? "-" : v > 0 ? "∞" : "-∞");
    else if (a >= 1e-3 && a < 1e5) snprintf(b, cap, "%.4g", v);
    else snprintf(b, cap, "%.3e", v);
}

void wk_len(double m, char *val, size_t vcap, char *unit, size_t ucap)
{
    double a = fabs(m);
    const char *u = "m"; double k = 1;
    if (a > 0 && a < 1e-3) { u = "µm"; k = 1e6; }
    else if (a > 0 && a < 1) { u = "mm"; k = 1e3; }
    wk_num(val, vcap, m * k);
    snprintf(unit, ucap, "%s", u);
}

static void put_sym(WkLine *l, const char *sym, const char *val, const char *unit, const char *what)
{
    snprintf(l->sym, sizeof l->sym, "%s", sym ? sym : "");
    snprintf(l->val, sizeof l->val, "%s", val ? val : "");
    snprintf(l->unit, sizeof l->unit, "%s", unit ? unit : "");
    snprintf(l->text, sizeof l->text, "%s", what ? what : "");
}

void wk_given_s(Working *w, const char *sym, const char *val, const char *unit, const char *what)
{
    WkLine *l = add(w, WK_GIVEN);
    if (l) put_sym(l, sym, val, unit, what);
}

void wk_given(Working *w, const char *sym, double v, const char *unit, const char *what)
{
    char b[40]; wk_num(b, sizeof b, v);
    wk_given_s(w, sym, b, unit, what);
}

void wk_result_s(Working *w, const char *sym, const char *val, const char *unit, const char *what)
{
    WkLine *l = add(w, WK_RESULT);
    if (l) put_sym(l, sym, val, unit, what);
}

void wk_result(Working *w, const char *sym, double v, const char *unit, const char *what)
{
    char b[40]; wk_num(b, sizeof b, v);
    wk_result_s(w, sym, b, unit, what);
}

void wk_eq(Working *w, const char *eq, const char *what)
{
    WkLine *l = add(w, WK_EQ);
    if (!l) return;
    snprintf(l->text, sizeof l->text, "%s", eq ? eq : "");
    snprintf(l->what, sizeof l->what, "%s", what ? what : "");
}

void wk_step(Working *w, const char *fmt, ...)
{
    WkLine *l = add(w, WK_STEP);
    if (!l) return;
    va_list ap; va_start(ap, fmt); vsnprintf(l->text, sizeof l->text, fmt, ap); va_end(ap);
}

void wk_note(Working *w, const char *fmt, ...)
{
    WkLine *l = add(w, WK_NOTE);
    if (!l) return;
    va_list ap; va_start(ap, fmt); vsnprintf(l->text, sizeof l->text, fmt, ap); va_end(ap);
}

static size_t cat(char *out, size_t cap, size_t o, const char *fmt, ...)
{
    if (o >= cap) return o;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(out + o, cap - o, fmt, ap);
    va_end(ap);
    if (n < 0) return o;
    return o + (size_t)n < cap ? o + (size_t)n : cap - 1;
}

int wk_text(const Working *w, char *out, size_t cap)
{
    static const char *const HEAD[] = { "Given", "Equations", "Working", "Result", "Notes" };
    size_t o = 0;
    if (!cap) return 0;
    out[0] = 0;
    if (w->title[0]) o = cat(out, cap, o, "%s\n", w->title);
    for (int kind = WK_GIVEN; kind <= WK_NOTE; kind++) {
        bool any = false;
        for (int i = 0; i < w->n; i++) {
            const WkLine *l = &w->l[i];
            if (l->kind != kind) continue;
            if (!any) { o = cat(out, cap, o, "\n%s\n", HEAD[kind]); any = true; }
            char t[240], q[160];
            gloss_strip(l->text, t, sizeof t, NULL, 0);
            gloss_strip(l->what, q, sizeof q, NULL, 0);
            if (kind == WK_GIVEN || kind == WK_RESULT)
                o = cat(out, cap, o, "  %s = %s%s%s%s%s\n", l->sym, l->val, l->unit[0] ? " " : "", l->unit, t[0] ? "   " : "", t);
            else if (kind == WK_EQ) o = cat(out, cap, o, "  %s%s%s\n", t, q[0] ? "   " : "", q);
            else o = cat(out, cap, o, "  %s\n", t);
        }
    }
    return (int)o;
}

/* ---- the surface cell (stlmesh.c mesher_surface_cell, level_of) */
void wk_surface_cell(Working *w, double base, double pct, bool octree, double L)
{
    char v[40], u[16], vb[40], ub[16], p[40];
    wk_init(w, "The surface cell size");
    wk_len(base, v, sizeof v, u, sizeof u);
    wk_given_s(w, "B", v, u, "the [[base-size]]");
    wk_num(p, sizeof p, pct);
    wk_given_s(w, "s", p, "%", "the target [[surface-size]], as a share of B");
    if (L > 0) { wk_len(L, v, sizeof v, u, sizeof u); wk_given_s(w, "L", v, u, "the part's longest side"); }
    double h;
    if (octree) {
        int lev = pct >= 100 ? 0 : (int)lround(log2(100.0 / pct));
        h = base / pow(2, lev);
        wk_eq(w, "level = round(log2(100 / s))", "each [[refinement-level]] halves the cell, so the size is rounded to a whole level");
        wk_eq(w, "h = B / 2^level", NULL);
        char lg[40]; wk_num(lg, sizeof lg, pct >= 100 ? 0 : log2(100.0 / pct));
        wk_step(w, "level = round(log2(100 / %s)) = round(%s) = %d", p, lg, lev);
        wk_len(base, v, sizeof v, u, sizeof u); wk_len(h, vb, sizeof vb, ub, sizeof ub);
        wk_step(w, "h = %s %s / 2^%d = %s %s", v, u, lev, vb, ub);
    } else {
        h = base * pct / 100;
        wk_eq(w, "h = B s / 100", NULL);
        wk_len(base, v, sizeof v, u, sizeof u); wk_len(h, vb, sizeof vb, ub, sizeof ub);
        wk_step(w, "h = %s %s × %s / 100 = %s %s", v, u, p, vb, ub);
    }
    wk_len(h, v, sizeof v, u, sizeof u);
    wk_result_s(w, "h", v, u, "a [[cell]] on the surface");
    if (L > 0 && h > 0) {
        wk_eq(w, "n = L / h", "cells along the part");
        char a[40], b[40]; wk_len(L, a, sizeof a, ub, sizeof ub); wk_len(h, b, sizeof b, u, sizeof u);
        char n[40]; wk_num(n, sizeof n, round(L / h));
        wk_step(w, "n = %s %s / %s %s = %s", a, ub, b, u, n);
        wk_result(w, "n", round(L / h), NULL, "cells along the part's length at its surface");
        if (octree && fabs(base * pct / 100 - h) > 1e-9 * base) {
            wk_len(base * pct / 100, a, sizeof a, ub, sizeof ub);
            wk_note(w, "Asked for %s %s; snappyHexMesh and cfMesh can only halve cells, so they make the nearest level.", a, ub);
        }
    }
}

/* ---- the prism-layer stack */
void wk_layer_stack(Working *w, double T, int N, double r, double h)
{
    char v[40], u[16], vb[40], ub[16];
    wk_init(w, "The prism layer stack");
    wk_len(T, v, sizeof v, u, sizeof u);
    wk_given_s(w, "T", v, u, "the [[layer-thickness|stack's thickness]]");
    wk_given(w, "N", N, NULL, "[[prism-layers|layers]]");
    wk_given(w, "r", r, NULL, "the [[expansion-ratio]]: each layer r times the one below");
    if (h > 0) { wk_len(h, v, sizeof v, u, sizeof u); wk_given_s(w, "h", v, u, "the [[surface-size|surface cell]] the layers sit under"); }
    if (N < 1) N = 1;
    double t1, tN;
    if (fabs(r - 1) < 1e-12) {
        wk_eq(w, "t1 = T / N", "equal layers (r = 1)");
        t1 = T / N;
        wk_len(T, v, sizeof v, u, sizeof u); wk_len(t1, vb, sizeof vb, ub, sizeof ub);
        wk_step(w, "t1 = %s %s / %d = %s %s", v, u, N, vb, ub);
        tN = t1;
    } else {
        wk_eq(w, "T = t1 + r t1 + r² t1 + ... = t1 (r^N - 1) / (r - 1)", "the layers are a geometric series");
        wk_eq(w, "t1 = T (r - 1) / (r^N - 1)", "so the [[first-layer]]");
        wk_eq(w, "tN = t1 r^(N-1)", "and the outer layer");
        double rN = pow(r, N);
        t1 = T * (r - 1) / (rN - 1);
        tN = t1 * pow(r, N - 1);
        char a[40], b[40];
        wk_num(a, sizeof a, r); wk_num(b, sizeof b, rN);
        wk_step(w, "r^N = %s^%d = %s", a, N, b);
        wk_len(T, v, sizeof v, u, sizeof u); wk_len(t1, vb, sizeof vb, ub, sizeof ub);
        char rm1[40]; wk_num(rm1, sizeof rm1, r - 1); wk_num(b, sizeof b, rN - 1);
        wk_step(w, "t1 = %s %s × %s / %s = %s %s", v, u, rm1, b, vb, ub);
        char c[40]; wk_num(c, sizeof c, pow(r, N - 1));
        wk_len(tN, v, sizeof v, u, sizeof u);
        wk_step(w, "tN = %s %s × %s^%d = %s %s × %s = %s %s", vb, ub, a, N - 1, vb, ub, c, v, u);
    }
    wk_len(t1, v, sizeof v, u, sizeof u);
    wk_result_s(w, "t1", v, u, "the [[first-layer|first layer]], against the wall");
    wk_len(tN, v, sizeof v, u, sizeof u);
    wk_result_s(w, "tN", v, u, "the outer layer");
    if (h > 0) {
        char p[40]; wk_num(p, sizeof p, 100 * tN / h);
        wk_len(tN, v, sizeof v, u, sizeof u); wk_len(h, vb, sizeof vb, ub, sizeof ub);
        wk_step(w, "tN / h = %s %s / %s %s = %s %%", v, u, vb, ub, p);
        wk_note(w, "The outer layer is held to 30 %% to 100 %% of the surface cell above it: a thinner one under a big cell is "
                   "a sudden jump in size, which snappyHexMesh's quality checks reject and the solver dislikes.");
    }
}
