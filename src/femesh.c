/* femesh.c -- see femesh.h */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "femesh.h"
#include "pl_util.h"

/* ---------------------------------------------------------------- element shapes, CalculiX's node order */
typedef struct { int k, nc, ne; const int *edge; int nfc; const int *face; const int *fsz; } Shape;
static const int TET_E[] = { 0,1, 1,2, 2,0, 0,3, 1,3, 2,3 };
static const int TET_F[] = { 0,1,2,-1, 0,3,1,-1, 1,3,2,-1, 2,3,0,-1 };
static const int TET_S[] = { 3, 3, 3, 3 };
static const int HEX_E[] = { 0,1, 1,2, 2,3, 3,0, 4,5, 5,6, 6,7, 7,4, 0,4, 1,5, 2,6, 3,7 };
static const int HEX_F[] = { 0,1,2,3, 4,5,6,7, 0,1,5,4, 1,2,6,5, 2,3,7,6, 3,0,4,7 };
static const int HEX_S[] = { 4, 4, 4, 4, 4, 4 };
static const int WED_E[] = { 0,1, 1,2, 2,0, 3,4, 4,5, 5,3, 0,3, 1,4, 2,5 };
static const int WED_F[] = { 0,1,2,-1, 3,4,5,-1, 0,1,4,3, 1,2,5,4, 2,0,3,5 };
static const int WED_S[] = { 3, 3, 4, 4, 4 };
static bool shape_of(int k, Shape *s)
{
    memset(s, 0, sizeof *s);
    switch (k) {
    case 4: case 10: *s = (Shape){ k, 4, 6, TET_E, 4, TET_F, TET_S }; return true;
    case 8: case 20: *s = (Shape){ k, 8, 12, HEX_E, 6, HEX_F, HEX_S }; return true;
    case 6: case 15: *s = (Shape){ k, 6, 9, WED_E, 5, WED_F, WED_S }; return true;
    default: return false;
    }
}
const char *femesh_ccx_type(int k)
{
    switch (k) { case 4: return "C3D4"; case 10: return "C3D10"; case 8: return "C3D8I"; case 20: return "C3D20R"; case 6: return "C3D6"; case 15: return "C3D15"; default: return NULL; }
}

static void sub3(const double *a, const double *b, double *o) { for (int k = 0; k < 3; k++) o[k] = a[k] - b[k]; }
static void cross3(const double *a, const double *b, double *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* ---------------------------------------------------------------- a hash of up to 4 sorted node indices */
typedef struct { int v[4]; int val, val2; } HKey;
typedef struct { HKey *t; size_t cap, n; } Hash;
static uint64_t hk(const int *v) { uint64_t h = 1469598103934665603ull; for (int i = 0; i < 4; i++) { h ^= (uint64_t)(uint32_t)v[i]; h *= 1099511628211ull; } return h; }
static void key_of(const int *nodes, int c, int *out)
{
    int v[4] = { -1, -1, -1, -1 };
    for (int i = 0; i < c; i++) v[i] = nodes[i];
    for (int i = 0; i < c; i++) for (int j = i + 1; j < c; j++) if (v[j] < v[i]) { int t = v[i]; v[i] = v[j]; v[j] = t; }
    memcpy(out, v, sizeof v);
}
static void hash_init(Hash *h, size_t n) { h->cap = 16; while (h->cap < 2 * n + 16) h->cap <<= 1; h->t = malloc(sizeof(HKey) * h->cap); for (size_t i = 0; i < h->cap; i++) h->t[i].v[0] = -2; h->n = 0; }
static HKey *hash_get(Hash *h, const int *v, bool add)
{
    size_t i = (size_t)hk(v) & (h->cap - 1);
    for (;;) {
        HKey *e = &h->t[i];
        if (e->v[0] == -2) { if (!add) return NULL; memcpy(e->v, v, sizeof e->v); e->val = -1; e->val2 = -1; h->n++; return e; }
        if (!memcmp(e->v, v, sizeof e->v)) return e;
        i = (i + 1) & (h->cap - 1);
    }
}

/* ---------------------------------------------------------------- reading */
void femesh_free(FeMesh *m)
{
    free(m->id); free(m->x); free(m->ek); free(m->en); free(m->fk); free(m->fn); free(m->fg); free(m->fe);
    free(m->gname); free(m->garea); free(m->gnorm); free(m->gcen); free(m->gplanar);
    memset(m, 0, sizeof *m);
}

double femesh_face_area(const FeMesh *m, int f, double nrm[3])
{
    const int *v = &m->fn[8 * f]; int c = m->fk[f] == 4 || m->fk[f] == 8 ? 4 : 3;
    double a[3], b[3], n1[3], n[3] = { 0, 0, 0 };
    for (int t = 0; t < c - 2; t++) {
        sub3(&m->x[3 * v[t + 1]], &m->x[3 * v[0]], a); sub3(&m->x[3 * v[t + 2]], &m->x[3 * v[0]], b);
        cross3(a, b, n1); for (int k = 0; k < 3; k++) n[k] += 0.5 * n1[k];
    }
    double A = sqrt(dot3(n, n));
    if (nrm) for (int k = 0; k < 3; k++) nrm[k] = A > 0 ? n[k] / A : 0;
    return A;
}

static const double *gSortArea;                 /* qsort's key: the groups' areas */
static int cmp_area_desc(const void *a, const void *b)
{
    const double *ar = gSortArea; int i = *(const int *)a, j = *(const int *)b;
    return (ar[j] > ar[i]) - (ar[j] < ar[i]);
}

bool femesh_load(const char *path, FeMesh *m, char *err, int cap)
{
    memset(m, 0, sizeof *m);
    err[0] = 0;
    char *t = pl_read_file(path, NULL);
    if (!t) { snprintf(err, (size_t)cap, "Could not read %s", path); return false; }
    char *p = t;
    #define NEXTLINE() do { char *e_ = strchr(p, '\n'); p = e_ ? e_ + 1 : p + strlen(p); } while (0)
    int nRows = 0, nGroups = 0;
    int *rowG = NULL, *rowK = NULL, *rowN = NULL;      /* the file's face rows (ids) */
    bool ok = !strncmp(p, "QCMESH", 6);
    if (ok) NEXTLINE();
    int maxId = 0;
    if (ok && sscanf(p, "nodes %d", &m->n) == 1 && m->n > 0) {
        NEXTLINE();
        m->id = malloc(sizeof(int) * (size_t)m->n); m->x = malloc(sizeof(double) * 3 * (size_t)m->n);
        for (int i = 0; i < m->n && ok; i++) {
            if (sscanf(p, "%d %lf %lf %lf", &m->id[i], &m->x[3 * i], &m->x[3 * i + 1], &m->x[3 * i + 2]) != 4) ok = false;
            if (m->id[i] > maxId) maxId = m->id[i];
            NEXTLINE();
        }
    } else ok = false;
    int *map = NULL;
    if (ok) {
        map = malloc(sizeof(int) * (size_t)(maxId + 1));
        for (int i = 0; i <= maxId; i++) map[i] = -1;
        for (int i = 0; i < m->n; i++) if (m->id[i] >= 0) map[m->id[i]] = i;
        for (int k = 0; k < 3; k++) { m->lo[k] = 1e300; m->hi[k] = -1e300; }
        for (int i = 0; i < m->n; i++) for (int k = 0; k < 3; k++) { double v = m->x[3 * i + k]; if (v < m->lo[k]) m->lo[k] = v; if (v > m->hi[k]) m->hi[k] = v; }
    }
    if (ok && sscanf(p, "volumes %d", &m->ne) == 1 && m->ne > 0) {
        NEXTLINE();
        m->ek = malloc((size_t)m->ne); m->en = malloc(sizeof(int) * FE_MAXK * (size_t)m->ne);
        for (int e = 0; e < m->ne && ok; e++) {
            char *q = p; int k = (int)strtol(q, &q, 10);
            Shape s;
            if (!shape_of(k, &s)) { snprintf(err, (size_t)cap, "%s has an element of %d nodes (pyramids and others CalculiX lacks)", pl_basename(path), k); ok = false; break; }
            m->ek[e] = (unsigned char)k;
            for (int i = 0; i < k; i++) {
                int id = (int)strtol(q, &q, 10);
                if (id < 0 || id > maxId || map[id] < 0) { ok = false; break; }
                m->en[FE_MAXK * e + i] = map[id];
            }
            NEXTLINE();
        }
    } else if (ok) { snprintf(err, (size_t)cap, "%s has no volume elements", pl_basename(path)); ok = false; }
    if (ok && sscanf(p, "faces %d", &nRows) == 1) {
        NEXTLINE();
        rowG = malloc(sizeof(int) * (size_t)(nRows + 1)); rowK = malloc(sizeof(int) * (size_t)(nRows + 1)); rowN = malloc(sizeof(int) * 8 * (size_t)(nRows + 1));
        for (int r = 0; r < nRows; r++) {
            char *q = p; rowG[r] = (int)strtol(q, &q, 10); rowK[r] = (int)strtol(q, &q, 10);
            for (int i = 0; i < rowK[r] && i < 8; i++) { int id = (int)strtol(q, &q, 10); rowN[8 * r + i] = id >= 0 && id <= maxId ? map[id] : -1; }
            NEXTLINE();
        }
        if (sscanf(p, "groups %d", &nGroups) == 1 && nGroups > 0) {
            NEXTLINE();
            m->ng = nGroups; m->gname = calloc((size_t)nGroups, 64);
            for (int g = 0; g < nGroups; g++) {
                int gi = 0, off = 0;
                if (sscanf(p, "%d %n", &gi, &off) >= 1 && gi >= 0 && gi < nGroups) {
                    char *e = strchr(p + off, '\n'); size_t len = e ? (size_t)(e - p - off) : strlen(p + off);
                    snprintf(m->gname[gi], 64, "%.*s", (int)(len < 63 ? len : 63), p + off);
                }
                NEXTLINE();
            }
        }
    }
    free(t);
    if (!ok) {
        if (!err[0]) snprintf(err, (size_t)cap, "%s is not a mesh Q-Continuum wrote (or it is cut short)", pl_basename(path));
        free(map); free(rowG); free(rowK); free(rowN); femesh_free(m); return false;
    }
    free(map);

    /* corners turned for a positive volume, mid-side nodes put on their edges' places */
    m->quadratic = true;
    for (int e = 0; e < m->ne; e++) {
        Shape s; shape_of(m->ek[e], &s);
        int *v = &m->en[FE_MAXK * e];
        if (s.k == s.nc) m->quadratic = false;
        double a[3], b[3], c[3], n[3];
        int o = s.nc == 8 ? 3 : 2, top = s.nc == 8 ? 4 : 3;
        sub3(&m->x[3 * v[1]], &m->x[3 * v[0]], a); sub3(&m->x[3 * v[o]], &m->x[3 * v[0]], b); sub3(&m->x[3 * v[top]], &m->x[3 * v[0]], c);
        cross3(a, b, n);
        if (dot3(n, c) < 0) {
            m->nFlipped++;
            int w[FE_MAXK]; memcpy(w, v, sizeof w);
            if (s.nc == 4) { v[1] = w[2]; v[2] = w[1]; }
            else { int half = s.nc / 2; for (int i = 0; i < half; i++) { v[i] = w[i + half]; v[i + half] = w[i]; } }
        }
        if (s.k > s.nc) {
            int mids[FE_MAXK], nm = 0;
            for (int i = s.nc; i < s.k; i++) mids[nm++] = v[i];
            for (int ed = 0; ed < s.ne; ed++) {
                double mp[3]; for (int k = 0; k < 3; k++) mp[k] = 0.5 * (m->x[3 * v[s.edge[2 * ed]] + k] + m->x[3 * v[s.edge[2 * ed + 1]] + k]);
                int best = -1; double bd = 1e300;
                for (int i = 0; i < nm; i++) if (mids[i] >= 0) { double d[3]; sub3(&m->x[3 * mids[i]], mp, d); double dd = dot3(d, d); if (dd < bd) { bd = dd; best = i; } }
                v[s.nc + ed] = mids[best]; mids[best] = -1;
            }
        }
    }

    /* boundary faces: in one element only, outward */
    Hash h; hash_init(&h, (size_t)m->ne * 4);
    for (int e = 0; e < m->ne; e++) {
        Shape s; shape_of(m->ek[e], &s);
        const int *v = &m->en[FE_MAXK * e];
        for (int f = 0; f < s.nfc; f++) {
            int c = s.fsz[f], cn[4], key[4];
            for (int i = 0; i < c; i++) cn[i] = v[s.face[4 * f + i]];
            key_of(cn, c, key);
            HKey *k = hash_get(&h, key, true);
            if (k->val < 0) k->val = e * 8 + f; else k->val2 = -3;   /* seen twice: inside */
        }
    }
    int fcap = 1024; m->fk = malloc((size_t)fcap); m->fn = malloc(sizeof(int) * 8 * (size_t)fcap); m->fe = malloc(sizeof(int) * (size_t)fcap);
    for (size_t i = 0; i < h.cap; i++) {
        HKey *k = &h.t[i];
        if (k->v[0] == -2 || k->val2 == -3) continue;
        int e = k->val / 8, f = k->val % 8;
        Shape s; shape_of(m->ek[e], &s);
        const int *v = &m->en[FE_MAXK * e];
        int c = s.fsz[f];
        if (m->nf == fcap) { fcap *= 2; m->fk = realloc(m->fk, (size_t)fcap); m->fn = realloc(m->fn, sizeof(int) * 8 * (size_t)fcap); m->fe = realloc(m->fe, sizeof(int) * (size_t)fcap); }
        int *o = &m->fn[8 * m->nf];
        int lc[4]; for (int j = 0; j < c; j++) lc[j] = s.face[4 * f + j];
        /* outward: the face's normal away from the element's centre */
        double ce[3] = { 0, 0, 0 }, cf[3] = { 0, 0, 0 };
        for (int j = 0; j < s.nc; j++) for (int q = 0; q < 3; q++) ce[q] += m->x[3 * v[j] + q] / s.nc;
        for (int j = 0; j < c; j++) for (int q = 0; q < 3; q++) cf[q] += m->x[3 * v[lc[j]] + q] / c;
        double a[3], b[3], n[3], d[3];
        sub3(&m->x[3 * v[lc[1]]], &m->x[3 * v[lc[0]]], a); sub3(&m->x[3 * v[lc[c - 1]]], &m->x[3 * v[lc[0]]], b); cross3(a, b, n); sub3(cf, ce, d);
        if (dot3(n, d) < 0) { for (int j = 1; j < c - j; j++) { int tt = lc[j]; lc[j] = lc[c - j]; lc[c - j] = tt; } }
        for (int j = 0; j < c; j++) o[j] = v[lc[j]];
        int fk = c;
        if (s.k > s.nc) {                              /* the mid-side nodes of the face's edges */
            for (int j = 0; j < c; j++) {
                int p0 = lc[j], p1 = lc[(j + 1) % c], mid = -1;
                for (int ed = 0; ed < s.ne; ed++)
                    if ((s.edge[2 * ed] == p0 && s.edge[2 * ed + 1] == p1) || (s.edge[2 * ed] == p1 && s.edge[2 * ed + 1] == p0)) mid = v[s.nc + ed];
                o[c + j] = mid;
            }
            fk = 2 * c;
        }
        m->fk[m->nf] = (unsigned char)fk; m->fe[m->nf] = e; m->nf++;
    }
    free(h.t);
    m->fg = malloc(sizeof(int) * (size_t)(m->nf + 1));
    for (int f = 0; f < m->nf; f++) m->fg[f] = -1;

    /* groups: the file's, matched by corners */
    if (nRows > 0 && m->ng > 0) {
        Hash fh; hash_init(&fh, (size_t)m->nf);
        for (int f = 0; f < m->nf; f++) { int key[4]; key_of(&m->fn[8 * f], m->fk[f] == 4 || m->fk[f] == 8 ? 4 : 3, key); hash_get(&fh, key, true)->val = f; }
        for (int r = 0; r < nRows; r++) {
            int c = rowK[r] == 4 || rowK[r] == 8 ? 4 : 3, key[4];
            bool bad = false; for (int i = 0; i < c; i++) if (rowN[8 * r + i] < 0) bad = true;
            if (bad) continue;
            key_of(&rowN[8 * r], c, key);
            HKey *k = hash_get(&fh, key, false);
            if (k && rowG[r] >= 0 && rowG[r] < m->ng) m->fg[k->val] = rowG[r];
        }
        free(fh.t);
        int un = 0; for (int f = 0; f < m->nf; f++) if (m->fg[f] < 0) un++;
        if (un) {                                       /* boundary the groups don't cover */
            m->gname = realloc(m->gname, 64 * (size_t)(m->ng + 1));
            snprintf(m->gname[m->ng], 64, "Other faces");
            for (int f = 0; f < m->nf; f++) if (m->fg[f] < 0) m->fg[f] = m->ng;
            m->ng++;
        }
        m->fileGroups = true;
    } else {
        /* by angle: neighbouring faces whose normals differ by under 35 degrees are one face */
        free(m->gname); m->gname = NULL; m->ng = 0;
        Hash eh; hash_init(&eh, (size_t)m->nf * 2);
        for (int f = 0; f < m->nf; f++) {
            int c = m->fk[f] == 4 || m->fk[f] == 8 ? 4 : 3;
            for (int j = 0; j < c; j++) {
                int ev[2] = { m->fn[8 * f + j], m->fn[8 * f + (j + 1) % c] }, key[4];
                key_of(ev, 2, key);
                HKey *k = hash_get(&eh, key, true);
                if (k->val < 0) k->val = f; else k->val2 = f;
            }
        }
        double *nrm = malloc(sizeof(double) * 3 * (size_t)m->nf);
        for (int f = 0; f < m->nf; f++) femesh_face_area(m, f, &nrm[3 * f]);
        int *stack = malloc(sizeof(int) * (size_t)m->nf);
        const double cosLim = cos(35.0 * M_PI / 180);
        for (int f0 = 0; f0 < m->nf; f0++) {
            if (m->fg[f0] >= 0) continue;
            int g = m->ng++, sp = 0;
            m->fg[f0] = g; stack[sp++] = f0;
            while (sp) {
                int f = stack[--sp], c = m->fk[f] == 4 || m->fk[f] == 8 ? 4 : 3;
                for (int j = 0; j < c; j++) {
                    int ev[2] = { m->fn[8 * f + j], m->fn[8 * f + (j + 1) % c] }, key[4];
                    key_of(ev, 2, key);
                    HKey *k = hash_get(&eh, key, false);
                    if (!k) continue;
                    int o = k->val == f ? k->val2 : k->val;
                    if (o >= 0 && m->fg[o] < 0 && dot3(&nrm[3 * f], &nrm[3 * o]) > cosLim) { m->fg[o] = g; stack[sp++] = o; }
                }
            }
        }
        free(stack); free(nrm); free(eh.t);
    }

    /* per group: area, normal, centre; groups found by angle numbered by size, largest first */
    m->garea = calloc((size_t)m->ng, sizeof(double)); m->gnorm = calloc(3 * (size_t)m->ng, sizeof(double));
    m->gcen = calloc(3 * (size_t)m->ng, sizeof(double)); m->gplanar = calloc((size_t)m->ng, sizeof(bool));
    for (int f = 0; f < m->nf; f++) {
        double n[3], A = femesh_face_area(m, f, n); int g = m->fg[f], c = m->fk[f] == 4 || m->fk[f] == 8 ? 4 : 3;
        m->garea[g] += A;
        for (int k = 0; k < 3; k++) { m->gnorm[3 * g + k] += A * n[k]; for (int j = 0; j < c; j++) m->gcen[3 * g + k] += A * m->x[3 * m->fn[8 * f + j] + k] / c; }
    }
    for (int g = 0; g < m->ng; g++) {
        double l = sqrt(dot3(&m->gnorm[3 * g], &m->gnorm[3 * g]));
        for (int k = 0; k < 3; k++) { m->gnorm[3 * g + k] = l > 0 ? m->gnorm[3 * g + k] / l : 0; m->gcen[3 * g + k] = m->garea[g] > 0 ? m->gcen[3 * g + k] / m->garea[g] : 0; }
        m->gplanar[g] = true;
    }
    for (int f = 0; f < m->nf; f++) { double n[3]; femesh_face_area(m, f, n); if (dot3(n, &m->gnorm[3 * m->fg[f]]) < cos(2.0 * M_PI / 180)) m->gplanar[m->fg[f]] = false; }
    if (!m->fileGroups && m->ng > 1) {
        int *ord = malloc(sizeof(int) * (size_t)m->ng), *inv = malloc(sizeof(int) * (size_t)m->ng);
        for (int g = 0; g < m->ng; g++) ord[g] = g;
        gSortArea = m->garea;
        qsort(ord, (size_t)m->ng, sizeof(int), cmp_area_desc);
        for (int i = 0; i < m->ng; i++) inv[ord[i]] = i;
        for (int f = 0; f < m->nf; f++) m->fg[f] = inv[m->fg[f]];
        size_t G = (unsigned)m->ng;
        double *a2 = malloc(sizeof(double) * G), *n2 = malloc(sizeof(double) * 3 * G), *c2 = malloc(sizeof(double) * 3 * G);
        bool *p2 = malloc(sizeof(bool) * G);
        for (int i = 0; i < m->ng; i++) { a2[i] = m->garea[ord[i]]; p2[i] = m->gplanar[ord[i]]; for (int k = 0; k < 3; k++) { n2[3 * i + k] = m->gnorm[3 * ord[i] + k]; c2[3 * i + k] = m->gcen[3 * ord[i] + k]; } }
        free(m->garea); free(m->gnorm); free(m->gcen); free(m->gplanar);
        m->garea = a2; m->gnorm = n2; m->gcen = c2; m->gplanar = p2;
        free(ord); free(inv);
    }
    if (!m->gname) { m->gname = calloc((size_t)m->ng, 64); for (int g = 0; g < m->ng; g++) snprintf(m->gname[g], 64, "Face %d", g + 1); }
    free(rowG); free(rowK); free(rowN);
    return true;
}
