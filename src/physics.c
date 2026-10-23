/* physics.c -- cell sizes, Courant and Fourier numbers, decomposition, ETA (see physics.h) */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "physics.h"

void decomp_split_n(long np, int dirs, long out[3])
{
    long d[3] = { 1, 1, 1 }, r = np < 1 ? 1 : np, pf[64];
    int npf = 0;
    for (long p = 2; p * p <= r && npf < 63; p++) while (r % p == 0 && npf < 63) { pf[npf++] = p; r /= p; }
    if (r > 1) pf[npf++] = r;
    if (dirs < 1 || dirs > 3) dirs = 3;
    for (int i = npf - 1; i >= 0; i--) { int s = 0; for (int k = 1; k < dirs; k++) if (d[k] < d[s]) s = k; d[s] *= pf[i]; }
    out[0] = d[0]; out[1] = d[1]; out[2] = d[2];
}

double graded_min_cell(double L, int n, double r)
{
    if (n < 1 || !(L > 0)) return 0;
    if (n == 1 || !(r > 0) || fabs(r - 1) < 1e-12) return L / n;
    /* a geometric progression of n cells, first to last ratio r: q = r^(1/(n-1)), first cell L (q - 1) / (q^n - 1) */
    double q = pow(r, 1.0 / (n - 1));
    double first = L * (q - 1) / (pow(q, n) - 1);
    return r > 1 ? first : first * pow(q, n - 1);           /* the smallest is the first cell when r > 1, else the last */
}

/* numbers in a "( ... )" group starting at s; returns how many, -1 on anything that isn't a plain number */
static int numbers(const char *s, const char *end, double *v, int max)
{
    int n = 0;
    while (s < end) {
        while (s < end && (isspace((unsigned char)*s) || *s == '(' || *s == ')')) s++;
        if (s >= end) break;
        if (*s == '$' || *s == '#' || isalpha((unsigned char)*s)) return -1;
        char *e; double d = strtod(s, &e);
        if (e == s) return -1;
        if (n < max) v[n] = d;
        n++; s = e;
    }
    return n;
}

void blockmesh_info(FDFile *bm, BlockMeshInfo *o)
{
    memset(o, 0, sizeof *o);
    o->oneCellDir = -1;
    char sv[64] = "1";
    if (!fd_get(bm, "scale", sv, sizeof sv)) fd_get(bm, "convertToMeters", sv, sizeof sv);
    double scale = pl_is_number(sv) ? atof(sv) : 1;
    if (scale == 0) scale = 1;
    FDEntry *ve = fd_find(bm, "vertices"), *be = fd_find(bm, "blocks");
    if (!ve || !be) { snprintf(o->why, sizeof o->why, "no vertices or blocks"); return; }
    /* vertices: ( (x y z) ... ) */
    static double V[8192][3];
    int nv = 0;
    const char *s = bm->text + ve->valStart, *end = bm->text + ve->valEnd;
    s = strchr(s, '(');
    if (!s || s >= end) { snprintf(o->why, sizeof o->why, "the vertices use code or variables"); return; }
    s++;
    while (s < end && nv < 8192) {
        while (s < end && *s != '(' && *s != ')') { if (*s == '$' || *s == '#') { snprintf(o->why, sizeof o->why, "the vertices use variables or code"); return; } s++; }
        if (s >= end || *s == ')') break;
        const char *c = strchr(s, ')');
        if (!c || c > end) break;
        double p[3];
        if (numbers(s + 1, c, p, 3) != 3) { snprintf(o->why, sizeof o->why, "the vertices use variables or code"); return; }
        for (int k = 0; k < 3; k++) V[nv][k] = p[k] * scale;
        nv++; s = c + 1;
    }
    if (!nv) { snprintf(o->why, sizeof o->why, "no vertices"); return; }
    for (int k = 0; k < 3; k++) { o->lo[k] = INFINITY; o->hi[k] = -INFINITY; }
    for (int i = 0; i < nv; i++) for (int k = 0; k < 3; k++) { o->lo[k] = fmin(o->lo[k], V[i][k]); o->hi[k] = fmax(o->hi[k], V[i][k]); }
    /* blocks: hex (8 vertices) [zone] (nx ny nz) simpleGrading (gx gy gz) | edgeGrading (12) */
    static const int E[3][4][2] = { { {0,1},{3,2},{7,6},{4,5} }, { {0,3},{1,2},{5,6},{4,7} }, { {0,4},{1,5},{2,6},{3,7} } };
    s = bm->text + be->valStart; end = bm->text + be->valEnd;
    o->dxMin = o->dxMinThin = INFINITY;
    int oneCell[3] = { 1, 1, 1 };
    double dirMin[3] = { INFINITY, INFINITY, INFINITY };
    while ((s = strstr(s, "hex")) && s < end) {
        s += 3;
        const char *a = strchr(s, '('), *b = a ? strchr(a, ')') : NULL;
        if (!a || !b || b > end) break;
        double vi[8];
        if (numbers(a + 1, b, vi, 8) != 8) { snprintf(o->why, sizeof o->why, "block %d names its vertices", o->blocks + 1); return; }
        s = b + 1;
        a = strchr(s, '('); b = a ? strchr(a, ')') : NULL;           /* an optional zone name comes first */
        if (!a || !b || b > end) break;
        double nc[3];
        if (numbers(a + 1, b, nc, 3) != 3) { snprintf(o->why, sizeof o->why, "block %d's cell counts use variables", o->blocks + 1); return; }
        s = b + 1;
        double g[12]; int ng = 0;
        while (s < end && isspace((unsigned char)*s)) s++;
        if (!strncmp(s, "simpleGrading", 13) || !strncmp(s, "edgeGrading", 11)) {
            bool simple = s[0] == 's';
            a = strchr(s, '(');
            if (a && a < end) {
                int d = 0; const char *q = a;                    /* the grading's own ( ... ), which may nest (multi-grading) */
                for (; q < end; q++) { if (*q == '(') d++; else if (*q == ')' && --d == 0) break; }
                ng = numbers(a + 1, q, g, 12);
                if (simple && ng != 3) ng = 0;                  /* multi-grading: sized as if uniform */
                if (!simple && ng != 12) ng = 0;
                s = q + 1;
            }
        }
        long cells = 1;
        for (int d = 0; d < 3; d++) {
            int n = (int)nc[d];
            if (n < 1) { snprintf(o->why, sizeof o->why, "block %d has a cell count below 1", o->blocks + 1); return; }
            cells *= n;
            if (n > 1) oneCell[d] = 0;
            for (int k = 0; k < 4; k++) {
                int i0 = (int)vi[E[d][k][0]], i1 = (int)vi[E[d][k][1]];
                if (i0 < 0 || i1 < 0 || i0 >= nv || i1 >= nv) { snprintf(o->why, sizeof o->why, "block %d has an unknown vertex", o->blocks + 1); return; }
                double L = sqrt(pow(V[i0][0] - V[i1][0], 2) + pow(V[i0][1] - V[i1][1], 2) + pow(V[i0][2] - V[i1][2], 2));
                double r = ng == 3 ? g[d] : ng == 12 ? g[d * 4 + k] : 1;
                double m = graded_min_cell(L, n, r);
                if (m > 0) dirMin[d] = fmin(dirMin[d], m);
            }
        }
        o->cells += cells;
        o->blocks++;
    }
    if (!o->blocks) { snprintf(o->why, sizeof o->why, "no hex blocks found"); return; }
    for (int d = 0; d < 3; d++) if (oneCell[d] && o->oneCellDir < 0) o->oneCellDir = d;
    for (int d = 0; d < 3; d++) {
        if (!isfinite(dirMin[d])) continue;
        o->dxMin = fmin(o->dxMin, dirMin[d]);
        if (d != o->oneCellDir) o->dxMinThin = fmin(o->dxMinThin, dirMin[d]);
    }
    if (!isfinite(o->dxMinThin)) o->dxMinThin = o->dxMin;
    o->ok = isfinite(o->dxMin);
    if (!o->ok) snprintf(o->why, sizeof o->why, "no cell sizes");
}

double courant(double U, double dt, double dx) { return dx > 0 ? fabs(U) * dt / dx : -1; }
double fourier(double alpha, double dt, double dx) { return dx > 0 ? alpha * dt / (dx * dx) : -1; }

void human_num(double v, char *out, int cap)
{
    double a = fabs(v);
    if (v == 0) snprintf(out, (size_t)cap, "0");
    else if (a >= 1000 && a < 1e5) snprintf(out, (size_t)cap, "%.0f", v);   /* 2280, not 2.28e+03 */
    else if (a >= 1e-3 && a < 1e5) {
        char t[64]; snprintf(t, sizeof t, "%.3g", v);
        if (!strchr(t, '.') && !strchr(t, 'e') && a < 10 && a == floor(a)) snprintf(t, sizeof t, "%.1f", v);   /* 1 -> 1.0 */
        snprintf(out, (size_t)cap, "%s", t);
    } else snprintf(out, (size_t)cap, "%.2g", v);
}

void courant_text(double U, double dt, double dx, char *out, int cap)
{
    char u[32], t[32], x[32], c[32];
    human_num(U, u, sizeof u); human_num(dt, t, sizeof t); human_num(dx, x, sizeof x); human_num(courant(U, dt, dx), c, sizeof c);
    snprintf(out, (size_t)cap, "Co ≈ |U|·Δt/Δx = %s m/s × %s s / %s m = %s", u, t, x, c);
}

void fourier_text(double k, double rho, double cp, double dt, double dx, char *out, int cap)
{
    double alpha = rho > 0 && cp > 0 ? k / (rho * cp) : 0;
    char a[32], t[32], x[32], f[32], ks[32], rs[32], cs[32];
    human_num(alpha, a, sizeof a); human_num(dt, t, sizeof t); human_num(dx, x, sizeof x); human_num(fourier(alpha, dt, dx), f, sizeof f);
    human_num(k, ks, sizeof ks); human_num(rho, rs, sizeof rs); human_num(cp, cs, sizeof cs);
    snprintf(out, (size_t)cap, "Fo = α·Δt/Δx² = %s m²/s × %s s / (%s m)² = %s, with α = k/(ρ·cp) = %s W/(m·K) / (%s kg/m³ × %s J/(kg·K))",
             a, t, x, f, ks, rs, cs);
}

long procs_for_cells(long want, long cells, long perRank)
{
    if (want < 1) want = 1;
    if (cells <= 0 || perRank <= 0) return want;
    long cap = cells / perRank;
    if (cap < 1) cap = 1;
    return want < cap ? want : cap;
}

double eta_seconds(double tStart, double tNow, double tEnd, double wallSoFar)
{
    if (!(wallSoFar > 0) || !(tNow > tStart) || !(tEnd > tNow)) return tEnd <= tNow && tNow > tStart ? 0 : -1;
    double rate = (tNow - tStart) / wallSoFar;                   /* simulated seconds per wall second */
    return (tEnd - tNow) / rate;
}

/* ============================================================================ 4.3 continuum check (Knudsen number) */
static const GasDiameter GASES[] = {
    { "H2", 2.016, 2.92e-10 }, { "He", 4.003, 2.33e-10 }, { "H2O", 18.015, 4.6e-10 }, { "N2", 28.013, 4.17e-10 },
    { "air", 28.96, 4.19e-10 }, { "O2", 31.999, 4.07e-10 }, { "Ar", 39.948, 4.17e-10 }, { "CO2", 44.01, 5.62e-10 },
};

const GasDiameter *gas_for_molweight(double W)
{
    const GasDiameter *best = NULL;
    double err = 0.03;
    for (size_t i = 0; i < sizeof GASES / sizeof GASES[0]; i++) {
        double e = fabs(W - GASES[i].molWeight) / GASES[i].molWeight;
        if (e < err) { err = e; best = &GASES[i]; }
    }
    return best;
}

double mean_free_path(double T, double p, double d)
{
    const double kB = 1.380649e-23;
    if (p <= 0 || d <= 0 || T <= 0) return INFINITY;
    return kB * T / (sqrt(2.0) * M_PI * d * d * p);
}

const char *knudsen_regime(double kn)
{
    return kn < 0.01 ? "continuum" : kn < 0.1 ? "slip" : kn < 10 ? "transitional" : "free molecular";
}

void knudsen_text(double T, double p, double d, double dx, char *out, int cap)
{
    double l = mean_free_path(T, p, d), kn = dx > 0 ? l / dx : INFINITY;
    char ts[32], ps[32], ds[32], ls[32], xs[32], ks[32];
    human_num(T, ts, sizeof ts); human_num(p, ps, sizeof ps); human_num(d, ds, sizeof ds);
    human_num(l, ls, sizeof ls); human_num(dx, xs, sizeof xs); human_num(kn, ks, sizeof ks);
    snprintf(out, (size_t)cap, "λ = k_B·T/(√2·π·d²·p) = 1.38e-23 J/K × %s K / (√2·π·(%s m)² × %s Pa) = %s m; "
             "Kn = λ/Δx = %s m / %s m = %s (%s)", ts, ds, ps, ls, ls, xs, ks, knudsen_regime(kn));
}
