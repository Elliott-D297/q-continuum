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

/* ============================================================================ settings */
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
    m->up = 3; m->down = 8; m->side = 3; m->top = 3; m->ground = false;
    m->mesher = MESHER_SNAPPY;
    double L = s ? body_length(s) : 1;
    double b = L / (m->domain == DOMAIN_INTERNAL ? 40 : 15);
    double e = pow(10, floor(log10(b)));                  /* a round number: 1, 2 or 5 x 10^n */
    m->base = b / e >= 5 ? 5 * e : b / e >= 2 ? 2 * e : e;
    m->targetPct = m->domain == DOMAIN_INTERNAL ? 100 : 25;
    m->minPct = m->domain == DOMAIN_INTERNAL ? 25 : 6.25;
    m->curvature = 36; m->surfGrowth = 1.3; m->volGrowth = 1.2; m->featureAngle = 30;
    m->prism = true; m->layers = 3; m->prismPct = m->domain == DOMAIN_INTERNAL ? 33.3 : 10; m->stretch = 1.2;
    m->maxNonOrtho = 65; m->maxSkew = 4;
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
    m->scale = ini_num(ini, "scale", 1);
    m->splitAngle = ini_num(ini, "split_angle", 30);
    char d[32] = ""; ini_str(ini, "domain", d, sizeof d);
    m->domain = !strcmp(d, "external") ? DOMAIN_EXTERNAL : DOMAIN_INTERNAL;
    m->up = ini_num(ini, "upstream", 3); m->down = ini_num(ini, "downstream", 8);
    m->side = ini_num(ini, "sides", 3); m->top = ini_num(ini, "top", 3);
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

bool mesher_save(const char *casePath, const MeshCfg *m)
{
    Str b; str_init(&b);
    str_cat(&b, "# Q-Continuum STL mesher settings (Mesh > STL mesher). SALOME's script salome/stl_mesh.py reads them too.\n"
                "# Sizes in metres; *_pct are percentages of base_size, as in STAR-CCM+'s reference values.\n[mesher]\n");
    str_catf(&b, "stl = %s\nscale = %.10g\nsplit_angle = %g\n", m->stl, m->scale, m->splitAngle);
    str_catf(&b, "domain = %s\nupstream = %g\ndownstream = %g\nsides = %g\ntop = %g\nground = %d\n",
             m->domain == DOMAIN_EXTERNAL ? "external" : "internal", m->up, m->down, m->side, m->top, m->ground);
    str_catf(&b, "mesher = %s\nremesh_surface = %d\n", MESHER_WORD[m->mesher], m->remesh);
    str_catf(&b, "base_size = %.6g\ntarget_size_pct = %g\nmin_size_pct = %g\ncurvature_points = %g\nsurface_growth = %g\nvolume_growth = %g\nfeature_angle = %g\n",
             m->base, m->targetPct, m->minPct, m->curvature, m->surfGrowth, m->volGrowth, m->featureAngle);
    str_catf(&b, "prism_layers_on = %d\nprism_layers = %d\nprism_thickness_pct = %g\nprism_stretching = %g\n", m->prism, m->layers, m->prismPct, m->stretch);
    str_catf(&b, "max_non_ortho = %g\nmax_skewness = %g\n", m->maxNonOrtho, m->maxSkew);
    str_catf(&b, "# zoneN = kind  a(x y z)  b(x y z)  radius  size_pct   (box: a = min, b = max; sphere: a = centre; cylinder: a, b = ends)\nzones = %d\n", m->nZones);
    for (int i = 0; i < m->nZones; i++) {
        const MeshZone *z = &m->zone[i];
        str_catf(&b, "zone%d = %s %.6g %.6g %.6g %.6g %.6g %.6g %.6g %g\n", i, ZONE_WORD[z->kind], z->a[0], z->a[1], z->a[2], z->b[0], z->b[1], z->b[2], z->r, z->sizePct);
    }
    str_catf(&b, "# boundaryN = name type, in the order of the regions in constant/triSurface/geometry.stl\nboundaries = %d\n", m->nNames);
    for (int i = 0; i < m->nNames; i++) str_catf(&b, "boundary%d = %s %s\n", i, m->names[i], BT_NAME[m->types[i]]);
    char d[PL_PATH], p[PL_PATH];
    pl_join(d, sizeof d, casePath, "mesher"); pl_mkdirs(d);
    ini_path(casePath, p, sizeof p);
    bool ok = pl_write_file(p, b.s, b.len);
    str_free(&b);
    return ok;
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
        return;
    }
    double L = body_length(s);                           /* a wind tunnel along +x, z up */
    lo[0] = s->bmin[0] - m->up * L;   hi[0] = s->bmax[0] + m->down * L;
    lo[1] = s->bmin[1] - m->side * L; hi[1] = s->bmax[1] + m->side * L;
    lo[2] = m->ground ? s->bmin[2] : s->bmin[2] - m->side * L; hi[2] = s->bmax[2] + m->top * L;
}

static int level_of(double pct) { return pct >= 100 ? 0 : (int)lround(log2(100.0 / pct)); }
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
    if (m->mesher == MESHER_SNAPPY || m->mesher == MESHER_BODYFIT) {
        n = V / (b * b * b);
        for (int l = 1; l <= L0; l++) { double h = b / pow(2, l); n += A * nb * h / (h * h * h) * 0.875; }
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
        double h = b * m->targetPct / 100;
        double wallA = 0;
        for (int i = 0; i < s->nRegions; i++) if (s->reg[i].type == BT_WALL) wallA += s->reg[i].area;
        n += wallA / (h * h) * m->layers * (m->mesher == MESHER_POLY ? 0.5 : 1);
    }
    return n;
}

/* ============================================================================ writing */
static bool write_ascii_stl(const Stl *s, const char *path)
{
    Str b; str_init(&b);
    for (int r = 0; r < s->nRegions; r++) {
        str_catf(&b, "solid %s\n", s->reg[r].name);
        for (int t = 0; t < s->nTri; t++) {
            if (s->region[t] != r) continue;
            const float *v = s->tri + 9 * (size_t)t;
            double n[3], a; tri_normal(v, n, &a);
            str_catf(&b, " facet normal %g %g %g\n  outer loop\n", n[0], n[1], n[2]);
            for (int k = 0; k < 3; k++) str_catf(&b, "   vertex %.9g %.9g %.9g\n", v[3 * k], v[3 * k + 1], v[3 * k + 2]);
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
        for (int k = 0; k < 3; k++) out[k] = lo[k] + (s->bmin[k] - lo[k]) * 0.5 + m->base * 0.137 * (k + 1);
        if (s->bmin[0] - lo[0] < m->base) out[0] = lo[0] + m->base * 0.61;
        if (m->ground) out[2] = lo[2] + fmin(m->base * 2.37, (hi[2] - lo[2]) * 0.4);
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
    for (int k = 0; k < 3; k++) nc[k] = (int)fmax(1, ceil((hi[k] - lo[k]) / m->base));
    for (int k = 0; k < 3; k++) hi[k] = lo[k] + nc[k] * m->base;   /* cubic cells: the box grows to whole cells */
    Str b; str_init(&b);
    /* ---- blockMeshDict: the background, cubes of the base size */
    str_cat(&b, HEADER("dictionary", "blockMeshDict") OURS "\nscale 1;\n\nvertices\n(\n");
    for (int i = 0; i < 8; i++) {
        double x = (i == 1 || i == 2 || i == 5 || i == 6) ? hi[0] : lo[0], y = (i == 2 || i == 3 || i == 6 || i == 7) ? hi[1] : lo[1], z = i >= 4 ? hi[2] : lo[2];
        str_catf(&b, "    (%.9g %.9g %.9g)\n", x, y, z);
    }
    str_catf(&b, ");\n\nblocks\n(\n    hex (0 1 2 3 4 5 6 7) (%d %d %d) simpleGrading (1 1 1)\n);\n\nboundary\n(\n", nc[0], nc[1], nc[2]);
    if (m->domain == DOMAIN_INTERNAL)
        str_cat(&b, "    background\n    {\n        type patch;\n        faces ((0 3 2 1) (4 5 6 7) (0 4 7 3) (2 6 5 1) (1 5 4 0) (3 7 6 2));\n    }\n");
    else {
        str_cat(&b, "    inlet\n    {\n        type patch;\n        faces ((0 4 7 3));\n    }\n");
        str_cat(&b, "    outlet\n    {\n        type patch;\n        faces ((2 6 5 1));\n    }\n");
        if (m->ground) {
            str_cat(&b, "    ground\n    {\n        type wall;\n        faces ((0 3 2 1));\n    }\n");
            str_cat(&b, "    farfield\n    {\n        type patch;\n        faces ((4 5 6 7) (1 5 4 0) (3 7 6 2));\n    }\n");
        } else str_cat(&b, "    farfield\n    {\n        type patch;\n        faces ((0 3 2 1) (4 5 6 7) (1 5 4 0) (3 7 6 2));\n    }\n");
    }
    str_cat(&b, ");\n");
    bool ok = put_dict(casePath, "system/blockMeshDict", b.s, err, cap);
    /* ---- surfaceFeatureExtractDict: edges sharper than the feature angle */
    b.len = 0; b.s[0] = 0;
    str_catf(&b, HEADER("dictionary", "surfaceFeatureExtractDict") OURS "\ngeometry.stl\n{\n    extractionMethod    extractFromSurface;\n"
                 "    includedAngle       %g;\n    subsetFeatures\n    {\n        nonManifoldEdges    yes;\n        openEdges           yes;\n    }\n"
                 "    writeObj            no;\n}\n", 180 - m->featureAngle);
    ok = ok && put_dict(casePath, "system/surfaceFeatureExtractDict", b.s, err, cap);
    /* ---- meshQualityDict */
    b.len = 0; b.s[0] = 0;
    str_catf(&b, HEADER("dictionary", "meshQualityDict") OURS "\n#includeEtc \"caseDicts/meshQualityDict\"\n\nmaxNonOrtho %g;\nmaxInternalSkewness %g;\nmaxBoundarySkewness %g;\n",
             m->maxNonOrtho, m->maxSkew, m->maxSkew * 5);
    ok = ok && put_dict(casePath, "system/meshQualityDict", b.s, err, cap);
    /* ---- snappyHexMeshDict */
    int L0 = level_of(m->targetPct), L1 = level_of(m->minPct);
    if (L1 < L0) L1 = L0;
    double loc[3]; location_in_mesh(m, s, loc);
    b.len = 0; b.s[0] = 0;
    str_catf(&b, HEADER("dictionary", "snappyHexMeshDict") OURS
                 "// base %g m; target surface size %g %% (level %d), minimum %g %% (level %d); curvature %g points a circle\n\n"
                 "castellatedMesh true;\nsnap            true;\naddLayers       %s;\n\n"
                 "// one surface a boundary (constant/triSurface/regions), so each patch is named exactly as the boundary\ngeometry\n{\n",
             m->base, m->targetPct, L0, m->minPct, L1, m->curvature, m->prism && m->layers > 0 ? "true" : "false");
    for (int r = 0; r < s->nRegions; r++)
        str_catf(&b, "    %s\n    {\n        type triSurfaceMesh;\n        file \"regions/%s.stl\";\n    }\n", s->reg[r].name, s->reg[r].name);
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
        str_catf(&b, "        %s { level (%d %d); patchInfo { type %s; } }\n", s->reg[r].name, L0, L1, foam_type(s->reg[r].type));
    str_catf(&b, "    }\n\n    resolveFeatureAngle %g;     // %g points a circle\n\n    refinementRegions\n    {\n",
             360.0 / fmax(3, m->curvature), m->curvature);
    for (int i = 0; i < m->nZones; i++)
        str_catf(&b, "        zone%d { mode inside; levels ((1E15 %d)); }\n", i, level_of(m->zone[i].sizePct));
    str_catf(&b, "    }\n\n    locationInMesh (%.9g %.9g %.9g);\n    allowFreeStandingZoneFaces true;\n}\n\n", loc[0], loc[1], loc[2]);
    str_cat(&b, "snapControls\n{\n    nSmoothPatch 3;\n    tolerance 2.0;\n    nSolveIter 50;\n    nRelaxIter 5;\n    nFeatureSnapIter 10;\n"
                "    implicitFeatureSnap false;\n    explicitFeatureSnap true;\n    multiRegionFeatureSnap false;\n}\n\n");
    double T = m->base * m->prismPct / 100 / pow(2, L0);           /* % of the base, at the surface's own level */
    str_catf(&b, "addLayersControls\n{\n    relativeSizes false;     // total thickness %g %% of base, scaled to the surface level\n    layers\n    {\n", m->prismPct);
    for (int r = 0; r < s->nRegions; r++)
        if (s->reg[r].type == BT_WALL) str_catf(&b, "        %s { nSurfaceLayers %d; }\n", s->reg[r].name, m->layers);
    if (m->domain == DOMAIN_EXTERNAL && m->ground) str_catf(&b, "        ground { nSurfaceLayers %d; }\n", m->layers);
    str_catf(&b, "    }\n    expansionRatio %g;\n    thickness %.6g;\n    minThickness %.6g;\n    nGrow 0;\n    featureAngle 130;\n    slipFeatureAngle 30;\n"
                 "    nRelaxIter 3;\n    nSmoothSurfaceNormals 1;\n    nSmoothNormals 3;\n    nSmoothThickness 10;\n    maxFaceThicknessRatio 0.5;\n"
                 "    maxThicknessToMedialRatio 0.3;\n    minMedialAxisAngle 90;\n    nBufferCellsNoExtrude 0;\n    nLayerIter 50;\n}\n\n"
                 "meshQualityControls\n{\n    #include \"meshQualityDict\"\n    nSmoothScale 4;\n    errorReduction 0.75;\n    relaxed\n    {\n        maxNonOrtho 75;\n    }\n}\n\nmergeTolerance 1e-6;\n",
             m->stretch, T, T * 0.05);
    ok = ok && put_dict(casePath, "system/snappyHexMeshDict", b.s, err, cap);
    str_free(&b);
    return ok;
}

void mesher_type_script(const MeshCfg *m, const Stl *s, Str *out)
{
    (void)m;
    for (int r = 0; r < s->nRegions; r++)
        str_catf(out, "foamDictionary constant/polyMesh/boundary -entry entry0/%s/type -set %s > /dev/null 2>&1 && echo 'patch %s: %s'\n",
                 s->reg[r].name, foam_type(s->reg[r].type), s->reg[r].name, foam_type(s->reg[r].type));
    if (m->domain == DOMAIN_EXTERNAL && m->ground)
        str_cat(out, "foamDictionary constant/polyMesh/boundary -entry entry0/ground/type -set wall > /dev/null 2>&1 && echo 'patch ground: wall'\n");
}

bool mesher_prepare(const char *casePath, const MeshCfg *m, Stl *s, char *err, int cap)
{
    if (!s->nTri) { snprintf(err, (size_t)cap, "No STL loaded"); return false; }
    if (m->base <= 0) { snprintf(err, (size_t)cap, "The base size must be above zero"); return false; }
    char p[PL_PATH], d[PL_PATH];
    pl_join(d, sizeof d, casePath, "constant/triSurface"); pl_mkdirs(d);
    pl_join(p, sizeof p, d, "geometry.stl");
    if (!write_ascii_stl(s, p)) { snprintf(err, (size_t)cap, "Could not write constant/triSurface/geometry.stl"); return false; }
    char td[PL_PATH]; pl_join(td, sizeof td, casePath, "constant/triSurface/regions");
    pl_remove_tree(td); pl_mkdirs(td);
    for (int r = 0; r < s->nRegions; r++) {
        Stl one = *s;                                      /* the same triangles, only this region written */
        one.nRegions = 1; one.reg[0] = s->reg[r];
        int *keep = one.region;
        one.region = malloc(sizeof(int) * (size_t)s->nTri);
        for (int t = 0; t < s->nTri; t++) one.region[t] = keep[t] == r ? 0 : -1;
        char f[96]; snprintf(f, sizeof f, "%s.stl", s->reg[r].name);
        pl_join(p, sizeof p, td, f);
        bool ok = write_ascii_stl(&one, p);
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
