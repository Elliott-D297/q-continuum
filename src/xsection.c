/* xsection.c -- reads a 2-D case's polyMesh (ASCII or binary) into its cross section (see xsection.h) */
#define _GNU_SOURCE
#include "xsection.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================ OpenFOAM list files */
typedef struct {
    char  *t;
    size_t len, pos;
    bool   binary;
    int    labelBytes;
    char   cls[64];
} FList;

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (1L << 30)) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t r = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[r] = 0;
    *len = r;
    return b;
}

static void header_word(const char *h, const char *key, char *out, int cap)
{
    out[0] = 0;
    const char *k = strstr(h, key);
    if (!k) return;
    k += strlen(key);
    while (*k == ' ' || *k == '\t') k++;
    int i = 0;
    while (*k && *k != ';' && *k != '\n' && i < cap - 1) { if (*k != '"') out[i++] = *k; k++; }
    out[i] = 0;
}

static bool fl_open(FList *f, const char *path)
{
    memset(f, 0, sizeof *f);
    f->t = read_all(path, &f->len);
    if (!f->t) return false;
    f->labelBytes = 4;
    char *ff = strstr(f->t, "FoamFile");
    if (ff) {
        char *e = strchr(ff, '}');
        if (e) {
            char save = *e; *e = 0;
            char fmt[32], arch[64];
            header_word(ff, "format", fmt, sizeof fmt);
            header_word(ff, "class", f->cls, sizeof f->cls);
            header_word(ff, "arch", arch, sizeof arch);
            f->binary = !strcmp(fmt, "binary");
            if (strstr(arch, "label=64")) f->labelBytes = 8;
            *e = save;
            f->pos = (size_t)(e + 1 - f->t);
        }
    }
    return true;
}

static void fl_close(FList *f) { free(f->t); memset(f, 0, sizeof *f); }

static void skip_space(FList *f)
{
    for (;;) {
        while (f->pos < f->len && isspace((unsigned char)f->t[f->pos])) f->pos++;
        if (f->pos + 1 < f->len && f->t[f->pos] == '/' && f->t[f->pos + 1] == '/') { while (f->pos < f->len && f->t[f->pos] != '\n') f->pos++; continue; }
        if (f->pos + 1 < f->len && f->t[f->pos] == '/' && f->t[f->pos + 1] == '*') {
            char *e = strstr(f->t + f->pos + 2, "*/");
            f->pos = e ? (size_t)(e + 2 - f->t) : f->len;
            continue;
        }
        break;
    }
}

/* "N (" : the count; the position is then just after the '(' */
static long fl_begin(FList *f)
{
    skip_space(f);
    char *e;
    long n = strtol(f->t + f->pos, &e, 10);
    if (e == f->t + f->pos) return -1;
    f->pos = (size_t)(e - f->t);
    skip_space(f);
    if (f->pos >= f->len || f->t[f->pos] != '(') return -1;
    f->pos++;
    return n;
}

static void fl_end(FList *f)
{
    while (f->pos < f->len && f->t[f->pos] != ')') f->pos++;
    if (f->pos < f->len) f->pos++;
}

static bool read_labels(FList *f, long n, long *out)
{
    if (f->binary) {
        size_t need = (size_t)n * (size_t)f->labelBytes;
        if (f->pos + need > f->len) return false;
        for (long i = 0; i < n; i++) {
            if (f->labelBytes == 8) { int64_t v; memcpy(&v, f->t + f->pos + (size_t)i * 8, 8); out[i] = (long)v; }
            else { int32_t v; memcpy(&v, f->t + f->pos + (size_t)i * 4, 4); out[i] = v; }
        }
        f->pos += need;
        fl_end(f);
        return true;
    }
    char *p = f->t + f->pos, *e;
    for (long i = 0; i < n; i++) {
        out[i] = strtol(p, &e, 10);
        if (e == p) return false;
        p = e;
    }
    f->pos = (size_t)(p - f->t);
    fl_end(f);
    return true;
}

static double *read_points(const char *path, long *n)
{
    FList f;
    if (!fl_open(&f, path)) return NULL;
    long np = fl_begin(&f);
    double *p = np > 0 ? malloc(sizeof(double) * 3 * (size_t)np) : NULL;
    bool ok = p != NULL;
    if (ok && f.binary) {
        size_t need = (size_t)np * 24;
        if (f.pos + need > f.len) ok = false;
        else memcpy(p, f.t + f.pos, need);
    } else if (ok) {
        char *q = f.t + f.pos, *e;
        for (long i = 0; i < np && ok; i++) {
            q = strchr(q, '(');
            if (!q) { ok = false; break; }
            q++;
            for (int k = 0; k < 3; k++) { p[i * 3 + k] = strtod(q, &e); if (e == q) ok = false; q = e; }
        }
    }
    fl_close(&f);
    if (!ok) { free(p); return NULL; }
    *n = np;
    return p;
}

/* faces as one flat list of point labels plus offsets (faceList or faceCompactList, ASCII or binary) */
static bool read_faces(const char *path, long **off, long **idx, long *nFaces)
{
    FList f;
    if (!fl_open(&f, path)) return false;
    bool ok = false;
    if (!strcmp(f.cls, "faceCompactList")) {
        long no = fl_begin(&f);
        if (no > 0) {
            *off = malloc(sizeof(long) * (size_t)no);
            if (read_labels(&f, no, *off)) {
                long nd = fl_begin(&f);
                if (nd >= 0) {
                    *idx = malloc(sizeof(long) * (size_t)(nd > 0 ? nd : 1));
                    ok = read_labels(&f, nd, *idx);
                    *nFaces = no - 1;
                }
            }
        }
    } else {                                                    /* faceList: N ( 4(a b c d) ... ) */
        long n = fl_begin(&f);
        if (n > 0) {
            *off = malloc(sizeof(long) * (size_t)(n + 1));
            long cap = n * 4 + 16, used = 0;
            *idx = malloc(sizeof(long) * (size_t)cap);
            char *p = f.t + f.pos, *e;
            ok = true;
            for (long i = 0; i < n && ok; i++) {
                long k = strtol(p, &e, 10);
                if (e == p) { ok = false; break; }
                p = strchr(e, '(');
                if (!p) { ok = false; break; }
                p++;
                (*off)[i] = used;
                if (used + k > cap) { cap = (used + k) * 2; *idx = realloc(*idx, sizeof(long) * (size_t)cap); }
                for (long j = 0; j < k; j++) { (*idx)[used++] = strtol(p, &e, 10); p = e; }
                p = strchr(p, ')');
                if (!p) { ok = false; break; }
                p++;
            }
            (*off)[n] = used;
            *nFaces = n;
        }
    }
    fl_close(&f);
    if (!ok) { free(*off); free(*idx); *off = *idx = NULL; }
    return ok;
}

static long *read_label_list(const char *path, long *n)
{
    FList f;
    if (!fl_open(&f, path)) return NULL;
    long c = fl_begin(&f);
    long *v = c > 0 ? malloc(sizeof(long) * (size_t)c) : NULL;
    if (v && !read_labels(&f, c, v)) { free(v); v = NULL; }
    /* the note in owner's header has the cell count: "nCells:1234" */
    fl_close(&f);
    *n = c;
    return v;
}

/* boundary: N ( name { type t; nFaces n; startFace s; ... } ... ) */
typedef struct { char name[64], type[24]; long nFaces, start; } BPatch;

static int read_boundary(const char *path, BPatch *out, int max)
{
    FList f;
    if (!fl_open(&f, path)) return 0;
    long n = fl_begin(&f);
    int k = 0;
    char *p = f.t + f.pos;
    for (long i = 0; i < n && k < max; i++) {
        while (*p && isspace((unsigned char)*p)) p++;
        char name[64]; int j = 0;
        while (*p && !isspace((unsigned char)*p) && *p != '{' && j < 63) name[j++] = *p++;
        name[j] = 0;
        char *o = strchr(p, '{'), *c = o ? strchr(o, '}') : NULL;
        if (!o || !c || !name[0]) break;
        *c = 0;
        BPatch *b = &out[k++];
        memset(b, 0, sizeof *b);
        pl_strlcpy(b->name, name, sizeof b->name);
        header_word(o, "type", b->type, sizeof b->type);
        char v[32];
        header_word(o, "nFaces", v, sizeof v); b->nFaces = atol(v);
        header_word(o, "startFace", v, sizeof v); b->start = atol(v);
        p = c + 1;
    }
    fl_close(&f);
    return k;
}

/* ============================================================================ the cross section */
bool xs_is_2d(const char *casePath)
{
    static char path[PL_PATH]; static time_t mt; static bool val;     /* asked every frame: read once per mesh */
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, "constant/polyMesh/boundary");
    time_t m = pl_mtime(p);
    if (!strcmp(path, p) && m == mt) return val;
    pl_strlcpy(path, p, sizeof path); mt = m; val = false;
    BPatch b[XS_MAX_PATCHES];
    int n = read_boundary(p, b, XS_MAX_PATCHES);
    for (int i = 0; i < n; i++) if (!strcmp(b[i].type, "empty") && b[i].nFaces > 0) val = true;
    return val;
}

void xs_free(XSection *x)
{
    free(x->start); free(x->u); free(x->v); free(x->cell); free(x->seg); free(x->segPatch); free(x->val);
    memset(x, 0, sizeof *x);
}

bool xs_load(XSection *x, const char *casePath)
{
    xs_free(x);
    char d[PL_PATH], p[PL_PATH];
    pl_join(d, sizeof d, casePath, "constant/polyMesh");
    pl_join(p, sizeof p, d, "owner");
    x->meshTime = pl_mtime(p);
    BPatch b[XS_MAX_PATCHES];
    pl_join(p, sizeof p, d, "boundary");
    int nb = read_boundary(p, b, XS_MAX_PATCHES);
    if (nb == 0) { snprintf(x->err, sizeof x->err, "No mesh yet (constant/polyMesh/boundary)."); return false; }
    bool any = false;
    for (int i = 0; i < nb; i++) if (!strcmp(b[i].type, "empty") && b[i].nFaces) any = true;
    if (!any) { snprintf(x->err, sizeof x->err, "Not a 2-D case: the mesh has no empty patches."); return false; }
    for (int i = 0; i < nb; i++) { pl_strlcpy(x->patch[i], b[i].name, sizeof x->patch[i]); pl_strlcpy(x->ptype[i], b[i].type, sizeof x->ptype[i]); }
    x->nPatch = nb;
    long np = 0, nf = 0, no = 0;
    pl_join(p, sizeof p, d, "points");
    double *pt = read_points(p, &np);
    long *off = NULL, *idx = NULL;
    pl_join(p, sizeof p, d, "faces");
    bool fok = pt && read_faces(p, &off, &idx, &nf);
    pl_join(p, sizeof p, d, "owner");
    long *own = fok ? read_label_list(p, &no) : NULL;
    if (!pt || !fok || !own) {
        snprintf(x->err, sizeof x->err, "Could not read the mesh (compressed .gz files are not read here).");
        free(pt); free(off); free(idx); free(own);
        return false;
    }
    /* the thin direction: the smallest extent */
    double lo[3] = { 1e300, 1e300, 1e300 }, hi[3] = { -1e300, -1e300, -1e300 };
    for (long i = 0; i < np; i++) for (int k = 0; k < 3; k++) { double c = pt[i * 3 + k]; if (c < lo[k]) lo[k] = c; if (c > hi[k]) hi[k] = c; }
    int thin = 0;
    for (int k = 1; k < 3; k++) if (hi[k] - lo[k] < hi[thin] - lo[thin]) thin = k;
    x->axU = thin == 0 ? 1 : 0;
    x->axV = thin == 2 ? 1 : 2;
    double tol = (hi[thin] - lo[thin]) * 1e-3 + 1e-12, side = lo[thin];
    x->lo[0] = (float)lo[x->axU]; x->hi[0] = (float)hi[x->axU];
    x->lo[1] = (float)lo[x->axV]; x->hi[1] = (float)hi[x->axV];
    /* cells: the faces of the empty patches on the low side */
    long cap = 1024, used = 0;
    x->start = malloc(sizeof(int) * 1024); x->cell = malloc(sizeof(int) * 1024);
    x->u = malloc(sizeof(float) * (size_t)cap); x->v = malloc(sizeof(float) * (size_t)cap);
    int polyCap = 1024;
    long maxCell = -1;
    for (int pi = 0; pi < nb; pi++) {
        if (strcmp(b[pi].type, "empty")) continue;
        for (long fi = b[pi].start; fi < b[pi].start + b[pi].nFaces && fi < nf; fi++) {
            long a = off[fi], e = off[fi + 1];
            bool low = true;
            for (long k = a; k < e && low; k++) if (fabs(pt[idx[k] * 3 + thin] - side) > tol) low = false;
            if (!low) continue;
            if (x->nPoly + 2 > polyCap) { polyCap *= 2; x->start = realloc(x->start, sizeof(int) * (size_t)polyCap); x->cell = realloc(x->cell, sizeof(int) * (size_t)polyCap); }
            if (used + (e - a) > cap) { cap = (used + (e - a)) * 2; x->u = realloc(x->u, sizeof(float) * (size_t)cap); x->v = realloc(x->v, sizeof(float) * (size_t)cap); }
            x->start[x->nPoly] = (int)used;
            x->cell[x->nPoly] = (int)own[fi];
            if (own[fi] > maxCell) maxCell = own[fi];
            for (long k = a; k < e; k++) { x->u[used] = (float)pt[idx[k] * 3 + x->axU]; x->v[used] = (float)pt[idx[k] * 3 + x->axV]; used++; }
            x->nPoly++;
        }
    }
    x->start[x->nPoly] = (int)used;
    x->nCells = (int)(maxCell + 1);
    /* boundary: each face of the other patches is a strip across the thickness; its low edge is a segment */
    int segCap = 1024;
    x->seg = malloc(sizeof(float) * 4 * (size_t)segCap); x->segPatch = malloc(sizeof(int) * (size_t)segCap);
    for (int pi = 0; pi < nb; pi++) {
        if (!strcmp(b[pi].type, "empty")) continue;
        for (long fi = b[pi].start; fi < b[pi].start + b[pi].nFaces && fi < nf; fi++) {
            long a = off[fi], e = off[fi + 1], lowPts[4]; int nl = 0;
            for (long k = a; k < e && nl < 4; k++) if (fabs(pt[idx[k] * 3 + thin] - side) <= tol) lowPts[nl++] = idx[k];
            if (nl < 2) continue;
            if (x->nSeg >= segCap) { segCap *= 2; x->seg = realloc(x->seg, sizeof(float) * 4 * (size_t)segCap); x->segPatch = realloc(x->segPatch, sizeof(int) * (size_t)segCap); }
            float *s = &x->seg[x->nSeg * 4];
            s[0] = (float)pt[lowPts[0] * 3 + x->axU]; s[1] = (float)pt[lowPts[0] * 3 + x->axV];
            s[2] = (float)pt[lowPts[1] * 3 + x->axU]; s[3] = (float)pt[lowPts[1] * 3 + x->axV];
            x->segPatch[x->nSeg++] = pi;
        }
    }
    free(pt); free(off); free(idx); free(own);
    if (x->nPoly == 0) { snprintf(x->err, sizeof x->err, "Found no cell faces on the empty patches."); return false; }
    x->ok = true;
    return true;
}

/* internalField: "uniform v", or "nonuniform List<scalar|vector> N ( ... )", ASCII or binary */
bool xs_load_field(XSection *x, const char *casePath, const char *time, const char *field)
{
    free(x->val); x->val = NULL; x->field[0] = 0;
    if (!x->ok || !field || !field[0]) return false;
    char rel[PL_PATH], p[PL_PATH];
    snprintf(rel, sizeof rel, "%s/%s", time, field);
    pl_join(p, sizeof p, casePath, rel);
    FList f;
    if (!fl_open(&f, p)) return false;
    char *in = strstr(f.t + f.pos, "internalField");
    bool ok = false;
    int nc = x->nCells;
    float *val = malloc(sizeof(float) * (size_t)(nc > 0 ? nc : 1));
    if (in) {
        char *q = in + strlen("internalField");
        while (*q == ' ' || *q == '\t') q++;
        if (pl_starts_with(q, "uniform")) {
            q += 7;
            while (*q == ' ') q++;
            double m;
            if (*q == '(') { double a, b, c; sscanf(q + 1, "%lf %lf %lf", &a, &b, &c); m = sqrt(a * a + b * b + c * c); }
            else m = atof(q);
            for (int i = 0; i < nc; i++) val[i] = (float)m;
            ok = true;
        } else if (pl_starts_with(q, "nonuniform")) {
            bool vec = strstr(q, "List<vector>") && strstr(q, "List<vector>") < q + 40;
            bool scal = strstr(q, "List<scalar>") && strstr(q, "List<scalar>") < q + 40;
            f.pos = (size_t)(strchr(q, '>') ? strchr(q, '>') + 1 - f.t : q - f.t);
            long n = fl_begin(&f);
            if ((vec || scal) && n >= nc && n > 0) {
                if (f.binary) {
                    size_t comp = vec ? 3 : 1;
                    if (f.pos + (size_t)n * comp * 8 <= f.len) {
                        for (int i = 0; i < nc; i++) {
                            double a[3] = { 0 };
                            memcpy(a, f.t + f.pos + (size_t)i * comp * 8, comp * 8);
                            val[i] = (float)(vec ? sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) : a[0]);
                        }
                        ok = true;
                    }
                } else {
                    char *r = f.t + f.pos, *e;
                    ok = true;
                    for (int i = 0; i < nc && ok; i++) {
                        if (vec) {
                            r = strchr(r, '(');
                            if (!r) { ok = false; break; }
                            r++;
                            double a = strtod(r, &e), bb = strtod(e, &e), c = strtod(e, &e);
                            r = e;
                            val[i] = (float)sqrt(a * a + bb * bb + c * c);
                        } else {
                            val[i] = (float)strtod(r, &e);
                            if (e == r) ok = false;
                            r = e;
                        }
                    }
                }
            }
        }
    }
    fl_close(&f);
    if (!ok) { free(val); return false; }
    x->val = val;
    pl_strlcpy(x->field, field, sizeof x->field);
    pl_strlcpy(x->time, time, sizeof x->time);
    x->vmin = 1e30f; x->vmax = -1e30f;
    for (int i = 0; i < nc; i++) { if (val[i] < x->vmin) x->vmin = val[i]; if (val[i] > x->vmax) x->vmax = val[i]; }
    return true;
}

/* ============================================================================ the boundary surface (3-D view) */
void ms_free(MeshSurf *m)
{
    free(m->tri); free(m->nrm); free(m->triPatch); free(m->edge); free(m->edgePatch);
    memset(m, 0, sizeof *m);
}

bool ms_load(MeshSurf *m, const char *dir, long maxEdges)
{
    ms_free(m);
    char p[PL_PATH];
    pl_join(p, sizeof p, dir, "owner");
    m->meshTime = pl_mtime(p);
    static BPatch b[MS_MAX_PATCHES];
    pl_join(p, sizeof p, dir, "boundary");
    int nb = read_boundary(p, b, MS_MAX_PATCHES);
    if (nb == 0) {
        char gz[PL_PATH]; pl_join(gz, sizeof gz, dir, "faces.gz");
        snprintf(m->err, sizeof m->err, pl_is_file(gz) ? "The mesh is compressed (.gz): Q-Continuum reads uncompressed polyMesh files."
                                                      : "No mesh yet: constant/polyMesh is empty.");
        return false;
    }
    long np = 0, nf = 0, no = 0;
    pl_join(p, sizeof p, dir, "points");
    double *pt = read_points(p, &np);
    long *off = NULL, *idx = NULL;
    pl_join(p, sizeof p, dir, "faces");
    bool fok = pt && read_faces(p, &off, &idx, &nf);
    pl_join(p, sizeof p, dir, "owner");
    long *own = fok ? read_label_list(p, &no) : NULL;
    if (!pt || !fok || !own) {
        snprintf(m->err, sizeof m->err, "Could not read the mesh files (compressed .gz files are not read here).");
        free(pt); free(off); free(idx); free(own);
        return false;
    }
    m->nPoints = np; m->nFaces = nf;
    long maxCell = -1; for (long i = 0; i < no; i++) if (own[i] > maxCell) maxCell = own[i];
    m->nCells = maxCell + 1;
    long nbf = 0, nTri = 0, nEdge = 0;
    for (int i = 0; i < nb; i++) {
        for (long f = b[i].start; f < b[i].start + b[i].nFaces && f < nf; f++) { long k = off[f + 1] - off[f]; if (k >= 3) { nTri += k - 2; nEdge += k; } }
        nbf += b[i].nFaces;
    }
    m->nInternal = b[0].start;
    bool edges = nEdge <= maxEdges;
    m->tri = malloc(sizeof(float) * 9 * (size_t)(nTri ? nTri : 1));
    m->nrm = malloc(sizeof(float) * 3 * (size_t)(nTri ? nTri : 1));
    m->triPatch = malloc(sizeof(int) * (size_t)(nTri ? nTri : 1));
    if (edges) { m->edge = malloc(sizeof(float) * 6 * (size_t)(nEdge ? nEdge : 1)); m->edgePatch = malloc(sizeof(int) * (size_t)(nEdge ? nEdge : 1)); }
    if (!m->tri || !m->nrm || !m->triPatch || (edges && (!m->edge || !m->edgePatch))) {
        snprintf(m->err, sizeof m->err, "Not enough memory for a boundary of %ld faces.", nbf);
        free(pt); free(off); free(idx); free(own); ms_free(m);
        return false;
    }
    for (int k = 0; k < 3; k++) { m->lo[k] = 1e30f; m->hi[k] = -1e30f; }
    long t = 0, e = 0;
    for (int i = 0; i < nb; i++) {
        pl_strlcpy(m->patch[i], b[i].name, sizeof m->patch[i]);
        pl_strlcpy(m->ptype[i], b[i].type, sizeof m->ptype[i]);
        m->patchFaces[i] = b[i].nFaces;
        for (long f = b[i].start; f < b[i].start + b[i].nFaces && f < nf; f++) {
            long a = off[f], z = off[f + 1];
            if (z - a < 3) continue;
            const double *p0 = &pt[idx[a] * 3];
            /* the face's normal: Newell's method over its points */
            double nx = 0, ny = 0, nz = 0;
            for (long k = a; k < z; k++) {
                const double *c = &pt[idx[k] * 3], *d = &pt[idx[k + 1 < z ? k + 1 : a] * 3];
                nx += (c[1] - d[1]) * (c[2] + d[2]); ny += (c[2] - d[2]) * (c[0] + d[0]); nz += (c[0] - d[0]) * (c[1] + d[1]);
            }
            double nl = sqrt(nx * nx + ny * ny + nz * nz); if (nl > 0) { nx /= nl; ny /= nl; nz /= nl; }
            for (long k = a + 1; k + 1 < z; k++) {
                const double *p1 = &pt[idx[k] * 3], *p2 = &pt[idx[k + 1] * 3];
                float *o = &m->tri[t * 9];
                for (int q = 0; q < 3; q++) { o[q] = (float)p0[q]; o[3 + q] = (float)p1[q]; o[6 + q] = (float)p2[q]; }
                m->nrm[t * 3] = (float)nx; m->nrm[t * 3 + 1] = (float)ny; m->nrm[t * 3 + 2] = (float)nz;
                m->triPatch[t] = i;
                t++;
            }
            for (long k = a; k < z; k++) {
                const double *c = &pt[idx[k] * 3];
                for (int q = 0; q < 3; q++) { if (c[q] < m->lo[q]) m->lo[q] = (float)c[q]; if (c[q] > m->hi[q]) m->hi[q] = (float)c[q]; }
                if (edges) {
                    const double *d = &pt[idx[k + 1 < z ? k + 1 : a] * 3];
                    float *o = &m->edge[e * 6];
                    for (int q = 0; q < 3; q++) { o[q] = (float)c[q]; o[3 + q] = (float)d[q]; }
                    m->edgePatch[e++] = i;
                }
            }
        }
    }
    m->nTri = t; m->nEdge = e; m->nPatch = nb;
    if (!t) for (int k = 0; k < 3; k++) m->lo[k] = m->hi[k] = 0;
    free(pt); free(off); free(idx); free(own);
    m->ok = true;
    return true;
}

/* ============================================================================ a slice through a 3-D mesh */
/* the mesh stays in memory between slices: dragging the plane only re-cuts */
static struct { char dir[PL_PATH]; time_t mt; double *pt; long np, nf, nOwn, nNei; long *off, *idx, *own, *nei; int nb; BPatch b[XS_MAX_PATCHES]; } SM;

static void sm_free(void)                                       /* the kept mesh, gone; every pointer NULL again */
{
    free(SM.pt); free(SM.off); free(SM.idx); free(SM.own); free(SM.nei);
    SM.pt = NULL; SM.off = SM.idx = SM.own = SM.nei = NULL;
    memset(&SM, 0, sizeof SM);
}

static bool slice_mesh(const char *dir, char *err, int cap)
{
    char p[PL_PATH]; pl_join(p, sizeof p, dir, "owner");
    time_t mt = pl_mtime(p);
    if (SM.pt && !strcmp(SM.dir, dir) && mt == SM.mt) return true;
    sm_free();
    pl_join(p, sizeof p, dir, "boundary");
    SM.nb = read_boundary(p, SM.b, XS_MAX_PATCHES);
    if (!SM.nb) { snprintf(err, (size_t)cap, "No mesh yet (constant/polyMesh/boundary)."); return false; }
    pl_join(p, sizeof p, dir, "points"); SM.pt = read_points(p, &SM.np);
    pl_join(p, sizeof p, dir, "faces"); bool fok = SM.pt && read_faces(p, &SM.off, &SM.idx, &SM.nf);
    pl_join(p, sizeof p, dir, "owner"); SM.own = fok ? read_label_list(p, &SM.nOwn) : NULL;
    pl_join(p, sizeof p, dir, "neighbour"); SM.nei = SM.own ? read_label_list(p, &SM.nNei) : NULL;
    if (!SM.pt || !fok || !SM.own || (!SM.nei && SM.nNei != 0)) {
        snprintf(err, (size_t)cap, "Could not read the mesh files (compressed .gz files are not read here).");
        sm_free();
        return false;
    }
    pl_strlcpy(SM.dir, dir, sizeof SM.dir); SM.mt = mt;
    return true;
}

typedef struct { int cell; float u0, v0, u1, v1; } CutSeg;
static int cut_cmp(const void *a, const void *b) { int x = ((const CutSeg *)a)->cell, y = ((const CutSeg *)b)->cell; return (x > y) - (x < y); }

bool xs_load_slice(XSection *x, const char *dir, int axis, float frac)
{
    xs_free(x);
    if (!slice_mesh(dir, x->err, sizeof x->err)) return false;
    char p[PL_PATH]; pl_join(p, sizeof p, dir, "owner");
    x->meshTime = pl_mtime(p);
    double lo[3] = { 1e300, 1e300, 1e300 }, hi[3] = { -1e300, -1e300, -1e300 };
    for (long i = 0; i < SM.np; i++) for (int k = 0; k < 3; k++) { double c = SM.pt[i * 3 + k]; if (c < lo[k]) lo[k] = c; if (c > hi[k]) hi[k] = c; }
    if (axis < 0 || axis > 2) axis = 2;
    x->slice = true; x->sliceAxis = axis; x->sliceLo = (float)lo[axis]; x->sliceHi = (float)hi[axis];
    /* in-plane axes as one would look at it: across x shows z right, y up; across y, x right, z up; across z, x right, y up */
    x->axU = axis == 0 ? 2 : 0;
    x->axV = axis == 1 ? 2 : 1;
    x->lo[0] = (float)lo[x->axU]; x->hi[0] = (float)hi[x->axU];
    x->lo[1] = (float)lo[x->axV]; x->hi[1] = (float)hi[x->axV];
    double span = hi[axis] - lo[axis];
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    /* nudged off the exact value: a plane through a layer of points would cut faces into slivers */
    double c = lo[axis] + span * (0.0005 + 0.999 * frac) + span * 1.234567e-6;
    x->slicePos = (float)c;
    for (int i = 0; i < SM.nb; i++) { pl_strlcpy(x->patch[i], SM.b[i].name, sizeof x->patch[i]); pl_strlcpy(x->ptype[i], SM.b[i].type, sizeof x->ptype[i]); }
    x->nPatch = SM.nb;
    long maxCell = -1; for (long i = 0; i < SM.nOwn; i++) if (SM.own[i] > maxCell) maxCell = SM.own[i];
    x->nCells = (int)(maxCell + 1);
    /* every face the plane crosses gives a segment, for its owner, its neighbour, and (boundary faces) its patch */
    long cap = 4096, n = 0, segCap = 1024;
    CutSeg *cs = malloc(sizeof(CutSeg) * (size_t)cap);
    x->seg = malloc(sizeof(float) * 4 * (size_t)segCap); x->segPatch = malloc(sizeof(int) * (size_t)segCap);
    if (!cs || !x->seg || !x->segPatch) { free(cs); snprintf(x->err, sizeof x->err, "Out of memory"); return false; }
    int bi = 0;
    for (long f = 0; f < SM.nf; f++) {
        long a = SM.off[f], z = SM.off[f + 1];
        if (z - a < 3) continue;
        float q[8][2]; int nq = 0;
        for (long k = a; k < z && nq < 8; k++) {
            const double *P = &SM.pt[SM.idx[k] * 3], *Q = &SM.pt[SM.idx[k + 1 < z ? k + 1 : a] * 3];
            double dp = P[axis] - c, dq = Q[axis] - c;
            if ((dp < 0) == (dq < 0)) continue;
            double t = dp / (dp - dq);
            q[nq][0] = (float)(P[x->axU] + t * (Q[x->axU] - P[x->axU]));
            q[nq][1] = (float)(P[x->axV] + t * (Q[x->axV] - P[x->axV]));
            nq++;
        }
        if (nq < 2) continue;
        for (int s2 = 0; s2 + 1 < nq; s2 += 2) {                /* a convex face: one pair; a bent one may give two */
            long cells[2] = { f < SM.nOwn ? SM.own[f] : -1, f < SM.nNei ? SM.nei[f] : -1 };
            for (int k = 0; k < 2; k++) {
                if (cells[k] < 0) continue;
                if (n == cap) { cap *= 2; CutSeg *nc = realloc(cs, sizeof(CutSeg) * (size_t)cap); if (!nc) { free(cs); return false; } cs = nc; }
                cs[n++] = (CutSeg){ (int)cells[k], q[s2][0], q[s2][1], q[s2 + 1][0], q[s2 + 1][1] };
            }
            if (f >= SM.nNei) {                                 /* a boundary face: its patch */
                while (bi + 1 < SM.nb && f >= SM.b[bi].start + SM.b[bi].nFaces) bi++;
                int pi = bi;
                if (f < SM.b[pi].start || f >= SM.b[pi].start + SM.b[pi].nFaces) for (pi = 0; pi < SM.nb && !(f >= SM.b[pi].start && f < SM.b[pi].start + SM.b[pi].nFaces); pi++) {}
                if (pi < SM.nb) {
                    if (x->nSeg == segCap) { segCap *= 2; x->seg = realloc(x->seg, sizeof(float) * 4 * (size_t)segCap); x->segPatch = realloc(x->segPatch, sizeof(int) * (size_t)segCap); }
                    float *g = &x->seg[x->nSeg * 4];
                    g[0] = q[s2][0]; g[1] = q[s2][1]; g[2] = q[s2 + 1][0]; g[3] = q[s2 + 1][1];
                    x->segPatch[x->nSeg++] = pi;
                }
            }
        }
    }
    /* each cell's segments chained into its polygon */
    qsort(cs, (size_t)n, sizeof *cs, cut_cmp);
    long vcap = n * 2 + 16; int pcap = 1024;
    x->u = malloc(sizeof(float) * (size_t)vcap); x->v = malloc(sizeof(float) * (size_t)vcap);
    x->start = malloc(sizeof(int) * (size_t)(pcap + 1)); x->cell = malloc(sizeof(int) * (size_t)pcap);
    long used = 0;
    float tol = (float)(fmax(hi[x->axU] - lo[x->axU], hi[x->axV] - lo[x->axV]) * 1e-7);
    for (long i = 0; i < n; ) {
        long j = i; while (j < n && cs[j].cell == cs[i].cell) j++;
        long k = j - i;
        if (k >= 3) {
            if (x->nPoly + 2 > pcap) { pcap *= 2; x->start = realloc(x->start, sizeof(int) * (size_t)(pcap + 1)); x->cell = realloc(x->cell, sizeof(int) * (size_t)pcap); }
            if (used + k + 2 > vcap) { vcap = (used + k + 2) * 2; x->u = realloc(x->u, sizeof(float) * (size_t)vcap); x->v = realloc(x->v, sizeof(float) * (size_t)vcap); }
            x->start[x->nPoly] = (int)used;
            x->cell[x->nPoly] = cs[i].cell;
            bool *done = calloc((size_t)k, 1);
            float cu = cs[i].u1, cv = cs[i].v1;
            x->u[used] = cs[i].u0; x->v[used] = cs[i].v0; used++;
            done[0] = true;
            for (long step = 1; step < k; step++) {
                long best = -1; bool flip = false; float bd = 1e30f;
                for (long m2 = 1; m2 < k; m2++) {
                    if (done[m2]) continue;
                    const CutSeg *g = &cs[i + m2];
                    float d0 = fabsf(g->u0 - cu) + fabsf(g->v0 - cv), d1 = fabsf(g->u1 - cu) + fabsf(g->v1 - cv);
                    if (d0 < bd) { bd = d0; best = m2; flip = false; }
                    if (d1 < bd) { bd = d1; best = m2; flip = true; }
                }
                if (best < 0) break;
                done[best] = true;
                x->u[used] = cu; x->v[used] = cv; used++;
                const CutSeg *g = &cs[i + best];
                cu = flip ? g->u0 : g->u1; cv = flip ? g->v0 : g->v1;
            }
            free(done);
            (void)tol;
            x->nPoly++;
        }
        i = j;
    }
    x->start[x->nPoly] = (int)used;
    free(cs);
    if (!x->nPoly) { snprintf(x->err, sizeof x->err, "The plane misses the mesh here: move it."); x->ok = true; return true; }
    x->ok = true;
    return true;
}
