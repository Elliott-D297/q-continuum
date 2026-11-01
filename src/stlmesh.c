/* stlmesh.c -- an STL surface turned into a mesh (see stlmesh.h).
 *
 * The STL is read whole (ASCII, multi-solid ASCII or binary), its vertices welded, and its triangles flooded into
 * regions across edges flatter than the split angle (STAR-CCM+'s "split by angle"); small slivers join a neighbour.
 * Open and non-manifold edges say whether it is closed. The settings live in <case>/mesher/mesher.ini, which SALOME's
 * script (assets/salome/stl_mesher.py) reads too, so both meshers mesh the same thing.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stlmesh.h"
#include "assets.h"

const char *const BT_NAME[BT__N] = { "Wall", "Patch", "Inlet", "Outlet", "Symmetry" };
const char *const MESHER_NAME[MESHER__N] = { "Trimmed hex: snappyHexMesh", "Tetrahedral: SALOME NETGEN",
                                             "Polyhedral: NETGEN + polyDualMesh", "Body-fitted hex: SALOME" };

/* ============================================================================ reading */
static void grow_tris(Stl *s, int *cap, int need)
{
    if (need <= *cap) return;
    int nc = *cap ? *cap * 2 : 4096;
    while (nc < need) nc *= 2;
    s->tri = realloc(s->tri, sizeof(float) * 9 * (size_t)nc);
    s->solid = realloc(s->solid, sizeof *s->solid * (size_t)nc);
    *cap = nc;
}

static bool load_ascii(Stl *s, const char *buf, size_t len)
{
    int cap = 0, nv = 0;
    char solid[64] = "surface";
    float v[9];
    const char *p = buf, *end = buf + len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        size_t n = (size_t)(p - ls);
        if (n >= 5 && !strncmp(ls, "solid", 5)) {
            const char *q = ls + 5;
            while (q < p && (*q == ' ' || *q == '\t')) q++;
            int k = 0;
            while (q < p && k < 63 && *q != ' ' && *q != '\t' && *q != '\r') solid[k++] = *q++;
            solid[k] = 0;
            if (!k) strcpy(solid, "surface");
        } else if (n >= 6 && !strncmp(ls, "vertex", 6)) {
            if (nv < 3) {
                char tmp[160]; size_t m = n < sizeof tmp - 1 ? n : sizeof tmp - 1;
                memcpy(tmp, ls, m); tmp[m] = 0;
                if (sscanf(tmp + 6, "%f %f %f", &v[nv * 3], &v[nv * 3 + 1], &v[nv * 3 + 2]) == 3) nv++;
            }
        } else if (n >= 7 && !strncmp(ls, "endloop", 7)) {
            if (nv == 3) {
                grow_tris(s, &cap, s->nTri + 1);
                memcpy(s->tri + 9 * (size_t)s->nTri, v, sizeof v);
                pl_strlcpy(s->solid[s->nTri], solid, 64);
                s->nTri++;
            }
            nv = 0;
        }
    }
    return s->nTri > 0;
}

bool stl_load(const char *path, Stl *s)
{
    memset(s, 0, sizeof *s);
    s->scale = 1;
    pl_strlcpy(s->path, path, sizeof s->path);
    size_t len = 0;
    char *buf = pl_read_file(path, &len);
    if (!buf) { snprintf(s->err, sizeof s->err, "Could not read %s", path); return false; }
    /* binary: 80-byte header, a count, 50 bytes a triangle. An ASCII file starts with "solid", but so do some binary ones,
       so the size decides */
    uint32_t cnt = 0;
    if (len >= 84) memcpy(&cnt, buf + 80, 4);
    bool bin = len >= 84 && (size_t)84 + (size_t)cnt * 50 == len;
    if (bin) {
        s->binary = true;
        int cap = 0;
        grow_tris(s, &cap, (int)cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            memcpy(s->tri + 9 * (size_t)i, buf + 84 + (size_t)i * 50 + 12, 36);
            strcpy(s->solid[i], "surface");
        }
        s->nTri = (int)cnt;
    } else if (!load_ascii(s, buf, len)) {
        free(buf);
        snprintf(s->err, sizeof s->err, "%s has no triangles: it is not an STL file, or an empty one", pl_basename(path));
        return false;
    }
    free(buf);
    if (s->nTri <= 0) { snprintf(s->err, sizeof s->err, "%s has no triangles", pl_basename(path)); return false; }
    s->region = calloc((size_t)s->nTri, sizeof(int));
    return true;
}

void stl_free(Stl *s)
{
    free(s->tri); free(s->region); free(s->solid);
    memset(s, 0, sizeof *s);
}

void stl_set_scale(Stl *s, double scale)
{
    if (scale <= 0 || fabs(scale - s->scale) < 1e-15) return;
    float k = (float)(scale / s->scale);
    for (size_t i = 0; i < 9 * (size_t)s->nTri; i++) s->tri[i] *= k;
    s->scale = scale;
}

/* ============================================================================ topology */
typedef struct { int64_t key; int val; } Slot;
static uint64_t mix64(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }

/* welded vertex ids, 3 a triangle */
static int *weld(const Stl *s, int *nVert)
{
    double d = 0;
    for (int k = 0; k < 3; k++) d += (double)(s->bmax[k] - s->bmin[k]) * (s->bmax[k] - s->bmin[k]);
    double tol = fmax(sqrt(d) * 1e-7, 1e-12);
    size_t n = 3 * (size_t)s->nTri, cap = 1;
    while (cap < n * 2) cap <<= 1;
    Slot *h = malloc(sizeof(Slot) * cap);
    for (size_t i = 0; i < cap; i++) h[i].key = INT64_MIN;
    int *id = malloc(sizeof(int) * n);
    int nv = 0;
    for (size_t i = 0; i < n; i++) {
        const float *p = s->tri + 3 * i;
        int64_t q[3];
        for (int k = 0; k < 3; k++) q[k] = (int64_t)llround((p[k] - s->bmin[k]) / tol);
        int64_t key = (q[0] * 73856093LL) ^ (q[1] * 19349663LL) ^ (q[2] * 83492791LL);
        /* the key is a hash of the cell; compare the cells exactly through the first vertex stored there */
        size_t j = mix64((uint64_t)key) & (cap - 1);
        int found = -1;
        while (h[j].key != INT64_MIN) {
            if (h[j].key == key) {
                const float *o = s->tri + 3 * (size_t)h[j].val;
                int64_t r0 = llround((o[0] - s->bmin[0]) / tol), r1 = llround((o[1] - s->bmin[1]) / tol), r2 = llround((o[2] - s->bmin[2]) / tol);
                if (r0 == q[0] && r1 == q[1] && r2 == q[2]) { found = (int)h[j].val; break; }
            }
            j = (j + 1) & (cap - 1);
        }
        if (found >= 0) id[i] = id[found];
        else { h[j].key = key; h[j].val = (int)i; id[i] = nv++; }
    }
    free(h);
    *nVert = nv;
    return id;
}

static void tri_normal(const float *t, double n[3], double *area)
{
    double a[3] = { t[3] - t[0], t[4] - t[1], t[5] - t[2] }, b[3] = { t[6] - t[0], t[7] - t[1], t[8] - t[2] };
    n[0] = a[1] * b[2] - a[2] * b[1]; n[1] = a[2] * b[0] - a[0] * b[2]; n[2] = a[0] * b[1] - a[1] * b[0];
    double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    *area = l / 2;
    if (l > 0) { n[0] /= l; n[1] /= l; n[2] /= l; }
}

void stl_split(Stl *s, double angleDeg)
{
    int N = s->nTri;
    for (int k = 0; k < 3; k++) { s->bmin[k] = 1e30f; s->bmax[k] = -1e30f; }
    for (size_t i = 0; i < 3 * (size_t)N; i++)
        for (int k = 0; k < 3; k++) {
            float v = s->tri[3 * i + k];
            if (v < s->bmin[k]) s->bmin[k] = v;
            if (v > s->bmax[k]) s->bmax[k] = v;
        }
    int nv = 0;
    int *vid = weld(s, &nv);
    /* edges: (a, b) with a < b, the triangles on each */
    size_t ne = 3 * (size_t)N, cap = 1;
    while (cap < ne * 2) cap <<= 1;
    int64_t *ek = malloc(sizeof(int64_t) * cap);
    int *et0 = malloc(sizeof(int) * cap), *et1 = malloc(sizeof(int) * cap), *ec = calloc(cap, sizeof(int));
    for (size_t i = 0; i < cap; i++) ek[i] = -1;
    int *triEdge = malloc(sizeof(int) * ne);
    for (int t = 0; t < N; t++)
        for (int e = 0; e < 3; e++) {
            int a = vid[3 * t + e], b = vid[3 * t + (e + 1) % 3];
            if (a > b) { int x = a; a = b; b = x; }
            int64_t key = (int64_t)a * nv + b;
            size_t j = mix64((uint64_t)key) & (cap - 1);
            while (ek[j] != -1 && ek[j] != key) j = (j + 1) & (cap - 1);
            if (ek[j] == -1) { ek[j] = key; et0[j] = t; }
            else if (ec[j] == 1) et1[j] = t;
            ec[j]++;
            triEdge[3 * t + e] = (int)j;
        }
    s->openEdges = s->nonManifold = 0;
    for (size_t j = 0; j < cap; j++) {
        if (ek[j] == -1) continue;
        if (ec[j] == 1) s->openEdges++;
        else if (ec[j] > 2) s->nonManifold++;
    }
    double *nrm = malloc(sizeof(double) * 3 * (size_t)N), *ar = malloc(sizeof(double) * (size_t)N);
    s->area = 0; s->volume = 0;
    for (int t = 0; t < N; t++) {
        tri_normal(s->tri + 9 * (size_t)t, nrm + 3 * t, ar + t);
        s->area += ar[t];
        const float *p = s->tri + 9 * (size_t)t;
        s->volume += (p[0] * (p[4] * p[8] - p[5] * p[7]) - p[1] * (p[3] * p[8] - p[5] * p[6]) + p[2] * (p[3] * p[7] - p[4] * p[6])) / 6.0;
    }
    s->volume = fabs(s->volume);
    /* flood fill: across an edge shared by exactly two triangles of the same solid, flatter than the angle */
    double cosA = cos((angleDeg > 0 ? angleDeg : 181) * M_PI / 180);
    int *reg = s->region, nr = 0;
    for (int t = 0; t < N; t++) reg[t] = -1;
    int *stack = malloc(sizeof(int) * (size_t)N);
    int *solidOf = NULL; int nSolidNames = 0, solidCap = 0;   /* region -> its solid's first triangle (for its name) */
    for (int t0 = 0; t0 < N; t0++) {
        if (reg[t0] >= 0) continue;
        int sp = 0; stack[sp++] = t0; reg[t0] = nr;
        while (sp) {
            int t = stack[--sp];
            for (int e = 0; e < 3; e++) {
                int j = triEdge[3 * t + e];
                if (ec[j] != 2) continue;
                int o = et0[j] == t ? et1[j] : et0[j];
                if (reg[o] >= 0 || strcmp(s->solid[o], s->solid[t])) continue;
                if (angleDeg > 0 && nrm[3 * t] * nrm[3 * o] + nrm[3 * t + 1] * nrm[3 * o + 1] + nrm[3 * t + 2] * nrm[3 * o + 2] < cosA) continue;
                reg[o] = nr; stack[sp++] = o;
            }
        }
        if (nr >= solidCap) { solidCap = solidCap ? solidCap * 2 : 64; solidOf = realloc(solidOf, sizeof(int) * (size_t)solidCap); }
        solidOf[nr] = t0;
        nr++;
    }
    (void)nSolidNames;
    /* by solid only (no angle): one region a solid, even where a solid has disconnected pieces */
    double *rArea = calloc((size_t)nr, sizeof(double));
    for (int t = 0; t < N; t++) rArea[reg[t]] += ar[t];
    int *remap = malloc(sizeof(int) * (size_t)nr);
    for (int r = 0; r < nr; r++) remap[r] = r;
    if (angleDeg <= 0) {
        for (int r = 0; r < nr; r++)
            for (int q = 0; q < r; q++)
                if (remap[q] == q && !strcmp(s->solid[solidOf[q]], s->solid[solidOf[r]])) { remap[r] = q; break; }
    } else {
        /* slivers (under 0.5 % of the area) join their largest neighbour, a few rounds so chains of slivers settle; one pass
           over the edges a round. Regions past the limit are folded into the last one when numbered below. */
        int *best = malloc(sizeof(int) * (size_t)nr);
        for (int round = 0; round < 4; round++) {
            for (int r = 0; r < nr; r++) best[r] = -1;
            for (size_t j = 0; j < cap; j++) {
                if (ek[j] == -1 || ec[j] != 2) continue;
                int ra = reg[et0[j]], rb = reg[et1[j]];
                while (remap[ra] != ra) ra = remap[ra];
                while (remap[rb] != rb) rb = remap[rb];
                if (ra == rb) continue;
                if (best[ra] < 0 || rArea[rb] > rArea[best[ra]]) best[ra] = rb;
                if (best[rb] < 0 || rArea[ra] > rArea[best[rb]]) best[rb] = ra;
            }
            bool changed = false;
            for (int r = 0; r < nr; r++) {
                if (remap[r] != r || best[r] < 0 || rArea[r] >= s->area * 0.005) continue;
                int b = best[r];
                while (remap[b] != b) b = remap[b];
                if (b == r || rArea[b] < rArea[r]) continue;   /* only into a larger one: no cycles */
                remap[r] = b; rArea[b] += rArea[r]; changed = true;
            }
            if (!changed) break;
        }
        free(best);
    }
    /* number the live regions 0.., largest first; past the limit everything left goes into the last one */
    int *live = malloc(sizeof(int) * (size_t)nr), nl = 0;
    for (int r = 0; r < nr; r++) if (remap[r] == r) live[nl++] = r;
    for (int a = 0; a < nl; a++)
        for (int b = a + 1; b < nl; b++) if (rArea[live[b]] > rArea[live[a]]) { int x = live[a]; live[a] = live[b]; live[b] = x; }
    int *final = malloc(sizeof(int) * (size_t)nr);
    for (int r = 0; r < nr; r++) final[r] = -1;
    for (int k = 0; k < nl; k++) final[live[k]] = k < STL_MAX_REGIONS ? k : STL_MAX_REGIONS - 1;
    s->nRegions = nl < STL_MAX_REGIONS ? nl : STL_MAX_REGIONS;
    memset(s->reg, 0, sizeof s->reg);
    for (int t = 0; t < N; t++) {
        int r = reg[t]; while (remap[r] != r) r = remap[r];
        reg[t] = final[r];
        StlRegion *R = &s->reg[reg[t]];
        R->nTri++; R->area += ar[t];
        for (int k = 0; k < 3; k++) R->n[k] += (float)(nrm[3 * t + k] * ar[t]);
    }
    int firstTri[STL_MAX_REGIONS];
    for (int k = 0; k < STL_MAX_REGIONS; k++) firstTri[k] = -1;
    for (int t = 0; t < N; t++) if (firstTri[reg[t]] < 0) firstTri[reg[t]] = t;
    for (int k = 0; k < s->nRegions; k++) {
        StlRegion *R = &s->reg[k];
        float l = sqrtf(R->n[0] * R->n[0] + R->n[1] * R->n[1] + R->n[2] * R->n[2]);
        if (l > 0) for (int c = 0; c < 3; c++) R->n[c] /= l;
        R->type = BT_WALL;
        /* the solid's own name when the region is the whole solid, else <solid>_<n> or wall_<n> */
        int first = firstTri[k], nSame = 0;
        for (int q = 0; q < s->nRegions; q++)
            if (firstTri[q] >= 0 && first >= 0 && !strcmp(s->solid[firstTri[q]], s->solid[first])) nSame++;
        bool named = first >= 0 && strcmp(s->solid[first], "surface") && s->solid[first][0];
        if (named && nSame == 1) pl_strlcpy(R->name, s->solid[first], sizeof R->name);
        else if (named) snprintf(R->name, sizeof R->name, "%.50s_%d", s->solid[first], k + 1);
        else snprintf(R->name, sizeof R->name, nl == 1 ? "walls" : "wall_%d", k + 1);
        for (char *c = R->name; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) *c = '_';
    }
    free(live); free(final); free(remap); free(rArea); free(solidOf); free(stack);
    free(nrm); free(ar); free(triEdge); free(ek); free(et0); free(et1); free(ec); free(vid);
}

/* ray parity along a direction no face is likely to be aligned with */
bool stl_inside(const Stl *s, const double p[3])
{
    const double d[3] = { 0.5773, 0.5774, 0.5775 };
    int hits = 0;
    for (int t = 0; t < s->nTri; t++) {
        const float *v = s->tri + 9 * (size_t)t;
        double e1[3] = { v[3] - v[0], v[4] - v[1], v[5] - v[2] }, e2[3] = { v[6] - v[0], v[7] - v[1], v[8] - v[2] };
        double h[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
        double a = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
        if (fabs(a) < 1e-30) continue;
        double f = 1 / a, sv[3] = { p[0] - v[0], p[1] - v[1], p[2] - v[2] };
        double u = f * (sv[0] * h[0] + sv[1] * h[1] + sv[2] * h[2]);
        if (u < 0 || u > 1) continue;
        double q[3] = { sv[1] * e1[2] - sv[2] * e1[1], sv[2] * e1[0] - sv[0] * e1[2], sv[0] * e1[1] - sv[1] * e1[0] };
        double w = f * (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]);
        if (w < 0 || u + w > 1) continue;
        if (f * (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) > 0) hits++;
    }
    return hits % 2 == 1;
}

/* the part's thickness where it is thin: from sampled triangles, a ray into the part to its other side; the 10th
   percentile of those, so a thin blade or fin counts and a thick hub doesn't hide it. 0: no hit (an open surface) */
double stl_thickness(const Stl *s)
{
    if (s->nTri < 2) return 0;
    long budget = 300000000;                                /* ray-triangle tests: about half a second at most */
    int ns = (int)fmin(1500, fmax(60, (double)budget / s->nTri)), step = s->nTri > ns ? s->nTri / ns : 1;
    double *d = malloc(sizeof(double) * (size_t)(s->nTri / step + 2));
    if (!d) return 0;
    int nd = 0;
    double diag = 0;
    for (int k = 0; k < 3; k++) diag += (double)(s->bmax[k] - s->bmin[k]) * (s->bmax[k] - s->bmin[k]);
    diag = sqrt(diag);
    for (int t0 = 0; t0 < s->nTri; t0 += step) {
        const float *v = s->tri + 9 * (size_t)t0;
        double e1[3] = { v[3] - v[0], v[4] - v[1], v[5] - v[2] }, e2[3] = { v[6] - v[0], v[7] - v[1], v[8] - v[2] };
        double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        double nl = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 1e-30) continue;
        double o[3] = { (v[0] + v[3] + v[6]) / 3, (v[1] + v[4] + v[7]) / 3, (v[2] + v[5] + v[8]) / 3 };
        double best = 1e30;
        for (int side = -1; side <= 1 && best > diag * 2; side += 2) {   /* into the part: against the normal, else along it */
            double dir[3] = { side * n[0] / nl, side * n[1] / nl, side * n[2] / nl };
            for (int t = 0; t < s->nTri; t++) {
                if (t == t0) continue;
                const float *w = s->tri + 9 * (size_t)t;
                double a1[3] = { w[3] - w[0], w[4] - w[1], w[5] - w[2] }, a2[3] = { w[6] - w[0], w[7] - w[1], w[8] - w[2] };
                double h[3] = { dir[1] * a2[2] - dir[2] * a2[1], dir[2] * a2[0] - dir[0] * a2[2], dir[0] * a2[1] - dir[1] * a2[0] };
                double a = a1[0] * h[0] + a1[1] * h[1] + a1[2] * h[2];
                if (fabs(a) < 1e-30) continue;
                double f = 1 / a, sv[3] = { o[0] - w[0], o[1] - w[1], o[2] - w[2] };
                double u = f * (sv[0] * h[0] + sv[1] * h[1] + sv[2] * h[2]);
                if (u < 0 || u > 1) continue;
                double q[3] = { sv[1] * a1[2] - sv[2] * a1[1], sv[2] * a1[0] - sv[0] * a1[2], sv[0] * a1[1] - sv[1] * a1[0] };
                double vv = f * (dir[0] * q[0] + dir[1] * q[1] + dir[2] * q[2]);
                if (vv < 0 || u + vv > 1) continue;
                double dist = f * (a2[0] * q[0] + a2[1] * q[1] + a2[2] * q[2]);
                if (dist > diag * 1e-6 && dist < best) best = dist;
            }
        }
        if (best < diag * 2) d[nd++] = best;
    }
    double r = 0;
    if (nd >= 3) {
        for (int i = 1; i < nd; i++) { double x = d[i]; int j = i - 1; while (j >= 0 && d[j] > x) { d[j + 1] = d[j]; j--; } d[j + 1] = x; }
        r = d[nd / 10];
    }
    free(d);
    return r;
}

/* ============================================================================ settings */
const char *const DIR_NAME[DIR__N] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
static const char *const DIR_WORD[DIR__N] = { "+x", "-x", "+y", "-y", "+z", "-z" };
static int dir_of(const char *w, int def) { for (int i = 0; i < DIR__N; i++) if (!strcasecmp(w, DIR_WORD[i])) return i; return def; }
static int dir_sign(int d) { return d % 2 ? -1 : 1; }

int mesher_up(const MeshCfg *m)
{
    int f = m->flowDir / 2, n = m->twoD ? m->normal2D : -1;
    if (m->upDir >= 0 && m->upDir < DIR__N && m->upDir / 2 != f && m->upDir / 2 != n) return m->upDir;
    for (int a = 2; a >= 0; a--) if (a != f && a != n) return 2 * a;   /* z, else y, else x: square to the flow (and the 2-D normal) */
    return DIR_PZ;
}

int stl_thin_axis(const Stl *s)
{
    int a = 0;
    for (int k = 1; k < 3; k++) if (s->bmax[k] - s->bmin[k] < s->bmax[a] - s->bmin[a]) a = k;
    return a;
}

/* 2-D: the slab snappyHexMesh meshes, across the extrusion at its middle. It is a base cell thick, so its cells are cubes:
   a slab as thin as the part (often a few mm against a base of cm) makes them flat, and the prism layers fail the
   quality checks on flat cells. The profile is stretched along the extrusion to go right through it (twod_stretch), and
   extrudeMesh then makes one cell of the slab's front face, as deep as the part (mesher_2d_script). */
double mesher_slab(const MeshCfg *m, const Stl *s, double *lo, double *hi)
{
    int n = m->normal2D % 3;
    double span = s->bmax[n] - s->bmin[n], c = (s->bmin[n] + s->bmax[n]) / 2;
    double t = m->base > 0 ? m->base : span / 2;
    *lo = c - t / 2; *hi = c + t / 2;
    return t;
}

/* 2-D: how much the profile is stretched along the extrusion (about its middle) to stand out of the slab either side */
static double twod_stretch(const MeshCfg *m, const Stl *s)
{
    if (!m || !m->twoD) return 1;
    int n = m->normal2D % 3;
    double lo, hi, span = s->bmax[n] - s->bmin[n];
    double t = mesher_slab(m, s, &lo, &hi);
    return span > 0 && 2 * t > span ? 2 * t / span : 1;
}

/* 2-D: the end caps face along the extrusion. They are outside the slab, and given to snappyHexMesh they pull the
   profile's points out of its plane (snapping to them and their edges), so they are left out of everything it reads */
bool mesher_cap_tri(const MeshCfg *m, const float *v)
{
    if (!m || !m->twoD) return false;
    double n[3], a; tri_normal(v, n, &a);
    return fabs(n[m->normal2D % 3]) > 0.9;
}

bool mesher_region_meshed(const MeshCfg *m, const Stl *s, int r)
{
    if (!m || !m->twoD) return true;
    for (int t = 0; t < s->nTri; t++) if (s->region[t] == r && !mesher_cap_tri(m, s->tri + 9 * (size_t)t)) return true;
    return false;
}

static double body_length(const Stl *s) { double L = 0; for (int k = 0; k < 3; k++) L = fmax(L, s->bmax[k] - s->bmin[k]); return L > 0 ? L : 1; }

void mesher_defaults(MeshCfg *m, const Stl *s)
{
    char keep[PL_PATH]; pl_strlcpy(keep, m->stl, sizeof keep);
    double scale = m->scale > 0 ? m->scale : 1;
    memset(m, 0, sizeof *m);
    pl_strlcpy(m->stl, keep, sizeof m->stl);
    m->scale = scale;
    m->splitAngle = 30;
    m->domain = s && s->openEdges == 0 ? DOMAIN_INTERNAL : DOMAIN_EXTERNAL;
    m->up = 3; m->down = 8; m->side = 3; m->top = 3; m->below = 3; m->ground = false;
    m->flowDir = DIR_PX; m->upDir = DIR_PZ; m->refLen = 0;
    m->twoD = false; m->normal2D = s ? stl_thin_axis(s) : 2;
    m->mesher = MESHER_SNAPPY;
    m->curvature = 36; m->surfGrowth = 1.3; m->volGrowth = 1.2; m->featureAngle = 30;
    m->prism = true; m->layers = 3; m->stretch = 1.2; m->evenLayers = -1;
    m->maxNonOrtho = 65; m->maxSkew = 4;
    mesher_size_defaults(m, s, s ? stl_thickness(s) : 0);
}

/* the sizes for m->domain (internal: the part is the fluid; external: the air round it, finer at the surface), and
   surface cells that fit across a thin part */
void mesher_size_defaults(MeshCfg *m, const Stl *s, double thick)
{
    double L = s ? body_length(s) : 1;
    double b = L / (m->domain == DOMAIN_INTERNAL ? 40 : 15);
    double e = pow(10, floor(log10(b)));                  /* a round number: 1, 2 or 5 x 10^n */
    m->base = b / e >= 5 ? 5 * e : b / e >= 2 ? 2 * e : e;
    m->targetPct = m->domain == DOMAIN_INTERNAL ? 100 : 25;
    m->minPct = m->domain == DOMAIN_INTERNAL ? 25 : 6.25;
    m->prismPct = m->domain == DOMAIN_INTERNAL ? 33.3 : 10;
    /* fitted when that stays a size for one machine (tiny fillets don't), and not when the thin part is the whole part
       along an axis: a profile extruded, which is better meshed 2-D (that is the user's choice) */
    int ta = s ? stl_thin_axis(s) : 0;
    bool extrusion = s && fabs((s->bmax[ta] - s->bmin[ta]) - thick) < 0.2 * (s->bmax[ta] - s->bmin[ta]);
    if (thick > 0 && !mesher_fits(m, thick) && s && !extrusion) {
        MeshCfg t = *m;
        mesher_fit(&t, thick);
        if (mesher_estimate_cells(&t, s) < 2e6) *m = t;
    }
}

/* snappyHexMesh carves a body out of the cells it cuts: one thinner than its surface cells comes out a sliver or not at
   all. Its surface cells fit when they are at most 0.7 of the thickness (the finest a few times smaller still) */
bool mesher_fits(const MeshCfg *m, double thick)
{
    if (!(thick > 0) || m->base <= 0) return true;
    return m->base * m->targetPct / 100 <= thick * 0.7 * 1.0001 && m->base * m->minPct / 100 <= thick * 0.35 * 1.0001;
}

void mesher_fit(MeshCfg *m, double thick)
{
    if (!(thick > 0) || m->base <= 0) return;
    int L0 = (int)ceil(log2(m->base / (thick * 0.7)) - 1e-9); /* the surface's level: base / 2^L0 within 0.7 of the thickness */
    if (L0 < 0) L0 = 0;
    if (m->base * m->targetPct / 100 > m->base / pow(2, L0)) m->targetPct = 100 / pow(2, L0);
    if (m->base * m->minPct / 100 > m->base / pow(2, L0 + 1)) m->minPct = 100 / pow(2, L0 + 1);
}

static char *ini_get(char *ini, const char *key)
{
    size_t kl = strlen(key);
    for (char *p = ini; p && *p; ) {
        char *nl = strchr(p, '\n');
        if (!strncmp(p, key, kl) && (p[kl] == ' ' || p[kl] == '=')) {
            char *v = strchr(p, '=');
            if (v && (!nl || v < nl)) { v++; while (*v == ' ') v++; return v; }
        }
        p = nl ? nl + 1 : NULL;
    }
    return NULL;
}
static double ini_num(char *ini, const char *key, double def) { char *v = ini_get(ini, key); return v ? atof(v) : def; }
static void ini_str(char *ini, const char *key, char *out, size_t cap)
{
    char *v = ini_get(ini, key);
    if (!v) return;
    size_t n = strcspn(v, "\r\n");
    if (n >= cap) n = cap - 1;
    memcpy(out, v, n); out[n] = 0;
}

static void ini_path(const char *casePath, char *out, size_t cap) { char d[PL_PATH]; pl_join(d, sizeof d, casePath, "mesher"); pl_join(out, cap, d, "mesher.ini"); }

bool mesher_load(const char *casePath, MeshCfg *m)
{
    char p[PL_PATH]; ini_path(casePath, p, sizeof p);
    char *ini = pl_read_file(p, NULL);
    if (!ini) return false;
    mesher_defaults(m, NULL);
    ini_str(ini, "stl", m->stl, sizeof m->stl);
    ini_str(ini, "stl_original", m->stlOriginal, sizeof m->stlOriginal);
    m->scale = ini_num(ini, "scale", 1);
    m->splitAngle = ini_num(ini, "split_angle", 30);
    char d[32] = ""; ini_str(ini, "domain", d, sizeof d);
    m->domain = !strcmp(d, "external") ? DOMAIN_EXTERNAL : DOMAIN_INTERNAL;
    m->up = ini_num(ini, "upstream", 3); m->down = ini_num(ini, "downstream", 8);
    m->side = ini_num(ini, "sides", 3); m->top = ini_num(ini, "top", 3);
    m->below = ini_num(ini, "below", m->side);            /* before there was one: as much as the sides */
    { char w[16] = "+x"; ini_str(ini, "flow_direction", w, sizeof w); m->flowDir = dir_of(w, DIR_PX);
      pl_strlcpy(w, "+z", sizeof w); ini_str(ini, "up_direction", w, sizeof w); m->upDir = dir_of(w, DIR_PZ); }
    m->refLen = ini_num(ini, "reference_length", 0);
    m->twoD = ini_num(ini, "two_d", 0) != 0;
    { char w[8] = "z"; ini_str(ini, "two_d_normal", w, sizeof w); m->normal2D = w[0] == 'x' ? 0 : w[0] == 'y' ? 1 : 2; }
    m->ground = ini_num(ini, "ground", 0) != 0;
    char ms[32] = ""; ini_str(ini, "mesher", ms, sizeof ms);
    m->mesher = !strcmp(ms, "tet") ? MESHER_TET : !strcmp(ms, "poly") ? MESHER_POLY : !strcmp(ms, "bodyfit") ? MESHER_BODYFIT : MESHER_SNAPPY;
    m->remesh = ini_num(ini, "remesh_surface", 0) != 0;
    m->base = ini_num(ini, "base_size", m->base);
    m->targetPct = ini_num(ini, "target_size_pct", m->targetPct); m->minPct = ini_num(ini, "min_size_pct", m->minPct);
    m->curvature = ini_num(ini, "curvature_points", m->curvature);
    m->surfGrowth = ini_num(ini, "surface_growth", m->surfGrowth); m->volGrowth = ini_num(ini, "volume_growth", m->volGrowth);
    m->featureAngle = ini_num(ini, "feature_angle", m->featureAngle);
    m->prism = ini_num(ini, "prism_layers_on", 1) != 0;
    m->layers = (int)ini_num(ini, "prism_layers", m->layers);
    m->prismPct = ini_num(ini, "prism_thickness_pct", m->prismPct); m->stretch = ini_num(ini, "prism_stretching", m->stretch);
    m->evenLayers = (int)ini_num(ini, "prism_even_surface", -1);
    m->maxNonOrtho = ini_num(ini, "max_non_ortho", 65); m->maxSkew = ini_num(ini, "max_skewness", 4);
    m->nZones = (int)ini_num(ini, "zones", 0);
    if (m->nZones > STL_MAX_ZONES) m->nZones = STL_MAX_ZONES;
    for (int i = 0; i < m->nZones; i++) {
        MeshZone *z = &m->zone[i];
        char k[64], v[256] = "";
        snprintf(k, sizeof k, "zone%d", i); ini_str(ini, k, v, sizeof v);
        char kind[16] = "";
        sscanf(v, "%15s %lf %lf %lf %lf %lf %lf %lf %lf", kind, &z->a[0], &z->a[1], &z->a[2], &z->b[0], &z->b[1], &z->b[2], &z->r, &z->sizePct);
        z->kind = !strcmp(kind, "sphere") ? ZONE_SPHERE : !strcmp(kind, "cylinder") ? ZONE_CYLINDER : ZONE_BOX;
    }
    m->nNames = (int)ini_num(ini, "boundaries", 0);
    if (m->nNames > STL_MAX_REGIONS) m->nNames = STL_MAX_REGIONS;
    for (int i = 0; i < m->nNames; i++) {
        char k[64], v[128] = "", ty[32] = "";
        snprintf(k, sizeof k, "boundary%d", i); ini_str(ini, k, v, sizeof v);
        sscanf(v, "%63s %31s", m->names[i], ty);
        m->types[i] = BT_WALL;
        for (int t = 0; t < BT__N; t++) if (!strcasecmp(ty, BT_NAME[t])) m->types[i] = t;
    }
    free(ini);
    return true;
}

static const char *const ZONE_WORD[3] = { "box", "sphere", "cylinder" };
static const char *const MESHER_WORD[MESHER__N] = { "snappy", "tet", "poly", "bodyfit" };

static const char *foam_type(int t);

/* mesher.ini's text; sig: what the mesh depends on only, each boundary's type as its patch type (an inlet made an
   outlet is the same mesh) */
static void cfg_text(const MeshCfg *m, Str *bp, bool sig)
{
    Str b = *bp;
    str_cat(&b, "# Q-Continuum STL mesher settings (Mesh > STL mesher). SALOME's script salome/stl_mesh.py reads them too.\n"
                "# Sizes in metres; *_pct are percentages of base_size, as in STAR-CCM+'s reference values.\n[mesher]\n");
    str_catf(&b, "stl = %s\nscale = %.10g\nsplit_angle = %g\n", m->stl, m->scale, m->splitAngle);
    if (!sig && m->stlOriginal[0]) str_catf(&b, "# stl is a smooth profile made of this one (Flow > Smooth the profile)\nstl_original = %s\n", m->stlOriginal);
    str_catf(&b, "domain = %s\nupstream = %g\ndownstream = %g\nsides = %g\ntop = %g\nbelow = %g\nground = %d\n",
             m->domain == DOMAIN_EXTERNAL ? "external" : "internal", m->up, m->down, m->side, m->top, m->below, m->ground);
    str_catf(&b, "# external: the flow goes along flow_direction, up_direction is up; the tunnel's distances are in reference_length\n"
                 "# (0: the body's length along the flow)\nflow_direction = %s\nup_direction = %s\nreference_length = %g\n",
             DIR_WORD[m->flowDir], DIR_WORD[mesher_up(m)], m->refLen);
    str_catf(&b, "# 2-D: one cell thick across two_d_normal (a profile extruded along it), front2D and back2D empty\ntwo_d = %d\ntwo_d_normal = %c\n",
             m->twoD, "xyz"[m->normal2D % 3]);
    str_catf(&b, "mesher = %s\nremesh_surface = %d\n", MESHER_WORD[m->mesher], m->remesh);
    str_catf(&b, "base_size = %.6g\ntarget_size_pct = %g\nmin_size_pct = %g\ncurvature_points = %g\nsurface_growth = %g\nvolume_growth = %g\nfeature_angle = %g\n",
             m->base, m->targetPct, m->minPct, m->curvature, m->surfGrowth, m->volGrowth, m->featureAngle);
    str_catf(&b, "prism_layers_on = %d\nprism_layers = %d\nprism_thickness_pct = %g\nprism_stretching = %g\n", m->prism, m->layers, m->prismPct, m->stretch);
    if (m->evenLayers >= 0)                                /* not written while it follows the kind of mesh: older meshes stay current */
        str_catf(&b, "# the layered walls' surface cells all the minimum size, so the layers don't break where the size steps\nprism_even_surface = %d\n", m->evenLayers);
    str_catf(&b, "max_non_ortho = %g\nmax_skewness = %g\n", m->maxNonOrtho, m->maxSkew);
    str_catf(&b, "# zoneN = kind  a(x y z)  b(x y z)  radius  size_pct   (box: a = min, b = max; sphere: a = centre; cylinder: a, b = ends)\nzones = %d\n", m->nZones);
    for (int i = 0; i < m->nZones; i++) {
        const MeshZone *z = &m->zone[i];
        str_catf(&b, "zone%d = %s %.6g %.6g %.6g %.6g %.6g %.6g %.6g %g\n", i, ZONE_WORD[z->kind], z->a[0], z->a[1], z->a[2], z->b[0], z->b[1], z->b[2], z->r, z->sizePct);
    }
    str_catf(&b, "# boundaryN = name type, in the order of the regions in constant/triSurface/geometry.stl\nboundaries = %d\n", m->nNames);
    for (int i = 0; i < m->nNames; i++) str_catf(&b, "boundary%d = %s %s\n", i, m->names[i], sig ? foam_type(m->types[i]) : BT_NAME[m->types[i]]);
    *bp = b;
}

bool mesher_save(const char *casePath, const MeshCfg *m)
{
    Str b; str_init(&b);
    cfg_text(m, &b, false);
    char d[PL_PATH], p[PL_PATH];
    pl_join(d, sizeof d, casePath, "mesher"); pl_mkdirs(d);
    ini_path(casePath, p, sizeof p);
    /* unchanged: left alone, so its time says when the settings last changed (the mesh is out of date when older) */
    char *old = pl_read_file(p, NULL);
    bool same = old && !strcmp(old, b.s);
    free(old);
    bool ok = same || pl_write_file(p, b.s, b.len);
    str_free(&b);
    return ok;
}

void mesher_signature(const MeshCfg *m, Str *out) { cfg_text(m, out, true); }

static void sig_path(const char *casePath, char *out, size_t cap) { pl_join(out, cap, casePath, "mesher/mesh.sig"); }

int mesher_mesh_current(const char *casePath, const MeshCfg *m)
{
    char p[PL_PATH]; sig_path(casePath, p, sizeof p);
    char *old = pl_read_file(p, NULL);
    if (!old) return -1;
    Str b; str_init(&b);
    cfg_text(m, &b, true);
    int same = !strcmp(old, b.s);
    str_free(&b); free(old);
    return same;
}

void mesher_take_names(MeshCfg *m, const Stl *s)
{
    m->nNames = s->nRegions;
    for (int i = 0; i < s->nRegions; i++) { pl_strlcpy(m->names[i], s->reg[i].name, 64); m->types[i] = s->reg[i].type; }
}

void mesher_apply_names(const MeshCfg *m, Stl *s)
{
    if (m->nNames != s->nRegions) return;                 /* split differently since: keep the fresh names */
    for (int i = 0; i < s->nRegions; i++) { if (m->names[i][0]) pl_strlcpy(s->reg[i].name, m->names[i], 64); s->reg[i].type = m->types[i]; }
}

void mesher_domain(const MeshCfg *m, const Stl *s, double lo[3], double hi[3])
{
    if (m->domain == DOMAIN_INTERNAL) {                  /* the part, a cell and a half larger all round */
        for (int k = 0; k < 3; k++) { lo[k] = s->bmin[k] - 1.5 * m->base; hi[k] = s->bmax[k] + 1.5 * m->base; }
        if (m->twoD) mesher_slab(m, s, &lo[m->normal2D], &hi[m->normal2D]);
        return;
    }
    /* a wind tunnel along the flow direction: upstream and downstream along it, the top above, the ground (or as much as
       the sides) below, the sides across */
    double L = mesher_ref_length(m, s);
    int f = m->flowDir / 2, fs = dir_sign(m->flowDir), ud = mesher_up(m), u = ud / 2, us = dir_sign(ud), sa = 3 - f - u;
    for (int k = 0; k < 3; k++) { lo[k] = s->bmin[k]; hi[k] = s->bmax[k]; }
    if (fs > 0) { lo[f] -= m->up * L; hi[f] += m->down * L; } else { lo[f] -= m->down * L; hi[f] += m->up * L; }
    lo[sa] -= m->side * L; hi[sa] += m->side * L;
    double below = m->ground ? 0 : m->below * L;
    if (us > 0) { hi[u] += m->top * L; lo[u] -= below; } else { lo[u] -= m->top * L; hi[u] += below; }
    if (m->twoD && m->normal2D != f) mesher_slab(m, s, &lo[m->normal2D], &hi[m->normal2D]);
}

double mesher_ref_length(const MeshCfg *m, const Stl *s)
{
    if (m->refLen > 0) return m->refLen;
    int f = m->flowDir / 2;
    double L = s->bmax[f] - s->bmin[f];                  /* the body's length along the flow (a blade's chord) */
    return L > 0 ? L : body_length(s);
}

int mesher_far_levels(const MeshCfg *m, const Stl *s)
{
    if (m->domain != DOMAIN_EXTERNAL || m->twoD || m->mesher != MESHER_SNAPPY || m->base <= 0) return 0;
    int K = (int)floor(log2(mesher_ref_length(m, s) / 2 / m->base) + 1e-9);   /* far cells no larger than half the body */
    return K < 0 ? 0 : K > 3 ? 3 : K;
}

void mesher_near_box(const MeshCfg *m, const Stl *s, double lo[3], double hi[3])
{
    double dlo[3], dhi[3];
    mesher_domain(m, s, dlo, dhi);
    double L = mesher_ref_length(m, s);
    int f = m->flowDir / 2, fs = dir_sign(m->flowDir);
    for (int k = 0; k < 3; k++) { lo[k] = s->bmin[k] - 0.5 * L; hi[k] = s->bmax[k] + 0.5 * L; }
    if (fs > 0) hi[f] = s->bmax[f] + 2 * L; else lo[f] = s->bmin[f] - 2 * L;   /* the wake */
    for (int k = 0; k < 3; k++) { lo[k] = fmax(lo[k], dlo[k]); hi[k] = fmin(hi[k], dhi[k]); }
}

static int level_of(double pct) { return pct >= 100 ? 0 : (int)lround(log2(100.0 / pct)); }

/* the outer layer's share of the total: n layers, each r times the one below */
static double outer_share(int n, double r)
{
    if (n < 1) n = 1;
    return fabs(r - 1) < 1e-9 ? 1.0 / n : pow(r, n - 1) * (r - 1) / (pow(r, n) - 1);
}

/* snappyHexMesh halves its cells per level, so its target rounds to one; SALOME's NETGEN takes the size as it is */
double mesher_surface_cell(const MeshCfg *m)
{
    return m->mesher == MESHER_SNAPPY ? m->base / pow(2, level_of(m->targetPct)) : m->base * m->targetPct / 100;
}

bool mesher_even_layers(const MeshCfg *m)
{
    return m->mesher == MESHER_SNAPPY && m->prism && m->layers > 0 && (m->evenLayers < 0 ? m->twoD : m->evenLayers == 1);
}

double mesher_layer_cell(const MeshCfg *m)
{
    return mesher_even_layers(m) ? m->base / pow(2, fmax(level_of(m->targetPct), level_of(m->minPct))) : mesher_surface_cell(m);
}

double mesher_layer_rel(const MeshCfg *m, double *outer)
{
    double h = mesher_layer_cell(m);
    double T = h > 0 ? m->base * m->prismPct / 100 / h : 0.5;
    double f = outer_share(m->layers, m->stretch > 0 ? m->stretch : 1);
    double o = fmax(LAYER_OUTER_MIN, fmin(LAYER_OUTER_MAX, T * f));
    if (outer) *outer = o;
    return o / f;
}
static int between_levels(double g) { return g <= 1.0 ? 6 : (int)fmax(1, fmin(8, lround(0.5 / (g - 1)))); }

double mesher_estimate_cells(const MeshCfg *m, const Stl *s)
{
    double lo[3], hi[3];
    mesher_domain(m, s, lo, hi);
    double box = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
    double V = m->domain == DOMAIN_INTERNAL ? s->volume : box - s->volume;
    if (V <= 0) V = box;
    double b = m->base, A = s->area, n = 0;
    int L0 = level_of(m->targetPct), nb = between_levels(m->volGrowth);
    if (m->twoD) {                                        /* one cell thick: cells per area of the plane, finer bands round the profile */
        int nax = m->normal2D, a1 = (nax + 1) % 3, a2 = (nax + 2) % 3;
        double span = fmax(s->bmax[nax] - s->bmin[nax], 1e-12);
        double plane = (hi[a1] - lo[a1]) * (hi[a2] - lo[a2]), prof = fabs(s->volume) / span;
        double perim = fmax(0, (A - 2 * prof) / span);     /* the profile's outline: its side walls over the extrusion */
        n = (m->domain == DOMAIN_INTERNAL ? prof : plane - prof) / (b * b);
        int Ls = mesher_even_layers(m) ? (int)fmax(L0, level_of(m->minPct)) : L0;
        for (int l = 1; l <= Ls; l++) { double h = b / pow(2, l); n += perim * nb * h / (h * h) * 0.75; }
        if (m->prism && m->layers > 0) n += perim / mesher_layer_cell(m) * m->layers;
        return n;
    }
    if (m->mesher == MESHER_SNAPPY || m->mesher == MESHER_BODYFIT) {
        int K = mesher_far_levels(m, s);
        if (K > 0) {                                       /* base-sized near the body, 2^K coarser beyond */
            double nlo[3], nhi[3]; mesher_near_box(m, s, nlo, nhi);
            double near = (nhi[0] - nlo[0]) * (nhi[1] - nlo[1]) * (nhi[2] - nlo[2]) - fabs(s->volume), bg = b * pow(2, K);
            n = near / (b * b * b) + fmax(0, V - near) / (bg * bg * bg) * 1.5;   /* (and the grading between) */
        } else n = V / (b * b * b);
        int Ls = mesher_even_layers(m) ? (int)fmax(L0, level_of(m->minPct)) : L0;   /* even: the walls' finer cells all over them */
        for (int l = 1; l <= Ls; l++) { double h = b / pow(2, l); n += A * nb * h / (h * h * h) * 0.875; }
    } else {
        double h = b * m->targetPct / 100;                 /* tets: ~6 to a cube of the size, coarsening away from the wall */
        n = 6 * V / (b * b * b) + 6 * A * 3 * h / (h * h * h) * 0.5;
        if (m->mesher == MESHER_POLY) n /= 5;
    }
    for (int i = 0; i < m->nZones; i++) {
        const MeshZone *z = &m->zone[i];
        double zv = z->kind == ZONE_BOX ? fabs((z->b[0] - z->a[0]) * (z->b[1] - z->a[1]) * (z->b[2] - z->a[2]))
                  : z->kind == ZONE_SPHERE ? 4.0 / 3 * M_PI * z->r * z->r * z->r
                  : M_PI * z->r * z->r * sqrt(pow(z->b[0] - z->a[0], 2) + pow(z->b[1] - z->a[1], 2) + pow(z->b[2] - z->a[2], 2));
        double h = b * z->sizePct / 100;
        n += zv / (h * h * h) - zv / (b * b * b);
    }
    if (m->prism && m->layers > 0) {
        double h = mesher_even_layers(m) ? mesher_layer_cell(m) : b * m->targetPct / 100;
        double wallA = 0;
        for (int i = 0; i < s->nRegions; i++) if (s->reg[i].type == BT_WALL) wallA += s->reg[i].area;
        n += wallA / (h * h) * m->layers * (m->mesher == MESHER_POLY ? 0.5 : 1);
    }
    return n;
}

/* ============================================================================ writing */
/* m: 2-D, the end caps are left out and the profile stretched through the slab (NULL: the STL as it is) */
static bool write_ascii_stl(const Stl *s, const MeshCfg *m, const char *path)
{
    int n2 = m && m->twoD ? m->normal2D % 3 : -1;
    double k = twod_stretch(m, s), c = n2 >= 0 ? (s->bmin[n2] + s->bmax[n2]) / 2 : 0;
    Str b; str_init(&b);
    for (int r = 0; r < s->nRegions; r++) {
        if (!mesher_region_meshed(m, s, r)) continue;
        str_catf(&b, "solid %s\n", s->reg[r].name);
        for (int t = 0; t < s->nTri; t++) {
            if (s->region[t] != r) continue;
            const float *v = s->tri + 9 * (size_t)t;
            if (mesher_cap_tri(m, v)) continue;
            double n[3], a; tri_normal(v, n, &a);
            str_catf(&b, " facet normal %g %g %g\n  outer loop\n", n[0], n[1], n[2]);
            for (int j = 0; j < 3; j++) {
                double p[3] = { v[3 * j], v[3 * j + 1], v[3 * j + 2] };
                if (n2 >= 0) p[n2] = c + (p[n2] - c) * k;
                str_catf(&b, "   vertex %.9g %.9g %.9g\n", p[0], p[1], p[2]);
            }
            str_cat(&b, "  endloop\n endfacet\n");
        }
        str_catf(&b, "endsolid %s\n", s->reg[r].name);
    }
    bool ok = pl_write_file(path, b.s ? b.s : "", b.len);
    str_free(&b);
    return ok;
}

static bool write_binary_region(const Stl *s, int r, const char *path)
{
    uint32_t n = (uint32_t)s->reg[r].nTri;
    size_t len = 84 + (size_t)n * 50;
    char *buf = calloc(1, len);
    snprintf(buf, 80, "Q-Continuum region %s", s->reg[r].name);
    memcpy(buf + 80, &n, 4);
    size_t o = 84;
    for (int t = 0; t < s->nTri; t++) {
        if (s->region[t] != r) continue;
        const float *v = s->tri + 9 * (size_t)t;
        double nd[3], a; tri_normal(v, nd, &a);
        float nf[3] = { (float)nd[0], (float)nd[1], (float)nd[2] };
        memcpy(buf + o, nf, 12); memcpy(buf + o + 12, v, 36);
        o += 50;
    }
    bool ok = pl_write_file(path, buf, len);
    free(buf);
    return ok;
}

#define OURS "// Written by Q-Continuum's STL mesher (Mesh > STL mesher): it rewrites this file on every Generate.\n"
#define HEADER(cls, obj) "FoamFile\n{\n    version     2.0;\n    format      ascii;\n    class       " cls ";\n    object      " obj ";\n}\n"

/* our dictionary over theirs: the first time, theirs is kept as <file>.orig */
static bool put_dict(const char *casePath, const char *rel, const char *text, char *err, int cap)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, rel);
    char *old = pl_read_head(p, 400);
    if (old && !strstr(old, "Q-Continuum's STL mesher")) {
        char bak[PL_PATH + 8]; snprintf(bak, sizeof bak, "%s.orig", p);
        if (!pl_exists(bak)) { char *all = pl_read_file(p, NULL); if (all) { pl_write_file(bak, all, strlen(all)); free(all); } }
    }
    free(old);
    char d[PL_PATH]; pl_dirname(d, sizeof d, p); pl_mkdirs(d);
    if (!pl_write_file(p, text, strlen(text))) { snprintf(err, (size_t)cap, "Could not write %s", rel); return false; }
    return true;
}
static void put_if_missing(const char *casePath, const char *rel, const char *text)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, rel);
    if (pl_exists(p)) return;
    char d[PL_PATH]; pl_dirname(d, sizeof d, p); pl_mkdirs(d);
    pl_write_file(p, text, strlen(text));
}

/* a point in the fluid, away from the surface: inside the part (internal), in the tunnel ahead of it (external) */
static void location_in_mesh(const MeshCfg *m, const Stl *s, double out[3])
{
    double lo[3], hi[3];
    mesher_domain(m, s, lo, hi);
    if (m->domain == DOMAIN_EXTERNAL) {
        /* ahead of the body, in the gap between the inlet and it (anything across the flow there is fluid) */
        int f = m->flowDir / 2, fs = dir_sign(m->flowDir), ud = mesher_up(m), u = ud / 2, us = dir_sign(ud);
        for (int k = 0; k < 3; k++) out[k] = (lo[k] + hi[k]) / 2 + m->base * 0.137 * (k + 1);
        double gap = fs > 0 ? s->bmin[f] - lo[f] : hi[f] - s->bmax[f];
        double in = gap < m->base ? m->base * 0.61 : gap * 0.5 + m->base * 0.113;
        out[f] = fs > 0 ? lo[f] + in : hi[f] - in;
        if (m->ground) {                                   /* near the floor, but a few cells off it */
            double d = fmin(m->base * 2.37, (hi[u] - lo[u]) * 0.4);
            out[u] = us > 0 ? lo[u] + d : hi[u] - d;
        }
        return;
    }
    /* the inside sample farthest from every triangle centre */
    double best = -1;
    out[0] = (s->bmin[0] + s->bmax[0]) / 2; out[1] = (s->bmin[1] + s->bmax[1]) / 2; out[2] = (s->bmin[2] + s->bmax[2]) / 2;
    int step = s->nTri > 20000 ? s->nTri / 20000 : 1;
    for (int i = 1; i < 8; i++) for (int j = 1; j < 8; j++) for (int k = 1; k < 8; k++) {
        double p[3] = { s->bmin[0] + (s->bmax[0] - s->bmin[0]) * (i + 0.013) / 8, s->bmin[1] + (s->bmax[1] - s->bmin[1]) * (j + 0.017) / 8,
                        s->bmin[2] + (s->bmax[2] - s->bmin[2]) * (k + 0.019) / 8 };
        if (!stl_inside(s, p)) continue;
        double dmin = 1e30;
        for (int t = 0; t < s->nTri; t += step) {
            const float *v = s->tri + 9 * (size_t)t;
            double c[3] = { (v[0] + v[3] + v[6]) / 3 - p[0], (v[1] + v[4] + v[7]) / 3 - p[1], (v[2] + v[5] + v[8]) / 3 - p[2] };
            double d = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
            if (d < dmin) dmin = d;
        }
        if (dmin > best) { best = dmin; out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; }
    }
}

static const char *foam_type(int t) { return t == BT_WALL ? "wall" : t == BT_SYMMETRY ? "symmetry" : "patch"; }

static bool write_snappy(const char *casePath, const MeshCfg *m, const Stl *s, char *err, int cap)
{
    double lo[3], hi[3];
    mesher_domain(m, s, lo, hi);
    int nc[3];
    int n2 = m->twoD ? m->normal2D : -1;                 /* 2-D: one cell across the slab, which keeps its thickness */
    int K = mesher_far_levels(m, s);                     /* external: the far field 2^K base sizes, refined to the base near the body */
    double bg = m->base * pow(2, K);
    for (int k = 0; k < 3; k++) nc[k] = k == n2 ? 1 : (int)fmax(1, ceil((hi[k] - lo[k]) / bg));
    for (int k = 0; k < 3; k++) if (k != n2) hi[k] = lo[k] + nc[k] * bg;   /* cubic cells: the box grows to whole cells */
    Str b; str_init(&b);
    /* ---- blockMeshDict: the background, cubes of the base size */
    str_cat(&b, HEADER("dictionary", "blockMeshDict") OURS "\nscale 1;\n\nvertices\n(\n");
    for (int i = 0; i < 8; i++) {
        double x = (i == 1 || i == 2 || i == 5 || i == 6) ? hi[0] : lo[0], y = (i == 2 || i == 3 || i == 6 || i == 7) ? hi[1] : lo[1], z = i >= 4 ? hi[2] : lo[2];
        str_catf(&b, "    (%.9g %.9g %.9g)\n", x, y, z);
    }
    str_catf(&b, ");\n\nblocks\n(\n    hex (0 1 2 3 4 5 6 7) (%d %d %d) simpleGrading (1 1 1)\n);\n\nboundary\n(\n", nc[0], nc[1], nc[2]);
    /* the box's faces, [axis][0 min, 1 max] */
    static const char *const FACE[3][2] = { { "(0 4 7 3)", "(2 6 5 1)" }, { "(1 5 4 0)", "(3 7 6 2)" }, { "(0 3 2 1)", "(4 5 6 7)" } };
    if (n2 >= 0)                                          /* 2-D: the slab's faces; extrudeMesh makes them front and back */
        str_catf(&b, "    back2D\n    {\n        type patch;\n        faces (%s);\n    }\n    front2D\n    {\n        type patch;\n        faces (%s);\n    }\n",
                 FACE[n2][0], FACE[n2][1]);
    if (m->domain == DOMAIN_INTERNAL) {
        str_cat(&b, "    background\n    {\n        type patch;\n        faces (");
        for (int k = 0; k < 3; k++) if (k != n2) str_catf(&b, " %s %s", FACE[k][0], FACE[k][1]);
        str_cat(&b, " );\n    }\n");
    } else {
        /* the inlet where the flow comes from, the outlet opposite, the ground below (when the body sits on it), the far
           field the rest */
        int f = m->flowDir / 2, fs = dir_sign(m->flowDir), ud = mesher_up(m), u = ud / 2, us = dir_sign(ud);
        int inF = fs > 0 ? 0 : 1, gF = us > 0 ? 0 : 1;
        str_catf(&b, "    inlet\n    {\n        type patch;\n        faces (%s);\n    }\n", FACE[f][inF]);
        str_catf(&b, "    outlet\n    {\n        type patch;\n        faces (%s);\n    }\n", FACE[f][1 - inF]);
        if (m->ground) str_catf(&b, "    ground\n    {\n        type wall;\n        faces (%s);\n    }\n", FACE[u][gF]);
        str_cat(&b, "    farfield\n    {\n        type patch;\n        faces (");
        for (int k = 0; k < 3; k++)
            for (int e = 0; e < 2; e++)
                if (k != f && k != n2 && !(m->ground && k == u && e == gF)) str_catf(&b, " %s", FACE[k][e]);
        str_cat(&b, " );\n    }\n");
    }
    str_cat(&b, ");\n");
    bool ok = put_dict(casePath, "system/blockMeshDict", b.s, err, cap);
    /* ---- surfaceFeatureExtractDict: edges sharper than the feature angle (2-D: not the open ends the caps leave) */
    b.len = 0; b.s[0] = 0;
    str_catf(&b, HEADER("dictionary", "surfaceFeatureExtractDict") OURS "\ngeometry.stl\n{\n    extractionMethod    extractFromSurface;\n"
                 "    includedAngle       %g;\n    subsetFeatures\n    {\n        nonManifoldEdges    yes;\n        openEdges           %s;\n    }\n"
                 "    writeObj            no;\n}\n", 180 - m->featureAngle, n2 >= 0 ? "no" : "yes");
    ok = ok && put_dict(casePath, "system/surfaceFeatureExtractDict", b.s, err, cap);
    /* ---- meshQualityDict */
    b.len = 0; b.s[0] = 0;
    str_catf(&b, HEADER("dictionary", "meshQualityDict") OURS "\n#includeEtc \"caseDicts/meshQualityDict\"\n\nmaxNonOrtho %g;\nmaxInternalSkewness %g;\nmaxBoundarySkewness %g;\n",
             m->maxNonOrtho, m->maxSkew, m->maxSkew * 5);
    ok = ok && put_dict(casePath, "system/meshQualityDict", b.s, err, cap);
    /* ---- snappyHexMeshDict */
    int L0 = level_of(m->targetPct) + K, L1 = level_of(m->minPct) + K;   /* levels of the background, whose cells are 2^K bases */
    if (L1 < L0) L1 = L0;
    double loc[3]; location_in_mesh(m, s, loc);
    if (n2 >= 0) loc[n2] = (lo[n2] + hi[n2]) / 2 + (hi[n2] - lo[n2]) * 0.0137;   /* inside the slab */
    b.len = 0; b.s[0] = 0;
    str_catf(&b, HEADER("dictionary", "snappyHexMeshDict") OURS
                 "// base %g m (background %g m, refined to the base round the body); target surface size %g %% (level %d), minimum %g %%\n"
                 "// (level %d); curvature %g points a circle\n\n"
                 "castellatedMesh true;\nsnap            true;\naddLayers       %s;\n\n"
                 "// one surface a boundary (constant/triSurface/regions), so each patch is named exactly as the boundary\ngeometry\n{\n",
             m->base, bg, m->targetPct, L0, m->minPct, L1, m->curvature, m->prism && m->layers > 0 ? "true" : "false");
    for (int r = 0; r < s->nRegions; r++)
        if (mesher_region_meshed(m, s, r)) str_catf(&b, "    %s\n    {\n        type triSurfaceMesh;\n        file \"regions/%s.stl\";\n    }\n", s->reg[r].name, s->reg[r].name);
    if (K > 0) {
        double nlo[3], nhi[3]; mesher_near_box(m, s, nlo, nhi);
        str_catf(&b, "    near\n    {\n        type searchableBox;     // the body and its wake: base-sized cells\n"
                     "        min (%.9g %.9g %.9g);\n        max (%.9g %.9g %.9g);\n    }\n", nlo[0], nlo[1], nlo[2], nhi[0], nhi[1], nhi[2]);
    }
    for (int i = 0; i < m->nZones; i++) {
        const MeshZone *z = &m->zone[i];
        if (z->kind == ZONE_BOX)
            str_catf(&b, "    zone%d\n    {\n        type searchableBox;\n        min (%.9g %.9g %.9g);\n        max (%.9g %.9g %.9g);\n    }\n", i,
                     fmin(z->a[0], z->b[0]), fmin(z->a[1], z->b[1]), fmin(z->a[2], z->b[2]), fmax(z->a[0], z->b[0]), fmax(z->a[1], z->b[1]), fmax(z->a[2], z->b[2]));
        else if (z->kind == ZONE_SPHERE)
            str_catf(&b, "    zone%d\n    {\n        type searchableSphere;\n        centre (%.9g %.9g %.9g);\n        radius %.9g;\n    }\n", i, z->a[0], z->a[1], z->a[2], z->r);
        else
            str_catf(&b, "    zone%d\n    {\n        type searchableCylinder;\n        point1 (%.9g %.9g %.9g);\n        point2 (%.9g %.9g %.9g);\n        radius %.9g;\n    }\n",
                     i, z->a[0], z->a[1], z->a[2], z->b[0], z->b[1], z->b[2], z->r);
    }
    str_catf(&b, "}\n\ncastellatedMeshControls\n{\n    maxLocalCells       2000000;\n    maxGlobalCells      100000000;\n    minRefinementCells  10;\n"
                 "    maxLoadUnbalance    0.10;\n    nCellsBetweenLevels %d;     // volume growth %g\n\n"
                 "    features\n    (\n        { file \"geometry.eMesh\"; level %d; }\n    );\n\n"
                 "    refinementSurfaces\n    {\n",
             between_levels(m->volGrowth), m->volGrowth, L1);
    for (int r = 0; r < s->nRegions; r++)
        if (mesher_region_meshed(m, s, r)) {
            /* under prism layers one size (even): snappy's layers collapse where the surface cells halve (at every facet
               corner of a coarse STL, with the curvature refinement), as they did on a 45-point airfoil */
            bool even = mesher_even_layers(m) && s->reg[r].type == BT_WALL;
            str_catf(&b, "        %s { level (%d %d); patchInfo { type %s; } }%s\n", s->reg[r].name, even ? L1 : L0, L1, foam_type(s->reg[r].type),
                     even ? "     // even under the layers" : "");
        }
    str_catf(&b, "    }\n\n    resolveFeatureAngle %g;     // %g points a circle\n\n    refinementRegions\n    {\n",
             360.0 / fmax(3, m->curvature), m->curvature);
    for (int i = 0; i < m->nZones; i++)
        str_catf(&b, "        zone%d { mode inside; levels ((1E15 %d)); }\n", i, level_of(m->zone[i].sizePct) + K);
    if (K > 0) str_catf(&b, "        near { mode inside; levels ((1E15 %d)); }\n", K);
    str_catf(&b, "    }\n\n    locationInMesh (%.9g %.9g %.9g);\n    allowFreeStandingZoneFaces true;\n}\n\n", loc[0], loc[1], loc[2]);
    str_cat(&b, "snapControls\n{\n    nSmoothPatch 3;\n    tolerance 2.0;\n    nSolveIter 50;\n    nRelaxIter 5;\n    nFeatureSnapIter 10;\n"
                "    implicitFeatureSnap false;\n    explicitFeatureSnap true;\n    multiRegionFeatureSnap false;\n}\n\n");
    double outer, rel = mesher_layer_rel(m, &outer);
    str_catf(&b, "addLayersControls\n{\n    // asked: total thickness %g %% of base = %g m. Sizes here are relative to the surface cell (%g m at the target\n"
                 "    // size): the outer layer %.3g of it (held between %g and %g, or the cells next to it fail the quality checks),\n"
                 "    // so %.3g in all. Where the surface is refined further (curvature, minimum size) the layers thin with its cells.\n"
                 "    relativeSizes true;\n    layers\n    {\n", m->prismPct, m->base * m->prismPct / 100, bg / pow(2, mesher_even_layers(m) ? L1 : L0),
             outer, LAYER_OUTER_MIN, LAYER_OUTER_MAX, rel);
    for (int r = 0; r < s->nRegions; r++)
        if (s->reg[r].type == BT_WALL && mesher_region_meshed(m, s, r)) str_catf(&b, "        %s { nSurfaceLayers %d; }\n", s->reg[r].name, m->layers);
    if (m->domain == DOMAIN_EXTERNAL && m->ground) str_catf(&b, "        ground { nSurfaceLayers %d; }\n", m->layers);
    str_catf(&b, "    }\n    expansionRatio %g;\n    finalLayerThickness %.4g;\n    minThickness %.4g;\n    nGrow 0;\n    featureAngle 130;\n    slipFeatureAngle 30;\n"
                 "    nRelaxIter 3;\n    nSmoothSurfaceNormals 1;\n    nSmoothNormals 3;\n    nSmoothThickness 10;\n    maxFaceThicknessRatio 0.5;\n"
                 "    maxThicknessToMedialRatio 0.3;\n    minMedialAxisAngle 90;\n    nBufferCellsNoExtrude 0;\n"
                 /* 50 iterations at ~40 s each on 300 k cells: the flange's corners kept a few faces failing to the end,
                    for 85 % of the layers in 195 s; 15 (relaxed after 5) gave 86 % in 107 s */
                 "    nLayerIter 15;\n    nRelaxedIter 5;\n}\n\n"
                 "meshQualityControls\n{\n    #include \"meshQualityDict\"\n    nSmoothScale 4;\n    errorReduction 0.75;\n    relaxed\n    {\n        maxNonOrtho 75;\n    }\n}\n\nmergeTolerance 1e-6;\n",
             m->stretch, outer, rel * 0.1);
    ok = ok && put_dict(casePath, "system/snappyHexMeshDict", b.s, err, cap);
    /* ---- 2-D: extrudeMesh makes one cell of the slab's front face (the meshed 3-D slab is copied to .qc-slab first) */
    if (n2 >= 0 && ok) {
        b.len = 0; b.s[0] = 0;
        double depth = s->bmax[n2] - s->bmin[n2];
        if (!(depth > 0)) depth = hi[n2] - lo[n2];
        str_catf(&b, HEADER("dictionary", "extrudeMeshDict") OURS "\n// 2-D: the 3-D slab snappyHexMesh made (copied to .qc-slab) gives its front face; one\n"
                     "// cell of it, %g m thick (the part's depth), is the mesh. front2D and back2D are then made empty.\n"
                     "constructFrom patch;\nsourceCase \"$FOAM_CASE/.qc-slab\";\nsourcePatches (front2D);\nexposedPatchName back2D;\nflipNormals false;\n"
                     "extrudeModel linearNormal;\nnLayers 1;\nexpansionRatio 1.0;\nlinearNormalCoeffs\n{\n    thickness %.9g;\n}\nmergeFaces false;\nmergeTol 0;\n",
                 depth, depth);
        ok = put_dict(casePath, "system/extrudeMeshDict", b.s, err, cap);
    }
    str_free(&b);
    return ok;
}

void mesher_2d_script(const MeshCfg *m, const Stl *s, Str *out)
{
    int n = m->normal2D % 3;
    double lo, hi;
    mesher_slab(m, s, &lo, &hi);
    double d[3] = { 0, 0, 0 };
    d[n] = s->bmin[n] - hi;                               /* extruded out of the slab's front face: moved back onto the part */
    str_catf(out, "rm -rf .qc-slab && mkdir .qc-slab && cp -r system constant .qc-slab/ && rm -rf constant/polyMesh && extrudeMesh || exit 1\n"
                  "if [ -d 0/polyMesh ]; then rm -rf constant/polyMesh && mv 0/polyMesh constant/polyMesh; fi\n"
                  "transformPoints -translate '(%.9g %.9g %.9g)' > /dev/null || exit 1\n"
                  "echo 'the 2-D mesh moved onto the part (%c from %.6g to %.6g m)'\n"
                  "for p in front2D back2D; do\n"
                  "  foamDictionary constant/polyMesh/boundary -entry entry0/$p/type -set empty > /dev/null && echo \"patch $p: empty\"\n"
                  "  foamDictionary constant/polyMesh/boundary -entry entry0/$p/inGroups -remove > /dev/null 2>&1\n"
                  "done\nrm -rf .qc-slab\n", d[0], d[1], d[2], "xyz"[n], s->bmin[n], s->bmax[n]);
}

void mesher_type_script(const MeshCfg *m, const Stl *s, Str *out)
{
    for (int r = 0; r < s->nRegions; r++)
        if (mesher_region_meshed(m, s, r)) str_catf(out, "foamDictionary constant/polyMesh/boundary -entry entry0/%s/type -set %s > /dev/null 2>&1 && echo 'patch %s: %s'\n",
                 s->reg[r].name, foam_type(s->reg[r].type), s->reg[r].name, foam_type(s->reg[r].type));
    if (m->domain == DOMAIN_EXTERNAL && m->ground)
        str_cat(out, "foamDictionary constant/polyMesh/boundary -entry entry0/ground/type -set wall > /dev/null 2>&1 && echo 'patch ground: wall'\n");
    if (m->twoD)                                          /* SALOME's 2-D mesh: its front and back are the empty ones */
        str_cat(out, "for p in front2D back2D; do foamDictionary constant/polyMesh/boundary -entry entry0/$p/type -set empty > /dev/null 2>&1 "
                     "&& foamDictionary constant/polyMesh/boundary -entry entry0/$p/inGroups -remove > /dev/null 2>&1; echo \"patch $p: empty\"; done\n");
}

bool mesher_prepare(const char *casePath, const MeshCfg *m, Stl *s, char *err, int cap)
{
    if (!s->nTri) { snprintf(err, (size_t)cap, "No STL loaded"); return false; }
    if (m->base <= 0) { snprintf(err, (size_t)cap, "The base size must be above zero"); return false; }
    char p[PL_PATH], d[PL_PATH];
    pl_join(d, sizeof d, casePath, "constant/triSurface"); pl_mkdirs(d);
    pl_join(p, sizeof p, d, "geometry.stl");
    if (!write_ascii_stl(s, m, p)) { snprintf(err, (size_t)cap, "Could not write constant/triSurface/geometry.stl"); return false; }
    char td[PL_PATH]; pl_join(td, sizeof td, casePath, "constant/triSurface/regions");
    pl_remove_tree(td); pl_mkdirs(td);
    for (int r = 0; r < s->nRegions; r++) {
        if (!mesher_region_meshed(m, s, r)) continue;
        Stl one = *s;                                      /* the same triangles, only this region written */
        one.nRegions = 1; one.reg[0] = s->reg[r];
        int *keep = one.region;
        one.region = malloc(sizeof(int) * (size_t)s->nTri);
        for (int t = 0; t < s->nTri; t++) one.region[t] = keep[t] == r ? 0 : -1;
        char f[96]; snprintf(f, sizeof f, "%s.stl", s->reg[r].name);
        pl_join(p, sizeof p, td, f);
        bool ok = write_ascii_stl(&one, m, p);
        free(one.region);
        if (!ok) { snprintf(err, (size_t)cap, "Could not write constant/triSurface/regions/%s", f); return false; }
    }
    pl_join(d, sizeof d, casePath, "mesher/regions");
    pl_remove_tree(d); pl_mkdirs(d);
    for (int r = 0; r < s->nRegions; r++) {
        char f[96]; snprintf(f, sizeof f, "%s.stl", s->reg[r].name);
        pl_join(p, sizeof p, d, f);
        if (!write_binary_region(s, r, p)) { snprintf(err, (size_t)cap, "Could not write mesher/regions/%s", f); return false; }
    }
    MeshCfg mm = *m;
    mesher_take_names(&mm, s);
    if (!mesher_save(casePath, &mm)) { snprintf(err, (size_t)cap, "Could not write mesher/mesher.ini"); return false; }
    {   /* what this mesh is made from: Mesh is out of date once that changes (mesher_mesh_current) */
        Str g; str_init(&g); cfg_text(&mm, &g, true);
        char sp[PL_PATH]; sig_path(casePath, sp, sizeof sp);
        pl_write_file(sp, g.s, g.len);
        str_free(&g);
    }
    /* SALOME's script: ours, rewritten each time (the user's own salome/mesh.py is never touched) */
    pl_join(d, sizeof d, casePath, "salome"); pl_mkdirs(d);
    pl_join(p, sizeof p, d, "stl_mesh.py");
    if (!pl_write_file(p, (const char *)asset_stl_mesher_py, asset_stl_mesher_py_len)) { snprintf(err, (size_t)cap, "Could not write salome/stl_mesh.py"); return false; }
    /* the least a case needs for the mesh tools */
    put_if_missing(casePath, "system/controlDict", HEADER("dictionary", "controlDict")
        "\napplication     simpleFoam;\nstartFrom       latestTime;\nstartTime       0;\nstopAt          endTime;\nendTime         1000;\ndeltaT          1;\n"
        "writeControl    timeStep;\nwriteInterval   100;\npurgeWrite      0;\nwriteFormat     binary;\nwritePrecision  8;\nwriteCompression off;\n"
        "timeFormat      general;\ntimePrecision   6;\nrunTimeModifiable true;\n");
    put_if_missing(casePath, "system/fvSchemes", HEADER("dictionary", "fvSchemes")
        "\nddtSchemes { default steadyState; }\ngradSchemes { default Gauss linear; }\ndivSchemes { default none; }\n"
        "laplacianSchemes { default Gauss linear corrected; }\ninterpolationSchemes { default linear; }\nsnGradSchemes { default corrected; }\n");
    put_if_missing(casePath, "system/fvSolution", HEADER("dictionary", "fvSolution") "\nsolvers {}\nSIMPLE {}\n");
    if (m->mesher == MESHER_SNAPPY) return write_snappy(casePath, m, s, err, cap);
    return true;
}

/* ============================================================================ 2-D: the profile, and a smooth one made of it
   A profile from CAD comes as few straight segments (the user's NACA airfoil: 45 points round a 1 m chord). snappyHexMesh
   refines at each of their corners and the prism layers broke at every one; a spline through the points, resampled finer
   (closer where it bends), keeps the shape the points describe without the corners. Sharp corners (a trailing edge) stay. */
#define PROF_MAX 20000

typedef struct { double p[2]; int nb[2], n; } PNode;

/* the side walls' edges on the extrusion's low end, chained into one loop of points in the plane ((n+1)%3, (n+2)%3) */
static int profile_loop(const Stl *s, int n, double (*out)[2], int max, int *regOf)
{
    int a1 = (n + 1) % 3, a2 = (n + 2) % 3;
    double lo = s->bmin[n], hi = s->bmax[n], tol = fmax(1e-9, (hi - lo) * 1e-4);
    PNode *P = malloc(sizeof(PNode) * PROF_MAX); int np = 0, *segReg = NULL;
    if (!P) return 0;
    (void)segReg;
    for (int t = 0; t < s->nTri; t++) {
        const float *v = s->tri + 9 * (size_t)t;
        int on[3], k = 0; bool top = false;
        for (int i = 0; i < 3; i++) { if (fabs(v[3 * i + n] - lo) < tol) on[k++] = i; else if (fabs(v[3 * i + n] - hi) < tol) top = true; }
        if (k != 2 || !top) continue;                     /* a side triangle with an edge on the low end */
        int id[2];
        for (int e = 0; e < 2; e++) {
            double q[2] = { v[3 * on[e] + a1], v[3 * on[e] + a2] };
            int f = -1;
            for (int j = np - 1; j >= 0 && f < 0; j--) if (fabs(P[j].p[0] - q[0]) < tol && fabs(P[j].p[1] - q[1]) < tol) f = j;
            if (f < 0) {
                if (np >= PROF_MAX) { free(P); return 0; }
                f = np++; P[f].p[0] = q[0]; P[f].p[1] = q[1]; P[f].n = 0;
            }
            id[e] = f;
        }
        for (int e = 0; e < 2; e++) {
            PNode *a = &P[id[e]]; int b = id[1 - e];
            if (a->n >= 2) { free(P); return 0; }         /* more than one loop meets here: not a plain profile */
            a->nb[a->n++] = b;
        }
        if (regOf) regOf[id[0] < id[1] ? id[0] : id[1]] = s->region[t];   /* refined below: the segment's region */
    }
    if (np < 4 || np > max) { free(P); return 0; }
    for (int j = 0; j < np; j++) if (P[j].n != 2) { free(P); return 0; }
    int cur = 0, prev = -1, k = 0;
    do {
        out[k][0] = P[cur].p[0]; out[k][1] = P[cur].p[1]; k++;
        int nx = P[cur].nb[0] == prev ? P[cur].nb[1] : P[cur].nb[0];
        prev = cur; cur = nx;
    } while (cur != 0 && k < np);
    free(P);
    return k == np ? k : 0;                                /* one loop through every point, or nothing */
}

static double turn_deg(const double (*p)[2], int n, int i)
{
    const double *a = p[(i + n - 1) % n], *b = p[i], *c = p[(i + 1) % n];
    double u0 = b[0] - a[0], u1 = b[1] - a[1], v0 = c[0] - b[0], v1 = c[1] - b[1];
    return fabs(atan2(u0 * v1 - u1 * v0, u0 * v0 + u1 * v1)) * 180 / M_PI;
}

bool stl_profile_facets(const Stl *s, int n, double cornerDeg, int *nPts, double *maxKink)
{
    *nPts = 0; *maxKink = 0;
    double (*p)[2] = malloc(sizeof(double[2]) * PROF_MAX);
    if (!p) return false;
    int np = profile_loop(s, n % 3, p, PROF_MAX, NULL);
    for (int i = 0; i < np; i++) { double t = turn_deg((const double (*)[2])p, np, i); if (t < cornerDeg && t > *maxKink) *maxKink = t; }
    free(p);
    *nPts = np;
    return np > 0;
}

/* centripetal Catmull-Rom between b and c (a, d the points either side), at u in [0, 1] */
static void catmull(const double *a, const double *b, const double *c, const double *d, double u, double *o)
{
    double t0 = 0, t1 = t0 + pow(hypot(b[0] - a[0], b[1] - a[1]), 0.5) + 1e-12, t2 = t1 + pow(hypot(c[0] - b[0], c[1] - b[1]), 0.5) + 1e-12,
           t3 = t2 + pow(hypot(d[0] - c[0], d[1] - c[1]), 0.5) + 1e-12, t = t1 + (t2 - t1) * u;
    for (int k = 0; k < 2; k++) {
        double A1 = (t1 - t) / (t1 - t0) * a[k] + (t - t0) / (t1 - t0) * b[k], A2 = (t2 - t) / (t2 - t1) * b[k] + (t - t1) / (t2 - t1) * c[k],
               A3 = (t3 - t) / (t3 - t2) * c[k] + (t - t2) / (t3 - t2) * d[k];
        double B1 = (t2 - t) / (t2 - t0) * A1 + (t - t0) / (t2 - t0) * A2, B2 = (t3 - t) / (t3 - t1) * A2 + (t - t1) / (t3 - t1) * A3;
        o[k] = (t2 - t) / (t2 - t1) * B1 + (t - t1) / (t2 - t1) * B2;
    }
}

static void put_tri(FILE *f, const double a[3], const double b[3], const double c[3], long *n)
{
    double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    double nr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    double l = sqrt(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]);
    float r[12] = { (float)(l > 0 ? nr[0] / l : 0), (float)(l > 0 ? nr[1] / l : 0), (float)(l > 0 ? nr[2] / l : 0),
                    (float)a[0], (float)a[1], (float)a[2], (float)b[0], (float)b[1], (float)b[2], (float)c[0], (float)c[1], (float)c[2] };
    unsigned short z = 0;
    fwrite(r, sizeof r, 1, f); fwrite(&z, 2, 1, f);
    (*n)++;
}

/* ear clipping of a counter-clockwise polygon: tri[] gets 3 indices a triangle; returns the count */
static int ear_clip(const double (*p)[2], int n, int *tri)
{
    if (n < 3) return 0;
    int *nx = malloc(sizeof(int) * (size_t)(unsigned)n), *pv = malloc(sizeof(int) * (size_t)(unsigned)n), nt = 0;
    if (!nx || !pv) { free(nx); free(pv); return 0; }
    for (int i = 0; i < n; i++) { nx[i] = (i + 1) % n; pv[i] = (i + n - 1) % n; }
    int left = n, i = 0, guard = 0;
    while (left > 3 && guard < 4 * n * n) {
        guard++;
        int a = pv[i], c = nx[i];
        const double *A = p[a], *B = p[i], *C = p[c];
        bool ear = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]) > 0;
        for (int j = nx[c]; ear && j != a; j = nx[j]) {
            const double *Q = p[j];
            double d1 = (B[0] - A[0]) * (Q[1] - A[1]) - (B[1] - A[1]) * (Q[0] - A[0]);
            double d2 = (C[0] - B[0]) * (Q[1] - B[1]) - (C[1] - B[1]) * (Q[0] - B[0]);
            double d3 = (A[0] - C[0]) * (Q[1] - C[1]) - (A[1] - C[1]) * (Q[0] - C[0]);
            if (d1 >= 0 && d2 >= 0 && d3 >= 0) ear = false;
        }
        if (ear) { tri[3 * nt] = a; tri[3 * nt + 1] = i; tri[3 * nt + 2] = c; nt++; nx[a] = c; pv[c] = a; left--; i = c; }
        else i = nx[i];
    }
    if (left == 3) { tri[3 * nt] = pv[i]; tri[3 * nt + 1] = i; tri[3 * nt + 2] = nx[i]; nt++; }
    free(nx); free(pv);
    return nt;
}

int stl_smooth_profile(const Stl *s, int n, double cornerDeg, double segLen, const char *outPath, char *err, int cap)
{
    n %= 3;
    int a1 = (n + 1) % 3, a2 = (n + 2) % 3;
    double (*p)[2] = malloc(sizeof(double[2]) * PROF_MAX);
    if (!p) { snprintf(err, (size_t)cap, "Out of memory"); return 0; }
    int np = profile_loop(s, n, p, PROF_MAX, NULL);
    if (np < 4 || np > PROF_MAX) { free(p); snprintf(err, (size_t)cap, "The STL isn't one profile extruded straight (its side walls don't make a single outline)"); return 0; }
    double area = 0;
    for (int i = 0; i < np; i++) area += p[i][0] * p[(i + 1) % np][1] - p[(i + 1) % np][0] * p[i][1];
    if (area < 0) for (int i = 0; i < np / 2; i++) { double t0 = p[i][0], t1 = p[i][1]; p[i][0] = p[np - 1 - i][0]; p[i][1] = p[np - 1 - i][1]; p[np - 1 - i][0] = t0; p[np - 1 - i][1] = t1; }
    /* the corners kept; the loop starts at one (if there are any) */
    int first = -1;
    bool *corner = calloc((size_t)np, sizeof(bool));
    for (int i = 0; i < np; i++) if (turn_deg((const double (*)[2])p, np, i) >= cornerDeg) { corner[i] = true; if (first < 0) first = i; }
    /* dense samples along the spline, 32 a segment; a corner's neighbours are mirrored so the curve meets it straight */
    int SUB = 32, nd = np * SUB;
    size_t ndz = (size_t)(unsigned)nd + 1;
    double (*d)[2] = malloc(sizeof(double[2]) * ndz), *w = malloc(sizeof(double) * ndz);
    bool *dc = calloc(ndz, sizeof(bool));
    if (!d || !w || !dc || !corner) { free(d); free(w); free(dc); free(corner); free(p); snprintf(err, (size_t)cap, "Out of memory"); return 0; }
    int st = first < 0 ? 0 : first;
    for (int k = 0; k < np; k++) {
        int i = (st + k) % np, j = (i + 1) % np;
        double A[2], D[2];
        const double *B = p[i], *C = p[j];
        if (corner[i]) { A[0] = 2 * B[0] - C[0]; A[1] = 2 * B[1] - C[1]; } else { A[0] = p[(i + np - 1) % np][0]; A[1] = p[(i + np - 1) % np][1]; }
        if (corner[j]) { D[0] = 2 * C[0] - B[0]; D[1] = 2 * C[1] - B[1]; } else { D[0] = p[(j + 1) % np][0]; D[1] = p[(j + 1) % np][1]; }
        for (int u = 0; u < SUB; u++) catmull(A, B, C, D, (double)u / SUB, d[k * SUB + u]);
        dc[k * SUB] = corner[i];
    }
    d[nd][0] = d[0][0]; d[nd][1] = d[0][1]; dc[nd] = dc[0];
    /* points closer where it bends: half by length, half by turning */
    double L = 0, Kt = 0;
    for (int q = 0; q < nd; q++) L += hypot(d[q + 1][0] - d[q][0], d[q + 1][1] - d[q][1]);
    for (int q = 0; q <= nd; q++) {
        double t = dc[q] ? 0 : turn_deg((const double (*)[2])d, nd, q % nd) * M_PI / 180;
        w[q] = t; Kt += t;
    }
    int N = (int)fmin(4000, fmax(400, L / fmax(segLen, 1e-12)));
    double *cw = malloc(sizeof(double) * ndz);
    double (*o)[2] = malloc(sizeof(double[2]) * ((size_t)(unsigned)N + (size_t)(unsigned)np + 2));
    if (!cw || !o) { free(cw); free(o); free(d); free(w); free(dc); free(corner); free(p); snprintf(err, (size_t)cap, "Out of memory"); return 0; }
    cw[0] = 0;
    for (int q = 0; q < nd; q++) {
        double len = hypot(d[q + 1][0] - d[q][0], d[q + 1][1] - d[q][1]);
        cw[q + 1] = cw[q] + 0.5 * len / L + 0.5 * (Kt > 0 ? (w[q] + w[q + 1]) / 2 / Kt : len / L);
    }
    int no = 0, q = 0;
    for (int k = 0; k < N; k++) {                          /* N points by weight, every corner on the way kept exactly */
        double target = cw[nd] * k / N;
        while (q < nd && cw[q + 1] < target) {
            q++;
            if (dc[q] && q < nd) { o[no][0] = d[q][0]; o[no][1] = d[q][1]; no++; }
        }
        if (k == 0 && dc[0]) { o[no][0] = d[0][0]; o[no][1] = d[0][1]; no++; continue; }
        double f = cw[q + 1] > cw[q] ? (target - cw[q]) / (cw[q + 1] - cw[q]) : 0;
        double x = d[q][0] + (d[q + 1][0] - d[q][0]) * f, y = d[q][1] + (d[q + 1][1] - d[q][1]) * f;
        if (no && hypot(x - o[no - 1][0], y - o[no - 1][1]) < L * 1e-6) continue;
        o[no][0] = x; o[no][1] = y; no++;
    }
    free(d); free(w); free(dc); free(cw); free(corner);
    /* each new side segment takes the region of the original side facet nearest its middle; caps the regions of the old caps */
    double lo = s->bmin[n], hi = s->bmax[n], tol = fmax(1e-9, (hi - lo) * 1e-4);
    int capLo = -1, capHi = -1;
    for (int t = 0; t < s->nTri && (capLo < 0 || capHi < 0); t++) {
        const float *v = s->tri + 9 * (size_t)t; int nl = 0, nh = 0;
        for (int i = 0; i < 3; i++) { if (fabs(v[3 * i + n] - lo) < tol) nl++; if (fabs(v[3 * i + n] - hi) < tol) nh++; }
        if (nl == 3 && capLo < 0) capLo = s->region[t];
        if (nh == 3 && capHi < 0) capHi = s->region[t];
    }
    if (no < 4) { free(o); free(p); snprintf(err, (size_t)cap, "The smoothed outline came out empty"); return 0; }
    int *segReg = malloc(sizeof(int) * (size_t)(unsigned)no), *ct = malloc(sizeof(int) * 3 * (size_t)(unsigned)no);
    if (!segReg || !ct) { free(segReg); free(ct); free(o); free(p); snprintf(err, (size_t)cap, "Out of memory"); return 0; }
    for (int k = 0; k < no; k++) {
        double mx = (o[k][0] + o[(k + 1) % no][0]) / 2, my = (o[k][1] + o[(k + 1) % no][1]) / 2, best = 1e300; int br = 0;
        for (int t = 0; t < s->nTri; t++) {
            const float *v = s->tri + 9 * (size_t)t; int nl = 0, nh = 0;
            for (int i = 0; i < 3; i++) { if (fabs(v[3 * i + n] - lo) < tol) nl++; if (fabs(v[3 * i + n] - hi) < tol) nh++; }
            if (nl == 3 || nh == 3) continue;
            double cx = (v[a1] + v[3 + a1] + v[6 + a1]) / 3, cy = (v[a2] + v[3 + a2] + v[6 + a2]) / 3, dd = (cx - mx) * (cx - mx) + (cy - my) * (cy - my);
            if (dd < best) { best = dd; br = s->region[t]; }
        }
        segReg[k] = br;
    }
    int nct = ear_clip((const double (*)[2])o, no, ct);
    /* written in the STL's own units (the scale is applied on loading), the regions in their old order so their names stay */
    FILE *f = fopen(outPath, "wb");
    if (!f) { snprintf(err, (size_t)cap, "Could not write %s", outPath); free(o); free(segReg); free(ct); free(p); return 0; }
    char hdr[80] = { 0 }; snprintf(hdr, sizeof hdr, "Q-Continuum: a smooth profile, %d points", no);
    fwrite(hdr, 80, 1, f);
    unsigned int cnt = 0; fwrite(&cnt, 4, 1, f);
    double sc = s->scale > 0 ? s->scale : 1;
    long nw = 0;
    #define P3(xy, z, out) do { out[n] = (z) / sc; out[a1] = (xy)[0] / sc; out[a2] = (xy)[1] / sc; } while (0)
    for (int r = 0; r < s->nRegions; r++) {
        if (r == capLo || r == capHi) {
            double z = r == capLo ? lo : hi;
            for (int k = 0; k < nct; k++) {
                double A[3], B[3], C[3];
                P3(o[ct[3 * k]], z, A); P3(o[ct[3 * k + 1]], z, B); P3(o[ct[3 * k + 2]], z, C);
                if (r == capLo) put_tri(f, A, C, B, &nw); else put_tri(f, A, B, C, &nw);   /* outward: -n at the low end */
            }
            if (capLo == capHi) {                          /* both caps one region */
                for (int k = 0; k < nct; k++) {
                    double A[3], B[3], C[3];
                    P3(o[ct[3 * k]], hi, A); P3(o[ct[3 * k + 1]], hi, B); P3(o[ct[3 * k + 2]], hi, C);
                    put_tri(f, A, B, C, &nw);
                }
            }
            continue;
        }
        for (int k = 0; k < no; k++) {
            if (segReg[k] != r) continue;
            double A[3], B[3], C[3], D[3];
            P3(o[k], lo, A); P3(o[(k + 1) % no], lo, B); P3(o[(k + 1) % no], hi, C); P3(o[k], hi, D);
            put_tri(f, A, B, C, &nw); put_tri(f, A, C, D, &nw);
        }
    }
    #undef P3
    cnt = (unsigned int)nw;
    fseek(f, 80, SEEK_SET); fwrite(&cnt, 4, 1, f);
    bool ok = fclose(f) == 0;
    free(o); free(segReg); free(ct); free(p);
    if (!ok) { snprintf(err, (size_t)cap, "Could not write %s", outPath); return 0; }
    return no;
}
