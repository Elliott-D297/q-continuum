/* precheck.c -- the checks behind the Run page's "Before you run" panel (see precheck.h) */
#define _GNU_SOURCE
#include "precheck.h"
#include "physics.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================ machine */
/* QC_TEST_HOST="<free MB> <load>" fixes both readings, so screenshots of the Run page are the same on any machine */
static bool test_host(int which, double *v)
{
    const char *t = getenv("QC_TEST_HOST");
    double m, l;
    if (!t || sscanf(t, "%lf %lf", &m, &l) != 2) return false;
    *v = which ? l : m;
    return true;
}

double mem_available_mb(void)
{
    double v;
    if (test_host(0, &v)) return v;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256]; double kb = -1;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "MemAvailable: %lf kB", &kb) == 1) break;
    fclose(f);
    return kb < 0 ? -1 : kb / 1024.0;
}

double load_average(void)
{
    double v;
    if (test_host(1, &v)) return v;
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return -1;
    double l = -1;
    if (fscanf(f, "%lf", &l) != 1) l = -1;
    fclose(f);
    return l;
}

/* ============================================================================ mesh size */
/* the bounding box of constant/polyMesh/points, ASCII or binary. Cached by path and mtime: a big mesh takes a moment. */
bool mesh_bounds(const char *casePath, double lo[3], double hi[3])
{
    static char cpath[PL_PATH]; static time_t cmt; static double clo[3], chi[3]; static bool cok;
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, "constant/polyMesh/points");
    time_t mt = pl_mtime(p);
    if (!mt) return false;
    if (!strcmp(cpath, p) && cmt == mt) { memcpy(lo, clo, sizeof clo); memcpy(hi, chi, sizeof chi); return cok; }
    pl_strlcpy(cpath, p, sizeof cpath); cmt = mt; cok = false;
    long long sz = pl_file_size(p);
    if (sz <= 0 || sz > (400LL << 20)) return false;        /* compressed or very large: don't hold the window up */
    size_t len; char *t = pl_read_file(p, &len);
    if (!t) return false;
    bool binary = false;
    char *hdrEnd = strstr(t, "}");
    if (hdrEnd) { char *fmt = strstr(t, "format"); if (fmt && fmt < hdrEnd && strstr(fmt, "binary") && strstr(fmt, "binary") < hdrEnd) binary = true; }
    /* after the header: the count, then "(" */
    char *q = hdrEnd ? hdrEnd + 1 : t;
    while (*q && !isdigit((unsigned char)*q)) { if (*q == '/' && q[1] == '/') { while (*q && *q != '\n') q++; } else q++; }
    long n = strtol(q, &q, 10);
    while (*q && *q != '(') q++;
    if (!*q || n <= 0) { free(t); return false; }
    q++;
    for (int k = 0; k < 3; k++) { lo[k] = 1e300; hi[k] = -1e300; }
    long got = 0;
    if (binary) {
        if ((size_t)(q - t) + (size_t)n * 24 <= len) {
            const char *v = q;                                   /* 3 doubles a point (double-precision builds) */
            for (long i = 0; i < n; i++) for (int k = 0; k < 3; k++) {
                double x; memcpy(&x, v + (i * 3 + k) * 8, 8);
                if (x < lo[k]) lo[k] = x;
                if (x > hi[k]) hi[k] = x;
            }
            got = n;
        }
    } else {
        char *e = t + len;
        while (got < n && q < e) {
            while (q < e && *q != '(') q++;
            if (q >= e) break;
            double v[3];
            char *r = q + 1;
            bool ok = true;
            for (int k = 0; k < 3 && ok; k++) { char *s = r; v[k] = strtod(r, &r); if (r == s) ok = false; }
            if (!ok) break;
            for (int k = 0; k < 3; k++) { if (v[k] < lo[k]) lo[k] = v[k]; if (v[k] > hi[k]) hi[k] = v[k]; }
            got++;
            q = r;
        }
    }
    free(t);
    cok = got == n && got > 0 && isfinite(lo[0]) && isfinite(hi[0]);
    memcpy(clo, lo, sizeof clo); memcpy(chi, hi, sizeof chi);
    return cok;
}

/* "uniform (1 0 0)" -> 1; nonuniform lists and expressions -> -1 */
static double uniform_vec_mag(const char *v)
{
    const char *u = strstr(v, "uniform");
    if (!u || (u > v && u[-1] == 'n')) return -1;               /* "nonuniform" */
    const char *o = strchr(u, '(');
    if (!o) return -1;
    double a, b, c;
    if (sscanf(o + 1, "%lf %lf %lf", &a, &b, &c) != 3) return -1;
    return sqrt(a * a + b * b + c * c);
}

double field_max_velocity(const char *casePath)
{
    const char *cands[] = { "0.orig/U", "0/U", NULL };
    for (int i = 0; cands[i]; i++) {
        char p[PL_PATH]; pl_join(p, sizeof p, casePath, cands[i]);
        if (!pl_is_file(p)) continue;
        FDFile *f = fd_load(p);
        if (!f) continue;
        double best = -1;
        char v[512];
        if (fd_get(f, "internalField", v, sizeof v)) best = fmax(best, uniform_vec_mag(v));
        FDEntry *bf = fd_find(f, "boundaryField");
        for (int k = 0; bf && k < bf->nChild; k++) {
            FDEntry *pe = &bf->child[k];
            if (pe->kind != FD_DICT) continue;
            static const char *keys[] = { "value", "inletValue", "freestreamValue", "uniformValue", "refValue", NULL };
            for (int j = 0; keys[j]; j++) {
                FDEntry *e = fd_child(pe, keys[j]);
                if (e && fd_entry_value(f, e, v, sizeof v)) best = fmax(best, uniform_vec_mag(v));
            }
            /* flowRateInletVelocity, surfaceNormalFixedValue ...: a refValue or a scalar speed */
            FDEntry *rv = fd_child(pe, "refValue");
            if (rv && fd_entry_value(f, rv, v, sizeof v) && strstr(v, "uniform") && !strchr(v, '(')) {
                double s = fabs(atof(strstr(v, "uniform") + 7)); if (s > best) best = s;
            }
        }
        fd_free(f);
        return best;
    }
    return -1;
}

/* steady solvers have no time step to be stable at */
static bool steady_app(const char *app)
{
    return strstr(app, "Simple") || strstr(app, "simple") || !strcmp(app, "potentialFoam") || !strcmp(app, "laplacianFoam") ||
           !strcmp(app, "solidDisplacementFoam") || !strcmp(app, "solidFoam") || !strcmp(app, "scalarTransportFoam") ||
           !strcmp(app, "boundaryFoam") || !strcmp(app, "chemFoam") || !strcmp(app, "dsmcFoam") || !strcmp(app, "mdFoam") ||
           !strcmp(app, "foamyQuadMesh") || !app[0];
}

/* memory per cell, measured with v2606 on this kind of machine: icoFoam 0.85 kB, simpleFoam k-epsilon 1.3 kB, 3.1 M-cell
   chtMultiRegionFoam ~1.5 kB; each process adds ~150 MB of libraries and buffers */
static double kb_per_cell(const CaseSummary *s)
{
    const char *a = s->app;
    if (strstr(a, "eacting") || strstr(a, "fireFoam") || strstr(a, "XiFoam") || strstr(a, "Combust") || strstr(a, "chemFoam")) return 4.0;
    if (!strcmp(a, "icoFoam") || !strcmp(a, "potentialFoam") || !strcmp(a, "laplacianFoam") || !strcmp(a, "scalarTransportFoam")) return 0.9;
    switch (s->cat) {
    case CAT_INCOMP: return 1.3;
    case CAT_HEAT: return s->nRegions ? 1.5 : 1.6;
    case CAT_MULTI: return 2.5;
    case CAT_COMP: return 2.0;
    default: return 1.5;
    }
}

static bool has_empty_patch(const char *casePath)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, "constant/polyMesh/boundary");
    char *t = pl_read_file(p, NULL);
    bool e = t && (strstr(t, "type            empty") || strstr(t, "type empty") || strstr(t, "type\tempty"));
    if (t && !e) {                                               /* any spacing */
        for (char *q = strstr(t, "type"); q && !e; q = strstr(q + 4, "type")) {
            char *r = q + 4; while (*r == ' ' || *r == '\t') r++;
            if (!strncmp(r, "empty", 5)) e = true;
        }
    }
    free(t);
    return e;
}

void precheck_run(const CaseSummary *s, int procs, int ourProcs, int nCpu, PreCheck *out)
{
    memset(out, 0, sizeof *out);
    out->co_ = -1;
    if (procs < 1) procs = 1;
    /* ---------------- memory */
    long cells = s->nCells;
    if (cells <= 0 && !s->nRegions) {                       /* no mesh yet: blockMeshDict says how many cells it will make */
        char bp[PL_PATH], sp[PL_PATH];
        pl_join(bp, sizeof bp, s->path, "system/blockMeshDict"); pl_join(sp, sizeof sp, s->path, "system/snappyHexMeshDict");
        FDFile *bm = pl_is_file(bp) && !pl_is_file(sp) ? fd_load(bp) : NULL;
        if (bm) { BlockMeshInfo b; blockmesh_info(bm, &b); if (b.ok) cells = b.cells; fd_free(bm); }
    }
    out->memAvailMB = mem_available_mb();
    PreItem *m = &out->mem;
    snprintf(m->title, sizeof m->title, "Memory");
    if (cells <= 0) {
        m->level = PC_UNKNOWN;
        snprintf(m->brief, sizeof m->brief, "unknown until the mesh exists");
        snprintf(m->text, sizeof m->text, "Unknown until the mesh exists. Rule of thumb: about 1-2 kB per cell plus 150 MB per process.");
    } else {
        double need = procs * 150.0 + cells * kb_per_cell(s) / 1024.0;
        out->memNeedMB = need;
        char nb[32], ab[32];
        snprintf(nb, sizeof nb, need >= 1024 ? "%.1f GB" : "%.0f MB", need >= 1024 ? need / 1024 : need);
        snprintf(ab, sizeof ab, out->memAvailMB >= 1024 ? "%.1f GB" : "%.0f MB", out->memAvailMB >= 1024 ? out->memAvailMB / 1024 : out->memAvailMB);
        snprintf(m->brief, sizeof m->brief, "~%s of %s free", nb, ab);
        if (out->memAvailMB > 0 && need > 0.9 * out->memAvailMB) {
            m->level = PC_BAD;
            snprintf(m->text, sizeof m->text, "About %s for %ld cells on %d process%s, but only %s is free. Linux would likely kill the "
                     "run (signal 9). Use fewer processes, a coarser mesh, or close other programs first.", nb, cells, procs, procs == 1 ? "" : "es", ab);
        } else if (out->memAvailMB > 0 && need > 0.7 * out->memAvailMB) {
            m->level = PC_WARN;
            snprintf(m->text, sizeof m->text, "About %s of the %s free (%ld cells, %d process%s): tight. Meshing and decomposePar can need "
                     "more than the solver.", nb, ab, cells, procs, procs == 1 ? "" : "es");
        } else {
            m->level = PC_OK;
            snprintf(m->text, sizeof m->text, "About %s of the %s free (%ld cells, %d process%s).", nb, ab, cells, procs, procs == 1 ? "" : "es");
        }
    }
    /* ---------------- Courant number */
    PreItem *c = &out->co;
    snprintf(c->title, sizeof c->title, "Time step");
    char cd[PL_PATH]; pl_join(cd, sizeof cd, s->path, "system/controlDict");
    FDFile *f = fd_load(cd);
    char v[64] = "", w[64] = "";
    bool adjust = false; double maxCo = 0;
    if (f) {
        if (fd_get(f, "adjustTimeStep", v, sizeof v)) { fd_word(v, w, sizeof w); adjust = !strcmp(w, "yes") || !strcmp(w, "true") || !strcmp(w, "on"); }
        if (fd_get(f, "maxCo", v, sizeof v)) maxCo = atof(v);
        fd_free(f);
    }
    double lo[3], hi[3];
    if (steady_app(s->app)) {
        c->level = PC_OK;
        snprintf(c->brief, sizeof c->brief, "steady solver");
        snprintf(c->text, sizeof c->text, "%s is steady: no time step to keep stable.", s->app[0] ? s->app : "The solver");
    } else if (adjust) {
        c->level = PC_OK;
        snprintf(c->brief, sizeof c->brief, "adaptive, maxCo %g", maxCo > 0 ? maxCo : 1.0);
        snprintf(c->text, sizeof c->text, "Adaptive: the solver sizes each step for a Courant number of %g.", maxCo > 0 ? maxCo : 1.0);
    } else {
        /* the cell size: the smallest graded cell from blockMeshDict when it describes the mesh (or there is no mesh yet),
           else the average cell of the mesh's bounding box */
        double dx = -1;
        const char *which = "";
        char bp[PL_PATH]; pl_join(bp, sizeof bp, s->path, "system/blockMeshDict");
        char sp[PL_PATH]; pl_join(sp, sizeof sp, s->path, "system/snappyHexMeshDict");
        FDFile *bm = !s->nRegions && pl_is_file(bp) && !pl_is_file(sp) ? fd_load(bp) : NULL;
        if (bm) {
            BlockMeshInfo b; blockmesh_info(bm, &b);
            if (b.ok && (cells <= 0 || s->meshStale || b.cells == cells)) { dx = b.dxMinThin; which = cells <= 0 ? "smallest cell in blockMeshDict, before meshing" : "smallest cell"; }
            fd_free(bm);
        }
        if (dx <= 0 && cells > 0 && !s->nRegions && mesh_bounds(s->path, lo, hi)) {
            double L[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] };
            if (has_empty_patch(s->path)) {                      /* 2-D: the thin direction is the empty one */
                int k = L[0] <= L[1] && L[0] <= L[2] ? 0 : L[1] <= L[2] ? 1 : 2;
                dx = sqrt(L[(k + 1) % 3] * L[(k + 2) % 3] / cells);
            } else dx = cbrt(L[0] * L[1] * L[2] / cells);
            which = "average cell";
        }
        double U = field_max_velocity(s->path);
        if (dx <= 0) {
            c->level = PC_UNKNOWN;
            snprintf(c->brief, sizeof c->brief, "Courant number not estimated");
            snprintf(c->text, sizeof c->text, "Not estimated: %s. Keep Co = |U|·Δt/Δx below 1.",
                     s->nRegions ? "a multi-region case" : cells <= 0 ? "no mesh yet and no blockMeshDict to size the cells from" : "the mesh couldn't be sized");
        } else if (U <= 0 || s->deltaT <= 0) {
            c->level = PC_UNKNOWN;
            snprintf(c->brief, sizeof c->brief, "Courant number not estimated");
            snprintf(c->text, sizeof c->text, "Not estimated: no uniform velocity in the initial U or on its patches. Keep Co = |U|·Δt/Δx below 1 "
                     "(cells here are %.3g m).", dx);
        } else {
            double co = courant(U, s->deltaT, dx);
            out->co_ = co;
            double safe = dx / U;                                /* Co = 1 */
            char work[200], cs[32], ss[32];
            courant_text(U, s->deltaT, dx, work, sizeof work);
            human_num(co, cs, sizeof cs); human_num(safe, ss, sizeof ss);
            snprintf(c->brief, sizeof c->brief, "Co ≈ %s", cs);
            if (co > 5) {
                c->level = PC_WARN;
                pl_fmt_ui(c->text, sizeof c->text, "%s (%s): far above 1. PISO/PIMPLE and explicit solvers usually diverge like this, and "
                         "the answer is inaccurate even when they don't. Co = 1 needs Δt ≤ %s s; or set adjustTimeStep yes with maxCo 1.",
                         work, which, ss);
            } else if (co > 1) {
                c->level = PC_WARN;
                pl_fmt_ui(c->text, sizeof c->text, "%s (%s): above 1, and higher where |U| is larger than at the inlet or walls. It may "
                         "diverge. Co = 1 needs Δt ≤ %s s; or set adjustTimeStep yes with maxCo 1.", work, which, ss);
            } else {
                c->level = PC_OK;
                pl_fmt_ui(c->text, sizeof c->text, "%s (%s). Where the flow is faster than the boundary values, Co is higher.", work, which);
            }
        }
    }
    /* ---------------- cores */
    PreItem *k = &out->cores;
    snprintf(k->title, sizeof k->title, "Cores");
    double load = load_average();
    int external = load >= 0 ? (int)(load + 0.5) - ourProcs : 0;
    if (external < 0) external = 0;
    int busy = ourProcs + external;
    if (busy > nCpu) busy = nCpu;
    out->freeCores = nCpu - busy;
    snprintf(k->brief, sizeof k->brief, "%d process%s, %d of %d cores free", procs, procs == 1 ? "" : "es", out->freeCores, nCpu);
    if (procs > out->freeCores && procs > 1) {
        k->level = PC_WARN;
        snprintf(k->text, sizeof k->text, "%d process%s, but only %d of %d cores are free (%d busy%s): every process will run slower, often "
                 "slower overall than with %d.", procs, procs == 1 ? "" : "es", out->freeCores, nCpu, busy,
                 ourProcs ? " with your other runs" : "", out->freeCores > 1 ? out->freeCores : 1);
    } else {
        k->level = PC_OK;
        snprintf(k->text, sizeof k->text, "%d process%s on %d free of %d cores.", procs, procs == 1 ? "" : "es", out->freeCores, nCpu);
    }
    int lv[3] = { m->level, c->level, k->level };
    for (int i = 0; i < 3; i++) if (lv[i] != PC_UNKNOWN && lv[i] > out->worst) out->worst = lv[i];
}
