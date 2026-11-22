/* viz.c -- Results > Visualization's engine (see viz.h) */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "viz.h"

static void sub3(const double *a, const double *b, double *o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static void cross3(const double *a, const double *b, double *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static double len3(const double *a) { return sqrt(dot3(a, a)); }

/* ================================================================ the mesh */
void vz_mesh_free(VzMesh *m)
{
    free(m->pt); free(m->fOff); free(m->fIdx); free(m->own); free(m->nei);
    free(m->fCtr); free(m->fArea); free(m->cCtr); free(m->cVol); free(m->cfOff); free(m->cfIdx); free(m->pcOff); free(m->pcIdx);
    free(m->grid); free(m->gridOff);
    for (int p = 0; p < m->nProc; p++) { free(m->proc[p].cell); free(m->proc[p].face); }
    free(m->proc);
    memset(m, 0, sizeof *m);
}

int vz_patch_of_face(const VzMesh *m, long f)
{
    if (f < m->nInternal) return -1;
    for (int p = 0; p < m->nPatch; p++) if (f >= m->patch[p].start && f < m->patch[p].start + m->patch[p].nFaces) return p;
    return -1;
}

static void face_geometry(VzMesh *m)
{
    m->fCtr = malloc(sizeof(double) * 3 * (size_t)m->nFaces);
    m->fArea = malloc(sizeof(double) * 3 * (size_t)m->nFaces);
    for (long f = 0; f < m->nFaces; f++) {
        long a = m->fOff[f], n = m->fOff[f + 1] - a;
        double avg[3] = { 0, 0, 0 };
        for (long k = 0; k < n; k++) for (int d = 0; d < 3; d++) avg[d] += m->pt[3 * m->fIdx[a + k] + d] / (double)n;
        double S[3] = { 0, 0, 0 }, C[3] = { 0, 0, 0 }, tot = 0;
        for (long k = 0; k < n; k++) {
            const double *p0 = &m->pt[3 * m->fIdx[a + k]], *p1 = &m->pt[3 * m->fIdx[a + (k + 1) % n]];
            double e0[3], e1[3], s[3]; sub3(p0, avg, e0); sub3(p1, avg, e1); cross3(e0, e1, s);
            double ar = len3(s) / 2;
            for (int d = 0; d < 3; d++) { S[d] += s[d] / 2; C[d] += ar * (avg[d] + p0[d] + p1[d]) / 3; }
            tot += ar;
        }
        for (int d = 0; d < 3; d++) { m->fArea[3 * f + d] = S[d]; m->fCtr[3 * f + d] = tot > 0 ? C[d] / tot : avg[d]; }
    }
}

static void cell_geometry(VzMesh *m)
{
    long nc = m->nCells;
    double *est = calloc((size_t)nc * 3, sizeof(double));
    int *cnt = calloc((size_t)nc, sizeof(int));
    for (long f = 0; f < m->nFaces; f++) {
        long cs[2] = { m->own[f], f < m->nInternal ? m->nei[f] : -1 };
        for (int s = 0; s < 2; s++) if (cs[s] >= 0) { for (int d = 0; d < 3; d++) est[3 * cs[s] + d] += m->fCtr[3 * f + d]; cnt[cs[s]]++; }
    }
    for (long c = 0; c < nc; c++) for (int d = 0; d < 3; d++) est[3 * c + d] /= cnt[c] ? cnt[c] : 1;
    m->cCtr = calloc((size_t)nc * 3, sizeof(double));
    m->cVol = calloc((size_t)nc, sizeof(double));
    for (long f = 0; f < m->nFaces; f++) {
        long cs[2] = { m->own[f], f < m->nInternal ? m->nei[f] : -1 };
        for (int s = 0; s < 2; s++) {
            long c = cs[s];
            if (c < 0) continue;
            double r[3]; sub3(&m->fCtr[3 * f], &est[3 * c], r);
            double v = dot3(&m->fArea[3 * f], r) / 3 * (s ? -1 : 1);      /* the pyramid on the face, apex at the estimate */
            m->cVol[c] += v;
            for (int d = 0; d < 3; d++) m->cCtr[3 * c + d] += v * (0.75 * m->fCtr[3 * f + d] + 0.25 * est[3 * c + d]);
        }
    }
    for (long c = 0; c < nc; c++) {
        if (fabs(m->cVol[c]) > 1e-300) for (int d = 0; d < 3; d++) m->cCtr[3 * c + d] /= m->cVol[c];
        else for (int d = 0; d < 3; d++) m->cCtr[3 * c + d] = est[3 * c + d];
    }
    free(est); free(cnt);
}

static void adjacency(VzMesh *m)
{
    long nc = m->nCells, np = m->nPoints;
    m->cfOff = calloc((size_t)nc + 1, sizeof(long));
    for (long f = 0; f < m->nFaces; f++) { m->cfOff[m->own[f] + 1]++; if (f < m->nInternal) m->cfOff[m->nei[f] + 1]++; }
    for (long c = 0; c < nc; c++) m->cfOff[c + 1] += m->cfOff[c];
    m->cfIdx = malloc(sizeof(long) * (size_t)(m->cfOff[nc] > 0 ? m->cfOff[nc] : 1));
    long *fill = calloc((size_t)nc, sizeof(long));
    for (long f = 0; f < m->nFaces; f++) {
        m->cfIdx[m->cfOff[m->own[f]] + fill[m->own[f]]++] = f;
        if (f < m->nInternal) m->cfIdx[m->cfOff[m->nei[f]] + fill[m->nei[f]]++] = f;
    }
    free(fill);
    /* point to cells, each cell once per point */
    long *last = malloc(sizeof(long) * (size_t)np);
    for (long p = 0; p < np; p++) last[p] = -1;
    m->pcOff = calloc((size_t)np + 1, sizeof(long));
    for (long c = 0; c < nc; c++)
        for (long k = m->cfOff[c]; k < m->cfOff[c + 1]; k++) {
            long f = m->cfIdx[k];
            for (long j = m->fOff[f]; j < m->fOff[f + 1]; j++) { long p = m->fIdx[j]; if (last[p] != c) { last[p] = c; m->pcOff[p + 1]++; } }
        }
    for (long p = 0; p < np; p++) m->pcOff[p + 1] += m->pcOff[p];
    m->pcIdx = malloc(sizeof(long) * (size_t)(m->pcOff[np] > 0 ? m->pcOff[np] : 1));
    long *pf = calloc((size_t)np, sizeof(long));
    for (long p = 0; p < np; p++) last[p] = -1;
    for (long c = 0; c < nc; c++)
        for (long k = m->cfOff[c]; k < m->cfOff[c + 1]; k++) {
            long f = m->cfIdx[k];
            for (long j = m->fOff[f]; j < m->fOff[f + 1]; j++) { long p = m->fIdx[j]; if (last[p] != c) { last[p] = c; m->pcIdx[m->pcOff[p] + pf[p]++] = c; } }
        }
    free(pf); free(last);
}

static void build_grid(VzMesh *m)
{
    double L[3]; for (int d = 0; d < 3; d++) L[d] = fmax(m->hi[d] - m->lo[d], 1e-12);
    double target = fmax(1, (double)m->nCells / 4), vol = L[0] * L[1] * L[2], s = cbrt(vol / target);
    for (int d = 0; d < 3; d++) { m->gn[d] = (int)fmin(256, fmax(1, ceil(L[d] / s))); }
    long nb = (long)m->gn[0] * m->gn[1] * m->gn[2];
    m->gridOff = calloc((size_t)nb + 1, sizeof(long));
    long *bin = malloc(sizeof(long) * (size_t)m->nCells);
    for (long c = 0; c < m->nCells; c++) {
        int ix[3];
        for (int d = 0; d < 3; d++) { ix[d] = (int)((m->cCtr[3 * c + d] - m->lo[d]) / L[d] * m->gn[d]); if (ix[d] < 0) ix[d] = 0; if (ix[d] >= m->gn[d]) ix[d] = m->gn[d] - 1; }
        bin[c] = ((long)ix[2] * m->gn[1] + ix[1]) * m->gn[0] + ix[0];
        m->gridOff[bin[c] + 1]++;
    }
    for (long b = 0; b < nb; b++) m->gridOff[b + 1] += m->gridOff[b];
    m->grid = malloc(sizeof(long) * (size_t)(m->nCells > 0 ? m->nCells : 1));
    long *fill = calloc((size_t)nb, sizeof(long));
    for (long c = 0; c < m->nCells; c++) m->grid[m->gridOff[bin[c]] + fill[bin[c]]++] = c;
    free(fill); free(bin);
}

bool vz_mesh_load(VzMesh *m, const char *dir)
{
    memset(m, 0, sizeof *m);
    char p[PL_PATH];
    pl_join(p, sizeof p, dir, "points"); m->pt = fr_points(p, &m->nPoints);
    pl_join(p, sizeof p, dir, "faces"); bool fo = fr_faces(p, &m->fOff, &m->fIdx, &m->nFaces);
    long no = 0, nn = 0;
    pl_join(p, sizeof p, dir, "owner"); m->own = fr_labels(p, &no);
    pl_join(p, sizeof p, dir, "neighbour"); m->nei = fr_labels(p, &nn);
    pl_join(p, sizeof p, dir, "boundary"); m->nPatch = fr_boundary(p, m->patch, VZ_MAX_PATCHES);
    if (!m->pt || !fo || !m->own || no != m->nFaces || nn < 0 || nn > m->nFaces) {
        const char *what = !m->pt ? "points" : !fo ? "faces" : !m->own ? "owner" : no != m->nFaces ? "owner (its length)" : "neighbour";
        vz_mesh_free(m);                                     /* (it clears err) */
        snprintf(m->err, sizeof m->err, "Could not read the mesh in %s (%s)", dir, what);
        return false;
    }
    if (!m->nei && nn == 0) m->nei = malloc(sizeof(long));
    m->nInternal = nn;
    long mc = -1;
    for (long f = 0; f < m->nFaces; f++) { if (m->own[f] > mc) mc = m->own[f]; }
    for (long f = 0; f < m->nInternal; f++) if (m->nei[f] > mc) mc = m->nei[f];
    m->nCells = mc + 1;
    for (long f = 0; f < m->nFaces; f++) {
        if (m->own[f] < 0 || (f < m->nInternal && m->nei[f] < 0)) { vz_mesh_free(m); snprintf(m->err, sizeof m->err, "A face has no cell"); return false; }
        for (long k = m->fOff[f]; k < m->fOff[f + 1]; k++)
            if (m->fIdx[k] < 0 || m->fIdx[k] >= m->nPoints) { vz_mesh_free(m); snprintf(m->err, sizeof m->err, "A face uses a point that isn't there"); return false; }
    }
    for (int d = 0; d < 3; d++) { m->lo[d] = 1e300; m->hi[d] = -1e300; }
    for (long i = 0; i < m->nPoints; i++) for (int d = 0; d < 3; d++) { m->lo[d] = fmin(m->lo[d], m->pt[3 * i + d]); m->hi[d] = fmax(m->hi[d], m->pt[3 * i + d]); }
    face_geometry(m);
    cell_geometry(m);
    adjacency(m);
    double vol = 0; for (long c = 0; c < m->nCells; c++) vol += fabs(m->cVol[c]);
    m->h = m->nCells ? cbrt(vol / m->nCells) : 1;
    build_grid(m);
    m->ok = true;
    return true;
}

/* ================================================================ fields */
void vz_field_free(VzField *f) { free(f->c); free(f->b); free(f->p); memset(f, 0, sizeof *f); }

static bool real_patch(const char *type)
{
    return strcmp(type, "empty") && strcmp(type, "processor") && strcmp(type, "processorCyclic");
}

/* point values: each cell round the point (and, on the boundary, each boundary face) extrapolated to the point with its
   cell's gradient, value + grad . (x_point - x_sample), and averaged. Plain averaging (as ParaView's cell-to-point does)
   is off at a mesh's edges and corners even for a linear field (0.125 on a 0.1 m mesh of 2x + 3y - z); this is exact for
   linear fields and second-order otherwise */
double *vz_gradient(const VzMesh *m, const VzField *f, int k);
void vz_field_points(VzField *f, const VzMesh *m)
{
    int nc = f->nc;
    free(f->p);
    f->p = calloc((size_t)m->nPoints * (size_t)nc, sizeof(double));
    double *g[3] = { NULL, NULL, NULL };
    for (int j = 0; j < nc; j++) g[j] = vz_gradient(m, f, j);
    for (long p = 0; p < m->nPoints; p++) {
        long a = m->pcOff[p], b = m->pcOff[p + 1];
        const double *xp = &m->pt[3 * p];
        for (long k = a; k < b; k++) {
            long c = m->pcIdx[k];
            double r[3]; sub3(xp, &m->cCtr[3 * c], r);
            for (int j = 0; j < nc; j++) f->p[p * nc + j] += f->c[c * nc + j] + dot3(&g[j][3 * c], r);
        }
        if (b > a) for (int j = 0; j < nc; j++) f->p[p * nc + j] /= (double)(b - a);
    }
    double *sum = calloc((size_t)m->nPoints * (size_t)nc, sizeof(double));
    int *cnt = calloc((size_t)m->nPoints, sizeof(int));
    for (int q = 0; q < m->nPatch; q++) {
        if (!real_patch(m->patch[q].type)) continue;
        for (long f2 = m->patch[q].start; f2 < m->patch[q].start + m->patch[q].nFaces; f2++) {
            const double *v = &f->b[(f2 - m->nInternal) * nc];
            long c = m->own[f2];
            for (long k = m->fOff[f2]; k < m->fOff[f2 + 1]; k++) {
                long p = m->fIdx[k];
                double r[3]; sub3(&m->pt[3 * p], &m->fCtr[3 * f2], r);
                for (int j = 0; j < nc; j++) sum[p * nc + j] += v[j] + dot3(&g[j][3 * c], r);
                cnt[p]++;
            }
        }
    }
    for (long p = 0; p < m->nPoints; p++) if (cnt[p]) for (int j = 0; j < nc; j++) f->p[p * nc + j] = sum[p * nc + j] / cnt[p];
    free(sum); free(cnt);
    for (int j = 0; j < nc; j++) free(g[j]);
}

bool vz_field_load(VzField *f, const VzMesh *m, const char *casePath, const char *time, const char *name)
{
    memset(f, 0, sizeof *f);
    char rel[PL_PATH], p[PL_PATH]; snprintf(rel, sizeof rel, "%s/%s", time, name); pl_join(p, sizeof p, casePath, rel);
    f->nc = fr_vol_field(p, m->nCells, m->patch, m->nPatch, m->nInternal, m->nFaces, &f->c, &f->b, f->dims, sizeof f->dims);
    if (!f->nc) return false;
    pl_strlcpy(f->name, name, sizeof f->name);
    for (long i = m->nInternal; i < m->nFaces; i++) {        /* no value on the patch: its cell's */
        double *v = &f->b[(i - m->nInternal) * f->nc];
        if (isnan(v[0])) for (int j = 0; j < f->nc; j++) v[j] = f->c[m->own[i] * f->nc + j];
    }
    vz_field_points(f, m);
    return true;
}

/* ================================================================ decomposed cases */
int vz_mesh_procs(VzMesh *m, const char *casePath)
{
    for (int p = 0; p < m->nProc; p++) { free(m->proc[p].cell); free(m->proc[p].face); }
    free(m->proc); m->proc = NULL; m->nProc = 0;
    int n = 0;
    for (;; n++) { char d[64], p[PL_PATH]; snprintf(d, sizeof d, "processor%d/constant/polyMesh/cellProcAddressing", n); pl_join(p, sizeof p, casePath, d); if (!pl_exists(p)) break; }
    if (!n) return 0;
    struct VzProc *pr = calloc((size_t)n, sizeof *pr);
    long covered = 0;
    for (int k = 0; k < n; k++) {
        char d[96], p[PL_PATH];
        snprintf(d, sizeof d, "processor%d/constant/polyMesh/cellProcAddressing", k); pl_join(p, sizeof p, casePath, d);
        pr[k].cell = fr_labels(p, &pr[k].nCells);
        snprintf(d, sizeof d, "processor%d/constant/polyMesh/faceProcAddressing", k); pl_join(p, sizeof p, casePath, d);
        pr[k].face = fr_labels(p, &pr[k].nFaces);
        snprintf(d, sizeof d, "processor%d/constant/polyMesh/boundary", k); pl_join(p, sizeof p, casePath, d);
        pr[k].nPatch = fr_boundary(p, pr[k].patch, VZ_MAX_PATCHES);
        bool ok = pr[k].cell && pr[k].face && pr[k].nPatch > 0;
        pr[k].nInternal = ok ? pr[k].patch[0].start : 0;
        for (long i = 0; ok && i < pr[k].nCells; i++) ok = pr[k].cell[i] >= 0 && pr[k].cell[i] < m->nCells;
        /* faceProcAddressing: the whole mesh's face + 1, negative when the processor's face points the other way */
        for (long i = 0; ok && i < pr[k].nFaces; i++) { long g = labs(pr[k].face[i]) - 1; ok = g >= 0 && g < m->nFaces; }
        if (!ok) {
            snprintf(m->err, sizeof m->err, "processor%d's maps don't fit constant/polyMesh: reconstruct the case (reconstructParMesh, reconstructPar) to see it", k);
            for (int j = 0; j <= k; j++) { free(pr[j].cell); free(pr[j].face); }
            free(pr);
            return 0;
        }
        covered += pr[k].nCells;
    }
    if (covered != m->nCells) {
        snprintf(m->err, sizeof m->err, "The processors hold %ld cells, constant/polyMesh %ld: reconstruct the case (reconstructParMesh) to see it", covered, m->nCells);
        for (int j = 0; j < n; j++) { free(pr[j].cell); free(pr[j].face); }
        free(pr);
        return 0;
    }
    m->proc = pr; m->nProc = n;
    return n;
}

bool vz_field_load_procs(VzField *f, const VzMesh *m, const char *casePath, const char *time, const char *name)
{
    memset(f, 0, sizeof *f);
    if (!m->nProc) return false;
    long nb = m->nFaces - m->nInternal;
    for (int k = 0; k < m->nProc; k++) {
        const struct VzProc *pr = &m->proc[k];
        char rel[PL_PATH], p[PL_PATH]; snprintf(rel, sizeof rel, "processor%d/%s/%s", k, time, name); pl_join(p, sizeof p, casePath, rel);
        double *c = NULL, *b = NULL; char dims[40] = "";
        int nc = fr_vol_field(p, pr->nCells, pr->patch, pr->nPatch, pr->nInternal, pr->nFaces, &c, &b, dims, sizeof dims);
        if (!nc || (f->nc && nc != f->nc)) { free(c); free(b); vz_field_free(f); return false; }
        if (!f->nc) {
            f->nc = nc;
            pl_strlcpy(f->dims, dims, sizeof f->dims);
            f->c = calloc((size_t)m->nCells * nc, sizeof(double));
            f->b = malloc(sizeof(double) * (size_t)(nb > 0 ? nb : 1) * nc);
            for (long i = 0; i < nb * nc; i++) f->b[i] = NAN;
        }
        for (long i = 0; i < pr->nCells; i++) for (int j = 0; j < nc; j++) f->c[pr->cell[i] * nc + j] = c[i * nc + j];
        for (int q = 0; q < pr->nPatch; q++) {                   /* the real patches' faces (not the processor ones) */
            if (!strncmp(pr->patch[q].type, "processor", 9)) continue;
            for (long i = pr->patch[q].start; i < pr->patch[q].start + pr->patch[q].nFaces && i < pr->nFaces; i++) {
                long g = labs(pr->face[i]) - 1;
                if (g < m->nInternal) continue;
                for (int j = 0; j < nc; j++) f->b[(g - m->nInternal) * nc + j] = b[(i - pr->nInternal) * nc + j];
            }
        }
        free(c); free(b);
    }
    pl_strlcpy(f->name, name, sizeof f->name);
    for (long i = m->nInternal; i < m->nFaces; i++) {
        double *v = &f->b[(i - m->nInternal) * f->nc];
        if (isnan(v[0])) for (int j = 0; j < f->nc; j++) v[j] = f->c[m->own[i] * f->nc + j];
    }
    vz_field_points(f, m);
    return true;
}

static double qof(const double *v, int nc, int q)
{
    if (nc == 1) return v[0];
    switch (q) {
    case VQ_X: return v[0];
    case VQ_Y: return v[1];
    case VQ_Z: return v[2];
    default: return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }
}
double vz_cell(const VzField *f, int q, long i) { return qof(&f->c[i * f->nc], f->nc, q); }
double vz_bnd(const VzField *f, int q, long i) { return qof(&f->b[i * f->nc], f->nc, q); }
double vz_point(const VzField *f, int q, long i) { return qof(&f->p[i * f->nc], f->nc, q); }

/* ================================================================ gradients and derived fields */
double *vz_gradient(const VzMesh *m, const VzField *f, int k)
{
    double *g = calloc((size_t)m->nCells * 3, sizeof(double));
    int nc = f->nc;
    for (long fa = 0; fa < m->nFaces; fa++) {
        long o = m->own[fa];
        double phi;
        if (fa < m->nInternal) {
            long n = m->nei[fa];
            double dO[3], dN[3]; sub3(&m->fCtr[3 * fa], &m->cCtr[3 * o], dO); sub3(&m->fCtr[3 * fa], &m->cCtr[3 * n], dN);
            double lo = len3(dO), ln = len3(dN), w = lo + ln > 0 ? ln / (lo + ln) : 0.5;
            phi = w * f->c[o * nc + k] + (1 - w) * f->c[n * nc + k];
            for (int d = 0; d < 3; d++) { g[3 * o + d] += phi * m->fArea[3 * fa + d]; g[3 * n + d] -= phi * m->fArea[3 * fa + d]; }
        } else {
            phi = f->b[(fa - m->nInternal) * nc + k];
            int p = vz_patch_of_face(m, fa);
            if (p >= 0 && !real_patch(m->patch[p].type)) phi = f->c[o * nc + k];   /* an empty face: zero gradient across */
            for (int d = 0; d < 3; d++) g[3 * o + d] += phi * m->fArea[3 * fa + d];
        }
    }
    for (long c = 0; c < m->nCells; c++) { double v = m->cVol[c]; for (int d = 0; d < 3; d++) g[3 * c + d] = v > 0 ? g[3 * c + d] / v : 0; }
    return g;
}

const char *const VD_NAME[VD__N] = { "", "Pressure", "Total pressure", "Velocity gradient magnitude", "Vorticity magnitude",
                                     "Q-criterion", "Helicity", "Cell volume" };

static bool kinematic(const VzField *p) { return strstr(p->dims, "[0 2 -2") != NULL; }

bool vz_derived(VzField *out, const VzMesh *m, int kind, const VzField *p, const VzField *U, double rho)
{
    memset(out, 0, sizeof *out);
    long nc = m->nCells, nb = m->nFaces - m->nInternal;
    out->nc = 1;
    out->c = calloc((size_t)(nc > 0 ? nc : 1), sizeof(double));
    out->b = calloc((size_t)(nb > 0 ? nb : 1), sizeof(double));
    pl_strlcpy(out->name, VD_NAME[kind], sizeof out->name);
    switch (kind) {
    case VD_PPA:
    case VD_PTOTAL: {
        if (!p || p->nc != 1 || (kind == VD_PTOTAL && (!U || U->nc != 3))) { vz_field_free(out); return false; }
        bool kin = kinematic(p);
        double k = kin && rho > 0 ? rho : 1;           /* kinematic p (m2/s2) times rho is Pa */
        double kd = kin ? 1 : (rho > 0 ? rho : 1);     /* the dynamic part: |U|^2/2 kinematic, rho |U|^2/2 in Pa */
        for (long i = 0; i < nc; i++) {
            double v = p->c[i];
            if (kind == VD_PTOTAL) { const double *u = &U->c[3 * i]; v += 0.5 * kd * dot3(u, u); }
            out->c[i] = v * k;
        }
        for (long i = 0; i < nb; i++) {
            double v = p->b[i];
            if (kind == VD_PTOTAL) { const double *u = &U->b[3 * i]; v += 0.5 * kd * dot3(u, u); }
            out->b[i] = v * k;
        }
        snprintf(out->dims, sizeof out->dims, "%s", (kin && rho > 0) || !kin ? "[1 -1 -2 0 0 0 0]" : "[0 2 -2 0 0 0 0]");
        if (kind == VD_PPA && !kin) pl_strlcpy(out->name, "Pressure (Pa)", sizeof out->name);
        break;
    }
    case VD_CELLVOL:
        for (long i = 0; i < nc; i++) out->c[i] = m->cVol[i];
        for (long i = 0; i < nb; i++) out->b[i] = m->cVol[m->own[m->nInternal + i]];
        pl_strlcpy(out->dims, "[0 3 0 0 0 0 0]", sizeof out->dims);
        break;
    default: {
        if (!U || U->nc != 3) { vz_field_free(out); return false; }
        double *g[3] = { vz_gradient(m, U, 0), vz_gradient(m, U, 1), vz_gradient(m, U, 2) };   /* g[i][d] = d u_i / d x_d */
        for (long c = 0; c < nc; c++) {
            double G[3][3];
            for (int i = 0; i < 3; i++) for (int d = 0; d < 3; d++) G[i][d] = g[i][3 * c + d];
            double w[3] = { G[2][1] - G[1][2], G[0][2] - G[2][0], G[1][0] - G[0][1] };
            double v = 0;
            if (kind == VD_GRADMAG) { for (int i = 0; i < 3; i++) for (int d = 0; d < 3; d++) v += G[i][d] * G[i][d]; v = sqrt(v); }
            else if (kind == VD_VORTMAG) v = len3(w);
            else if (kind == VD_HELICITY) v = dot3(&U->c[3 * c], w);
            else {                                     /* Q = (|W|^2 - |S|^2) / 2 */
                double S2 = 0, W2 = 0;
                for (int i = 0; i < 3; i++) for (int d = 0; d < 3; d++) {
                    double s = 0.5 * (G[i][d] + G[d][i]), a = 0.5 * (G[i][d] - G[d][i]);
                    S2 += s * s; W2 += a * a;
                }
                v = 0.5 * (W2 - S2);
            }
            out->c[c] = v;
        }
        for (long i = 0; i < nb; i++) out->b[i] = out->c[m->own[m->nInternal + i]];
        free(g[0]); free(g[1]); free(g[2]);
        pl_strlcpy(out->dims, kind == VD_Q ? "[0 0 -2 0 0 0 0]" : kind == VD_HELICITY ? "[0 1 -2 0 0 0 0]" : "[0 0 -1 0 0 0 0]", sizeof out->dims);
    }
    }
    vz_field_points(out, m);
    return true;
}

/* ================================================================ statistics */
void vz_stats_cells(const VzMesh *m, const VzField *f, int q, VzStats *s)
{
    memset(s, 0, sizeof *s);
    s->min = 1e300; s->max = -1e300;
    double wv = 0;
    for (long c = 0; c < m->nCells; c++) {
        double v = vz_cell(f, q, c);
        if (isnan(v)) continue;
        s->min = fmin(s->min, v); s->max = fmax(s->max, v);
        s->sum += v * m->cVol[c]; wv += m->cVol[c]; s->n++;
    }
    s->mean = wv > 0 ? s->sum / wv : 0;
}

void vz_stats_patch(const VzMesh *m, const VzField *f, int q, int patch, VzStats *s)
{
    memset(s, 0, sizeof *s);
    s->min = 1e300; s->max = -1e300;
    if (patch < 0 || patch >= m->nPatch) return;
    double wa = 0;
    for (long fa = m->patch[patch].start; fa < m->patch[patch].start + m->patch[patch].nFaces; fa++) {
        double v = vz_bnd(f, q, fa - m->nInternal), a = len3(&m->fArea[3 * fa]);
        s->min = fmin(s->min, v); s->max = fmax(s->max, v);
        s->sum += v * a; wa += a; s->n++;
    }
    s->mean = wa > 0 ? s->sum / wa : 0;
}

void vz_histogram(const VzMesh *m, const VzField *f, int q, double lo, double hi, int nb, double *bins)
{
    for (int i = 0; i < nb; i++) bins[i] = 0;
    if (!(hi > lo) || nb < 1) return;
    double tot = 0;
    for (long c = 0; c < m->nCells; c++) {
        double v = vz_cell(f, q, c);
        if (isnan(v)) continue;
        int k = (int)((v - lo) / (hi - lo) * nb);
        if (k < 0) k = 0;
        if (k >= nb) k = nb - 1;
        bins[k] += m->cVol[c]; tot += m->cVol[c];
    }
    if (tot > 0) for (int i = 0; i < nb; i++) bins[i] /= tot;
}

/* ================================================================ location */
static long nearest_centre(const VzMesh *m, const double x[3])
{
    double L[3]; int ix[3];
    for (int d = 0; d < 3; d++) {
        L[d] = fmax(m->hi[d] - m->lo[d], 1e-12);
        ix[d] = (int)((x[d] - m->lo[d]) / L[d] * m->gn[d]);
        if (ix[d] < 0) ix[d] = 0;
        if (ix[d] >= m->gn[d]) ix[d] = m->gn[d] - 1;
    }
    long best = -1; double bd = 1e300;
    for (int r = 0; r < 64 && (best < 0 || r < 2); r++) {     /* rings of bins until one holds a centre, then one more */
        for (int k = ix[2] - r; k <= ix[2] + r; k++) for (int j = ix[1] - r; j <= ix[1] + r; j++) for (int i = ix[0] - r; i <= ix[0] + r; i++) {
            if (i < 0 || j < 0 || k < 0 || i >= m->gn[0] || j >= m->gn[1] || k >= m->gn[2]) continue;
            if (abs(i - ix[0]) != r && abs(j - ix[1]) != r && abs(k - ix[2]) != r) continue;
            long b = ((long)k * m->gn[1] + j) * m->gn[0] + i;
            for (long q = m->gridOff[b]; q < m->gridOff[b + 1]; q++) {
                long c = m->grid[q];
                double d[3]; sub3(x, &m->cCtr[3 * c], d);
                double dd = dot3(d, d);
                if (dd < bd) { bd = dd; best = c; }
            }
        }
    }
    return best;
}

/* how far outside cell c's face f the point is (positive: outside), in metres */
static double outside(const VzMesh *m, long c, long f, const double x[3])
{
    double r[3]; sub3(x, &m->fCtr[3 * f], r);
    const double *a = &m->fArea[3 * f];
    double s = dot3(a, r) / (len3(a) + 1e-300);
    return m->own[f] == c ? s : -s;
}

/* walk from cell c toward x, across the face x is furthest outside of: the cell holding x, -1 out through the boundary,
   -2 no answer (cycling) */
static long walk(const VzMesh *m, const double x[3], long c, double tol)
{
    long prev = -1;
    for (int step = 0; step < 4000; step++) {
        long worstF = -1; double worst = tol;
        for (long k = m->cfOff[c]; k < m->cfOff[c + 1]; k++) {
            long f = m->cfIdx[k];
            double s = outside(m, c, f, x);
            if (s > worst) { worst = s; worstF = f; }
        }
        if (worstF < 0) return c;                                      /* inside every face */
        if (worstF >= m->nInternal) return -1;                         /* out through the boundary */
        long nxt = m->own[worstF] == c ? m->nei[worstF] : m->own[worstF];
        if (nxt == prev) return c;                                     /* bouncing across one face: x is on it */
        prev = c; c = nxt;
    }
    return -2;
}

static bool inside_cell(const VzMesh *m, long c, const double x[3], double tol)
{
    for (long k = m->cfOff[c]; k < m->cfOff[c + 1]; k++) if (outside(m, c, m->cfIdx[k], x) > tol) return false;
    return true;
}

long vz_find_cell(const VzMesh *m, const double x[3], long hint)
{
    if (!m->ok || !m->nCells) return -1;
    double tol = 1e-9 * m->h;
    for (int d = 0; d < 3; d++) if (x[d] < m->lo[d] - tol || x[d] > m->hi[d] + tol) return -1;
    bool hinted = hint >= 0 && hint < m->nCells;
    long r = walk(m, x, hinted ? hint : nearest_centre(m, x), tol);
    if (r >= 0) return r;
    if (hinted) { r = walk(m, x, nearest_centre(m, x), tol); if (r >= 0) return r; }
    /* a concave boundary between the start and x: the cells whose centres are in x's bin and those round it */
    int ix[3];
    for (int d = 0; d < 3; d++) {
        ix[d] = (int)((x[d] - m->lo[d]) / fmax(m->hi[d] - m->lo[d], 1e-12) * m->gn[d]);
        if (ix[d] < 0) ix[d] = 0;
        if (ix[d] >= m->gn[d]) ix[d] = m->gn[d] - 1;
    }
    for (int k = ix[2] - 1; k <= ix[2] + 1; k++) for (int j = ix[1] - 1; j <= ix[1] + 1; j++) for (int i = ix[0] - 1; i <= ix[0] + 1; i++) {
        if (i < 0 || j < 0 || k < 0 || i >= m->gn[0] || j >= m->gn[1] || k >= m->gn[2]) continue;
        long b = ((long)k * m->gn[1] + j) * m->gn[0] + i;
        for (long q = m->gridOff[b]; q < m->gridOff[b + 1]; q++) if (inside_cell(m, m->grid[q], x, tol)) return m->grid[q];
    }
    return -1;
}

bool vz_sample(const VzMesh *m, const VzField *f, int q, const double x[3], long *cell, double *out)
{
    long c = vz_find_cell(m, x, cell ? *cell : -1);
    if (cell) *cell = c;
    if (c < 0) return false;
    int nc = f->nc;
    double acc[3] = { 0, 0, 0 }, ws = 0;
    /* the cell's points, weighted by inverse squared distance, with the cell's own value at its centre */
    {
        double d[3]; sub3(x, &m->cCtr[3 * c], d);
        double w = 1 / (dot3(d, d) + 1e-24 * m->h * m->h);
        for (int j = 0; j < nc; j++) acc[j] += w * f->c[c * nc + j];
        ws += w;
    }
    for (long k = m->cfOff[c]; k < m->cfOff[c + 1]; k++) {
        long fa = m->cfIdx[k];
        for (long j2 = m->fOff[fa]; j2 < m->fOff[fa + 1]; j2++) {
            long p = m->fIdx[j2];
            double d[3]; sub3(x, &m->pt[3 * p], d);
            double w = 1 / (dot3(d, d) + 1e-24 * m->h * m->h);
            for (int j = 0; j < nc; j++) acc[j] += w * f->p[p * nc + j];
            ws += w;
        }
    }
    for (int j = 0; j < nc; j++) acc[j] /= ws;
    if (q < 0) { for (int j = 0; j < nc; j++) out[j] = acc[j]; }
    else *out = qof(acc, nc, q);
    return true;
}

/* ================================================================ drawn geometry */
void vz_tris_free(VzTris *t) { free(t->xyz); free(t->val); free(t->nrm); free(t->cell); free(t->grp); memset(t, 0, sizeof *t); }
void vz_lines_free(VzLines *l) { free(l->xyz); free(l->val); free(l->off); memset(l, 0, sizeof *l); }

typedef struct { VzTris *t; long cap; } TB;
static void tb_add(TB *b, const double *p0, const double *p1, const double *p2, double v0, double v1, double v2, int cell, int grp)
{
    VzTris *t = b->t;
    if (t->n >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 1024;
        t->xyz = realloc(t->xyz, sizeof(float) * 9 * (size_t)b->cap);
        t->val = realloc(t->val, sizeof(float) * 3 * (size_t)b->cap);
        t->nrm = realloc(t->nrm, sizeof(float) * 3 * (size_t)b->cap);
        t->cell = realloc(t->cell, sizeof(int) * (size_t)b->cap);
        t->grp = realloc(t->grp, sizeof(int) * (size_t)b->cap);
    }
    long i = t->n++;
    const double *P[3] = { p0, p1, p2 };
    for (int k = 0; k < 3; k++) for (int d = 0; d < 3; d++) t->xyz[9 * i + 3 * k + d] = (float)P[k][d];
    t->val[3 * i] = (float)v0; t->val[3 * i + 1] = (float)v1; t->val[3 * i + 2] = (float)v2;
    double e0[3], e1[3], n[3]; sub3(p1, p0, e0); sub3(p2, p0, e1); cross3(e0, e1, n);
    double l = len3(n);
    for (int d = 0; d < 3; d++) t->nrm[3 * i + d] = l > 0 ? (float)(n[d] / l) : 0;
    t->cell[i] = cell; t->grp[i] = grp;
}

void vz_boundary(const VzMesh *m, const VzField *f, int q, const bool *show, bool smooth, VzTris *out)
{
    memset(out, 0, sizeof *out);
    TB b = { out, 0 };
    for (int p = 0; p < m->nPatch; p++) {
        if (show ? !show[p] : !real_patch(m->patch[p].type)) continue;
        for (long fa = m->patch[p].start; fa < m->patch[p].start + m->patch[p].nFaces && fa < m->nFaces; fa++) {
            long a = m->fOff[fa], n = m->fOff[fa + 1] - a;
            double fv = f ? vz_bnd(f, q, fa - m->nInternal) : NAN;
            const double *c = &m->fCtr[3 * fa];
            for (long k = 0; k < n; k++) {
                long p0 = m->fIdx[a + k], p1 = m->fIdx[a + (k + 1) % n];
                double v0 = fv, v1 = fv;
                if (f && smooth) { v0 = vz_point(f, q, p0); v1 = vz_point(f, q, p1); }
                tb_add(&b, c, &m->pt[3 * p0], &m->pt[3 * p1], fv, v0, v1, (int)m->own[fa], p);
            }
        }
    }
}

/* ---- the cell cutter */
typedef struct { long a, b; double x[3]; double v; } Cross;   /* where the zero set crosses edge a-b */

void vz_cut(const VzMesh *m, const double *fn, const VzField *f, int q, bool smooth, VzTris *out)
{
    memset(out, 0, sizeof *out);
    TB tb = { out, 0 };
    Cross *cr = malloc(sizeof(Cross) * 512);
    int (*seg)[2] = malloc(sizeof(int) * 2 * 512);
    for (long c = 0; c < m->nCells; c++) {
        /* a quick look: does the function change sign over the cell's points? */
        bool pos = false, neg = false;
        for (long k = m->cfOff[c]; k < m->cfOff[c + 1] && !(pos && neg); k++) {
            long fa = m->cfIdx[k];
            for (long j = m->fOff[fa]; j < m->fOff[fa + 1]; j++) { double s = fn[m->fIdx[j]]; if (s >= 0) pos = true; else neg = true; }
        }
        if (!(pos && neg)) continue;
        int nCr = 0, nSeg = 0;
        double cv = f ? vz_cell(f, q, c) : NAN;
        for (long k = m->cfOff[c]; k < m->cfOff[c + 1]; k++) {
            long fa = m->cfIdx[k], a = m->fOff[fa], n = m->fOff[fa + 1] - a;
            int here[16], nh = 0;
            for (long e = 0; e < n && nh < 16; e++) {
                long pa = m->fIdx[a + e], pb = m->fIdx[a + (e + 1) % n];
                double sa = fn[pa], sb = fn[pb];
                if ((sa >= 0) == (sb >= 0)) continue;
                long lo = pa < pb ? pa : pb, hi = pa < pb ? pb : pa;
                int found = -1;
                for (int i = 0; i < nCr; i++) if (cr[i].a == lo && cr[i].b == hi) { found = i; break; }
                if (found < 0 && nCr < 512) {
                    double t = sa / (sa - sb);
                    Cross *x = &cr[nCr];
                    x->a = lo; x->b = hi;
                    for (int d = 0; d < 3; d++) x->x[d] = m->pt[3 * pa + d] + t * (m->pt[3 * pb + d] - m->pt[3 * pa + d]);
                    x->v = f && smooth ? vz_point(f, q, pa) + t * (vz_point(f, q, pb) - vz_point(f, q, pa)) : cv;
                    found = nCr++;
                }
                if (found >= 0) here[nh++] = found;
            }
            for (int i = 0; i + 1 < nh && nSeg < 512; i += 2) { seg[nSeg][0] = here[i]; seg[nSeg][1] = here[i + 1]; nSeg++; }
        }
        /* the segments into loops, each fanned from its centre */
        bool used[512] = { false };
        for (int s0 = 0; s0 < nSeg; s0++) {
            if (used[s0]) continue;
            int loop[512], nl = 0;
            used[s0] = true;
            loop[nl++] = seg[s0][0];
            int cur = seg[s0][1];
            for (int guard = 0; guard < nSeg && cur != loop[0] && nl < 511; guard++) {
                loop[nl++] = cur;
                int nx = -1;
                for (int s = 0; s < nSeg; s++) {
                    if (used[s]) continue;
                    if (seg[s][0] == cur) { nx = seg[s][1]; used[s] = true; break; }
                    if (seg[s][1] == cur) { nx = seg[s][0]; used[s] = true; break; }
                }
                if (nx < 0) break;
                cur = nx;
            }
            if (nl < 3) continue;
            double ctr[3] = { 0, 0, 0 }, cvv = 0;
            for (int i = 0; i < nl; i++) { for (int d = 0; d < 3; d++) ctr[d] += cr[loop[i]].x[d] / nl; cvv += cr[loop[i]].v / nl; }
            if (!smooth) cvv = cv;
            for (int i = 0; i < nl; i++) {
                const Cross *A = &cr[loop[i]], *B = &cr[loop[(i + 1) % nl]];
                tb_add(&tb, ctr, A->x, B->x, cvv, A->v, B->v, (int)c, -1);
            }
        }
    }
    free(cr); free(seg);
}

void vz_section(const VzMesh *m, const double origin[3], const double normal[3], const VzField *f, int q, bool smooth, VzTris *out)
{
    double n[3] = { normal[0], normal[1], normal[2] }, l = len3(n);
    if (l <= 0) { memset(out, 0, sizeof *out); return; }
    for (int d = 0; d < 3; d++) n[d] /= l;
    double *fn = malloc(sizeof(double) * (size_t)m->nPoints);
    /* a point exactly on the plane counts as just above it, so a plane through a layer of points cuts cleanly */
    for (long p = 0; p < m->nPoints; p++) { double r[3]; sub3(&m->pt[3 * p], origin, r); fn[p] = dot3(r, n); if (fn[p] == 0) fn[p] = 1e-12 * m->h; }
    vz_cut(m, fn, f, q, smooth, out);
    free(fn);
}

void vz_isosurface(const VzMesh *m, const VzField *iso, int isoQ, double value, const VzField *f, int q, VzTris *out)
{
    double *fn = malloc(sizeof(double) * (size_t)m->nPoints);
    for (long p = 0; p < m->nPoints; p++) { fn[p] = vz_point(iso, isoQ, p) - value; if (fn[p] == 0) fn[p] = 1e-300; }
    vz_cut(m, fn, f ? f : iso, f ? q : isoQ, true, out);
    free(fn);
}

void vz_threshold(const VzMesh *m, const VzField *f, int q, double lo, double hi, const VzField *col, int colQ, VzTris *out)
{
    memset(out, 0, sizeof *out);
    TB tb = { out, 0 };
    unsigned char *sel = calloc((size_t)m->nCells, 1);
    for (long c = 0; c < m->nCells; c++) { double v = vz_cell(f, q, c); sel[c] = v >= lo && v <= hi; }
    if (!col) { col = f; colQ = q; }
    for (long fa = 0; fa < m->nFaces; fa++) {
        long o = m->own[fa], n = fa < m->nInternal ? m->nei[fa] : -1;
        bool so = sel[o], sn = n >= 0 && sel[n];
        if (so == sn) continue;
        if (n < 0) {                                         /* a boundary face of a selected cell; empty faces only in 3-D */
            int p = vz_patch_of_face(m, fa);
            if (p >= 0 && !strcmp(m->patch[p].type, "processor")) continue;
        }
        long c = so ? o : n;
        long a = m->fOff[fa], k = m->fOff[fa + 1] - a;
        double cv = vz_cell(col, colQ, c);
        for (long e = 0; e < k; e++) {
            long p0 = m->fIdx[a + e], p1 = m->fIdx[a + (e + 1) % k];
            if (so) tb_add(&tb, &m->fCtr[3 * fa], &m->pt[3 * p0], &m->pt[3 * p1], cv, vz_point(col, colQ, p0), vz_point(col, colQ, p1), (int)c, -1);
            else tb_add(&tb, &m->fCtr[3 * fa], &m->pt[3 * p1], &m->pt[3 * p0], cv, vz_point(col, colQ, p1), vz_point(col, colQ, p0), (int)c, -1);
        }
    }
    free(sel);
}

void vz_clip(VzTris *t, const double origin[3], const double normal[3])
{
    long w = 0;
    for (long i = 0; i < t->n; i++) {
        bool keep = false;
        for (int k = 0; k < 3 && !keep; k++) {
            double r[3] = { t->xyz[9 * i + 3 * k] - origin[0], t->xyz[9 * i + 3 * k + 1] - origin[1], t->xyz[9 * i + 3 * k + 2] - origin[2] };
            keep = dot3(r, normal) <= 0;
        }
        if (!keep) continue;
        if (w != i) {
            memcpy(&t->xyz[9 * w], &t->xyz[9 * i], sizeof(float) * 9);
            memcpy(&t->val[3 * w], &t->val[3 * i], sizeof(float) * 3);
            memcpy(&t->nrm[3 * w], &t->nrm[3 * i], sizeof(float) * 3);
            t->cell[w] = t->cell[i]; t->grp[w] = t->grp[i];
        }
        w++;
    }
    t->n = w;
}

typedef struct { VzLines *l; long capV, capL; } LB;
static void lb_begin(LB *b)
{
    VzLines *l = b->l;
    if (l->nl + 2 > b->capL) { b->capL = b->capL ? b->capL * 2 : 256; l->off = realloc(l->off, sizeof(long) * (size_t)(b->capL + 1)); }
    if (l->nl == 0) l->off[0] = l->nv;
}
static void lb_vert(LB *b, const double *x, double v)
{
    VzLines *l = b->l;
    if (l->nv >= b->capV) { b->capV = b->capV ? b->capV * 2 : 1024; l->xyz = realloc(l->xyz, sizeof(float) * 3 * (size_t)b->capV); l->val = realloc(l->val, sizeof(float) * (size_t)b->capV); }
    for (int d = 0; d < 3; d++) l->xyz[3 * l->nv + d] = (float)x[d];
    l->val[l->nv++] = (float)v;
}
static void lb_end(LB *b) { VzLines *l = b->l; l->off[++l->nl] = l->nv; }

void vz_isolines(const VzTris *t, double lo, double hi, int n, bool logScale, VzLines *out)
{
    memset(out, 0, sizeof *out);
    LB b = { out, 0, 0 };
    if (n < 1 || !(hi > lo)) return;
    for (int k = 0; k < n; k++) {
        double f = (k + 1.0) / (n + 1.0);
        double lev = logScale && lo > 0 ? lo * pow(hi / lo, f) : lo + f * (hi - lo);
        for (long i = 0; i < t->n; i++) {
            double pts[3][3];
            int np = 0;
            for (int e = 0; e < 3 && np < 3; e++) {
                int a = e, c = (e + 1) % 3;
                double va = t->val[3 * i + a], vc = t->val[3 * i + c];
                if (isnan(va) || isnan(vc) || (va >= lev) == (vc >= lev)) continue;
                double s = (lev - va) / (vc - va);
                for (int d = 0; d < 3; d++) pts[np][d] = t->xyz[9 * i + 3 * a + d] + s * (t->xyz[9 * i + 3 * c + d] - t->xyz[9 * i + 3 * a + d]);
                np++;
            }
            if (np == 2) { lb_begin(&b); lb_vert(&b, pts[0], lev); lb_vert(&b, pts[1], lev); lb_end(&b); }
        }
    }
}

/* ================================================================ vectors */
long vz_glyph_points(const VzMesh *m, const VzField *U, const VzTris *t, double spacing, float **pos, float **vec)
{
    *pos = *vec = NULL;
    if (!U || U->nc != 3 || t->n == 0 || !(spacing > 0)) return 0;
    /* one glyph per occupied box of side spacing: a hash of the box index */
    long cap = 1024, n = 0, hcap = 1;
    while (hcap < t->n * 2) hcap <<= 1;
    int64_t *keys = malloc(sizeof(int64_t) * (size_t)hcap);
    for (long i = 0; i < hcap; i++) keys[i] = INT64_MIN;
    float *P = malloc(sizeof(float) * 3 * (size_t)cap), *V = malloc(sizeof(float) * 3 * (size_t)cap);
    for (long i = 0; i < t->n; i++) {
        double c[3];
        for (int d = 0; d < 3; d++) c[d] = (t->xyz[9 * i + d] + t->xyz[9 * i + 3 + d] + t->xyz[9 * i + 6 + d]) / 3;
        int64_t ix = (int64_t)floor(c[0] / spacing), iy = (int64_t)floor(c[1] / spacing), iz = (int64_t)floor(c[2] / spacing);
        int64_t key = (ix * 73856093) ^ (iy * 19349663) ^ (iz * 83492791);
        uint64_t h = (uint64_t)key * 0x9E3779B97F4A7C15ULL;
        long s = (long)(h & (uint64_t)(hcap - 1));
        bool seen = false;
        while (keys[s] != INT64_MIN) { if (keys[s] == key) { seen = true; break; } s = (s + 1) & (hcap - 1); }
        if (seen) continue;
        keys[s] = key;
        double u[3];
        long cell = t->cell ? t->cell[i] : -1;
        if (!vz_sample(m, U, -1, c, &cell, u)) {
            if (t->cell && t->cell[i] >= 0) for (int d = 0; d < 3; d++) u[d] = U->c[3 * t->cell[i] + d];
            else continue;
        }
        if (n >= cap) { cap *= 2; P = realloc(P, sizeof(float) * 3 * (size_t)cap); V = realloc(V, sizeof(float) * 3 * (size_t)cap); }
        for (int d = 0; d < 3; d++) { P[3 * n + d] = (float)c[d]; V[3 * n + d] = (float)u[d]; }
        n++;
    }
    free(keys);
    *pos = P; *vec = V;
    return n;
}

static void plane_axes(const double n[3], double eu[3], double ev[3])
{
    double a[3] = { 1, 0, 0 };
    if (fabs(n[0]) > 0.9) { a[0] = 0; a[1] = 1; }
    cross3(n, a, ev);                               /* ev = n x a; eu = ev x n */
    double l = len3(ev); for (int d = 0; d < 3; d++) ev[d] /= l;
    cross3(ev, n, eu);
    l = len3(eu); for (int d = 0; d < 3; d++) eu[d] /= l;
}

long vz_seed_points(const VzMesh *m, const VzSeeds *s, double **pts)
{
    int n = s->n < 1 ? 1 : s->n;
    long cap = s->kind == VS_PLANE ? (long)n * n : n;
    double *P = malloc(sizeof(double) * 3 * (size_t)(cap > 0 ? cap : 1));
    long k = 0;
    if (s->kind == VS_POINT) {
        double R = s->b[0];
        for (int i = 0; i < n; i++) {                  /* a Fibonacci spiral through the ball: even, repeatable */
            double t = (i + 0.5) / n, z = 1 - 2 * t, r = sqrt(fmax(0, 1 - z * z)), ph = i * 2.399963229728653;
            double rad = R * cbrt((i + 0.5) / n);
            P[3 * k] = s->a[0] + rad * r * cos(ph); P[3 * k + 1] = s->a[1] + rad * r * sin(ph); P[3 * k + 2] = s->a[2] + rad * z; k++;
        }
    } else if (s->kind == VS_LINE) {
        for (int i = 0; i < n; i++) {
            double t = n == 1 ? 0.5 : (double)i / (n - 1);
            for (int d = 0; d < 3; d++) P[3 * k + d] = s->a[d] + t * (s->b[d] - s->a[d]);
            k++;
        }
    } else if (s->kind == VS_PLANE) {
        double nn[3] = { s->b[0], s->b[1], s->b[2] }, l = len3(nn), eu[3], ev[3];
        if (l <= 0) { nn[0] = 1; l = 1; }
        for (int d = 0; d < 3; d++) nn[d] /= l;
        plane_axes(nn, eu, ev);
        for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) {
            double u = ((i + 0.5) / n - 0.5) * s->size, v = ((j + 0.5) / n - 0.5) * s->size;
            for (int d = 0; d < 3; d++) P[3 * k + d] = s->a[d] + u * eu[d] + v * ev[d];
            k++;
        }
    } else if (s->patch >= 0 && s->patch < m->nPatch) {
        long nf = m->patch[s->patch].nFaces, st = m->patch[s->patch].start;
        long take = nf < n ? nf : n;
        for (long i = 0; i < take; i++) {
            long fa = st + (long)((double)i * nf / take);
            long c = m->own[fa];
            for (int d = 0; d < 3; d++) P[3 * k + d] = m->fCtr[3 * fa + d] + 0.02 * (m->cCtr[3 * c + d] - m->fCtr[3 * fa + d]);   /* just inside */
            k++;
        }
    }
    *pts = P;
    return k;
}

/* one direction from x0: the points into buf (3 each, x0 first), how many */
static int trace(const VzMesh *m, const VzField *U, const double x0[3], double sign, int maxSteps, double maxLen, double *buf)
{
    double x[3] = { x0[0], x0[1], x0[2] };
    long c = vz_find_cell(m, x, -1);
    if (c < 0) return 0;
    memcpy(buf, x, sizeof x);
    int n = 1;
    double len = 0;
    for (int s = 0; s < maxSteps; s++) {
        double hc = cbrt(fabs(m->cVol[c]) > 0 ? fabs(m->cVol[c]) : m->h * m->h * m->h);
        double k[4][3], xt[3], u[3];
        if (!vz_sample(m, U, -1, x, &c, u)) break;
        double sp = len3(u);
        if (!(sp > 1e-12)) break;
        double dt = 0.2 * hc / sp * sign;
        for (int d = 0; d < 3; d++) k[0][d] = u[d];
        for (int st = 1; st < 4; st++) {
            double f = st == 3 ? 1 : 0.5;
            for (int d = 0; d < 3; d++) xt[d] = x[d] + f * dt * k[st - 1][d];
            long cc = c;
            if (!vz_sample(m, U, -1, xt, &cc, k[st])) { s = maxSteps; break; }
        }
        if (s >= maxSteps) break;
        double xn[3];
        for (int d = 0; d < 3; d++) xn[d] = x[d] + dt / 6 * (k[0][d] + 2 * k[1][d] + 2 * k[2][d] + k[3][d]);
        long cn = vz_find_cell(m, xn, c);
        if (cn < 0) break;                                   /* out of the mesh */
        double dl[3]; sub3(xn, x, dl); len += len3(dl);
        memcpy(x, xn, sizeof x);
        c = cn;
        memcpy(&buf[3 * n], x, sizeof x); n++;
        if (len > maxLen) break;
    }
    return n;
}

void vz_streamlines(const VzMesh *m, const VzField *U, const VzSeeds *s, const VzField *col, int colQ, VzLines *out)
{
    memset(out, 0, sizeof *out);
    if (!U || U->nc != 3) return;
    LB b = { out, 0, 0 };
    double *seeds; long ns = vz_seed_points(m, s, &seeds);
    double diag[3]; sub3(m->hi, m->lo, diag);
    double maxLen = s->maxLen > 0 ? s->maxLen : 10 * len3(diag);
    int maxSteps = s->maxSteps > 0 ? s->maxSteps : 4000;
    double *fw = malloc(sizeof(double) * 3 * (size_t)(maxSteps + 2)), *bw = malloc(sizeof(double) * 3 * (size_t)(maxSteps + 2));
    for (long i = 0; i < ns; i++) {
        int nf = s->dir != 1 ? trace(m, U, &seeds[3 * i], 1, maxSteps, maxLen, fw) : 0;
        int nb = s->dir != 0 ? trace(m, U, &seeds[3 * i], -1, maxSteps, maxLen, bw) : 0;
        if (nf + nb < 2) continue;
        lb_begin(&b);
        long hint = -1;
        for (int k = nb - 1; k >= 0; k--) {               /* backward, reversed, then forward (the seed once) */
            double v; if (!vz_sample(m, col ? col : U, col ? colQ : VQ_MAG, &bw[3 * k], &hint, &v)) v = NAN;
            lb_vert(&b, &bw[3 * k], v);
        }
        for (int k = nb > 0 ? 1 : 0; k < nf; k++) {
            double v; if (!vz_sample(m, col ? col : U, col ? colQ : VQ_MAG, &fw[3 * k], &hint, &v)) v = NAN;
            lb_vert(&b, &fw[3 * k], v);
        }
        lb_end(&b);
    }
    free(fw); free(bw); free(seeds);
}

/* ================================================================ images, volumes, probes */
static float noise_at(int i, int j)
{
    uint32_t h = (uint32_t)i * 374761393u + (uint32_t)j * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

void vz_lic(const VzMesh *m, const VzField *U, const VzField *col, int colQ, const double origin[3], const double normal[3],
            double size, int w, int h, int len, float *lum, float *val, double eu[3], double ev[3])
{
    double n[3] = { normal[0], normal[1], normal[2] }, l = len3(n);
    if (l <= 0) { n[0] = 0; n[1] = 0; n[2] = 1; l = 1; }
    for (int d = 0; d < 3; d++) n[d] /= l;
    plane_axes(n, eu, ev);
    float *vx = malloc(sizeof(float) * (size_t)w * h), *vy = malloc(sizeof(float) * (size_t)w * h);
    long hint = -1;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        double x[3], u[3];
        double a = ((i + 0.5) / w - 0.5) * size, b = ((j + 0.5) / h - 0.5) * size;
        for (int d = 0; d < 3; d++) x[d] = origin[d] + a * eu[d] + b * ev[d];
        long k = (long)j * w + i;
        if (U && U->nc == 3 && vz_sample(m, U, -1, x, &hint, u)) {
            vx[k] = (float)dot3(u, eu); vy[k] = (float)dot3(u, ev);
            double cv;
            long h2 = hint;
            val[k] = col && vz_sample(m, col, colQ, x, &h2, &cv) ? (float)cv : (float)sqrt(dot3(u, u));
        } else { vx[k] = vy[k] = NAN; val[k] = NAN; }
    }
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        long k = (long)j * w + i;
        if (isnan(vx[k])) { lum[k] = NAN; continue; }
        double sum = noise_at(i, j); int cnt = 1;
        for (int dir = -1; dir <= 1; dir += 2) {
            double x = i + 0.5, y = j + 0.5;
            for (int s = 0; s < len; s++) {
                int ii = (int)x, jj = (int)y;
                if (ii < 0 || jj < 0 || ii >= w || jj >= h) break;
                long kk = (long)jj * w + ii;
                float a = vx[kk], b = vy[kk];
                if (isnan(a)) break;
                double mag = sqrt(a * a + b * b);
                if (mag < 1e-30) break;
                x += dir * a / mag; y += dir * b / mag;
                ii = (int)x; jj = (int)y;
                if (ii < 0 || jj < 0 || ii >= w || jj >= h || isnan(vx[(long)jj * w + ii])) break;
                sum += noise_at(ii, jj); cnt++;
            }
        }
        lum[k] = (float)(sum / cnt);
    }
    free(vx); free(vy);
}

float *vz_resample(const VzMesh *m, const VzField *f, int q, const int n[3])
{
    long tot = (long)n[0] * n[1] * n[2];
    float *g = malloc(sizeof(float) * (size_t)(tot > 0 ? tot : 1));
    long hint = -1;
    for (int k = 0; k < n[2]; k++) for (int j = 0; j < n[1]; j++) for (int i = 0; i < n[0]; i++) {
        double x[3] = { m->lo[0] + (i + 0.5) / n[0] * (m->hi[0] - m->lo[0]), m->lo[1] + (j + 0.5) / n[1] * (m->hi[1] - m->lo[1]),
                        m->lo[2] + (k + 0.5) / n[2] * (m->hi[2] - m->lo[2]) };
        double v;
        g[((long)k * n[1] + j) * n[0] + i] = vz_sample(m, f, q, x, &hint, &v) ? (float)v : NAN;
    }
    return g;
}

int vz_line_probe(const VzMesh *m, const VzField *f, int q, const double a[3], const double b[3], int n, double *pos, double *val)
{
    int in = 0; long hint = -1;
    for (int i = 0; i < n; i++) {
        double t = n == 1 ? 0 : (double)i / (n - 1), x[3];
        for (int d = 0; d < 3; d++) x[d] = a[d] + t * (b[d] - a[d]);
        memcpy(&pos[3 * i], x, sizeof x);
        if (vz_sample(m, f, q, x, &hint, &val[i])) in++; else val[i] = NAN;
    }
    return in;
}

long vz_pick(const VzTris *t, const double o[3], const double d[3], double *dist)
{
    long best = -1; double bt = 1e300;
    for (long i = 0; i < t->n; i++) {
        double v0[3], v1[3], v2[3];
        for (int k = 0; k < 3; k++) { v0[k] = t->xyz[9 * i + k]; v1[k] = t->xyz[9 * i + 3 + k]; v2[k] = t->xyz[9 * i + 6 + k]; }
        double e1[3], e2[3], p[3], q[3], s[3]; sub3(v1, v0, e1); sub3(v2, v0, e2);
        cross3(d, e2, p);
        double det = dot3(e1, p);
        if (fabs(det) < 1e-300) continue;
        double inv = 1 / det;
        sub3(o, v0, s);
        double u = dot3(s, p) * inv;
        if (u < 0 || u > 1) continue;
        cross3(s, e1, q);
        double v = dot3(d, q) * inv;
        if (v < 0 || u + v > 1) continue;
        double tt = dot3(e2, q) * inv;
        if (tt > 0 && tt < bt) { bt = tt; best = i; }
    }
    if (dist) *dist = bt;
    return best;
}

void vz_surface_report(const VzMesh *m, const VzTris *t, const VzField *U, const double dir[3], VzSurfReport *r)
{
    memset(r, 0, sizeof *r);
    double wv = 0;
    long hint = -1;
    for (long i = 0; i < t->n; i++) {
        const float *x = &t->xyz[9 * i];
        double a[3] = { x[3] - x[0], x[4] - x[1], x[5] - x[2] }, b[3] = { x[6] - x[0], x[7] - x[1], x[8] - x[2] }, n[3];
        cross3(a, b, n);
        double A = len3(n) / 2;
        if (!(A > 0)) continue;
        r->area += A; r->n++;
        double v = 0; int k = 0;
        for (int j = 0; j < 3; j++) if (!isnan(t->val[3 * i + j])) { v += t->val[3 * i + j]; k++; }
        if (k) { r->mean += A * v / k; wv += A; }
        if (U && U->nc == 3) {
            for (int d = 0; d < 3; d++) n[d] /= 2 * A;
            if (dir && dot3(n, dir) < 0) for (int d = 0; d < 3; d++) n[d] = -n[d];
            double c[3] = { (x[0] + x[3] + x[6]) / 3, (x[1] + x[4] + x[7]) / 3, (x[2] + x[5] + x[8]) / 3 }, u[3];
            long h = t->cell && t->cell[i] >= 0 ? t->cell[i] : hint;
            if (vz_sample(m, U, -1, c, &h, u)) { double q = A * dot3(u, n); r->flow += q; r->flowAbs += fabs(q); hint = h; }
        }
    }
    r->mean = wv > 0 ? r->mean / wv : NAN;
}
