/* femeshq.c -- see femeshq.h */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "femeshq.h"

static void sub3(const double *a, const double *b, double *o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const double *a, const double *b, double *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static double len3(const double *a) { return sqrt(dot3(a, a)); }

/* a tetrahedron's aspect ratio from its corners: longest edge / (2 sqrt(6) inradius) */
static double tet_ar(const double *p0, const double *p1, const double *p2, const double *p3)
{
    const double *P[4] = { p0, p1, p2, p3 };
    double emax = 0;
    for (int i = 0; i < 4; i++) for (int j = i + 1; j < 4; j++) { double d[3]; sub3(P[j], P[i], d); emax = fmax(emax, len3(d)); }
    double a[3], b[3], c[3], x[3]; sub3(p1, p0, a); sub3(p2, p0, b); sub3(p3, p0, c); cross3(a, b, x);
    double V = fabs(dot3(x, c)) / 6, S = 0;
    static const int F[4][3] = { { 0, 1, 2 }, { 0, 1, 3 }, { 0, 2, 3 }, { 1, 2, 3 } };
    for (int f = 0; f < 4; f++) { double u[3], v[3], n[3]; sub3(P[F[f][1]], P[F[f][0]], u); sub3(P[F[f][2]], P[F[f][0]], v); cross3(u, v, n); S += len3(n) / 2; }
    if (V <= 0 || S <= 0) return 1e9;
    double r = 3 * V / S;
    return emax / (2 * sqrt(6) * r);
}

/* a quadratic tetrahedron's scaled Jacobian at its corners: the corner's three edges (to the mid-side nodes, doubled) as
   a frame, its determinant over the product of their lengths, against the straight-sided tetrahedron's */
static double tet10_jac(const double *x, const int *v)
{
    /* CalculiX C3D10: corners 0-3, mid-sides 4 (0-1), 5 (1-2), 6 (2-0), 7 (0-3), 8 (1-3), 9 (2-3) */
    static const int E[4][3][2] = { { { 0, 4 }, { 0, 6 }, { 0, 7 } }, { { 1, 5 }, { 1, 4 }, { 1, 8 } }, { { 2, 6 }, { 2, 5 }, { 2, 9 } }, { { 3, 7 }, { 3, 9 }, { 3, 8 } } };
    double worst = 1e9;
    for (int c = 0; c < 4; c++) {
        double e[3][3], l[3];
        for (int k = 0; k < 3; k++) { sub3(&x[3 * v[E[c][k][1]]], &x[3 * v[E[c][k][0]]], e[k]); l[k] = len3(e[k]); }
        double n[3]; cross3(e[0], e[1], n);
        double j = l[0] * l[1] * l[2] > 0 ? dot3(n, e[2]) / (l[0] * l[1] * l[2]) : 0;
        /* the straight-sided element's own value at this corner, so a regular quadratic tetrahedron scores 1 */
        double s[3][3], ls[3];
        static const int C[4][3] = { { 1, 2, 3 }, { 2, 0, 3 }, { 0, 1, 3 }, { 0, 2, 1 } };
        for (int k = 0; k < 3; k++) { sub3(&x[3 * v[C[c][k]]], &x[3 * v[c]], s[k]); ls[k] = len3(s[k]); }
        double ns[3]; cross3(s[0], s[1], ns);
        double j0 = ls[0] * ls[1] * ls[2] > 0 ? dot3(ns, s[2]) / (ls[0] * ls[1] * ls[2]) : 0;
        double r = fabs(j0) > 1e-12 ? j / j0 : 0;
        if (r < worst) worst = r;
    }
    return worst;
}

void fe_quality(const FeMesh *m, FeQuality *q)
{
    memset(q, 0, sizeof *q);
    q->jacMin = 1; q->worst = -1;
    double sum = 0;
    for (int e = 0; e < m->ne; e++) {
        int k = m->ek[e];
        if (k != 4 && k != 10) continue;
        const int *v = &m->en[FE_MAXK * e];
        double ar = tet_ar(&m->x[3 * v[0]], &m->x[3 * v[1]], &m->x[3 * v[2]], &m->x[3 * v[3]]);
        if (ar > q->arMax) { q->arMax = ar; q->worst = e; }
        sum += ar; q->n++;
        if (ar > 10) q->arOver10++;
        if (ar > 30) q->arOver30++;
        if (k == 10) { double j = tet10_jac(m->x, v); if (j < q->jacMin) q->jacMin = j; if (j < 0.2) q->jacBad++; }
    }
    q->arMean = q->n ? sum / q->n : 0;
}

/* ---- the walls: rays against the boundary triangles, through a uniform grid of them */
typedef struct { int nx, ny, nz; double lo[3], h; int *off, *idx; } Grid;

static void tri_of(const FeMesh *m, int f, int t, const double **a, const double **b, const double **c)
{
    const int *v = &m->fn[8 * f];
    int quad = m->fk[f] == 4 || m->fk[f] == 8;
    if (!quad || t == 0) { *a = &m->x[3 * v[0]]; *b = &m->x[3 * v[1]]; *c = &m->x[3 * v[2]]; }
    else { *a = &m->x[3 * v[0]]; *b = &m->x[3 * v[2]]; *c = &m->x[3 * v[3]]; }
}

static bool grid_build(const FeMesh *m, Grid *g)
{
    memset(g, 0, sizeof *g);
    double L = 0; for (int k = 0; k < 3; k++) { L = fmax(L, m->hi[k] - m->lo[k]); g->lo[k] = m->lo[k] - 1e-6; }
    double cells = fmax(8, fmin(64, cbrt((double)m->nf)));
    g->h = L / cells + 1e-9;
    g->nx = (int)((m->hi[0] - g->lo[0]) / g->h) + 1; g->ny = (int)((m->hi[1] - g->lo[1]) / g->h) + 1; g->nz = (int)((m->hi[2] - g->lo[2]) / g->h) + 1;
    size_t nc = (size_t)g->nx * g->ny * g->nz;
    int *cnt = calloc(nc + 1, sizeof(int));
    if (!cnt) return false;
    for (int pass = 0; pass < 2; pass++) {
        for (int f = 0; f < m->nf; f++) {
            double lo[3] = { 1e300, 1e300, 1e300 }, hi[3] = { -1e300, -1e300, -1e300 };
            int kc = m->fk[f] == 4 || m->fk[f] == 8 ? 4 : 3;
            for (int j = 0; j < kc; j++) for (int q = 0; q < 3; q++) { double x = m->x[3 * m->fn[8 * f + j] + q]; lo[q] = fmin(lo[q], x); hi[q] = fmax(hi[q], x); }
            int i0 = (int)((lo[0] - g->lo[0]) / g->h), i1 = (int)((hi[0] - g->lo[0]) / g->h);
            int j0 = (int)((lo[1] - g->lo[1]) / g->h), j1 = (int)((hi[1] - g->lo[1]) / g->h);
            int k0 = (int)((lo[2] - g->lo[2]) / g->h), k1 = (int)((hi[2] - g->lo[2]) / g->h);
            for (int k = k0; k <= k1 && k < g->nz; k++) for (int j = j0; j <= j1 && j < g->ny; j++) for (int i = i0; i <= i1 && i < g->nx; i++) {
                size_t c = ((size_t)k * g->ny + j) * g->nx + i;
                if (pass == 0) cnt[c]++; else g->idx[g->off[c] + cnt[c]++] = f;
            }
        }
        if (pass == 0) {
            g->off = malloc(sizeof(int) * (nc + 1));
            if (!g->off) { free(cnt); return false; }
            g->off[0] = 0;
            for (size_t c = 0; c < nc; c++) g->off[c + 1] = g->off[c] + cnt[c];
            g->idx = g->off[nc] >= 0 ? malloc(sizeof(int) * ((size_t)(unsigned)g->off[nc] + 1)) : NULL;
            if (!g->idx) { free(cnt); free(g->off); g->off = NULL; return false; }
            memset(cnt, 0, sizeof(int) * nc);
        }
    }
    free(cnt);
    return true;
}

/* the nearest hit of a ray from o along d (unit) with the boundary, skipping face skip: the distance, or -1 */
static double ray(const FeMesh *m, const Grid *g, const double o[3], const double d[3], int skip, int *mark, int stamp)
{
    double best = -1;
    /* walk the grid's cells along the ray (fixed steps of half a cell: simple, enough for a thickness) */
    double maxL = (g->nx + g->ny + g->nz) * g->h;
    int lastCell = -1;
    for (double s = 0; s <= maxL; s += g->h * 0.5) {
        double p[3] = { o[0] + d[0] * s, o[1] + d[1] * s, o[2] + d[2] * s };
        int i = (int)((p[0] - g->lo[0]) / g->h), j = (int)((p[1] - g->lo[1]) / g->h), k = (int)((p[2] - g->lo[2]) / g->h);
        if (i < 0 || j < 0 || k < 0 || i >= g->nx || j >= g->ny || k >= g->nz) break;
        int c = (k * g->ny + j) * g->nx + i;
        if (c == lastCell) continue;
        lastCell = c;
        for (int q = g->off[c]; q < g->off[c + 1]; q++) {
            int f = g->idx[q];
            if (f == skip || mark[f] == stamp) continue;
            mark[f] = stamp;
            int nt = m->fk[f] == 4 || m->fk[f] == 8 ? 2 : 1;
            for (int t = 0; t < nt; t++) {
                const double *a, *b, *cc; tri_of(m, f, t, &a, &b, &cc);
                double e1[3], e2[3], pv[3], tv[3], qv[3];
                sub3(b, a, e1); sub3(cc, a, e2); cross3(d, e2, pv);
                double det = dot3(e1, pv);
                if (fabs(det) < 1e-14) continue;
                sub3(o, a, tv);
                double u = dot3(tv, pv) / det; if (u < 0 || u > 1) continue;
                cross3(tv, e1, qv);
                double v = dot3(d, qv) / det; if (v < 0 || u + v > 1) continue;
                double tt = dot3(e2, qv) / det;
                if (tt > 1e-9 && (best < 0 || tt < best)) best = tt;
            }
        }
        if (best >= 0 && best < s - g->h) break;                    /* nothing nearer can come after this */
    }
    return best;
}

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

bool fe_thickness(const FeMesh *m, FeThickness *t)
{
    memset(t, 0, sizeof *t);
    if (m->nf <= 0) return false;
    Grid g;
    if (!grid_build(m, &g)) return false;
    int *mark = calloc((size_t)m->nf, sizeof(int));
    int every = m->nf > 3000 ? m->nf / 3000 + 1 : 1, n = 0;
    double *d = malloc(sizeof(double) * (size_t)(m->nf / every + 2));
    if (!mark || !d) { free(mark); free(d); free(g.off); free(g.idx); return false; }
    int stamp = 0;
    t->thin = 1e300;
    for (int f = 0; f < m->nf; f += every) {
        double nrm[3], c[3] = { 0, 0, 0 };
        if (femesh_face_area(m, f, nrm) <= 0) continue;
        int kc = m->fk[f] == 4 || m->fk[f] == 8 ? 4 : 3;
        for (int j = 0; j < kc; j++) for (int q = 0; q < 3; q++) c[q] += m->x[3 * m->fn[8 * f + j] + q] / kc;
        double dir[3] = { -nrm[0], -nrm[1], -nrm[2] };
        double o[3] = { c[0] + dir[0] * 1e-7, c[1] + dir[1] * 1e-7, c[2] + dir[2] * 1e-7 };
        double s = ray(m, &g, o, dir, f, mark, ++stamp);
        if (s <= 0) continue;
        d[n++] = s;
        if (s < t->thin) { t->thin = s; memcpy(t->at, c, sizeof c); }
        t->mean += s;
    }
    free(mark); free(g.off); free(g.idx);
    if (!n) { free(d); t->thin = 0; return false; }
    qsort(d, (size_t)n, sizeof(double), cmpd);
    t->p5 = d[n / 20];
    t->mean /= n; t->rays = n;
    free(d);
    return true;
}
