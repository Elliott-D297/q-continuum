/* page_viz.c -- Results > Visualization (docs/VISUALIZATION_PLAN.md): a scene of displayers over the case's mesh and fields,
 * after STAR-CCM+'s scenes. Boundary surfaces, section planes, iso-surfaces, thresholds, streamlines, vectors, LIC, volume
 * rendering and line probes, each coloured by any field (smooth, banded or cell values, twelve colour maps, auto / whole
 * field / manual ranges, log scale, clamp or hide beyond the range, iso-lines); one or two views with standard and saved
 * views, perspective or parallel, mirror and periodic copies, lighting and background; a probe on click; scalar bars,
 * title, time and a statistic drawn in the image; transient playback; screenshots and movies; the scene saved in
 * <case>/visualization/scene.ini.
 *
 * The engine (meshes, fields, builders) is viz.c, the colour maps vizcolor.c. Surfaces are GPU meshes drawn with a small
 * shader (colour-map texture looked up per pixel, headlight shading), so turning a view costs a redraw of what is already
 * on the GPU; the textures' coordinates are rewritten only when a range changes. */
#define _GNU_SOURCE
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "app.h"
#include "foamdict.h"
#include "orbit.h"
#include "raymath.h"
#include "rlgl.h"
#include "simsetup.h"
#include "viz.h"
#include "vizcolor.h"

/* ================================================================ the scene */
enum { DK_BOUNDARY = 0, DK_SECTION, DK_ISO, DK_THRESHOLD, DK_STREAM, DK_VECTOR, DK_LIC, DK_VOLUME, DK_LINE, DK__N };
static const char *const DK_NAME[DK__N] = { "Boundary surfaces", "Section plane", "Iso-surface", "Threshold", "Streamlines",
                                            "Vectors", "Surface flow (LIC)", "Volume rendering", "Line probe" };
enum { RM_PART = 0, RM_FIELD, RM_MANUAL };
static const char *const RM_NAME[3] = { "Auto: this part", "Auto: whole field", "Manual" };
enum { SS_LINES = 0, SS_TUBES, SS_RIBBONS };

/* what to colour by: a field (with a vector's magnitude or component) or a derived quantity ("@" + its index) */
typedef struct { char field[64]; int q; } Qty;

#define MAX_DISP 24
typedef struct {
    int    kind;
    bool   vis;
    char   name[48];
    Qty    col;                       /* field "" : a solid colour                          */
    unsigned char rgb[3];
    int    cmap, style, levels, rangeMode, clip;
    bool   rev, logs, isolines, edges;
    int    nIso;
    double lo, hi;                    /* the manual range                                    */
    float  opacity;
    bool   show[VZ_MAX_PATCHES];      /* boundary; vectors on the boundary                   */
    double o[3], n[3];                /* section, LIC, vector plane; the boundary's clip     */
    bool   clipOn;
    Qty    iso;                       /* iso-surface / threshold: what is tested             */
    double isoVal[4]; int nIsoVal;
    double tlo, thi;
    VzSeeds seeds;
    int    sstyle; float swidth; bool sanim;
    int    vtarget;                   /* 0 a plane (o, n), 1 the boundary                    */
    double vspacing, vlen;            /* m (0: automatic)                                    */
    int    vstyle;                    /* 0 lines, 1 cones                                    */
    bool   vscale, vproject;
    int    licRes; double licSize;
    int    volN; float volAlpha;
    double la[3], lb[3]; int lN;
    char   lref[PL_PATH];             /* a line probe's reference data (plan A3): CSV, distance (m) and value */
    double *rx, *ry; int rn;
    /* built */
    bool   dirty, texDirty, built, building;
    int    uid;                       /* which displayer a finished build belongs to             */
    char   err[200];
    VzTris t;                         /* surfaces (or tubes, cones)                          */
    VzLines l;                        /* streamlines, iso-lines (in iso)                    */
    VzLines il;                       /* iso-lines over the surface                          */
    float *gp, *gv, *gval; long ng;
    float *lic, *licVal; int licW, licH; double eu[3], ev[3];
    float *vol; int vn[3];
    double *px, *pv; int pn;
    double dmin, dmax;                /* the built geometry's values                         */
    VzSurfReport rep; bool haveRep;   /* a section plane: area, mean, flow through it        */
    double fmin, fmax;                /* the whole field's                                   */
    Mesh   gm; bool gmOk;             /* the surfaces on the GPU                             */
    Texture2D tex; bool texOk; int texKey;
    Texture2D licTex; bool licOk;
    Texture2D volTex[3][128]; int volTexN[3]; bool volOk;
} Disp;

typedef struct { char name[32]; Orbit O; bool persp; } SavedView;

static struct {
    char   path[PL_PATH];
    char   times[256][32]; int nTimes, ti;
    bool   timeProc[256];             /* only in the processors (a parallel run not reconstructed) */
    char   fields[64][64]; int fieldNc[64], nFields;
    /* the mesh, loaded on a thread */
    VzMesh m; bool mOk, mLoading; char mErr[200];
    pthread_t th; pthread_mutex_t mu; VzMesh mNew; bool mNewReady, thRunning;
    /* fields at the current time */
    struct { char name[64]; char time[32]; VzField f; bool ok; long long sig; } cache[24]; int nCache;
    double rescanAt;                  /* the times and fields looked at again every couple of seconds (a run going on) */
    long long meshSig;                /* constant/polyMesh/owner as it was read                 */
    double rho;
    Disp   d[MAX_DISP]; int nd, sel;
    /* views */
    Orbit  O[2]; int nViews; bool linked, persp, fitted[2], touched[2];
    RenderTexture2D rt[2]; bool rtOk[2]; int rtW[2], rtH[2]; bool dirty[2];
    bool   dragging, panning; int dragView; Vector2 last; double lastClick;
    SavedView sv[12]; int nsv;
    /* options */
    bool   mirror[3]; int periodic, periodicAxis; bool light; int bg;
    char   title[96]; bool showTime, showStat, showLogo;
    /* playback and recording */
    bool   playing; double playAt; float fps;
    int    rec;                       /* 0 none, 1 time frames, 2 orbit frames, 3 a plane swept */
    int    recStep, recN; char recDir[PL_PATH];
    int    sweepDisp, sweepUid; double sweepLo, sweepHi, sweepO0[3]; bool recPlaced;   /* rec 3: a plane swept through the mesh */
    int    shotMag;
    /* probe */
    bool   probeOk; double probeX[3], probeV; char probeTxt[200]; int probeDisp;
    Rectangle probeBox;               /* the probe's readout and its button, last frame: not the view's clicks */
    /* panels */
    UiScroll left; bool histOn;
    Shader sh; bool shOk; int locView, locLit;
    bool   loaded;                    /* the scene file was read                             */
    double saveAt; bool saveDirty;
} V = { .sel = -1, .nViews = 1, .linked = true, .light = true, .showTime = true, .fps = 6, .shotMag = 2 };

static const Color BG[3] = { { 10, 14, 22, 255 }, { 236, 240, 247, 255 }, { 0, 0, 0, 255 } };
static const char *BG_NAME[3] = { "Dark", "Light", "Black" };

static void mark_all(void) { for (int i = 0; i < V.nd; i++) V.d[i].dirty = true; V.dirty[0] = V.dirty[1] = true; V.saveDirty = true; }
static void redraw_all(void) { V.dirty[0] = V.dirty[1] = true; V.saveDirty = true; }

/* ================================================================ case, times, fields */

/* the times: the case's own, and a parallel run's that are only in processor0 (read from the processors) */
typedef struct { char name[32]; bool proc; } TimeDir;
static int cmp_tdir(const void *a, const void *b) { double x = atof(((const TimeDir *)a)->name), y = atof(((const TimeDir *)b)->name); return (x > y) - (x < y); }

static int list_times(const char *dir, TimeDir *out, int n, int max, bool proc)
{
    DIR *dr = opendir(dir);
    if (!dr) return n;
    struct dirent *e;
    while ((e = readdir(dr)) && n < max) {
        char *end; strtod(e->d_name, &end);
        if (end == e->d_name || *end || strlen(e->d_name) >= 32) continue;
        char p[PL_PATH]; pl_join(p, sizeof p, dir, e->d_name);
        if (!pl_is_dir(p)) continue;
        bool dup = false;
        for (int i = 0; i < n && !dup; i++) dup = atof(out[i].name) == atof(e->d_name);
        if (dup) continue;                                       /* the case's own copy wins */
        pl_strlcpy(out[n].name, e->d_name, sizeof out[n].name); out[n].proc = proc; n++;
    }
    closedir(dr);
    return n;
}

static void scan_times(void)
{
    TimeDir t[256];
    int n = list_times(V.path, t, 0, 256, false);
    char p0[PL_PATH]; pl_join(p0, sizeof p0, V.path, "processor0");
    n = list_times(p0, t, n, 256, true);
    qsort(t, (size_t)n, sizeof t[0], cmp_tdir);
    V.nTimes = n;
    for (int i = 0; i < n; i++) { pl_strlcpy(V.times[i], t[i].name, sizeof V.times[0]); V.timeProc[i] = t[i].proc; }
    if (V.ti >= V.nTimes || V.ti < 0) V.ti = V.nTimes - 1;
}

static void scan_fields(void)
{
    V.nFields = 0;
    if (V.ti < 0) return;
    char rel[64], d[PL_PATH];
    snprintf(rel, sizeof rel, "%s%s", V.timeProc[V.ti] ? "processor0/" : "", V.times[V.ti]);
    pl_join(d, sizeof d, V.path, rel);
    DIR *dr = opendir(d);
    if (!dr) return;
    struct dirent *e;
    while ((e = readdir(dr)) && V.nFields < 64) {
        if (e->d_name[0] == '.') continue;
        char p[PL_PATH]; pl_join(p, sizeof p, d, e->d_name);
        if (!pl_is_file(p)) continue;
        char *h = pl_read_head(p, 2048);
        if (!h) continue;
        int nc = strstr(h, "volVectorField") ? 3 : strstr(h, "volScalarField") ? 1 : 0;
        free(h);
        if (!nc) continue;
        pl_strlcpy(V.fields[V.nFields], e->d_name, sizeof V.fields[0]);
        V.fieldNc[V.nFields++] = nc;
    }
    closedir(dr);
    for (int i = 0; i < V.nFields; i++) for (int j = i + 1; j < V.nFields; j++)
        if (strcmp(V.fields[j], V.fields[i]) < 0) {
            char t[64]; memcpy(t, V.fields[i], 64); memcpy(V.fields[i], V.fields[j], 64); memcpy(V.fields[j], t, 64);
            int n = V.fieldNc[i]; V.fieldNc[i] = V.fieldNc[j]; V.fieldNc[j] = n;
        }
}

static bool has_field(const char *n) { for (int i = 0; i < V.nFields; i++) if (!strcmp(V.fields[i], n)) return true; return false; }

static void worker_wait(void);
static void cache_clear(void) { for (int i = 0; i < V.nCache; i++) vz_field_free(&V.cache[i].f); V.nCache = 0; }

static VzField *field_of(const char *name);
/* a derived field ("@k"): built from p and U at the current time */
static VzField *derived(int k)
{
    char nm[64]; snprintf(nm, sizeof nm, "@%d", k);
    const char *t = V.ti >= 0 ? V.times[V.ti] : "";
    for (int i = 0; i < V.nCache; i++) if (!strcmp(V.cache[i].name, nm) && !strcmp(V.cache[i].time, t)) return V.cache[i].ok ? &V.cache[i].f : NULL;
    if (V.nCache >= 22) { worker_wait(); cache_clear(); }         /* room for p, U and this, so loading them can't clear it */
    VzField *p = has_field("p") ? field_of("p") : NULL, *U = has_field("U") ? field_of("U") : NULL;
    int slot = V.nCache++;
    pl_strlcpy(V.cache[slot].name, nm, sizeof V.cache[slot].name); pl_strlcpy(V.cache[slot].time, t, sizeof V.cache[slot].time);
    V.cache[slot].ok = vz_derived(&V.cache[slot].f, &V.m, k, p, U, V.rho);
    return V.cache[slot].ok ? &V.cache[slot].f : NULL;
}

/* which version of a field is on disk: its file (a decomposed time: processor0's) */
static long long field_sig(const char *time, bool proc, const char *name)
{
    char rel[PL_PATH], p[PL_PATH]; snprintf(rel, sizeof rel, "%s%s/%s", proc ? "processor0/" : "", time, name);
    pl_join(p, sizeof p, V.path, rel);
    FDSig g; return fd_sig(p, &g) ? g.sec * 1000003LL + g.nsec + g.size * 7 + g.ino * 13 : 0;
}

static VzField *field_of(const char *name)
{
    if (!name || !name[0] || !V.mOk || V.ti < 0) return NULL;
    if (name[0] == '@') return derived(atoi(name + 1));
    const char *t = V.times[V.ti];
    for (int i = 0; i < V.nCache; i++) if (!strcmp(V.cache[i].name, name) && !strcmp(V.cache[i].time, t)) return V.cache[i].ok ? &V.cache[i].f : NULL;
    if (V.nCache >= 24) { worker_wait(); cache_clear(); }
    int slot = V.nCache++;
    pl_strlcpy(V.cache[slot].name, name, sizeof V.cache[slot].name); pl_strlcpy(V.cache[slot].time, t, sizeof V.cache[slot].time);
    V.cache[slot].sig = field_sig(t, V.timeProc[V.ti], name);
    if (V.timeProc[V.ti]) {
        V.cache[slot].ok = V.m.nProc > 0 && vz_field_load_procs(&V.cache[slot].f, &V.m, V.path, t, name);
        if (!V.m.nProc && V.m.err[0]) ui_toast(TOAST_ERR, "%s", V.m.err);
    } else V.cache[slot].ok = vz_field_load(&V.cache[slot].f, &V.m, V.path, t, name);
    return V.cache[slot].ok ? &V.cache[slot].f : NULL;
}

/* a quantity's words, with its units from the field's dimensions */
static void unit_of(const char *dims, char *out, int cap)
{
    static const struct { const char *d, *u; } U[] = {
        { "[0 2 -2 0 0 0 0]", "m²/s², kinematic" }, { "[1 -1 -2 0 0 0 0]", "Pa" }, { "[0 1 -1 0 0 0 0]", "m/s" },
        { "[0 0 0 1 0 0 0]", "K" }, { "[0 2 -3 0 0 0 0]", "m²/s³" }, { "[0 0 -1 0 0 0 0]", "1/s" }, { "[0 0 -2 0 0 0 0]", "1/s²" },
        { "[0 2 -1 0 0 0 0]", "m²/s" }, { "[1 -3 0 0 0 0 0]", "kg/m³" }, { "[0 0 0 0 0 0 0]", "" }, { "[0 3 0 0 0 0 0]", "m³" },
        { "[0 1 -2 0 0 0 0]", "m/s²" }, { "[1 -1 -1 0 0 0 0]", "Pa s" } };
    out[0] = 0;
    for (size_t i = 0; i < sizeof U / sizeof U[0]; i++) if (!strcmp(dims, U[i].d)) { pl_strlcpy(out, U[i].u, (size_t)cap); return; }
    pl_strlcpy(out, dims, (size_t)cap);
}

static void qty_label(const Qty *q, char *out, int cap)
{
    if (!q->field[0]) { pl_strlcpy(out, "Solid colour", (size_t)cap); return; }
    VzField *f = field_of(q->field);
    char u[64] = "";
    if (f) unit_of(f->dims, u, sizeof u);
    const char *base = q->field[0] == '@' ? VD_NAME[atoi(q->field + 1)] : q->field;
    static const char *const QN[5] = { "", " magnitude", " x", " y", " z" };
    const char *qn = f && f->nc == 3 ? QN[q->q] : "";
    if (u[0]) pl_fmt_ui(out, (size_t)cap, "%s%s (%s)", base, qn, u); else pl_fmt_ui(out, (size_t)cap, "%s%s", base, qn);
}

/* the colour-by choices: each field (vectors: magnitude, x, y, z), the derived quantities, a solid colour */
typedef struct { Qty q; char label[96]; } Choice;
static int choices(Choice *c, int max, bool solid, bool vectorsOnly)
{
    int n = 0;
    if (solid && n < max) { c[n].q.field[0] = 0; c[n].q.q = 0; pl_strlcpy(c[n].label, "Solid colour", sizeof c[n].label); n++; }
    for (int i = 0; i < V.nFields && n < max; i++) {
        if (vectorsOnly && V.fieldNc[i] != 3) continue;
        if (V.fieldNc[i] == 1 || vectorsOnly) { pl_strlcpy(c[n].q.field, V.fields[i], 64); c[n].q.q = vectorsOnly ? VQ_MAG : VQ_VALUE; pl_strlcpy(c[n].label, V.fields[i], sizeof c[n].label); n++; continue; }
        static const char *const S[4] = { " magnitude", " x", " y", " z" };
        for (int k = 0; k < 4 && n < max; k++) {
            pl_strlcpy(c[n].q.field, V.fields[i], 64); c[n].q.q = VQ_MAG + k;
            snprintf(c[n].label, sizeof c[n].label, "%s%s", V.fields[i], S[k]); n++;
        }
    }
    if (!vectorsOnly) {
        bool p = has_field("p"), U = has_field("U");
        for (int k = 1; k < VD__N && n < max; k++) {
            bool ok = k == VD_CELLVOL || ((k == VD_PPA) && p) || (k == VD_PTOTAL && p && U) || (k >= VD_GRADMAG && k <= VD_HELICITY && U);
            if (!ok) continue;
            snprintf(c[n].q.field, 64, "@%d", k); c[n].q.q = VQ_VALUE;
            pl_strlcpy(c[n].label, k == VD_PPA ? "Pressure in Pa (p times the density)" : VD_NAME[k], sizeof c[n].label); n++;
        }
    }
    return n;
}

/* ================================================================ the mesh, on a thread */
static void *load_thread(void *arg)
{
    char *cs = arg, dir[PL_PATH], p0[PL_PATH];
    pl_join(dir, sizeof dir, cs, "constant/polyMesh");
    VzMesh m; vz_mesh_load(&m, dir);
    pl_join(p0, sizeof p0, cs, "processor0");
    if (m.ok && pl_is_dir(p0)) vz_mesh_procs(&m, cs);          /* a parallel run's maps, for times only the processors hold */
    free(cs);
    pthread_mutex_lock(&V.mu);
    V.mNew = m; V.mNewReady = true;
    pthread_mutex_unlock(&V.mu);
    return NULL;
}

static void start_load(void)
{
    if (V.thRunning) return;
    char *dir = malloc(PL_PATH);
    if (!dir) { snprintf(V.mErr, sizeof V.mErr, "Out of memory"); return; }
    pl_strlcpy(dir, V.path, PL_PATH);
    V.mLoading = true; V.thRunning = true; V.mErr[0] = 0;
    if (pthread_create(&V.th, NULL, load_thread, dir) != 0) { free(dir); V.mLoading = V.thRunning = false; snprintf(V.mErr, sizeof V.mErr, "Could not start reading the mesh"); }
}

static void poll_load(void)
{
    if (!V.thRunning) return;
    pthread_mutex_lock(&V.mu);
    bool ready = V.mNewReady;
    pthread_mutex_unlock(&V.mu);
    if (!ready) { ui_request_frames(2); return; }
    pthread_join(V.th, NULL);
    worker_wait();
    V.thRunning = false; V.mLoading = false; V.mNewReady = false;
    vz_mesh_free(&V.m);
    V.m = V.mNew;
    V.mOk = V.m.ok;
    if (!V.mOk) pl_strlcpy(V.mErr, V.m.err, sizeof V.mErr);
    cache_clear();
    mark_all();
    V.fitted[0] = V.fitted[1] = false;
}

/* ================================================================ displayers */
/* a line probe's reference data: the first two numbers of each line of a CSV (or space-separated) file, header lines
   skipped: distance along the line from its start (m), value (plan A3: measurements or another code's answer) */
static void load_ref(Disp *d)
{
    free(d->rx); free(d->ry); d->rx = d->ry = NULL; d->rn = 0;
    if (!d->lref[0]) return;
    char p[PL_PATH]; pl_expand_home(p, sizeof p, d->lref);
    char *t = pl_read_file(p, NULL);
    if (!t) return;
    int cap = 0;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        for (char *q = l; *q; q++) if (*q == ',' || *q == ';' || *q == '\t') *q = ' ';
        double a, b;
        if (sscanf(l, "%lf %lf", &a, &b) != 2 || !isfinite(a) || !isfinite(b)) continue;
        if (d->rn == cap) {
            cap = cap ? cap * 2 : 256;
            double *nx = realloc(d->rx, sizeof(double) * (size_t)cap), *ny = nx ? realloc(d->ry, sizeof(double) * (size_t)cap) : NULL;
            if (!nx || !ny) { free(nx ? nx : d->rx); free(d->ry); d->rx = d->ry = NULL; d->rn = 0; free(t); return; }
            d->rx = nx; d->ry = ny;
        }
        d->rx[d->rn] = a; d->ry[d->rn] = b; d->rn++;
    }
    free(t);
}

static void disp_free_cpu(Disp *d)
{
    vz_tris_free(&d->t); vz_lines_free(&d->l); vz_lines_free(&d->il);
    free(d->gp); free(d->gv); free(d->gval); d->gp = d->gv = d->gval = NULL; d->ng = 0;
    free(d->lic); free(d->licVal); d->lic = d->licVal = NULL;
    free(d->vol); d->vol = NULL;
    free(d->px); free(d->pv); d->px = d->pv = NULL; d->pn = 0;
    free(d->rx); free(d->ry); d->rx = d->ry = NULL; d->rn = 0;
}

static void disp_free_built(Disp *d)
{
    disp_free_cpu(d);
    if (d->gmOk) { UnloadMesh(d->gm); d->gmOk = false; }
    if (d->licOk) { UnloadTexture(d->licTex); d->licOk = false; }
    if (d->volOk) { for (int a = 0; a < 3; a++) for (int k = 0; k < d->volTexN[a]; k++) UnloadTexture(d->volTex[a][k]); d->volOk = false; }
    d->built = false;
}

/* a displayer gone for good (deleted, or the case left): its colour texture too */
static void disp_release(Disp *d)
{
    disp_free_built(d);
    if (d->texOk) { UnloadTexture(d->tex); d->texOk = false; }
}

static void centre_of_mesh(double c[3]) { for (int k = 0; k < 3; k++) c[k] = (V.m.lo[k] + V.m.hi[k]) / 2; }
static double lic_side(void) { return fmax(V.m.hi[0] - V.m.lo[0], fmax(V.m.hi[1] - V.m.lo[1], V.m.hi[2] - V.m.lo[2])); }
static double diag_of_mesh(void) { double s = 0; for (int k = 0; k < 3; k++) s += (V.m.hi[k] - V.m.lo[k]) * (V.m.hi[k] - V.m.lo[k]); return sqrt(s); }

/* the default colour quantity: the pressure, else the first field */
static Qty default_qty(void)
{
    Qty q = { "", 0 };
    VzField *p = has_field("p") ? field_of("p") : NULL;
    if (p && p->nc == 1 && strstr(p->dims, "[0 2 -2")) snprintf(q.field, sizeof q.field, "@%d", VD_PPA);   /* incompressible: in Pa */
    else if (has_field("p")) pl_strlcpy(q.field, "p", sizeof q.field);
    else if (has_field("U")) { pl_strlcpy(q.field, "U", sizeof q.field); q.q = VQ_MAG; }
    else if (V.nFields) { pl_strlcpy(q.field, V.fields[0], sizeof q.field); q.q = V.fieldNc[0] == 3 ? VQ_MAG : VQ_VALUE; }
    return q;
}

/* a displayer's defaults for the case's mesh and fields */
static void disp_defaults(Disp *d, int kind)
{
    memset(d, 0, sizeof *d);
    d->kind = kind; d->vis = true; d->dirty = true;
    snprintf(d->name, sizeof d->name, "%s", DK_NAME[kind]);
    d->col = default_qty();
    d->rgb[0] = 180; d->rgb[1] = 190; d->rgb[2] = 205;
    d->cmap = VC_BLUERED; d->levels = 12; d->nIso = 10; d->opacity = 1;
    d->rangeMode = RM_FIELD;                  /* one range for the field across the scene: the same colour means the same value */
    d->style = VC_SMOOTH;
    for (int p = 0; p < VZ_MAX_PATCHES; p++) d->show[p] = p < V.m.nPatch && strcmp(V.m.patch[p].type, "empty") && strcmp(V.m.patch[p].type, "processor") && strcmp(V.m.patch[p].type, "wedge");
    double c[3]; centre_of_mesh(c);
    memcpy(d->o, c, sizeof c); d->n[0] = 0; d->n[1] = 0; d->n[2] = 1;
    /* a plane across the thin direction of a 2-D mesh would miss it: then along it */
    double L[3]; for (int k = 0; k < 3; k++) L[k] = V.m.hi[k] - V.m.lo[k];
    int thin = L[0] <= L[1] && L[0] <= L[2] ? 0 : L[1] <= L[2] ? 1 : 2;
    if (L[thin] < 0.05 * fmax(L[0], fmax(L[1], L[2]))) { d->n[2] = 0; d->n[thin] = 1; }
    d->iso = d->col;
    VzField *f = field_of(d->iso.field);
    if (f) { VzStats s; vz_stats_cells(&V.m, f, d->iso.q, &s); d->isoVal[0] = (s.min + s.max) / 2; d->tlo = s.min + 0.5 * (s.max - s.min); d->thi = s.max; }
    d->nIsoVal = 1;
    double Lmax = fmax(L[0], fmax(L[1], L[2]));
    d->seeds.kind = VS_POINT; d->seeds.n = 60; d->seeds.dir = 2;     /* a ball in the middle, both ways; an inlet if there is one */
    for (int p = 0; p < V.m.nPatch; p++) if (strstr(V.m.patch[p].name, "inlet") || strstr(V.m.patch[p].name, "Inlet")) { d->seeds.kind = VS_PATCH; d->seeds.dir = 0; d->seeds.patch = p; break; }
    memcpy(d->seeds.a, c, sizeof c); d->seeds.b[0] = 0.25 * Lmax; d->seeds.b[2] = 1; d->seeds.size = 0.6 * Lmax;
    d->sstyle = SS_LINES; d->swidth = 1;
    if (kind == DK_STREAM || kind == DK_VECTOR || kind == DK_LIC || kind == DK_VOLUME) { if (has_field("U")) { pl_strlcpy(d->col.field, "U", 64); d->col.q = VQ_MAG; } }
    d->vtarget = 0; d->vspacing = 0; d->vlen = 0; d->vscale = true; d->vstyle = 0;
    d->licRes = 512; d->licSize = 0;
    (void)Lmax;
    d->volN = 64; d->volAlpha = 1.0f;
    for (int k = 0; k < 3; k++) { d->la[k] = V.m.lo[k] + 0.1 * L[k]; d->lb[k] = V.m.hi[k] - 0.1 * L[k]; }
    d->lN = 200;
}

static Disp *add_disp(int kind)
{
    if (V.nd >= MAX_DISP) return NULL;
    Disp *d = &V.d[V.nd++];
    disp_defaults(d, kind);
    V.saveDirty = true;
    return d;
}

/* room for cap triangles; false when memory runs out (the arrays stay valid at their old size) */
static bool tris_reserve(VzTris *t, long cap)
{
    void *q;
    if (!(q = realloc(t->xyz, sizeof(float) * 9 * (size_t)cap))) return false;
    t->xyz = q;
    if (!(q = realloc(t->val, sizeof(float) * 3 * (size_t)cap))) return false;
    t->val = q;
    if (!(q = realloc(t->nrm, sizeof(float) * 3 * (size_t)cap))) return false;
    t->nrm = q;
    if (!(q = realloc(t->cell, sizeof(int) * (size_t)cap))) return false;
    t->cell = q;
    if (!(q = realloc(t->grp, sizeof(int) * (size_t)cap))) return false;
    t->grp = q;
    return true;
}

static void tris_append(VzTris *a, const VzTris *b)
{
    if (!b->n) return;
    long n = a->n + b->n;
    if (!tris_reserve(a, n)) return;                       /* out of memory: b is left out */
    memcpy(a->xyz + 9 * a->n, b->xyz, sizeof(float) * 9 * (size_t)b->n);
    memcpy(a->val + 3 * a->n, b->val, sizeof(float) * 3 * (size_t)b->n);
    memcpy(a->nrm + 3 * a->n, b->nrm, sizeof(float) * 3 * (size_t)b->n);
    memcpy(a->cell + a->n, b->cell, sizeof(int) * (size_t)b->n);
    memcpy(a->grp + a->n, b->grp, sizeof(int) * (size_t)b->n);
    a->n = n;
}

/* tubes or ribbons along the streamlines: triangles with the lines' values */
static void sweep_lines(const VzLines *l, int style, double w, VzTris *out)
{
    memset(out, 0, sizeof *out);
    long cap = 0;
    for (long k = 0; k < l->nl; k++) {
        long a = l->off[k], b = l->off[k + 1];
        double ref[3] = { 0, 0, 1 };
        for (long i = a; i + 1 < b; i++) {
            double p0[3] = { l->xyz[3 * i], l->xyz[3 * i + 1], l->xyz[3 * i + 2] }, p1[3] = { l->xyz[3 * i + 3], l->xyz[3 * i + 4], l->xyz[3 * i + 5] };
            double t[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, tl = sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            if (tl <= 0) continue;
            for (int d = 0; d < 3; d++) t[d] /= tl;
            /* a frame carried along the line: u square to the tangent, as close to the last one as possible */
            double u[3] = { ref[1] * t[2] - ref[2] * t[1], ref[2] * t[0] - ref[0] * t[2], ref[0] * t[1] - ref[1] * t[0] };
            double ul = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            if (ul < 1e-6) { double alt[3] = { 1, 0, 0 }; if (fabs(t[0]) > 0.9) { alt[0] = 0; alt[1] = 1; } u[0] = alt[1] * t[2] - alt[2] * t[1]; u[1] = alt[2] * t[0] - alt[0] * t[2]; u[2] = alt[0] * t[1] - alt[1] * t[0]; ul = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]); }
            for (int d = 0; d < 3; d++) u[d] /= ul;
            double v[3] = { t[1] * u[2] - t[2] * u[1], t[2] * u[0] - t[0] * u[2], t[0] * u[1] - t[1] * u[0] };
            ref[0] = v[0]; ref[1] = v[1]; ref[2] = v[2];
            int sides = style == SS_TUBES ? 6 : 1;
            for (int s = 0; s < sides; s++) {
                double a0 = 2 * M_PI * s / sides, a1 = 2 * M_PI * (s + 1) / sides;
                double d0[3], d1[3];
                for (int d = 0; d < 3; d++) {
                    if (style == SS_TUBES) { d0[d] = w * (cos(a0) * u[d] + sin(a0) * v[d]); d1[d] = w * (cos(a1) * u[d] + sin(a1) * v[d]); }
                    else { d0[d] = -w * u[d]; d1[d] = w * u[d]; }
                }
                if (out->n + 2 > cap) {
                    long nc = cap ? cap * 2 : 4096;
                    if (!tris_reserve(out, nc)) return;    /* out of memory: the tubes made so far */
                    cap = nc;
                }
                double q[4][3]; float vv[4] = { l->val[i], l->val[i], l->val[i + 1], l->val[i + 1] };
                for (int d = 0; d < 3; d++) { q[0][d] = p0[d] + d0[d]; q[1][d] = p0[d] + d1[d]; q[2][d] = p1[d] + d1[d]; q[3][d] = p1[d] + d0[d]; }
                int tri[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
                for (int h = 0; h < 2; h++) {
                    long id = out->n++;
                    for (int c = 0; c < 3; c++) { for (int d = 0; d < 3; d++) out->xyz[9 * id + 3 * c + d] = (float)q[tri[h][c]][d]; out->val[3 * id + c] = vv[tri[h][c]]; }
                    double nm[3] = { 0, 0, 0 };
                    for (int d = 0; d < 3; d++) nm[d] = style == SS_TUBES ? (cos((a0 + a1) / 2) * u[d] + sin((a0 + a1) / 2) * v[d]) : v[d];
                    for (int d = 0; d < 3; d++) out->nrm[3 * id + d] = (float)nm[d];
                    out->cell[id] = -1; out->grp[id] = -1;
                }
            }
        }
    }
}

/* cones for vector glyphs: one per glyph, along its vector */
static void cones(const float *gp, const float *gv, const float *gval, long ng, double len, bool scale, double vmax, VzTris *out)
{
    memset(out, 0, sizeof *out);
    if (!ng) return;
    const int S = 8;
    long cap = ng * S * 2;
    if (!tris_reserve(out, cap)) { vz_tris_free(out); return; }
    for (long g = 0; g < ng; g++) {
        double v[3] = { gv[3 * g], gv[3 * g + 1], gv[3 * g + 2] }, m = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(m > 0)) continue;
        double L = len * (scale && vmax > 0 ? m / vmax : 1), r = 0.18 * len * (scale && vmax > 0 ? fmax(0.3, m / vmax) : 1);
        double t[3] = { v[0] / m, v[1] / m, v[2] / m }, a[3] = { 1, 0, 0 };
        if (fabs(t[0]) > 0.9) { a[0] = 0; a[1] = 1; }
        double u[3] = { t[1] * a[2] - t[2] * a[1], t[2] * a[0] - t[0] * a[2], t[0] * a[1] - t[1] * a[0] }, ul = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        for (int d = 0; d < 3; d++) u[d] /= ul;
        double w[3] = { t[1] * u[2] - t[2] * u[1], t[2] * u[0] - t[0] * u[2], t[0] * u[1] - t[1] * u[0] };
        double base[3] = { gp[3 * g] - 0.3 * L * t[0], gp[3 * g + 1] - 0.3 * L * t[1], gp[3 * g + 2] - 0.3 * L * t[2] };
        double tip[3] = { base[0] + L * t[0], base[1] + L * t[1], base[2] + L * t[2] };
        for (int s = 0; s < S; s++) {
            double a0 = 2 * M_PI * s / S, a1 = 2 * M_PI * (s + 1) / S, p0[3], p1[3];
            for (int d = 0; d < 3; d++) { p0[d] = base[d] + r * (cos(a0) * u[d] + sin(a0) * w[d]); p1[d] = base[d] + r * (cos(a1) * u[d] + sin(a1) * w[d]); }
            const double *tr[2][3] = { { tip, p0, p1 }, { base, p1, p0 } };
            for (int h = 0; h < 2; h++) {
                long id = out->n++;
                for (int c = 0; c < 3; c++) { for (int d = 0; d < 3; d++) out->xyz[9 * id + 3 * c + d] = (float)tr[h][c][d]; out->val[3 * id + c] = gval ? gval[g] : (float)m; }
                double nm[3]; for (int d = 0; d < 3; d++) nm[d] = h ? -t[d] : (cos((a0 + a1) / 2) * u[d] + sin((a0 + a1) / 2) * w[d]);
                for (int d = 0; d < 3; d++) out->nrm[3 * id + d] = (float)nm[d];
                out->cell[id] = -1; out->grp[id] = -1;
            }
        }
    }
}

static void range_of_values(const float *v, long n, double *lo, double *hi)
{
    for (long i = 0; i < n; i++) if (!isnan(v[i])) { *lo = fmin(*lo, v[i]); *hi = fmax(*hi, v[i]); }  // NOLINT(clang-analyzer-core.CallAndMessage): false positive, the analyzer assumes ng <= 0 and ng != 0 on one path
}

/* the range a displayer colours over */
static void disp_range(const Disp *d, double *lo, double *hi)
{
    if (d->rangeMode == RM_MANUAL) { *lo = d->lo; *hi = d->hi; }
    else if (d->rangeMode == RM_FIELD) { *lo = d->fmin; *hi = d->fmax; }
    else { *lo = d->dmin; *hi = d->dmax; }
    if (!(*hi > *lo)) { double c = *lo; *lo = c - 0.5 * fmax(fabs(c), 1e-12); *hi = c + 0.5 * fmax(fabs(c), 1e-12); }
}

/* the geometry of a displayer, from fields the main thread has loaded: runs on the worker thread (see worker_*), on a copy
   of the displayer whose built data starts empty; reads only the mesh and these fields */
static void build(Disp *d, VzField *fc, VzField *fi, VzField *U, const char *timeName)
{
    d->err[0] = 0;
    if (d->col.field[0] && !fc) snprintf(d->err, sizeof d->err, "%s isn't at time %s", d->col.field, timeName);
    int q = d->col.q;
    bool smooth = d->style != VC_CELL;
    switch (d->kind) {
    case DK_BOUNDARY:
        vz_boundary(&V.m, fc, q, d->show, smooth, &d->t);
        if (d->clipOn) vz_clip(&d->t, d->o, d->n);
        break;
    case DK_SECTION:
        vz_section(&V.m, d->o, d->n, fc, q, smooth, &d->t);
        if (!d->t.n) snprintf(d->err, sizeof d->err, "The plane misses the mesh");
        else { vz_surface_report(&V.m, &d->t, U && U->nc == 3 ? U : NULL, d->n, &d->rep); d->haveRep = true; }
        break;
    case DK_ISO: {
        if (!fi) { snprintf(d->err, sizeof d->err, "No field to take the iso-surface of"); break; }
        for (int k = 0; k < d->nIsoVal && k < 4; k++) {
            VzTris t; vz_isosurface(&V.m, fi, d->iso.q, d->isoVal[k], fc ? fc : fi, fc ? q : d->iso.q, &t);
            tris_append(&d->t, &t); vz_tris_free(&t);
        }
        if (!d->t.n) snprintf(d->err, sizeof d->err, "No cell has that value");
        break;
    }
    case DK_THRESHOLD: {
        if (!fi) { snprintf(d->err, sizeof d->err, "No field to threshold"); break; }
        vz_threshold(&V.m, fi, d->iso.q, d->tlo, d->thi, fc ? fc : fi, fc ? q : d->iso.q, &d->t);
        if (!d->t.n) snprintf(d->err, sizeof d->err, "No cell is in that range");
        break;
    }
    case DK_STREAM:
        if (!U || U->nc != 3) { snprintf(d->err, sizeof d->err, "Streamlines need the velocity U"); break; }
        vz_streamlines(&V.m, U, &d->seeds, fc, q, &d->l);
        if (!d->l.nl) snprintf(d->err, sizeof d->err, "No streamline left its seeds: the seeds may be outside the flow or where it is still");
        if (d->sstyle != SS_LINES && d->l.nl) sweep_lines(&d->l, d->sstyle, d->swidth * 0.002 * diag_of_mesh(), &d->t);
        break;
    case DK_VECTOR: {
        if (!U || U->nc != 3) { snprintf(d->err, sizeof d->err, "Vectors need the velocity U"); break; }
        VzTris base;
        if (d->vtarget == 0) vz_section(&V.m, d->o, d->n, NULL, 0, true, &base);
        else vz_boundary(&V.m, NULL, 0, d->show, true, &base);
        double sp = d->vspacing > 0 ? d->vspacing : diag_of_mesh() / 40;
        d->ng = vz_glyph_points(&V.m, U, &base, sp, &d->gp, &d->gv);
        vz_tris_free(&base);
        if (d->vproject && d->vtarget == 0) {                   /* in the plane only */
            double nn = sqrt(d->n[0] * d->n[0] + d->n[1] * d->n[1] + d->n[2] * d->n[2]);
            for (long g = 0; g < d->ng; g++) {
                double dt = (d->gv[3 * g] * d->n[0] + d->gv[3 * g + 1] * d->n[1] + d->gv[3 * g + 2] * d->n[2]) / (nn * nn);
                for (int k = 0; k < 3; k++) d->gv[3 * g + k] -= (float)(dt * d->n[k]);
            }
        }
        d->gval = malloc(sizeof(float) * (size_t)(d->ng > 0 ? d->ng : 1));
        if (!d->gval) { snprintf(d->err, sizeof d->err, "Out of memory: fewer vectors (a larger spacing)"); break; }
        long hint = -1;
        double vmax = 0;
        for (long g = 0; g < d->ng; g++) {
            double x[3] = { d->gp[3 * g], d->gp[3 * g + 1], d->gp[3 * g + 2] }, v;
            d->gval[g] = fc && vz_sample(&V.m, fc, q, x, &hint, &v) ? (float)v : (float)sqrt(d->gv[3 * g] * d->gv[3 * g] + d->gv[3 * g + 1] * d->gv[3 * g + 1] + d->gv[3 * g + 2] * d->gv[3 * g + 2]);
            vmax = fmax(vmax, sqrt(d->gv[3 * g] * d->gv[3 * g] + d->gv[3 * g + 1] * d->gv[3 * g + 1] + d->gv[3 * g + 2] * d->gv[3 * g + 2]));
        }
        double len = d->vlen > 0 ? d->vlen : sp * 1.6;
        if (d->vstyle == 1) cones(d->gp, d->gv, d->gval, d->ng, len, d->vscale, vmax, &d->t);
        else {                                                    /* lines: shaft and two barbs, each a polyline */
            memset(&d->l, 0, sizeof d->l);
            d->l.xyz = malloc(sizeof(float) * 3 * 5 * (size_t)(d->ng + 1)); d->l.val = malloc(sizeof(float) * 5 * (size_t)(d->ng + 1));
            d->l.off = malloc(sizeof(long) * (size_t)(d->ng + 2));
            if (!d->l.xyz || !d->l.val || !d->l.off) { vz_lines_free(&d->l); snprintf(d->err, sizeof d->err, "Out of memory: fewer vectors (a larger spacing)"); break; }
            d->l.off[0] = 0;
            for (long g = 0; g < d->ng; g++) {
                double v[3] = { d->gv[3 * g], d->gv[3 * g + 1], d->gv[3 * g + 2] }, m = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (!(m > 0)) continue;
                double L = len * (d->vscale && vmax > 0 ? m / vmax : 1), t[3] = { v[0] / m, v[1] / m, v[2] / m };
                double a[3] = { 1, 0, 0 }; if (fabs(t[0]) > 0.9) { a[0] = 0; a[1] = 1; }
                double u[3] = { t[1] * a[2] - t[2] * a[1], t[2] * a[0] - t[0] * a[2], t[0] * a[1] - t[1] * a[0] }, ul = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                if (d->vtarget == 0) {                            /* barbs in the plane */
                    double nn[3] = { d->n[0], d->n[1], d->n[2] };
                    u[0] = t[1] * nn[2] - t[2] * nn[1]; u[1] = t[2] * nn[0] - t[0] * nn[2]; u[2] = t[0] * nn[1] - t[1] * nn[0]; ul = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                    if (ul < 1e-9) { u[0] = t[1] * a[2] - t[2] * a[1]; u[1] = t[2] * a[0] - t[0] * a[2]; u[2] = t[0] * a[1] - t[1] * a[0]; ul = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]); }
                }
                for (int k = 0; k < 3; k++) u[k] /= ul;
                double p0[3], p1[3], b1[3], b2[3];
                for (int k = 0; k < 3; k++) { p0[k] = d->gp[3 * g + k] - 0.4 * L * t[k]; p1[k] = p0[k] + L * t[k]; b1[k] = p1[k] - 0.3 * L * t[k] + 0.15 * L * u[k]; b2[k] = p1[k] - 0.3 * L * t[k] - 0.15 * L * u[k]; }
                long nv = d->l.nv;
                const double *P[5] = { p0, p1, b1, p1, b2 };
                for (int s = 0; s < 5; s++) { for (int k = 0; k < 3; k++) d->l.xyz[3 * (nv + s) + k] = (float)P[s][k]; d->l.val[nv + s] = d->gval[g]; }
                d->l.nv += 5; d->l.off[++d->l.nl] = d->l.nv;
            }
        }
        if (!d->ng) snprintf(d->err, sizeof d->err, "No glyph: the plane or surface misses the flow");
        break;
    }
    case DK_LIC: {
        if (!U || U->nc != 3) { snprintf(d->err, sizeof d->err, "Surface flow needs the velocity U"); break; }
        int r = d->licRes < 64 ? 64 : d->licRes > 1024 ? 1024 : d->licRes;
        d->licW = d->licH = r;
        d->lic = malloc(sizeof(float) * (size_t)r * r); d->licVal = malloc(sizeof(float) * (size_t)r * r);
        if (!d->lic || !d->licVal) { free(d->lic); free(d->licVal); d->lic = d->licVal = NULL; snprintf(d->err, sizeof d->err, "Out of memory: a lower resolution"); break; }
        double size = d->licSize > 0 ? d->licSize : 1.02 * lic_side();
        vz_lic(&V.m, U, fc, q, d->o, d->n, size, r, r, r / 20 + 4, d->lic, d->licVal, d->eu, d->ev);
        break;
    }
    case DK_VOLUME: {
        VzField *f = fc ? fc : NULL;
        if (!f) { snprintf(d->err, sizeof d->err, "Volume rendering needs a field to show"); break; }
        double L[3], mx = 0; for (int k = 0; k < 3; k++) { L[k] = V.m.hi[k] - V.m.lo[k]; mx = fmax(mx, L[k]); }
        int N = d->volN < 16 ? 16 : d->volN > 128 ? 128 : d->volN;
        for (int k = 0; k < 3; k++) d->vn[k] = (int)fmax(2, ceil(N * L[k] / mx));
        d->vol = vz_resample(&V.m, f, q, d->vn);
        if (!d->vol) snprintf(d->err, sizeof d->err, "Not enough memory free for the volume: a lower resolution");
        break;
    }
    case DK_LINE: {
        VzField *f = fc;
        if (!f) { snprintf(d->err, sizeof d->err, "A line probe needs a field"); break; }
        int n = d->lN < 2 ? 2 : d->lN > 4000 ? 4000 : d->lN;
        d->px = malloc(sizeof(double) * 3 * (size_t)n); d->pv = malloc(sizeof(double) * (size_t)n); d->pn = n;
        if (!d->px || !d->pv) { free(d->px); free(d->pv); d->px = d->pv = NULL; d->pn = 0; snprintf(d->err, sizeof d->err, "Out of memory"); break; }
        if (!vz_line_probe(&V.m, f, q, d->la, d->lb, n, d->px, d->pv)) snprintf(d->err, sizeof d->err, "The line is outside the mesh");
        break;
    }
    }
    /* the values it shows, for the auto range; the field's, for the whole-field range */
    d->dmin = 1e300; d->dmax = -1e300;
    range_of_values(d->t.val, d->t.n * 3, &d->dmin, &d->dmax);
    range_of_values(d->l.val, d->l.nv, &d->dmin, &d->dmax);
    range_of_values(d->gval, d->ng, &d->dmin, &d->dmax);
    if (d->licVal) range_of_values(d->licVal, (long)d->licW * d->licH, &d->dmin, &d->dmax);
    if (d->vol) range_of_values(d->vol, (long)d->vn[0] * d->vn[1] * d->vn[2], &d->dmin, &d->dmax);
    if (d->pv) for (int i = 0; i < d->pn; i++) if (!isnan(d->pv[i])) { d->dmin = fmin(d->dmin, d->pv[i]); d->dmax = fmax(d->dmax, d->pv[i]); }
    if (d->dmin > d->dmax) { d->dmin = 0; d->dmax = 1; }
    if (fc) { VzStats s; vz_stats_cells(&V.m, fc, q, &s); d->fmin = s.min; d->fmax = s.max; } else { d->fmin = d->dmin; d->fmax = d->dmax; }
    if (d->rangeMode == RM_MANUAL && !(d->hi > d->lo)) { d->lo = d->dmin; d->hi = d->dmax; }
}

/* the surfaces to the GPU; the colour coordinates from the current range */
static void upload(Disp *d)
{
    if (d->gmOk) { UnloadMesh(d->gm); d->gmOk = false; }
    if (!d->t.n) return;
    Mesh m = { 0 };
    m.vertexCount = (int)(d->t.n * 3); m.triangleCount = (int)d->t.n;
    m.vertices = MemAlloc(sizeof(float) * 3 * (unsigned)m.vertexCount);
    m.normals = MemAlloc(sizeof(float) * 3 * (unsigned)m.vertexCount);
    m.texcoords = MemAlloc(sizeof(float) * 2 * (unsigned)m.vertexCount);
    memcpy(m.vertices, d->t.xyz, sizeof(float) * 9 * (size_t)d->t.n);
    for (long i = 0; i < d->t.n; i++) for (int k = 0; k < 3; k++) memcpy(&m.normals[9 * i + 3 * k], &d->t.nrm[3 * i], sizeof(float) * 3);
    double lo, hi; disp_range(d, &lo, &hi);
    for (long i = 0; i < d->t.n * 3; i++) { m.texcoords[2 * i] = vc_u(vc_norm(d->t.val[i], lo, hi, d->logs), d->style, d->levels); m.texcoords[2 * i + 1] = 0.5f; }
    UploadMesh(&m, true);
    d->gm = m; d->gmOk = true;
}

static void retexcoord(Disp *d)
{
    if (!d->gmOk) return;
    double lo, hi; disp_range(d, &lo, &hi);
    for (long i = 0; i < d->t.n * 3; i++) { d->gm.texcoords[2 * i] = vc_u(vc_norm(d->t.val[i], lo, hi, d->logs), d->style, d->levels); }
    UpdateMeshBuffer(d->gm, 1, d->gm.texcoords, (int)(sizeof(float) * 2 * (size_t)d->gm.vertexCount), 0);
}

static void make_tex(Disp *d)
{
    int key = d->cmap * 1000003 + d->style * 10007 + d->levels * 101 + d->clip * 7 + d->rev * 3 + (d->col.field[0] ? 1 : 0) * 13 + d->rgb[0] + d->rgb[1] * 3 + d->rgb[2] * 5;
    if (d->texOk && key == d->texKey) return;
    if (d->texOk) UnloadTexture(d->tex);
    int W = d->col.field[0] ? vc_tex_width(d->style, d->levels) : 2;
    unsigned char *px = malloc(4 * (size_t)W);
    if (!px) { d->texOk = false; return; }
    if (d->col.field[0]) vc_texels(d->cmap, d->rev, d->style, d->levels, d->clip, px);
    else for (int i = 0; i < W; i++) { px[4 * i] = d->rgb[0]; px[4 * i + 1] = d->rgb[1]; px[4 * i + 2] = d->rgb[2]; px[4 * i + 3] = 255; }
    Image im = { px, W, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    d->tex = LoadTextureFromImage(im);
    free(px);
    SetTextureFilter(d->tex, d->style == VC_BANDED ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(d->tex, TEXTURE_WRAP_CLAMP);
    d->texOk = true; d->texKey = key;
}

static Color map_color(const Disp *d, double v, double lo, double hi)
{
    if (!d->col.field[0] || isnan(v)) return (Color){ d->rgb[0], d->rgb[1], d->rgb[2], 255 };
    double t = vc_norm(v, lo, hi, d->logs);
    if ((t < 0 || t > 1) && d->clip == VC_HIDE) return (Color){ 0, 0, 0, 0 };
    if (d->style == VC_BANDED) { int L = d->levels < 2 ? 2 : d->levels; t = (floor(fmin(fmax(t, 0), 1 - 1e-9) * L) + 0.5) / L; }
    unsigned char c[3]; vc_rgb(d->cmap, t, d->rev, c);
    return (Color){ c[0], c[1], c[2], 255 };
}

/* the LIC image: the colour map times the streaks */
static void make_lic_tex(Disp *d)
{
    if (d->licOk) { UnloadTexture(d->licTex); d->licOk = false; }
    if (!d->lic) return;
    int W = d->licW, H = d->licH;
    unsigned char *px = malloc(4 * (size_t)W * H);
    if (!px) return;                                       /* out of memory: no picture */
    double lo, hi; disp_range(d, &lo, &hi);
    for (long i = 0; i < (long)W * H; i++) {
        if (isnan(d->lic[i])) { px[4 * i] = px[4 * i + 1] = px[4 * i + 2] = 0; px[4 * i + 3] = 0; continue; }
        Color c = map_color(d, d->licVal[i], lo, hi);
        float s = 0.25f + 1.1f * (d->lic[i] - 0.5f) + 0.5f;
        s = fminf(1.3f, fmaxf(0.15f, s));
        px[4 * i] = (unsigned char)fminf(255, c.r * s); px[4 * i + 1] = (unsigned char)fminf(255, c.g * s); px[4 * i + 2] = (unsigned char)fminf(255, c.b * s);
        px[4 * i + 3] = c.a ? (unsigned char)(255 * d->opacity) : 0;
    }
    Image im = { px, W, H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    d->licTex = LoadTextureFromImage(im);
    SetTextureFilter(d->licTex, TEXTURE_FILTER_BILINEAR);
    free(px);
    d->licOk = true;
}

/* volume rendering: the resampled grid as slices along each axis, colour from the map, opacity rising with the value */
static void make_vol_tex(Disp *d)
{
    if (d->volOk) { for (int a = 0; a < 3; a++) for (int k = 0; k < d->volTexN[a]; k++) UnloadTexture(d->volTex[a][k]); d->volOk = false; }
    if (!d->vol) return;
    double lo, hi; disp_range(d, &lo, &hi);
    int *n = d->vn;
    for (int a = 0; a < 3; a++) {
        int u = (a + 1) % 3, v = (a + 2) % 3, W = n[u], H = n[v];
        d->volTexN[a] = n[a] > 128 ? 128 : n[a];
        unsigned char *px = malloc(4 * (size_t)W * H);
        if (!px) { d->volTexN[a] = 0; continue; }          /* out of memory: no slices along this axis */
        for (int k = 0; k < d->volTexN[a]; k++) {
            for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
                int idx[3]; idx[a] = k; idx[u] = i; idx[v] = j;
                float val = d->vol[((long)idx[2] * n[1] + idx[1]) * n[0] + idx[0]];
                long p = (long)j * W + i;
                if (isnan(val)) { px[4 * p + 3] = 0; px[4 * p] = px[4 * p + 1] = px[4 * p + 2] = 0; continue; }
                Color c = map_color(d, val, lo, hi);
                double t = fmin(1, fmax(0, vc_norm(val, lo, hi, d->logs)));
                double al = 1 - exp(-4.0 * d->volAlpha * t * t / d->volTexN[a]);
                px[4 * p] = c.r; px[4 * p + 1] = c.g; px[4 * p + 2] = c.b; px[4 * p + 3] = c.a ? (unsigned char)fmin(255, 255 * al) : 0;
            }
            Image im = { px, W, H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
            d->volTex[a][k] = LoadTextureFromImage(im);
            SetTextureFilter(d->volTex[a][k], TEXTURE_FILTER_BILINEAR);
        }
        free(px);
    }
    d->volOk = true;
}

/* ---- the worker: one displayer built at a time off the main thread; the old geometry stays drawn meanwhile */
static struct {
    pthread_t th; bool busy; int uid;
    Disp d;                                  /* the copy being built                                    */
    VzField *fc, *fi, *U; char time[32];
    pthread_mutex_t mu; bool done, init;
} W;
static int nextUid = 1;

static void *worker_main(void *arg)
{
    (void)arg;
    build(&W.d, W.fc, W.fi, W.U, W.time);
    pthread_mutex_lock(&W.mu); W.done = true; pthread_mutex_unlock(&W.mu);
    return NULL;
}

/* the finished build into its displayer (if it is still there), or thrown away */
static void worker_apply(void)
{
    pthread_join(W.th, NULL);
    W.busy = false;
    Disp *d = NULL;
    for (int i = 0; i < V.nd; i++) if (V.d[i].uid == W.uid) d = &V.d[i];
    if (!d) { disp_free_cpu(&W.d); return; }
    disp_free_built(d);
    const Disp *s = &W.d;
    d->t = s->t; d->l = s->l; d->il = s->il;
    d->gp = s->gp; d->gv = s->gv; d->gval = s->gval; d->ng = s->ng;
    d->lic = s->lic; d->licVal = s->licVal; d->licW = s->licW; d->licH = s->licH;
    memcpy(d->eu, s->eu, sizeof d->eu); memcpy(d->ev, s->ev, sizeof d->ev);
    d->vol = s->vol; memcpy(d->vn, s->vn, sizeof d->vn);
    d->px = s->px; d->pv = s->pv; d->pn = s->pn;
    d->dmin = s->dmin; d->dmax = s->dmax; d->fmin = s->fmin; d->fmax = s->fmax;
    d->rep = s->rep; d->haveRep = s->haveRep;
    pl_strlcpy(d->err, s->err, sizeof d->err);
    if (d->rangeMode == RM_MANUAL && !(d->hi > d->lo)) { d->lo = s->lo; d->hi = s->hi; }
    d->built = true; d->building = false; d->texDirty = true;
    upload(d);
    V.dirty[0] = V.dirty[1] = true;
}

/* wait for the build in flight (before the mesh or the field cache changes under it) */
static void worker_wait(void) { if (W.busy) worker_apply(); }

static bool worker_idle(void)
{
    if (W.busy) return false;
    for (int i = 0; i < V.nd; i++) if (V.d[i].dirty) return false;
    return true;
}

/* each frame: collect a finished build, start the next (the visible ones first) */
static void worker_tick(void)
{
    if (!W.init) { pthread_mutex_init(&W.mu, NULL); W.init = true; }
    if (W.busy) {
        pthread_mutex_lock(&W.mu); bool done = W.done; pthread_mutex_unlock(&W.mu);
        if (!done) { ui_request_frames(2); return; }
        worker_apply();
    }
    if (!V.mOk) return;
    Disp *d = NULL;
    for (int pass = 0; pass < 2 && !d; pass++) for (int i = 0; i < V.nd && !d; i++) if (V.d[i].dirty && (pass || V.d[i].vis)) d = &V.d[i];
    if (!d) return;
    if (!d->uid) d->uid = nextUid++;
    d->dirty = false; d->building = true;
    /* the fields, loaded here on the main thread (the cache isn't shared with the worker) */
    VzField *fc = d->col.field[0] ? field_of(d->col.field) : NULL;
    VzField *fi = (d->kind == DK_ISO || d->kind == DK_THRESHOLD) ? field_of(d->iso.field) : NULL;
    VzField *U = has_field("U") ? field_of("U") : NULL;
    W.d = *d;
    memset(&W.d.t, 0, sizeof W.d.t); memset(&W.d.l, 0, sizeof W.d.l); memset(&W.d.il, 0, sizeof W.d.il);
    W.d.gp = W.d.gv = W.d.gval = NULL; W.d.ng = 0; W.d.lic = W.d.licVal = NULL; W.d.vol = NULL; W.d.px = W.d.pv = NULL; W.d.pn = 0;
    W.d.gmOk = W.d.texOk = W.d.licOk = W.d.volOk = false; W.d.haveRep = false;
    W.fc = fc; W.fi = fi; W.U = U; W.uid = d->uid;
    pl_strlcpy(W.time, V.ti >= 0 ? V.times[V.ti] : "-", sizeof W.time);
    W.done = false; W.busy = true;
    if (pthread_create(&W.th, NULL, worker_main, NULL) != 0) {   /* no thread: build here */
        build(&W.d, fc, fi, U, W.time);
        W.done = true;
        worker_apply();
    }
    ui_request_frames(2);
}

static void refresh_disp(Disp *d)
{
    if (d->texDirty) {
        double lo, hi; disp_range(d, &lo, &hi);
        retexcoord(d);
        vz_lines_free(&d->il);
        if (d->isolines && d->t.n && d->col.field[0]) vz_isolines(&d->t, lo, hi, d->nIso, d->logs, &d->il);
        if (d->kind == DK_LIC) make_lic_tex(d);
        if (d->kind == DK_VOLUME) make_vol_tex(d);
        d->texDirty = false;
    }
    make_tex(d);
}

/* ================================================================ drawing */
static const char *VS =
    "#version 330\n"
    "in vec3 vertexPosition; in vec2 vertexTexCoord; in vec3 vertexNormal;\n"
    "uniform mat4 mvp; uniform mat4 matModel;\n"
    "out vec2 fragTexCoord; out vec3 fragNormal; out vec3 fragPos;\n"
    "void main() { fragTexCoord = vertexTexCoord; fragNormal = mat3(matModel) * vertexNormal;\n"
    "  fragPos = vec3(matModel * vec4(vertexPosition, 1.0)); gl_Position = mvp * vec4(vertexPosition, 1.0); }\n";
static const char *FS =
    "#version 330\n"
    "in vec2 fragTexCoord; in vec3 fragNormal; in vec3 fragPos;\n"
    "uniform sampler2D texture0; uniform vec4 colDiffuse; uniform vec3 viewPos; uniform float lit;\n"
    "out vec4 finalColor;\n"
    "void main() { vec4 c = texture(texture0, fragTexCoord) * colDiffuse; if (c.a < 0.01) discard;\n"
    "  vec3 n = normalize(fragNormal); vec3 v = normalize(viewPos - fragPos); float d = abs(dot(n, v));\n"
    "  float s = mix(1.0, 0.32 + 0.68 * d, lit); finalColor = vec4(c.rgb * s, c.a); }\n";

static void ensure_shader(void)
{
    if (V.shOk) return;
    V.sh = LoadShaderFromMemory(VS, FS);
    V.locView = GetShaderLocation(V.sh, "viewPos");
    V.locLit = GetShaderLocation(V.sh, "lit");
    V.shOk = V.sh.id != 0;
}

/* the copies drawn: the scene itself, its mirror images and its periodic copies */
static int transforms(Matrix *out)
{
    int n = 0;
    Matrix base[8]; int nb = 0;
    base[nb++] = MatrixIdentity();
    for (int a = 0; a < 3; a++) {
        if (!V.mirror[a]) continue;
        int was = nb;
        for (int i = 0; i < was && nb < 8; i++) {
            Vector3 s = { a == 0 ? -1.f : 1.f, a == 1 ? -1.f : 1.f, a == 2 ? -1.f : 1.f };
            base[nb++] = MatrixMultiply(base[i], MatrixScale(s.x, s.y, s.z));
        }
    }
    int per = V.periodic > 1 ? V.periodic : 1;
    for (int i = 0; i < nb; i++)
        for (int k = 0; k < per && n < 64; k++) {
            float ang = 2 * PI * k / per;
            Matrix r = V.periodicAxis == 0 ? MatrixRotateX(ang) : V.periodicAxis == 1 ? MatrixRotateY(ang) : MatrixRotateZ(ang);
            out[n++] = MatrixMultiply(base[i], r);
        }
    return n;
}

static void draw_lines(const VzLines *l, const Disp *d, double lo, double hi, float width, bool dark)
{
    if (!l->nl) return;
    rlSetLineWidth(width);
    rlBegin(RL_LINES);
    for (long k = 0; k < l->nl; k++)
        for (long i = l->off[k]; i + 1 < l->off[k + 1]; i++) {
            for (int e = 0; e < 2; e++) {
                long j = i + e;
                Color c = dark ? (Color){ 20, 20, 25, 200 } : map_color(d, l->val[j], lo, hi);
                if (!c.a) c = (Color){ 0, 0, 0, 0 };
                rlColor4ub(c.r, c.g, c.b, (unsigned char)(c.a * d->opacity));
                rlVertex3f(l->xyz[3 * j], l->xyz[3 * j + 1], l->xyz[3 * j + 2]);
            }
        }
    rlEnd();
    rlDrawRenderBatchActive();
    rlSetLineWidth(1);
}

static void draw_disp(Disp *d, Camera3D cam, Matrix tr, int pass)
{
    double lo, hi; disp_range(d, &lo, &hi);
    bool transparent = d->opacity < 0.999f || d->kind == DK_VOLUME || d->kind == DK_LIC;
    if ((pass == 0) == transparent) return;                   /* opaque first, then the see-through ones */
    if (d->gmOk && V.shOk) {
        Material mat = LoadMaterialDefault();
        mat.shader = V.sh;
        mat.maps[MATERIAL_MAP_DIFFUSE].texture = d->tex;
        mat.maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 255, 255, 255, (unsigned char)(255 * d->opacity) };
        float vp[3] = { cam.position.x, cam.position.y, cam.position.z }, lit = V.light ? 1.f : 0.f;
        SetShaderValue(V.sh, V.locView, vp, SHADER_UNIFORM_VEC3);
        SetShaderValue(V.sh, V.locLit, &lit, SHADER_UNIFORM_FLOAT);
        if (transparent) rlDisableDepthMask();
        DrawMesh(d->gm, mat, tr);
        if (transparent) rlEnableDepthMask();
        RL_FREE(mat.maps);
    }
    rlPushMatrix();
    rlMultMatrixf(MatrixToFloat(tr));
    if (d->kind == DK_STREAM && d->sstyle == SS_LINES) {
        if (d->sanim) {                                           /* moving dashes: a stretch of each line lit, the rest faint */
            float ph = (float)fmod(ui_time() * 0.6, 1.0);
            rlBegin(RL_LINES);
            for (long k = 0; k < d->l.nl; k++) {
                long a = d->l.off[k], b = d->l.off[k + 1];
                for (long i = a; i + 1 < b; i++) {
                    float f = (float)fmod((double)(i - a) / 30.0 - ph * 1.0 + 10, 1.0);
                    unsigned char al = (unsigned char)(255 * d->opacity * (f < 0.35f ? 1 : 0.25f));
                    for (int e = 0; e < 2; e++) { Color c = map_color(d, d->l.val[i + e], lo, hi); rlColor4ub(c.r, c.g, c.b, al); rlVertex3f(d->l.xyz[3 * (i + e)], d->l.xyz[3 * (i + e) + 1], d->l.xyz[3 * (i + e) + 2]); }
                }
            }
            rlEnd();
            rlDrawRenderBatchActive();
            ui_request_frames(2);
            V.dirty[0] = V.dirty[1] = true;
        } else draw_lines(&d->l, d, lo, hi, 1.5f * d->swidth, false);
    }
    if (d->kind == DK_VECTOR && d->vstyle == 0) draw_lines(&d->l, d, lo, hi, 1.4f, false);
    if (d->isolines) draw_lines(&d->il, d, lo, hi, 1.2f, true);
    if (d->edges && d->kind == DK_BOUNDARY && V.m.nFaces - V.m.nInternal < 400000) {
        rlBegin(RL_LINES);
        rlColor4ub(15, 18, 25, 110);
        for (int p = 0; p < V.m.nPatch; p++) {
            if (!d->show[p]) continue;
            for (long f = V.m.patch[p].start; f < V.m.patch[p].start + V.m.patch[p].nFaces; f++) {
                if (d->clipOn) { double r[3]; for (int k = 0; k < 3; k++) r[k] = V.m.fCtr[3 * f + k] - d->o[k]; if (r[0] * d->n[0] + r[1] * d->n[1] + r[2] * d->n[2] > 0) continue; }
                long a = V.m.fOff[f], n = V.m.fOff[f + 1] - a;
                for (long e = 0; e < n; e++) {
                    const double *p0 = &V.m.pt[3 * V.m.fIdx[a + e]], *p1 = &V.m.pt[3 * V.m.fIdx[a + (e + 1) % n]];
                    rlVertex3f((float)p0[0], (float)p0[1], (float)p0[2]); rlVertex3f((float)p1[0], (float)p1[1], (float)p1[2]);
                }
            }
        }
        rlEnd();
        rlDrawRenderBatchActive();
    }
    if (d->kind == DK_LIC && d->licOk) {
        double s = (d->licSize > 0 ? d->licSize : 1.02 * lic_side()) / 2;
        double c[4][3], nn[3] = { d->n[0], d->n[1], d->n[2] }, nl = sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        double tc[3] = { cam.position.x - d->o[0], cam.position.y - d->o[1], cam.position.z - d->o[2] };
        double side = (tc[0] * nn[0] + tc[1] * nn[1] + tc[2] * nn[2]) >= 0 ? 1 : -1, off = 2e-3 * diag_of_mesh() * side / (nl > 0 ? nl : 1);
        for (int k = 0; k < 3; k++) {
            c[0][k] = d->o[k] - s * d->eu[k] - s * d->ev[k]; c[1][k] = d->o[k] + s * d->eu[k] - s * d->ev[k];
            c[2][k] = d->o[k] + s * d->eu[k] + s * d->ev[k]; c[3][k] = d->o[k] - s * d->eu[k] + s * d->ev[k];
        }
        for (int q = 0; q < 4; q++) for (int k = 0; k < 3; k++) c[q][k] += off * nn[k];
        rlDisableDepthMask();
        rlSetTexture(d->licTex.id);
        rlBegin(RL_QUADS);
        rlColor4ub(255, 255, 255, 255);
        float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (int i = 0; i < 4; i++) { rlTexCoord2f(uv[i][0], uv[i][1]); rlVertex3f((float)c[i][0], (float)c[i][1], (float)c[i][2]); }
        rlEnd();
        rlSetTexture(0);
        rlDrawRenderBatchActive();
        rlEnableDepthMask();
    }
    if (d->kind == DK_VOLUME && d->volOk) {
        /* the slices square to the axis closest to the view, back to front */
        Vector3 vd = Vector3Subtract(cam.target, cam.position);
        float av[3] = { fabsf(vd.x), fabsf(vd.y), fabsf(vd.z) }, sv[3] = { vd.x, vd.y, vd.z };
        int a = av[0] >= av[1] && av[0] >= av[2] ? 0 : av[1] >= av[2] ? 1 : 2, u = (a + 1) % 3, v = (a + 2) % 3;
        int N = d->volTexN[a];
        rlDisableDepthMask();
        for (int s = 0; s < N; s++) {
            int k = sv[a] > 0 ? N - 1 - s : s;
            double pos = V.m.lo[a] + (k + 0.5) / N * (V.m.hi[a] - V.m.lo[a]);
            double c[4][3];
            for (int q = 0; q < 4; q++) {
                c[q][a] = pos;
                c[q][u] = (q == 1 || q == 2) ? V.m.hi[u] : V.m.lo[u];
                c[q][v] = (q >= 2) ? V.m.hi[v] : V.m.lo[v];
            }
            rlSetTexture(d->volTex[a][k].id);
            rlBegin(RL_QUADS);
            rlColor4ub(255, 255, 255, 255);
            float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
            for (int q = 0; q < 4; q++) { rlTexCoord2f(uv[q][0], uv[q][1]); rlVertex3f((float)c[q][0], (float)c[q][1], (float)c[q][2]); }
            rlEnd();
            rlSetTexture(0);
        }
        rlDrawRenderBatchActive();
        rlEnableDepthMask();
    }
    if (d->kind == DK_LINE) {
        rlSetLineWidth(2.5f);
        rlBegin(RL_LINES);
        rlColor4ub(255, 210, 60, 255);
        rlVertex3f((float)d->la[0], (float)d->la[1], (float)d->la[2]); rlVertex3f((float)d->lb[0], (float)d->lb[1], (float)d->lb[2]);
        rlEnd();
        rlDrawRenderBatchActive();
        rlSetLineWidth(1);
    }
    if (d->kind == DK_LIC && V.sel >= 0 && &V.d[V.sel] == d) {   /* the selected LIC square's outline */
        double eu[3], ev[3], nn[3] = { d->n[0], d->n[1], d->n[2] }, l = sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        if (l > 0) {
            for (int k = 0; k < 3; k++) nn[k] /= l;
            double a[3] = { 1, 0, 0 }; if (fabs(nn[0]) > 0.9) { a[0] = 0; a[1] = 1; }
            ev[0] = nn[1] * a[2] - nn[2] * a[1]; ev[1] = nn[2] * a[0] - nn[0] * a[2]; ev[2] = nn[0] * a[1] - nn[1] * a[0];
            double lv = sqrt(ev[0] * ev[0] + ev[1] * ev[1] + ev[2] * ev[2]); for (int k = 0; k < 3; k++) ev[k] /= lv;
            eu[0] = ev[1] * nn[2] - ev[2] * nn[1]; eu[1] = ev[2] * nn[0] - ev[0] * nn[2]; eu[2] = ev[0] * nn[1] - ev[1] * nn[0];
            double s = (d->licSize > 0 ? d->licSize : 1.02 * lic_side()) / 2;
            rlBegin(RL_LINES);
            rlColor4ub(255, 210, 60, 160);
            for (int q = 0; q < 4; q++) {
                double sg[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
                for (int e = 0; e < 2; e++) {
                    int qq = (q + e) % 4;
                    rlVertex3f((float)(d->o[0] + s * (sg[qq][0] * eu[0] + sg[qq][1] * ev[0])), (float)(d->o[1] + s * (sg[qq][0] * eu[1] + sg[qq][1] * ev[1])),
                               (float)(d->o[2] + s * (sg[qq][0] * eu[2] + sg[qq][1] * ev[2])));
                }
            }
            rlEnd();
            rlDrawRenderBatchActive();
        }
    }
    rlPopMatrix();
}

/* ---- the overlays drawn into the image itself (so a screenshot has them): scalar bars, title, time, statistic */
static void text_px(const char *s, float x, float y, float size, Color c)
{
    Font f = ui_font(F_REG, size);
    DrawTextEx(f, s, (Vector2){ x, y }, size, 0, c);
}

static void overlays(int w, int h, float sc)
{
    Color fg = V.bg == 1 ? (Color){ 30, 35, 45, 255 } : (Color){ 225, 230, 240, 255 };
    float y = 62 * sc;
    if (V.title[0]) { text_px(V.title, 16 * sc, y, 20 * sc, fg); y += 28 * sc; }
    if (V.showTime && V.ti >= 0) { char t[64]; snprintf(t, sizeof t, "t = %s", V.times[V.ti]); text_px(t, 16 * sc, y, 14 * sc, fg); }
    /* scalar bars down the right edge */
    int nb = 0;
    for (int i = 0; i < V.nd && nb < 3; i++) {
        Disp *d = &V.d[i];
        if (!d->vis || !d->col.field[0] || !d->built || d->kind == DK_LINE) continue;
        double lo, hi; disp_range(d, &lo, &hi);
        bool same = false;                                     /* the same field, range and map as a bar already drawn */
        for (int j = 0; j < i && !same; j++) {
            Disp *e = &V.d[j];
            if (!e->vis || !e->built || e->kind == DK_LINE || strcmp(e->col.field, d->col.field) || e->col.q != d->col.q) continue;
            double l2, h2; disp_range(e, &l2, &h2);
            same = l2 == lo && h2 == hi && e->cmap == d->cmap && e->rev == d->rev && e->style == d->style && e->logs == d->logs;
        }
        if (same) continue;
        float bw = 16 * sc, bh = fmaxf(90 * sc, fminf(200 * sc, (h - 80 * sc) / 3 - 50 * sc)), bx = w - 90 * sc, by = 72 * sc + nb * (bh + 56 * sc);
        if (by + bh > h - 20 * sc) break;                     /* no room for another */
        char lab[160]; qty_label(&d->col, lab, sizeof lab);
        float tw = MeasureTextEx(ui_font(F_REG, 13 * sc), lab, 13 * sc, 0).x;
        text_px(lab, fminf(bx, w - tw - 10 * sc), by - 20 * sc, 13 * sc, fg);
        int steps = 96;
        for (int s = 0; s < steps; s++) {
            double t = (s + 0.5) / steps;
            double v = d->logs && lo > 0 ? lo * pow(hi / lo, t) : lo + t * (hi - lo);
            Color c = map_color(d, v, lo, hi);
            if (!c.a) continue;
            DrawRectangle((int)bx, (int)(by + bh - (s + 1) * bh / steps), (int)bw, (int)ceilf(bh / steps) + 1, c);
        }
        DrawRectangleLinesEx((Rectangle){ bx, by, bw, bh }, 1, fg);
        double tk[32]; int nt = vc_ticks(lo, hi, d->logs, 6, tk);
        for (int k = 0; k < nt; k++) {
            double t = vc_norm(tk[k], lo, hi, d->logs);
            if (t < -1e-9 || t > 1 + 1e-9) continue;
            float yy = by + bh - (float)t * bh;
            DrawLine((int)(bx + bw), (int)yy, (int)(bx + bw + 5 * sc), (int)yy, fg);
            char s[32]; vc_fmt(tk[k], s, sizeof s);
            text_px(s, bx + bw + 8 * sc, yy - 7 * sc, 12 * sc, fg);
        }
        nb++;
    }
    if (V.showStat && V.sel >= 0 && V.d[V.sel].col.field[0]) {
        VzField *f = field_of(V.d[V.sel].col.field);
        if (f) {
            VzStats s; vz_stats_cells(&V.m, f, V.d[V.sel].col.q, &s);
            char a[32], b[32], c2[32], t[320]; vc_fmt(s.min, a, sizeof a); vc_fmt(s.max, b, sizeof b); vc_fmt(s.mean, c2, sizeof c2);
            char lab[120]; qty_label(&V.d[V.sel].col, lab, sizeof lab);
            pl_fmt_ui(t, sizeof t, "%s: min %s, max %s, volume mean %s", lab, a, b, c2);
            text_px(t, 16 * sc, h - 30 * sc, 13 * sc, fg);
        }
    }
    if (V.showLogo) text_px("Q-Continuum", w - 120 * sc, h - 28 * sc, 14 * sc, ui_alpha(fg, 0.7f));
}

static int transforms(Matrix *out);
static void fit_view(int v, float aspect)
{
    Orbit *o = &V.O[v];
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    Matrix tr[64]; int nt = transforms(tr);
    for (int k = 0; k < nt; k++)                              /* the box round every copy drawn (mirror, periodic) */
        for (int c = 0; c < 8; c++) {
            Vector3 p = { (float)(c & 1 ? V.m.hi[0] : V.m.lo[0]), (float)(c & 2 ? V.m.hi[1] : V.m.lo[1]), (float)(c & 4 ? V.m.hi[2] : V.m.lo[2]) };
            p = Vector3Transform(p, tr[k]);
            float q[3] = { p.x, p.y, p.z };
            for (int d = 0; d < 3; d++) { lo[d] = fminf(lo[d], q[d]); hi[d] = fmaxf(hi[d], q[d]); }
        }
    o->target = (Vector3){ (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2 };
    float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
    if (!V.fitted[v]) {
        o->upAxis = 1; o->spin = -1;
        float mn = fminf(dx, fminf(dy, dz)), mx = fmaxf(dx, fmaxf(dy, dz));
        if (mn < 0.05f * mx && dz == mn) orbit_set(o, 0, 0); else orbit_set(o, 0.6f, 0.4f);
    }
    float tv = tanf(35.f / 2 * DEG2RAD), th = tv * fmaxf(aspect > 0 ? aspect : 1.6f, 0.1f);
    Vector3 back = o->back, right = orbit_right(o), up = o->up;
    float need = 1e-9f;
    for (int c = 0; c < 8; c++) {
        float p[3] = { (c & 1 ? dx : -dx) / 2, (c & 2 ? dy : -dy) / 2, (c & 4 ? dz : -dz) / 2 };
        float x = p[0] * right.x + p[1] * right.y + p[2] * right.z, y = p[0] * up.x + p[1] * up.y + p[2] * up.z;
        float z = p[0] * back.x + p[1] * back.y + p[2] * back.z;
        need = fmaxf(need, fmaxf(z + fabsf(x) / th, z + fabsf(y) / tv));
    }
    o->dist = need * 1.12f;
    V.fitted[v] = true;
}

static Camera3D camera_of(int v)
{
    Camera3D c = orbit_camera(&V.O[v]);
    if (!V.persp) {
        c.projection = CAMERA_ORTHOGRAPHIC;
        c.fovy = V.O[v].dist * 2 * tanf(35.f / 2 * DEG2RAD);
    }
    return c;
}

static void render(int v, int w, int h, float sc, RenderTexture2D *rt)
{
    ensure_shader();
    EndScissorMode();
    BeginTextureMode(*rt);
    ClearBackground(BG[V.bg]);
    rlDrawRenderBatchActive();
    Camera3D cam = camera_of(v);
    BeginMode3D(cam);
    rlSetClipPlanes(V.O[v].dist * 0.005, V.O[v].dist * 40.0);
    rlDisableBackfaceCulling();
    Matrix tr[64]; int nt = transforms(tr);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < V.nd; i++) if (V.d[i].vis && V.d[i].built) for (int k = 0; k < nt; k++) draw_disp(&V.d[i], cam, tr[k], pass);
    if (V.probeOk) {
        Vector3 p = { (float)V.probeX[0], (float)V.probeX[1], (float)V.probeX[2] };
        DrawSphere(p, (float)(0.006 * diag_of_mesh()), (Color){ 255, 220, 60, 255 });
    }
    rlEnableBackfaceCulling();
    EndMode3D();
    rlSetClipPlanes(RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR);
    orbit_draw_axes(&V.O[v], 46 * sc, h - 46 * sc, 28 * sc, sc);
    overlays(w, h, sc);
    EndTextureMode();
    ui_restore_2d();
}

/* ================================================================ the scene file */
static void scene_path(char *out, size_t cap) { pl_join(out, cap, V.path, "visualization/scene.ini"); }

static void qty_put(Str *o, const char *k, const Qty *q) { str_catf(o, "%s = %s,%d\n", k, q->field[0] ? q->field : "-", q->q); }
static void qty_get(const char *v, Qty *q) { char f[64] = ""; int qq = 0; if (sscanf(v, "%63[^,],%d", f, &qq) >= 1) { pl_strlcpy(q->field, strcmp(f, "-") ? f : "", sizeof q->field); q->q = qq; } }
static void vec_put(Str *o, const char *k, const double *v) { str_catf(o, "%s = %.10g %.10g %.10g\n", k, v[0], v[1], v[2]); }

static void scene_save(void)
{
    if (!V.path[0]) return;
    char d[PL_PATH], p[PL_PATH]; pl_join(d, sizeof d, V.path, "visualization"); pl_mkdirs(d); scene_path(p, sizeof p);
    Str o; str_init(&o);
    str_cat(&o, "# Q-Continuum, Results > Visualization: the scene\n");
    str_catf(&o, "[scene]\nviews = %d\nlinked = %d\npersp = %d\nlight = %d\nbg = %d\nmirror = %d %d %d\nperiodic = %d %d\ntitle = %s\nshowTime = %d\nshowStat = %d\nshowLogo = %d\nhist = %d\nselected = %d\n",
             V.nViews, V.linked, V.persp, V.light, V.bg, V.mirror[0], V.mirror[1], V.mirror[2], V.periodic, V.periodicAxis, V.title, V.showTime, V.showStat, V.showLogo, V.histOn, V.sel);
    for (int v = 0; v < 2; v++) str_catf(&o, "view%d = %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d %d\n", v, V.O[v].target.x, V.O[v].target.y, V.O[v].target.z, V.O[v].dist,
                                         V.O[v].back.x, V.O[v].back.y, V.O[v].back.z, V.O[v].up.x, V.O[v].up.y, V.O[v].up.z, V.O[v].upAxis, V.O[v].spin);
    for (int s = 0; s < V.nsv; s++) str_catf(&o, "saved = %s|%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d\n", V.sv[s].name, V.sv[s].O.target.x, V.sv[s].O.target.y, V.sv[s].O.target.z,
                                            V.sv[s].O.dist, V.sv[s].O.back.x, V.sv[s].O.back.y, V.sv[s].O.back.z, V.sv[s].O.up.x, V.sv[s].O.up.y, V.sv[s].O.up.z, V.sv[s].persp);
    for (int i = 0; i < V.nd; i++) {
        const Disp *d = &V.d[i];
        str_catf(&o, "\n[displayer]\nkind = %d\nvisible = %d\nname = %s\n", d->kind, d->vis, d->name);
        qty_put(&o, "colour", &d->col); qty_put(&o, "iso", &d->iso);
        str_catf(&o, "rgb = %d %d %d\ncmap = %d\nstyle = %d\nlevels = %d\nrange = %d\nclip = %d\nreverse = %d\nlog = %d\nisolines = %d %d\nedges = %d\nlo = %.10g\nhi = %.10g\nopacity = %g\n",
                 d->rgb[0], d->rgb[1], d->rgb[2], d->cmap, d->style, d->levels, d->rangeMode, d->clip, d->rev, d->logs, d->isolines, d->nIso, d->edges, d->lo, d->hi, d->opacity);
        vec_put(&o, "origin", d->o); vec_put(&o, "normal", d->n);
        str_catf(&o, "clipOn = %d\nisoValues = %d %.10g %.10g %.10g %.10g\nthreshold = %.10g %.10g\n", d->clipOn, d->nIsoVal, d->isoVal[0], d->isoVal[1], d->isoVal[2], d->isoVal[3], d->tlo, d->thi);
        str_catf(&o, "seeds = %d %d %d %d %.10g %.10g %d\n", d->seeds.kind, d->seeds.n, d->seeds.patch, d->seeds.dir, d->seeds.size, d->seeds.maxLen, d->seeds.maxSteps);
        vec_put(&o, "seedA", d->seeds.a); vec_put(&o, "seedB", d->seeds.b);
        str_catf(&o, "stream = %d %g %d\nvector = %d %.10g %.10g %d %d %d\nlic = %d %.10g\nvolume = %d %g\n", d->sstyle, d->swidth, d->sanim, d->vtarget, d->vspacing, d->vlen, d->vstyle, d->vscale, d->vproject,
                 d->licRes, d->licSize, d->volN, d->volAlpha);
        vec_put(&o, "lineA", d->la); vec_put(&o, "lineB", d->lb);
        if (d->lref[0]) str_catf(&o, "lineRef = %s\n", d->lref);
        str_catf(&o, "lineN = %d\nshow =", d->lN);
        for (int p = 0; p < V.m.nPatch; p++) str_catf(&o, " %d", d->show[p]);
        str_cat(&o, "\n");
    }
    pl_write_file(p, o.s, (size_t)o.len);
    str_free(&o);
    V.saveDirty = false;
}

static void vec_get(const char *v, double *o) { sscanf(v, "%lf %lf %lf", &o[0], &o[1], &o[2]); }

static bool scene_load(void)
{
    char p[PL_PATH]; scene_path(p, sizeof p);
    char *t = pl_read_file(p, NULL);
    if (!t) return false;
    V.nd = 0; V.nsv = 0;
    Disp *d = NULL;
    bool inDisp = false;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        if (l[0] == '#') continue;
        if (!strcmp(l, "[displayer]")) { inDisp = true; d = V.nd < MAX_DISP ? &V.d[V.nd++] : NULL; if (d) disp_defaults(d, DK_BOUNDARY); continue; }
        char k[32], v[1024];
        if (sscanf(l, " %31[^= ] = %1023[^\n]", k, v) < 1) continue;
        if (sscanf(l, " %31[^= ] = %1023[^\n]", k, v) == 1) v[0] = 0;
        if (inDisp && !d) continue;                           /* past the most displayers: skipped */
        if (!inDisp) {
            if (!strcmp(k, "views")) V.nViews = atoi(v) == 2 ? 2 : 1;
            else if (!strcmp(k, "linked")) V.linked = atoi(v);
            else if (!strcmp(k, "persp")) V.persp = atoi(v);
            else if (!strcmp(k, "light")) V.light = atoi(v);
            else if (!strcmp(k, "bg")) V.bg = atoi(v) % 3;
            else if (!strcmp(k, "mirror")) { int a, b, c; if (sscanf(v, "%d %d %d", &a, &b, &c) == 3) { V.mirror[0] = a; V.mirror[1] = b; V.mirror[2] = c; } }
            else if (!strcmp(k, "periodic")) sscanf(v, "%d %d", &V.periodic, &V.periodicAxis);
            else if (!strcmp(k, "title")) pl_strlcpy(V.title, v, sizeof V.title);
            else if (!strcmp(k, "showTime")) V.showTime = atoi(v);
            else if (!strcmp(k, "showStat")) V.showStat = atoi(v);
            else if (!strcmp(k, "showLogo")) V.showLogo = atoi(v);
            else if (!strcmp(k, "hist")) V.histOn = atoi(v);
            else if (!strcmp(k, "selected")) V.sel = atoi(v);
            else if (!strncmp(k, "view", 4) && (k[4] == '0' || k[4] == '1')) {
                Orbit *o = &V.O[k[4] - '0'];
                if (sscanf(v, "%f %f %f %f %f %f %f %f %f %f %d %d", &o->target.x, &o->target.y, &o->target.z, &o->dist, &o->back.x, &o->back.y, &o->back.z,
                           &o->up.x, &o->up.y, &o->up.z, &o->upAxis, &o->spin) == 12) { V.fitted[k[4] - '0'] = true; V.touched[k[4] - '0'] = true; }
            } else if (!strcmp(k, "saved") && V.nsv < 12) {
                SavedView *s = &V.sv[V.nsv];
                char *bar = strchr(v, '|');
                if (bar) {
                    *bar = 0; pl_strlcpy(s->name, v, sizeof s->name);
                    int pp = 0;
                    if (sscanf(bar + 1, "%f %f %f %f %f %f %f %f %f %f %d", &s->O.target.x, &s->O.target.y, &s->O.target.z, &s->O.dist, &s->O.back.x, &s->O.back.y, &s->O.back.z,
                               &s->O.up.x, &s->O.up.y, &s->O.up.z, &pp) == 11) { s->persp = pp; s->O.upAxis = 1; s->O.spin = -1; V.nsv++; }
                }
            }
            continue;
        }
        if (!strcmp(k, "kind")) { int kk = atoi(v); d->kind = kk >= 0 && kk < DK__N ? kk : 0; }
        else if (!strcmp(k, "visible")) d->vis = atoi(v);
        else if (!strcmp(k, "name")) pl_strlcpy(d->name, v, sizeof d->name);
        else if (!strcmp(k, "colour")) qty_get(v, &d->col);
        else if (!strcmp(k, "iso")) qty_get(v, &d->iso);
        else if (!strcmp(k, "rgb")) { int a, b, c; if (sscanf(v, "%d %d %d", &a, &b, &c) == 3) { d->rgb[0] = (unsigned char)a; d->rgb[1] = (unsigned char)b; d->rgb[2] = (unsigned char)c; } }
        else if (!strcmp(k, "cmap")) d->cmap = atoi(v) % VC__N;
        else if (!strcmp(k, "style")) d->style = atoi(v) % 3;
        else if (!strcmp(k, "levels")) d->levels = atoi(v);
        else if (!strcmp(k, "range")) d->rangeMode = atoi(v) % 3;
        else if (!strcmp(k, "clip")) d->clip = atoi(v) % 2;
        else if (!strcmp(k, "reverse")) d->rev = atoi(v);
        else if (!strcmp(k, "log")) d->logs = atoi(v);
        else if (!strcmp(k, "isolines")) { int a, b; if (sscanf(v, "%d %d", &a, &b) == 2) { d->isolines = a; d->nIso = b; } }
        else if (!strcmp(k, "edges")) d->edges = atoi(v);
        else if (!strcmp(k, "lo")) d->lo = atof(v);
        else if (!strcmp(k, "hi")) d->hi = atof(v);
        else if (!strcmp(k, "opacity")) d->opacity = (float)atof(v);
        else if (!strcmp(k, "origin")) vec_get(v, d->o);
        else if (!strcmp(k, "normal")) vec_get(v, d->n);
        else if (!strcmp(k, "clipOn")) d->clipOn = atoi(v);
        else if (!strcmp(k, "isoValues")) sscanf(v, "%d %lf %lf %lf %lf", &d->nIsoVal, &d->isoVal[0], &d->isoVal[1], &d->isoVal[2], &d->isoVal[3]);
        else if (!strcmp(k, "threshold")) sscanf(v, "%lf %lf", &d->tlo, &d->thi);
        else if (!strcmp(k, "seeds")) sscanf(v, "%d %d %d %d %lf %lf %d", &d->seeds.kind, &d->seeds.n, &d->seeds.patch, &d->seeds.dir, &d->seeds.size, &d->seeds.maxLen, &d->seeds.maxSteps);
        else if (!strcmp(k, "seedA")) vec_get(v, d->seeds.a);
        else if (!strcmp(k, "seedB")) vec_get(v, d->seeds.b);
        else if (!strcmp(k, "stream")) { int a, c; float b; if (sscanf(v, "%d %f %d", &a, &b, &c) == 3) { d->sstyle = a; d->swidth = b; d->sanim = c; } }
        else if (!strcmp(k, "vector")) { int a, b, c, e; double s, l2; if (sscanf(v, "%d %lf %lf %d %d %d", &a, &s, &l2, &b, &c, &e) == 6) { d->vtarget = a; d->vspacing = s; d->vlen = l2; d->vstyle = b; d->vscale = c; d->vproject = e; } }
        else if (!strcmp(k, "lic")) sscanf(v, "%d %lf", &d->licRes, &d->licSize);
        else if (!strcmp(k, "volume")) sscanf(v, "%d %f", &d->volN, &d->volAlpha);
        else if (!strcmp(k, "lineA")) vec_get(v, d->la);
        else if (!strcmp(k, "lineB")) vec_get(v, d->lb);
        else if (!strcmp(k, "lineN")) d->lN = atoi(v);
        else if (!strcmp(k, "lineRef")) { pl_strlcpy(d->lref, v, sizeof d->lref); load_ref(d); }
        else if (!strcmp(k, "show")) { char *q = v; for (int pp = 0; pp < VZ_MAX_PATCHES; pp++) { char *e; long x = strtol(q, &e, 10); if (e == q) break; d->show[pp] = x != 0; q = e; } }
    }
    free(t);
    for (int i = 0; i < V.nd; i++) {                          /* a hand-edited file: keep the numbers sane */
        Disp *e = &V.d[i];
        e->dirty = true;
        e->levels = e->levels < 2 ? 2 : e->levels > 64 ? 64 : e->levels;
        e->nIso = e->nIso < 1 ? 1 : e->nIso > 100 ? 100 : e->nIso;
        e->nIsoVal = e->nIsoVal < 1 ? 1 : e->nIsoVal > 4 ? 4 : e->nIsoVal;
        e->seeds.n = e->seeds.n < 1 ? 1 : e->seeds.n > 2000 ? 2000 : e->seeds.n;
        if (e->seeds.kind == VS_PLANE && e->seeds.n > 60) e->seeds.n = 60;
        e->seeds.kind = e->seeds.kind < 0 || e->seeds.kind > VS_PATCH ? VS_POINT : e->seeds.kind;
        e->seeds.dir = e->seeds.dir < 0 || e->seeds.dir > 2 ? 2 : e->seeds.dir;
        e->sstyle = e->sstyle < 0 || e->sstyle > SS_RIBBONS ? SS_LINES : e->sstyle;
        if (!(e->swidth >= 0.1f && e->swidth <= 20)) e->swidth = 1;
        if (!(e->opacity >= 0.05f && e->opacity <= 1)) e->opacity = 1;
        if (!(e->volAlpha >= 0.01f && e->volAlpha <= 5)) e->volAlpha = 1;
        if (e->cmap < 0) e->cmap = 0;
        if (e->style < 0) e->style = 0;
        if (e->rangeMode < 0) e->rangeMode = RM_FIELD;
        if (e->clip < 0) e->clip = 0;
    }
    if (V.periodic < 1) V.periodic = 1;
    if (V.periodic > 64) V.periodic = 64;
    if (V.periodicAxis < 0 || V.periodicAxis > 2) V.periodicAxis = 2;
    if (V.bg < 0) V.bg = 0;
    if (V.sel >= V.nd || V.sel < -1) V.sel = V.nd - 1;
    return true;
}

/* ================================================================ open a case */
static void open_case(Case *c)
{
    worker_wait();
    if (V.saveDirty && V.mOk && V.loaded) scene_save();      /* the last change to the case being left */
    V.saveDirty = false;
    for (int i = 0; i < V.nd; i++) disp_release(&V.d[i]);
    V.nd = 0; V.sel = -1; V.nsv = 0; V.probeOk = false;
    cache_clear();
    vz_mesh_free(&V.m); V.mOk = false;
    pl_strlcpy(V.path, c->s.path, sizeof V.path);
    V.ti = -1;
    scan_times();
    scan_fields();
    SimSetup s; V.rho = sim_load(c->s.path, &s, 0) && s.rho > 0 ? s.rho : 1.204;
    V.fitted[0] = V.fitted[1] = false; V.touched[0] = V.touched[1] = false;
    V.loaded = false;
    V.meshSig = 0; V.rescanAt = ui_time();
    start_load();
}

static void after_mesh(void)
{
    if (V.loaded || !V.mOk) return;
    V.loaded = true;
    if (!scene_load()) {                                     /* a first scene: the boundary coloured by the pressure */
        V.nd = 0;
        Disp *d = add_disp(DK_BOUNDARY);
        if (d) V.sel = 0;
    }
}

/* a run going on, or run again (another write interval, the same times written over): new times appear, old ones go, fields
   change under the same name. Every 2 s: the times listed again (the one shown kept by name, or the newest when it went or
   the newest was shown) and any cached field whose file changed thrown away, its displayers built again */
static void refresh_case(void)
{
    if (ui_time() - V.rescanAt < 2 || !V.mOk || V.rec || V.thRunning) return;
    V.rescanAt = ui_time();
    {   char op[PL_PATH]; pl_join(op, sizeof op, V.path, "constant/polyMesh/owner");
        FDSig g; long long ms = fd_sig(op, &g) ? g.sec * 1000003LL + g.nsec + g.size * 7 : 0;
        if (!V.meshSig) V.meshSig = ms;
        else if (ms && ms != V.meshSig) {                  /* meshed again: everything built on the old one goes */
            V.meshSig = ms;
            worker_wait(); cache_clear();
            for (int i = 0; i < V.nd; i++) { disp_free_built(&V.d[i]); V.d[i].dirty = true; }
            V.probeOk = false;
            scan_times(); scan_fields();
            start_load();
            return;
        }
    }
    char cur[32] = ""; bool atNewest = V.ti >= 0 && V.ti == V.nTimes - 1;
    if (V.ti >= 0) pl_strlcpy(cur, V.times[V.ti], sizeof cur);
    char was[256][32]; int nWas = V.nTimes; memcpy(was, V.times, sizeof was);
    bool wasProc[256]; memcpy(wasProc, V.timeProc, sizeof wasProc);
    scan_times();
    bool listChanged = nWas != V.nTimes || memcmp(was, V.times, sizeof was) || memcmp(wasProc, V.timeProc, sizeof wasProc);
    if (listChanged) {
        int ti = -1;
        for (int i = 0; i < V.nTimes && !atNewest; i++) if (!strcmp(V.times[i], cur)) ti = i;
        if (ti < 0) ti = V.nTimes - 1;
        if (ti >= 0 && (strcmp(V.times[ti], cur) || V.ti != ti)) {
            V.ti = ti; scan_fields();
            for (int i = 0; i < V.nd; i++) V.d[i].dirty = true;
            redraw_all();
        } else V.ti = ti;
    }
    /* fields rewritten under the same time */
    bool stale = false;
    for (int i = 0; i < V.nCache && !stale; i++) {
        if (V.cache[i].name[0] == '@') continue;
        bool proc = false; int k = -1;
        for (int j = 0; j < V.nTimes; j++) if (!strcmp(V.times[j], V.cache[i].time)) { k = j; proc = V.timeProc[j]; }
        if (k < 0 || field_sig(V.cache[i].time, proc, V.cache[i].name) != V.cache[i].sig) stale = true;
    }
    if (stale) {
        worker_wait();
        cache_clear();
        if (V.ti >= 0) scan_fields();
        for (int i = 0; i < V.nd; i++) V.d[i].dirty = true;
        redraw_all();
    }
}

static void set_time(int ti)
{
    if (ti < 0 || ti >= V.nTimes || ti == V.ti) return;
    V.ti = ti;
    scan_fields();
    for (int i = 0; i < V.nd; i++) V.d[i].dirty = true;
    redraw_all();
}

/* ================================================================ widgets */
static bool num_box(const char *id, Rectangle r, double *v, double lo, double hi)
{
    char b[48];
    if (*v != 0 && (fabs(*v) < 1e-3 || fabs(*v) >= 1e6)) snprintf(b, sizeof b, "%.5g", *v); else snprintf(b, sizeof b, "%.6g", *v);
    if (!ui_input(ui_id(id), r, b, sizeof b, NULL, INPUT_MONO | INPUT_NUMERIC)) return false;
    char *e; double d = strtod(b, &e);
    while (e && *e == ' ') e++;
    if (e == b || (e && *e) || !isfinite(d) || d < lo || d > hi) { ui_toast(TOAST_ERR, "Give a number from %g to %g", lo, hi); return false; }
    *v = d;
    return true;
}

static float row_label(const char *s, float x, float y) { ui_text(F_REG, 12.5f, s, x, y + 8, TH.text2); return 0; }

static bool vec_row(const char *id, float x, float y, float w, const char *label, double *v)
{
    row_label(label, x, y);
    bool ch = false;
    float bw = (w - 96 - 12) / 3;
    for (int k = 0; k < 3; k++) { char i2[48]; snprintf(i2, sizeof i2, "%s%d", id, k); ch |= num_box(i2, (Rectangle){ x + 96 + k * (bw + 6), y, bw, 30 }, &v[k], -1e12, 1e12); }
    return ch;
}

static bool qty_select(const char *id, float x, float y, float w, const char *label, Qty *q, bool solid, bool vectorsOnly)
{
    Choice c[160]; int n = choices(c, 160, solid, vectorsOnly);
    const char *items[160]; int sel = -1;
    for (int i = 0; i < n; i++) { items[i] = c[i].label; if (!strcmp(c[i].q.field, q->field) && (c[i].q.q == q->q || (!q->field[0]))) sel = i; }
    row_label(label, x, y);
    char cur[96] = ""; qty_label(q, cur, sizeof cur);
    if (ui_select_ph(ui_id(id), (Rectangle){ x + 96, y, w - 96, 30 }, items, n, &sel, cur) && sel >= 0 && (strcmp(c[sel].q.field, q->field) || c[sel].q.q != q->q)) {
        *q = c[sel].q;
        return true;
    }
    return false;
}

/* ================================================================ the panel: the selected displayer's settings */
static float colour_settings(Disp *d, float x, float y, float w)
{
    float y0 = y;
    if (d->kind != DK_LINE && d->kind != DK_VOLUME) {
        if (qty_select("vz-col", x, y, w, "Colour by", &d->col, true, false)) { d->dirty = true; d->rangeMode = d->rangeMode == RM_MANUAL ? RM_PART : d->rangeMode; redraw_all(); }
    } else if (qty_select("vz-col", x, y, w, d->kind == DK_LINE ? "Plot" : "Show", &d->col, false, false)) { d->dirty = true; redraw_all(); }
    y += 38;
    if (!d->col.field[0]) {
        row_label("Colour", x, y);
        static const char *SOLID[6] = { "Grey", "White", "Steel", "Sand", "Teal", "Red" };
        static const unsigned char RGB[6][3] = { { 180, 190, 205 }, { 240, 240, 240 }, { 120, 140, 170 }, { 220, 200, 150 }, { 60, 170, 170 }, { 210, 70, 60 } };
        int s = -1;
        for (int i = 0; i < 6; i++) if (!memcmp(d->rgb, RGB[i], 3)) s = i;
        if (ui_select_ph(ui_id("vz-solid"), (Rectangle){ x + 96, y, w - 96, 30 }, SOLID, 6, &s, "Custom") && s >= 0) { memcpy(d->rgb, RGB[s], 3); redraw_all(); }
        return y + 38 - y0;
    }
    row_label("Colour map", x, y);
    int cm = d->cmap;
    if (ui_select(ui_id("vz-cmap"), (Rectangle){ x + 96, y, w - 96 - 90, 30 }, VC_NAME, VC__N, &cm) && cm != d->cmap) { d->cmap = cm; d->texDirty = true; redraw_all(); }
    if (ui_checkbox(ui_id("vz-rev"), (Rectangle){ x + w - 84, y + 5, 84, 20 }, &d->rev, "Reverse")) { d->texDirty = true; redraw_all(); }
    y += 38;
    static const char *STY[3] = { "Smooth", "Banded", "Cell values" };
    row_label("Style", x, y);
    int st = d->style;
    if (ui_segmented(ui_id("vz-style"), (Rectangle){ x + 96, y, w - 96, 30 }, STY, 3, &st) && st != d->style) {
        bool rebuild = (st == VC_CELL) != (d->style == VC_CELL);
        d->style = st; if (rebuild) d->dirty = true; else d->texDirty = true;
        if (d->gmOk) { UnloadMesh(d->gm); d->gmOk = false; upload(d); }
        redraw_all();
    }
    y += 38;
    if (d->style == VC_BANDED) {
        double lv = d->levels; row_label("Levels", x, y);
        if (num_box("vz-levels", (Rectangle){ x + 96, y, 80, 30 }, &lv, 2, 64)) { d->levels = (int)lv; d->texDirty = true; if (d->gmOk) { UnloadMesh(d->gm); d->gmOk = false; upload(d); } redraw_all(); }
        y += 38;
    }
    row_label("Range", x, y);
    int rm = d->rangeMode;
    if (ui_select(ui_id("vz-range"), (Rectangle){ x + 96, y, w - 96, 30 }, RM_NAME, 3, &rm) && rm != d->rangeMode) {
        if (rm == RM_MANUAL) { disp_range(d, &d->lo, &d->hi); }
        d->rangeMode = rm; d->texDirty = true; redraw_all();
    }
    y += 38;
    if (d->rangeMode == RM_MANUAL) {
        row_label("From, to", x, y);
        float bw = (w - 96 - 6) / 2;
        bool ch = num_box("vz-lo", (Rectangle){ x + 96, y, bw, 30 }, &d->lo, -1e300, 1e300);
        ch |= num_box("vz-hi", (Rectangle){ x + 96 + bw + 6, y, bw, 30 }, &d->hi, -1e300, 1e300);
        if (ch) { d->texDirty = true; redraw_all(); }
        y += 38;
    } else {
        double lo, hi; disp_range(d, &lo, &hi);
        char a[32], b[32], t[96]; vc_fmt(lo, a, sizeof a); vc_fmt(hi, b, sizeof b); snprintf(t, sizeof t, "%s to %s", a, b);
        ui_text(F_MONO, 12, t, x + 96, y - 4, TH.text3);
        y += 16;
    }
    if (ui_checkbox(ui_id("vz-log"), (Rectangle){ x, y + 4, 110, 20 }, &d->logs, "Log scale")) { d->texDirty = true; redraw_all(); }
    static const char *CL[2] = { "Clamp", "Hide" };
    ui_text(F_REG, 12.5f, "Beyond", x + 118, y + 8, TH.text2);
    int cl = d->clip;
    if (ui_segmented(ui_id("vz-clip"), (Rectangle){ x + 170, y, w - 170, 30 }, CL, 2, &cl) && cl != d->clip) { d->clip = cl; d->texDirty = true; redraw_all(); }
    y += 38;
    if (d->kind <= DK_THRESHOLD) {
        if (ui_checkbox(ui_id("vz-isol"), (Rectangle){ x, y + 4, 110, 20 }, &d->isolines, "Iso-lines")) { d->texDirty = true; redraw_all(); }
        if (d->isolines) { double ni = d->nIso; if (num_box("vz-niso", (Rectangle){ x + 118, y, 70, 30 }, &ni, 1, 100)) { d->nIso = (int)ni; d->texDirty = true; redraw_all(); } }
        y += 38;
    }
    return y - y0;
}

static float kind_settings(Case *c, Disp *d, float x, float y, float w)
{
    float y0 = y;
    (void)c;
    switch (d->kind) {
    case DK_BOUNDARY: {
        ui_text(F_MED, 12.5f, "Patches", x, y + 2, TH.text3); y += 22;
        for (int p = 0; p < V.m.nPatch; p++) {
            if (!strcmp(V.m.patch[p].type, "processor")) continue;
            char lab[96]; pl_fmt_ui(lab, sizeof lab, "%s (%s)", V.m.patch[p].name, V.m.patch[p].type);
            if (ui_checkbox(ui_idn(ui_id("vz-pt"), p), (Rectangle){ x, y, w, 22 }, &d->show[p], lab)) { d->dirty = true; redraw_all(); }
            y += 26;
        }
        if (ui_checkbox(ui_id("vz-edges"), (Rectangle){ x, y + 4, 140, 20 }, &d->edges, "Mesh edges")) redraw_all();
        if (ui_checkbox(ui_id("vz-clipon"), (Rectangle){ x + 150, y + 4, 140, 20 }, &d->clipOn, "Clip by a plane")) { d->dirty = true; redraw_all(); }
        y += 34;
        if (d->clipOn) {
            if (vec_row("vz-co", x, y, w, "Clip through", d->o)) { d->dirty = true; redraw_all(); }
            y += 38;
            if (vec_row("vz-cn", x, y, w, "Clip normal", d->n)) { d->dirty = true; redraw_all(); }
            y += 38;
        }
        break;
    }
    case DK_SECTION: case DK_LIC: case DK_VECTOR: {
        if (d->kind == DK_VECTOR) {
            static const char *T[2] = { "On a plane", "On the boundary" };
            row_label("Glyphs", x, y);
            int tg = d->vtarget;
            if (ui_segmented(ui_id("vz-vt"), (Rectangle){ x + 96, y, w - 96, 30 }, T, 2, &tg) && tg != d->vtarget) { d->vtarget = tg; d->dirty = true; redraw_all(); }
            y += 38;
        }
        if (d->kind != DK_VECTOR || d->vtarget == 0) {
            if (vec_row("vz-o", x, y, w, "Through", d->o)) { d->dirty = true; redraw_all(); }
            y += 38;
            if (vec_row("vz-n", x, y, w, "Normal", d->n)) { d->dirty = true; redraw_all(); }
            y += 38;
            static const char *AX[3] = { "Normal x", "Normal y", "Normal z" };
            int ax = -1;
            if (ui_segmented(ui_id("vz-ax"), (Rectangle){ x + 96, y, w - 96, 30 }, AX, 3, &ax) && ax >= 0) { d->n[0] = d->n[1] = d->n[2] = 0; d->n[ax] = 1; d->dirty = true; redraw_all(); }
            y += 38;
            /* a slider along the normal through the mesh */
            double nn = sqrt(d->n[0] * d->n[0] + d->n[1] * d->n[1] + d->n[2] * d->n[2]);
            if (nn > 0) {
                double lo = 1e300, hi = -1e300;
                for (int k = 0; k < 8; k++) { double p[3] = { k & 1 ? V.m.hi[0] : V.m.lo[0], k & 2 ? V.m.hi[1] : V.m.lo[1], k & 4 ? V.m.hi[2] : V.m.lo[2] }; double s = (p[0] * d->n[0] + p[1] * d->n[1] + p[2] * d->n[2]) / nn; lo = fmin(lo, s); hi = fmax(hi, s); }
                double cur = (d->o[0] * d->n[0] + d->o[1] * d->n[1] + d->o[2] * d->n[2]) / nn, f = hi > lo ? (cur - lo) / (hi - lo) : 0.5;
                row_label("Position", x, y);
                Rectangle tr = { x + 96, y + 12, w - 96, 6 };
                ui_rect(tr, 3, TH.surface2);
                float kx = tr.x + (float)fmin(1, fmax(0, f)) * tr.width;
                DrawCircleV((Vector2){ kx, tr.y + 3 }, 8, TH.lcars ? TH.lc[1] : TH.accent);
                Rectangle hit = { tr.x - 8, y, tr.width + 16, 30 };
                ui_register(ui_id("vz-slide"), hit, "Plane position");
                if (ui_hovered(hit) && ui_mouse_down()) {
                    double nf = fmin(1, fmax(0, (ui_mouse().x - tr.x) / tr.width)), ns = lo + nf * (hi - lo);
                    for (int k = 0; k < 3; k++) d->o[k] += (ns - cur) * d->n[k] / nn;
                    d->dirty = true; redraw_all();
                }
                y += 38;
            }
            if (d->kind == DK_SECTION && d->haveRep && d->built) {    /* the plane's report: area, mean, flow through it */
                char a[32], mv[32], t[400], lab[120] = "";
                vc_fmt(d->rep.area, a, sizeof a); vc_fmt(d->rep.mean, mv, sizeof mv);
                if (d->col.field[0]) qty_label(&d->col, lab, sizeof lab);
                ui_text(F_MED, 12.5f, "On this plane", x, y + 2, TH.text2); y += 20;
                pl_fmt_ui(t, sizeof t, "area %s m²%s%s%s%s", a, lab[0] ? ",  mean " : "", lab[0] ? lab : "", lab[0] ? " = " : "", lab[0] ? mv : "");
                ui_text_wrap(F_MONO, 12, t, (Rectangle){ x, y, w, 40 }, 17, TH.text, true); y += 20;
                if (has_field("U")) {
                    char f[32], fa[32], mf[32]; vc_fmt(d->rep.flow, f, sizeof f); vc_fmt(d->rep.flowAbs, fa, sizeof fa);
                    VzField *pp = has_field("p") ? field_of("p") : NULL;
                    bool kin = pp && strstr(pp->dims, "[0 2 -2");
                    vc_fmt(d->rep.flow * V.rho, mf, sizeof mf);
                    pl_fmt_ui(t, sizeof t, "flow through it along the normal %s m³/s%s%s%s  (|flow| %s m³/s)", f, kin ? ",  mass " : "", kin ? mf : "", kin ? " kg/s" : "", fa);
                    float th = ui_text_wrap(F_MONO, 12, t, (Rectangle){ x, y, w, 60 }, 17, TH.text, true); y += th + 4;
                }
                float bw = ui_button_width(ICON_COPY, "Copy");
                if (ui_button(ui_id("vz-rep-copy"), (Rectangle){ x, y, bw, 28 }, ICON_COPY, "Copy", BTN_GHOST)) {
                    char c2[300]; snprintf(c2, sizeof c2, "area_m2\tmean\tflow_m3s\n%.10g\t%.10g\t%.10g\n", d->rep.area, d->rep.mean, d->rep.flow);
                    SetClipboardText(c2); ui_toast(TOAST_OK, "Copied the plane's area, mean and flow");
                }
                y += 36;
            }
        } else {
            for (int p = 0; p < V.m.nPatch; p++) {
                if (!strcmp(V.m.patch[p].type, "processor")) continue;
                if (ui_checkbox(ui_idn(ui_id("vz-vpt"), p), (Rectangle){ x, y, w, 22 }, &d->show[p], V.m.patch[p].name)) { d->dirty = true; redraw_all(); }
                y += 26;
            }
        }
        if (d->kind == DK_VECTOR) {
            static const char *S[2] = { "Lines", "3-D arrows" };
            row_label("Arrows", x, y);
            int vs = d->vstyle;
            if (ui_segmented(ui_id("vz-vs"), (Rectangle){ x + 96, y, w - 96, 30 }, S, 2, &vs) && vs != d->vstyle) { d->vstyle = vs; d->dirty = true; redraw_all(); }
            y += 38;
            row_label("Spacing, length", x, y);
            float bw = (w - 96 - 6) / 2;
            bool ch = num_box("vz-vsp", (Rectangle){ x + 96, y, bw, 30 }, &d->vspacing, 0, 1e9);
            ch |= num_box("vz-vl", (Rectangle){ x + 96 + bw + 6, y, bw, 30 }, &d->vlen, 0, 1e9);
            if (ch) { d->dirty = true; redraw_all(); }
            y += 32;
            ui_text(F_REG, 11.5f, "0: automatic", x + 96, y, TH.text3); y += 18;
            if (ui_checkbox(ui_id("vz-vsc"), (Rectangle){ x, y + 4, 160, 20 }, &d->vscale, "Scale by magnitude")) { d->dirty = true; redraw_all(); }
            if (d->vtarget == 0 && ui_checkbox(ui_id("vz-vpr"), (Rectangle){ x + 170, y + 4, 160, 20 }, &d->vproject, "In the plane only")) { d->dirty = true; redraw_all(); }
            y += 34;
        }
        if (d->kind == DK_LIC) {
            row_label("Resolution", x, y);
            double r = d->licRes;
            if (num_box("vz-licr", (Rectangle){ x + 96, y, 90, 30 }, &r, 64, 1024)) { d->licRes = (int)r; d->dirty = true; redraw_all(); }
            y += 38;
            row_label("Square side", x, y);
            if (num_box("vz-lics", (Rectangle){ x + 96, y, 120, 30 }, &d->licSize, 0, 1e9)) { d->dirty = true; redraw_all(); }
            ui_text(F_REG, 11.5f, "0: the mesh's largest side", x + 224, y + 9, TH.text3);
            y += 38;
        }
        break;
    }
    case DK_ISO: case DK_THRESHOLD: {
        if (qty_select("vz-iso", x, y, w, d->kind == DK_ISO ? "Iso-surface of" : "Threshold on", &d->iso, false, false)) { d->dirty = true; redraw_all(); }
        y += 38;
        VzField *fi = field_of(d->iso.field);
        if (fi) {
            VzStats s; vz_stats_cells(&V.m, fi, d->iso.q, &s);
            char a[32], b[32], t[80]; vc_fmt(s.min, a, sizeof a); vc_fmt(s.max, b, sizeof b); snprintf(t, sizeof t, "it runs from %s to %s", a, b);
            ui_text(F_REG, 12, t, x + 96, y - 4, TH.text3); y += 16;
        }
        if (d->kind == DK_ISO) {
            row_label("Values", x, y);
            double nv = d->nIsoVal; if (num_box("vz-nisov", (Rectangle){ x + 96, y, 60, 30 }, &nv, 1, 4)) { d->nIsoVal = (int)nv; d->dirty = true; redraw_all(); }
            y += 38;
            for (int k = 0; k < d->nIsoVal; k++) {
                char id[24]; snprintf(id, sizeof id, "vz-isov%d", k);
                if (num_box(id, (Rectangle){ x + 96, y, w - 96, 30 }, &d->isoVal[k], -1e300, 1e300)) { d->dirty = true; redraw_all(); }
                y += 36;
            }
        } else {
            row_label("From, to", x, y);
            float bw = (w - 96 - 6) / 2;
            bool ch = num_box("vz-tlo", (Rectangle){ x + 96, y, bw, 30 }, &d->tlo, -1e300, 1e300);
            ch |= num_box("vz-thi", (Rectangle){ x + 96 + bw + 6, y, bw, 30 }, &d->thi, -1e300, 1e300);
            if (ch) { d->dirty = true; redraw_all(); }
            y += 38;
        }
        break;
    }
    case DK_STREAM: {
        static const char *SK[4] = { "Point", "Line", "Plane", "Patch" };
        row_label("Seeds from", x, y);
        int sk = d->seeds.kind;
        if (ui_segmented(ui_id("vz-sk"), (Rectangle){ x + 96, y, w - 96, 30 }, SK, 4, &sk) && sk != d->seeds.kind) { d->seeds.kind = sk; d->dirty = true; redraw_all(); }
        y += 38;
        if (d->seeds.kind == VS_PATCH) {
            const char *pn[VZ_MAX_PATCHES]; int np = 0, ps = -1;
            int map[VZ_MAX_PATCHES];
            for (int p = 0; p < V.m.nPatch; p++) { if (V.m.patch[p].nFaces <= 0 || !strcmp(V.m.patch[p].type, "empty")) continue; if (p == d->seeds.patch) ps = np; map[np] = p; pn[np++] = V.m.patch[p].name; }
            row_label("Patch", x, y);
            if (ui_select_ph(ui_id("vz-spatch"), (Rectangle){ x + 96, y, w - 96, 30 }, pn, np, &ps, "a patch") && ps >= 0 && map[ps] != d->seeds.patch) { d->seeds.patch = map[ps]; d->dirty = true; redraw_all(); }
            y += 38;
        } else {
            if (vec_row("vz-sa", x, y, w, d->seeds.kind == VS_LINE ? "From" : "Centre", d->seeds.a)) { d->dirty = true; redraw_all(); }
            y += 38;
            if (d->seeds.kind == VS_LINE) { if (vec_row("vz-sb", x, y, w, "To", d->seeds.b)) { d->dirty = true; redraw_all(); } y += 38; }
            else if (d->seeds.kind == VS_PLANE) {
                if (vec_row("vz-sb", x, y, w, "Normal", d->seeds.b)) { d->dirty = true; redraw_all(); }
                y += 38;
                row_label("Side", x, y);
                if (num_box("vz-ssz", (Rectangle){ x + 96, y, 120, 30 }, &d->seeds.size, 1e-12, 1e12)) { d->dirty = true; redraw_all(); }
                y += 38;
            } else {
                row_label("Radius", x, y);
                if (num_box("vz-srad", (Rectangle){ x + 96, y, 120, 30 }, &d->seeds.b[0], 0, 1e12)) { d->dirty = true; redraw_all(); }
                y += 38;
            }
        }
        row_label(d->seeds.kind == VS_PLANE ? "Seeds a side" : "Seeds", x, y);
        double ns = d->seeds.n;
        if (num_box("vz-sn", (Rectangle){ x + 96, y, 90, 30 }, &ns, 1, d->seeds.kind == VS_PLANE ? 60 : 2000)) { d->seeds.n = (int)ns; d->dirty = true; redraw_all(); }
        y += 38;
        static const char *DR[3] = { "Forward", "Backward", "Both" };
        row_label("Direction", x, y);
        int dr = d->seeds.dir;
        if (ui_segmented(ui_id("vz-sdir"), (Rectangle){ x + 96, y, w - 96, 30 }, DR, 3, &dr) && dr != d->seeds.dir) { d->seeds.dir = dr; d->dirty = true; redraw_all(); }
        y += 38;
        row_label("Max length", x, y);
        if (num_box("vz-smax", (Rectangle){ x + 96, y, 120, 30 }, &d->seeds.maxLen, 0, 1e12)) { d->dirty = true; redraw_all(); }
        ui_text(F_REG, 11.5f, "0: ten times the mesh", x + 224, y + 9, TH.text3);
        y += 38;
        static const char *SS[3] = { "Lines", "Tubes", "Ribbons" };
        row_label("Draw as", x, y);
        int ss = d->sstyle;
        if (ui_segmented(ui_id("vz-sst"), (Rectangle){ x + 96, y, w - 96, 30 }, SS, 3, &ss) && ss != d->sstyle) { d->sstyle = ss; d->dirty = true; redraw_all(); }
        y += 38;
        row_label("Width", x, y);
        double sw = d->swidth;
        if (num_box("vz-sw", (Rectangle){ x + 96, y, 80, 30 }, &sw, 0.1, 20)) { d->swidth = (float)sw; d->dirty = true; redraw_all(); }
        if (d->sstyle == SS_LINES && ui_checkbox(ui_id("vz-sanim"), (Rectangle){ x + 190, y + 5, 150, 20 }, &d->sanim, "Animate the flow")) redraw_all();
        y += 38;
        break;
    }
    case DK_VOLUME: {
        row_label("Resolution", x, y);
        double n = d->volN;
        if (num_box("vz-voln", (Rectangle){ x + 96, y, 80, 30 }, &n, 16, 128)) { d->volN = (int)n; d->dirty = true; redraw_all(); }
        y += 38;
        row_label("Opacity", x, y);
        double a = d->volAlpha;
        if (num_box("vz-vola", (Rectangle){ x + 96, y, 80, 30 }, &a, 0.01, 5)) { d->volAlpha = (float)a; d->texDirty = true; redraw_all(); }
        ui_text(F_REG, 11.5f, "the high end opaque, the low clear", x + 184, y + 9, TH.text3);
        y += 38;
        break;
    }
    case DK_LINE: {
        if (vec_row("vz-la", x, y, w, "From", d->la)) { d->dirty = true; redraw_all(); }
        y += 38;
        if (vec_row("vz-lb", x, y, w, "To", d->lb)) { d->dirty = true; redraw_all(); }
        y += 38;
        row_label("Samples", x, y);
        double n = d->lN;
        if (num_box("vz-ln", (Rectangle){ x + 96, y, 90, 30 }, &n, 2, 4000)) { d->lN = (int)n; d->dirty = true; redraw_all(); }
        float bw = ui_button_width(ICON_SAVE, "CSV");
        if (d->pn && ui_button(ui_id("vz-lcsv"), (Rectangle){ x + w - bw, y, bw, 30 }, ICON_SAVE, "CSV", BTN_GHOST)) {
            char dir[PL_PATH], p[PL_PATH]; pl_join(dir, sizeof dir, V.path, "visualization"); pl_mkdirs(dir);
            char nm[96]; snprintf(nm, sizeof nm, "lineprobe_%d.csv", (int)(d - V.d)); pl_join(p, sizeof p, dir, nm);
            Str o; str_init(&o);
            char lab[120]; qty_label(&d->col, lab, sizeof lab);
            str_catf(&o, "distance,x,y,z,%s\n", lab);
            double L = sqrt(pow(d->lb[0] - d->la[0], 2) + pow(d->lb[1] - d->la[1], 2) + pow(d->lb[2] - d->la[2], 2));
            for (int i = 0; i < d->pn; i++) str_catf(&o, "%.10g,%.10g,%.10g,%.10g,%.10g\n", L * i / (d->pn - 1), d->px[3 * i], d->px[3 * i + 1], d->px[3 * i + 2], d->pv[i]);
            pl_write_file(p, o.s, (size_t)o.len);
            str_free(&o);
            ui_toast(TOAST_OK, "Wrote visualization/%s", nm);
        }
        y += 38;
        row_label("Reference", x, y);
        UiId rid = ui_id("vz-lref");
        ui_input(rid, (Rectangle){ x + 96, y, w - 96, 30 }, d->lref, sizeof d->lref, "a CSV: distance (m), value", INPUT_MONO);
        if (ui_input_entered(rid)) {
            load_ref(d);
            if (d->lref[0] && !d->rn) ui_toast(TOAST_ERR, "No rows with two numbers in %s", d->lref);
            else if (d->rn) ui_toast(TOAST_OK, "%d reference points: squares on the plot", d->rn);
            V.saveDirty = true; V.saveAt = ui_time();
        }
        y += 38;
        break;
    }
    }
    if (d->kind != DK_VOLUME && d->kind != DK_LINE) {
        row_label("Opacity", x, y);
        double op = d->opacity;
        if (num_box("vz-op", (Rectangle){ x + 96, y, 80, 30 }, &op, 0.05, 1)) { d->opacity = (float)op; d->texDirty = true; redraw_all(); }
        y += 38;
    }
    return y - y0;
}

static float scene_list(float x, float y, float w)
{
    float y0 = y;
    y += 6;
    ui_text(F_SEMI, 13, ui_cap("Scene"), x, y + 2, head_color());
    static const char *ADD[DK__N];
    for (int k = 0; k < DK__N; k++) ADD[k] = DK_NAME[k];
    int picked = -1;
    float aw = ui_button_width(ICON_PLUS, "Add");
    if (ui_menu_button(ui_id("vz-add"), (Rectangle){ x + w - aw, y - 4, aw, 30 }, ICON_PLUS, "Add", BTN_SECONDARY, ADD, DK__N, &picked) && picked >= 0) {
        Disp *d = add_disp(picked);
        if (d) {
            V.sel = (int)(d - V.d);
            if (picked != DK_BOUNDARY)
                for (int i = 0; i < V.nd - 1; i++)
                    if (V.d[i].kind == DK_BOUNDARY && V.d[i].vis && V.d[i].opacity > 0.99f) {
                        V.d[i].opacity = 0.2f; V.d[i].texDirty = true;
                        ui_toast(TOAST_INFO, "%s made see-through, so what's inside shows (its Opacity puts it back)", V.d[i].name);
                    }
            redraw_all();
        }
    }
    y += 30;
    int del = -1, up = -1, dup = -1;
    for (int i = 0; i < V.nd; i++) {
        Disp *d = &V.d[i];
        Rectangle r = { x, y, w, 34 };
        if (ui_row(ui_idn(ui_id("vz-d"), i), r, V.sel == i)) V.sel = i;
        if (ui_button(ui_idn(ui_id("vz-eye"), i), (Rectangle){ x + 4, y + 2, 30, 30 }, d->vis ? ICON_EYE : ICON_CIRCLE_X, NULL, BTN_ICON)) { d->vis = !d->vis; redraw_all(); }
        ui_text_clip(F_MED, 13, d->name, x + 40, y + 9, w - 146, d->vis ? TH.text : TH.text3);
        if (d->building || (d->dirty && W.busy)) ui_spinner(x + w - 114, y + 17, 6, TH.accent);
        else if (d->err[0]) { ui_icon(ICON_TRIANGLE_ALERT, x + w - 114, y + 17, 13, TH.warn); ui_tooltip((Rectangle){ x + w - 124, y, 20, 34 }, d->err); }
        if (i > 0 && ui_button(ui_idn(ui_id("vz-up"), i), (Rectangle){ x + w - 66, y + 2, 30, 30 }, ICON_CHEVRON_UP, NULL, BTN_ICON)) up = i;
        if (ui_button(ui_idn(ui_id("vz-dup"), i), (Rectangle){ x + w - 98, y + 2, 30, 30 }, ICON_COPY, NULL, BTN_ICON)) dup = i;
        ui_tooltip((Rectangle){ x + w - 98, y + 2, 30, 30 }, "A copy of it (a second plane, the same streamlines coloured another way ...)");
        if (ui_button(ui_idn(ui_id("vz-del"), i), (Rectangle){ x + w - 34, y + 2, 30, 30 }, ICON_TRASH_2, NULL, BTN_ICON)) del = i;
        y += 36;
    }
    if (dup >= 0 && V.nd < MAX_DISP) {                       /* its settings, built afresh */
        Disp *n = &V.d[V.nd++], *o = &V.d[dup];
        disp_defaults(n, o->kind);
        Disp keep = *n;
        *n = *o;
        n->t = keep.t; n->l = keep.l; n->il = keep.il; n->gp = n->gv = n->gval = NULL; n->ng = 0; n->lic = n->licVal = NULL; n->vol = NULL;
        n->px = n->pv = NULL; n->pn = 0; n->gmOk = n->texOk = n->licOk = n->volOk = false; n->built = n->building = false; n->haveRep = false;
        n->uid = 0; n->dirty = true; n->err[0] = 0;
        char nm[64]; snprintf(nm, sizeof nm, "%.40s copy", o->name); pl_strlcpy(n->name, nm, sizeof n->name);
        V.sel = V.nd - 1;
        redraw_all();
    } else if (dup >= 0) ui_toast(TOAST_WARN, "%d displayers at most", MAX_DISP);
    if (up > 0) { Disp t = V.d[up]; V.d[up] = V.d[up - 1]; V.d[up - 1] = t; if (V.sel == up) V.sel--; else if (V.sel == up - 1) V.sel++; redraw_all(); }
    if (del >= 0) {
        disp_release(&V.d[del]);
        if (V.probeOk && V.probeDisp == del) V.probeOk = false;
        for (int i = del; i < V.nd - 1; i++) V.d[i] = V.d[i + 1];
        V.nd--;
        if (V.sel >= V.nd) V.sel = V.nd - 1;
        redraw_all();
    }
    if (!V.nd) { ui_text_wrap(F_REG, 12.5f, "Nothing in the scene: Add puts boundaries, planes, iso-surfaces, streamlines and more in it.", (Rectangle){ x, y, w, 60 }, 18, TH.text3, true); y += 40; }
    return y - y0 + 8;
}

static void fit_view(int v, float aspect);
/* the copies drawn changed: fit the views again (keeping the direction they look from) */
static void refit(void)
{
    for (int v = 0; v < 2; v++) if (V.rtH[v]) fit_view(v, (float)V.rtW[v] / V.rtH[v]);
    redraw_all();
}

static float options_panel(float x, float y, float w)
{
    float y0 = y;
    ui_text(F_SEMI, 13, ui_cap("Scene options"), x, y, head_color()); y += 26;
    row_label("Title", x, y);
    if (ui_input(ui_id("vz-title"), (Rectangle){ x + 96, y, w - 96, 30 }, V.title, sizeof V.title, "none", 0)) redraw_all();
    y += 38;
    if (ui_checkbox(ui_id("vz-st"), (Rectangle){ x, y, 90, 20 }, &V.showTime, "Time")) redraw_all();
    if (ui_checkbox(ui_id("vz-ss"), (Rectangle){ x + 90, y, 110, 20 }, &V.showStat, "Statistics")) redraw_all();
    if (ui_checkbox(ui_id("vz-sl"), (Rectangle){ x + 210, y, 90, 20 }, &V.showLogo, "Mark")) redraw_all();
    y += 30;
    if (ui_checkbox(ui_id("vz-light"), (Rectangle){ x, y, 100, 20 }, &V.light, "Lighting")) redraw_all();
    if (ui_checkbox(ui_id("vz-persp"), (Rectangle){ x + 110, y, 120, 20 }, &V.persp, "Perspective")) redraw_all();
    y += 30;
    row_label("Background", x, y);
    int bg = V.bg;
    if (ui_segmented(ui_id("vz-bg"), (Rectangle){ x + 96, y, w - 96, 30 }, BG_NAME, 3, &bg) && bg != V.bg) { V.bg = bg; redraw_all(); }
    y += 38;
    row_label("Mirror", x, y);
    if (ui_checkbox(ui_id("vz-mx"), (Rectangle){ x + 96, y + 5, 50, 20 }, &V.mirror[0], "x")) refit();
    if (ui_checkbox(ui_id("vz-my"), (Rectangle){ x + 150, y + 5, 50, 20 }, &V.mirror[1], "y")) refit();
    if (ui_checkbox(ui_id("vz-mz"), (Rectangle){ x + 204, y + 5, 50, 20 }, &V.mirror[2], "z")) refit();
    y += 34;
    row_label("Periodic copies", x, y);
    double pc = V.periodic > 1 ? V.periodic : 1;
    if (num_box("vz-per", (Rectangle){ x + 96, y, 60, 30 }, &pc, 1, 64)) { V.periodic = (int)pc; refit(); }
    static const char *PA[3] = { "about x", "about y", "about z" };
    int pa = V.periodicAxis;
    if (ui_segmented(ui_id("vz-pax"), (Rectangle){ x + 162, y, w - 162, 30 }, PA, 3, &pa) && pa != V.periodicAxis) { V.periodicAxis = pa; refit(); }
    y += 38;
    if (ui_checkbox(ui_id("vz-hist"), (Rectangle){ x, y, 200, 20 }, &V.histOn, "Histogram and probe plot")) {}
    y += 30;
    return y - y0;
}

/* ================================================================ the views */
static void view_interact(int v, Rectangle view)
{
    Rectangle turnR = { view.x + 12, view.y + 10, orbit_tools_w(false) + 16, 40 };
    bool hov = ui_hovered(view) && !ui_hovered(turnR) && !(V.probeOk && ui_hovered(V.probeBox)) && A.modal == MD_NONE;
    Vector2 mp = ui_mouse();
    static Vector2 pressAt;
    if (hov && ui_pressed(view)) {
        if (ui_time() - V.lastClick < 0.35) { V.touched[v] = false; V.fitted[v] = false; fit_view(v, view.width / view.height); V.lastClick = 0; V.dirty[v] = true; }
        else V.lastClick = ui_time();
        bool shift = inp_key_down(KEY_LEFT_SHIFT) || inp_key_down(KEY_RIGHT_SHIFT);
        V.dragging = !shift; V.panning = shift; V.last = mp; V.dragView = v; pressAt = mp;
    }
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) { V.panning = true; V.dragging = false; V.last = mp; V.dragView = v; pressAt = mp; }
    if ((V.dragging || V.panning) && V.dragView == v) {
        bool down = V.dragging ? ui_mouse_down() : (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || ui_mouse_down());
        if (!down) {
            /* a click without a drag: the probe */
            if (V.dragging && fabsf(mp.x - pressAt.x) + fabsf(mp.y - pressAt.y) < 3 && ui_hovered(view)) {
                float sc = ui_scale();
                Camera3D cam = camera_of(v);
                Ray ray = GetScreenToWorldRayEx((Vector2){ (mp.x - view.x) * sc, (mp.y - view.y) * sc }, cam, V.rtW[v], V.rtH[v]);
                double o[3] = { ray.position.x, ray.position.y, ray.position.z }, dr[3] = { ray.direction.x, ray.direction.y, ray.direction.z };
                double best = 1e300; int bd = -1; long bt = -1;
                for (int i = 0; i < V.nd; i++) {
                    if (!V.d[i].vis || !V.d[i].t.n || V.d[i].kind == DK_STREAM) continue;
                    double dist; long t = vz_pick(&V.d[i].t, o, dr, &dist);
                    if (t >= 0 && dist < best) { best = dist; bd = i; bt = t; }
                }
                V.probeOk = false;
                if (bd >= 0) {
                    Disp *d = &V.d[bd];
                    for (int k = 0; k < 3; k++) V.probeX[k] = o[k] + best * dr[k];
                    double val = (d->t.val[3 * bt] + d->t.val[3 * bt + 1] + d->t.val[3 * bt + 2]) / 3;
                    VzField *f = d->col.field[0] ? field_of(d->col.field) : NULL;
                    long cell = d->t.cell[bt];
                    if (f) { double sv; long h = cell; if (vz_sample(&V.m, f, d->col.q, V.probeX, &h, &sv)) val = sv; }
                    char lab[120] = "", vs[32] = "";
                    if (f) { qty_label(&d->col, lab, sizeof lab); vc_fmt(val, vs, sizeof vs); }
                    const char *patch = d->t.grp[bt] >= 0 && d->t.grp[bt] < V.m.nPatch ? V.m.patch[d->t.grp[bt]].name : "";
                    if (cell < 0) cell = vz_find_cell(&V.m, V.probeX, -1);
                    char cs[32] = ""; if (cell >= 0) snprintf(cs, sizeof cs, "  cell %ld", cell);
                    pl_fmt_ui(V.probeTxt, sizeof V.probeTxt, "%s%s%s  at (%.4g, %.4g, %.4g)%s%s%s", lab, f ? " = " : "", vs, V.probeX[0], V.probeX[1], V.probeX[2],
                              cs, patch[0] ? "  on " : "", patch);
                    V.probeOk = true; V.probeV = val; V.probeDisp = bd;
                }
                V.dirty[0] = V.dirty[1] = true;
            }
            V.dragging = V.panning = false;
        } else {
            float dx = mp.x - V.last.x, dy = mp.y - V.last.y;
            if (dx != 0 || dy != 0) {
                if (V.dragging) orbit_drag(&V.O[v], V.last, mp, (Vector2){ view.x + view.width / 2, view.y + view.height / 2 });
                else orbit_pan(&V.O[v], dx, dy);
                V.dirty[v] = true; V.touched[v] = true;
                if (V.linked && V.nViews == 2) { V.O[1 - v] = V.O[v]; V.dirty[1 - v] = true; }
                V.saveDirty = true;
            }
            V.last = mp;
            ui_request_frames(2);
        }
    }
    if (hov) {
        if (orbit_keys(&V.O[v])) { V.dirty[v] = true; if (V.linked && V.nViews == 2) { V.O[1 - v] = V.O[v]; V.dirty[1 - v] = true; } }
        float wh = ui_take_wheel();
        if (wh != 0) { V.O[v].dist *= powf(0.88f, wh); V.dirty[v] = true; V.touched[v] = true; if (V.linked && V.nViews == 2) { V.O[1 - v] = V.O[v]; V.dirty[1 - v] = true; } V.saveDirty = true; }
        ui_cursor(V.panning ? MOUSE_CURSOR_RESIZE_ALL : MOUSE_CURSOR_POINTING_HAND);
    }
    if (orbit_tools(&V.O[v], v ? "vz1" : "vz0", turnR.x + 8, turnR.y + 5, false)) { V.dirty[v] = true; if (V.linked && V.nViews == 2) { V.O[1 - v] = V.O[v]; V.dirty[1 - v] = true; } }
}

static void draw_view(int v, Rectangle view)
{
    float sc = ui_scale();
    int w = (int)(view.width * sc), h = (int)(view.height * sc);
    if (w < 8 || h < 8) return;
    if (!V.rtOk[v] || w != V.rtW[v] || h != V.rtH[v]) {
        if (V.rtOk[v]) UnloadRenderTexture(V.rt[v]);
        V.rt[v] = LoadRenderTexture(w, h);
        V.rtOk[v] = V.rt[v].id != 0; V.rtW[v] = w; V.rtH[v] = h; V.dirty[v] = true;
        if (!V.touched[v]) { V.fitted[v] = false; }
    }
    if (!V.fitted[v] && V.mOk) { fit_view(v, view.width / view.height); V.dirty[v] = true; }
    if (V.dirty[v] && V.rtOk[v]) { render(v, w, h, sc, &V.rt[v]); V.dirty[v] = false; }
    if (V.rtOk[v]) DrawTexturePro(V.rt[v].texture, (Rectangle){ 0, 0, (float)w, -(float)h }, view, (Vector2){ 0, 0 }, 0, WHITE);
    ui_rect_lines(view, 12, 1, TH.lineSoft);
    ui_register(ui_idn(ui_id("vz-view"), v), view, "Visualization view");
    Rectangle turnR = { view.x + 12, view.y + 10, orbit_tools_w(false) + 16, 40 };
    ui_rect(turnR, 9, ui_alpha(TH.surface, 0.82f));
    view_interact(v, view);
}

/* ---- screenshots and movies */
static void export_image(int v, const char *path, int mag)
{
    if (!V.rtOk[v]) return;
    int w = V.rtW[v] * mag, h = V.rtH[v] * mag;
    RenderTexture2D rt = LoadRenderTexture(w, h);
    if (!rt.id) { ui_toast(TOAST_ERR, "Could not make a %d x %d image", w, h); return; }
    render(v, w, h, ui_scale() * mag, &rt);
    Image im = LoadImageFromTexture(rt.texture);
    ImageFlipVertical(&im);
    ExportImage(im, path);
    UnloadImage(im);
    UnloadRenderTexture(rt);
}

static void screenshot(void)
{
    char d[PL_PATH], p[PL_PATH]; pl_join(d, sizeof d, V.path, "visualization"); pl_mkdirs(d);
    for (int i = 1; i < 10000; i++) {
        char nm[64]; snprintf(nm, sizeof nm, "screenshot_%03d.png", i); pl_join(p, sizeof p, d, nm);
        if (!pl_exists(p)) { export_image(0, p, V.shotMag); ui_toast(TOAST_OK, "Saved visualization/%s (%dx)", nm, V.shotMag); return; }
    }
}

static void start_record(int kind)
{
    if (kind == 1 && V.nTimes < 2) { ui_toast(TOAST_INFO, "One time only: nothing to animate over time (try the orbit)"); return; }
    if (kind == 3) {                                         /* the selected plane, from one side of the mesh to the other */
        Disp *d = V.sel >= 0 && V.sel < V.nd ? &V.d[V.sel] : NULL;
        if (!d || (d->kind != DK_SECTION && d->kind != DK_LIC && !(d->kind == DK_VECTOR && d->vtarget == 0))) {
            ui_toast(TOAST_INFO, "Select a section plane (or a surface-flow or vector plane) to sweep"); return;
        }
        double nn = sqrt(d->n[0] * d->n[0] + d->n[1] * d->n[1] + d->n[2] * d->n[2]);
        if (!(nn > 0)) { ui_toast(TOAST_ERR, "The plane has no normal"); return; }
        double lo = 1e300, hi = -1e300;
        for (int k = 0; k < 8; k++) { double p[3] = { k & 1 ? V.m.hi[0] : V.m.lo[0], k & 2 ? V.m.hi[1] : V.m.lo[1], k & 4 ? V.m.hi[2] : V.m.lo[2] };
                                      double sp = (p[0] * d->n[0] + p[1] * d->n[1] + p[2] * d->n[2]) / nn; lo = fmin(lo, sp); hi = fmax(hi, sp); }
        double pad = 0.02 * (hi - lo);
        V.sweepLo = lo + pad; V.sweepHi = hi - pad; V.sweepDisp = V.sel; V.sweepUid = d->uid; memcpy(V.sweepO0, d->o, sizeof d->o); V.recPlaced = false;
    }
    if (!A.env.ffmpeg[0]) { ui_toast(TOAST_ERR, "ffmpeg isn't installed: frames can't become a movie"); return; }
    pl_join(V.recDir, sizeof V.recDir, V.path, "visualization/frames");
    pl_remove_tree(V.recDir);                              /* (no shell needed) */
    pl_mkdirs(V.recDir);
    V.rec = kind; V.recStep = 0; V.recN = kind == 1 ? V.nTimes : kind == 3 ? 72 : 120;
    if (kind == 1) set_time(0);
}

static void record_tick(void)
{
    if (!V.rec) return;
    if (!worker_idle()) { ui_request_frames(2); return; }   /* this frame's displayers first */
    Disp *sw = V.rec == 3 && V.sweepDisp >= 0 && V.sweepDisp < V.nd && V.d[V.sweepDisp].uid == V.sweepUid ? &V.d[V.sweepDisp] : NULL;
    if (V.rec == 3 && !sw) { V.rec = 0; ui_toast(TOAST_WARN, "The plane being swept went: the movie stopped"); return; }
    if (sw && !V.recPlaced) {                                /* the plane to this frame's position, built, then the frame */
        double nn = sqrt(sw->n[0] * sw->n[0] + sw->n[1] * sw->n[1] + sw->n[2] * sw->n[2]);
        double cur = (sw->o[0] * sw->n[0] + sw->o[1] * sw->n[1] + sw->o[2] * sw->n[2]) / nn;
        double at = V.sweepLo + (V.sweepHi - V.sweepLo) * V.recStep / fmax(1, V.recN - 1);
        for (int k = 0; k < 3; k++) sw->o[k] += (at - cur) * sw->n[k] / nn;
        sw->dirty = true; V.recPlaced = true; ui_request_frames(2);
        return;
    }
    V.recPlaced = false;
    if (V.rec == 2) orbit_spin(&V.O[0], 2 * PI / V.recN);
    V.dirty[0] = true;
    char p[PL_PATH], nm[32]; snprintf(nm, sizeof nm, "%05d.png", V.recStep); pl_join(p, sizeof p, V.recDir, nm);
    export_image(0, p, 1);
    V.recStep++;
    ui_request_frames(2);
    if (V.recStep >= V.recN) {
        int kind = V.rec;
        V.rec = 0;
        char out[PL_PATH], qd[PL_PATH + 8], qo[PL_PATH + 8], qf[PL_PATH + 8], cmd[PL_PATH * 4];
        if (kind == 3 && sw) { memcpy(sw->o, V.sweepO0, sizeof sw->o); sw->dirty = true; }   /* the plane back where it was */
        char nm2[64]; snprintf(nm2, sizeof nm2, "visualization/movie_%s_%ld.mp4", kind == 1 ? "time" : kind == 3 ? "sweep" : "orbit", (long)time(NULL));
        pl_join(out, sizeof out, V.path, nm2);
        pl_shell_quote(qd, sizeof qd, V.recDir); pl_shell_quote(qo, sizeof qo, out); pl_shell_quote(qf, sizeof qf, A.env.ffmpeg);
        snprintf(cmd, sizeof cmd, "%s -y -loglevel error -framerate %d -i %s/%%05d.png -vf 'scale=trunc(iw/2)*2:trunc(ih/2)*2' -pix_fmt yuv420p %s && echo 'Q-Continuum: wrote %s'",
                 qf, kind == 1 ? (int)fmax(2, V.fps) : kind == 3 ? 18 : 30, qd, qo, nm2);
        app_start_tool("Scene movie", "encode", "Encode the movie (ffmpeg)", cmd, "log.vizmovie");
    } else if (V.rec == 1) set_time(V.recStep);
}

/* ---- the plot under the views: line probes and a histogram */
static void plot_panel(Rectangle r)
{
    ui_rect(r, 10, TH.surface2);
    float pad = 34, x0 = r.x + pad + 20, y0 = r.y + 14, w = r.width - pad - 40, h = r.height - 44;
    int nl = 0;
    for (int i = 0; i < V.nd; i++) if (V.d[i].kind == DK_LINE && V.d[i].vis && V.d[i].pn) nl++;
    bool hist = !nl && V.sel >= 0 && V.d[V.sel].col.field[0];
    if (!nl && !hist) { ui_text(F_REG, 12.5f, "Add a line probe, or select a coloured displayer for its histogram.", r.x + 16, r.y + 16, TH.text3); return; }
    ui_line(x0, y0 + h, x0 + w, y0 + h, 1, TH.lineSoft); ui_line(x0, y0, x0, y0 + h, 1, TH.lineSoft);
    if (nl) {
        double lo = 1e300, hi = -1e300;
        for (int i = 0; i < V.nd; i++) if (V.d[i].kind == DK_LINE && V.d[i].vis) for (int k = 0; k < V.d[i].pn; k++) if (!isnan(V.d[i].pv[k])) { lo = fmin(lo, V.d[i].pv[k]); hi = fmax(hi, V.d[i].pv[k]); }
        for (int i = 0; i < V.nd; i++) if (V.d[i].kind == DK_LINE && V.d[i].vis) for (int k = 0; k < V.d[i].rn; k++) { lo = fmin(lo, V.d[i].ry[k]); hi = fmax(hi, V.d[i].ry[k]); }
        if (!(hi > lo)) { hi = lo + 1; }
        static const Color C[4] = { { 255, 200, 60, 255 }, { 90, 180, 255, 255 }, { 255, 110, 140, 255 }, { 120, 230, 160, 255 } };
        int ci = 0;
        for (int i = 0; i < V.nd; i++) {
            Disp *d = &V.d[i];
            if (d->kind != DK_LINE || !d->vis || !d->pn) continue;
            Color c = C[ci++ % 4];
            for (int k = 0; k + 1 < d->pn; k++) {
                if (isnan(d->pv[k]) || isnan(d->pv[k + 1])) continue;
                float xa = x0 + w * k / (d->pn - 1), xb = x0 + w * (k + 1) / (d->pn - 1);
                float ya = y0 + h - (float)((d->pv[k] - lo) / (hi - lo)) * h, yb = y0 + h - (float)((d->pv[k + 1] - lo) / (hi - lo)) * h;
                ui_line(xa, ya, xb, yb, 2, c);
            }
            double L = sqrt(pow(d->lb[0] - d->la[0], 2) + pow(d->lb[1] - d->la[1], 2) + pow(d->lb[2] - d->la[2], 2));
            for (int k = 0; k < d->rn && L > 0; k++) {                 /* reference data: hollow squares (plan A3) */
                if (d->rx[k] < 0 || d->rx[k] > L) continue;
                float xx = x0 + w * (float)(d->rx[k] / L), yy = y0 + h - (float)((d->ry[k] - lo) / (hi - lo)) * h;
                DrawRectangleLinesEx((Rectangle){ xx - 4, yy - 4, 8, 8 }, 1.5f, c);
            }
            char lab[120]; qty_label(&d->col, lab, sizeof lab);
            if (d->rn) { char l2[160]; pl_fmt_ui(l2, sizeof l2, "%s  (squares: %s)", lab, pl_basename(d->lref)); ui_text_clip(F_REG, 12, l2, x0 + 8, y0 + 4 + 16 * (ci - 1), w - 16, c); }
            else ui_text_clip(F_REG, 12, lab, x0 + 8, y0 + 4 + 16 * (ci - 1), w - 16, c);
        }
        char a[32], b[32]; vc_fmt(lo, a, sizeof a); vc_fmt(hi, b, sizeof b);
        ui_text(F_MONO, 11, b, r.x + 6, y0 - 4, TH.text3); ui_text(F_MONO, 11, a, r.x + 6, y0 + h - 10, TH.text3);
        ui_text(F_REG, 11.5f, "distance along the line", x0 + w / 2 - 60, y0 + h + 6, TH.text3);
        return;
    }
    Disp *d = &V.d[V.sel];
    VzField *f = field_of(d->col.field);
    if (!f) return;
    double lo, hi; disp_range(d, &lo, &hi);
    double bins[40]; vz_histogram(&V.m, f, d->col.q, lo, hi, 40, bins);
    double mx = 0; for (int i = 0; i < 40; i++) mx = fmax(mx, bins[i]);
    for (int i = 0; i < 40; i++) {
        float bh = mx > 0 ? (float)(bins[i] / mx) * h : 0;
        Color c = map_color(d, lo + (i + 0.5) / 40 * (hi - lo), lo, hi);
        ui_rect((Rectangle){ x0 + w * i / 40 + 1, y0 + h - bh, w / 40 - 2, bh }, 2, c.a ? c : TH.text3);
    }
    char lab[120], a[32], b[32], t[200]; qty_label(&d->col, lab, sizeof lab); vc_fmt(lo, a, sizeof a); vc_fmt(hi, b, sizeof b);
    pl_fmt_ui(t, sizeof t, "Histogram of %s, volume-weighted: %s to %s", lab, a, b);
    ui_text_clip(F_REG, 12, t, x0 + 8, y0 + 2, w - 16, TH.text2);
}

/* ================================================================ the page */
float viz_page(Case *c, float x, float y, float w, float h)
{
    if (strcmp(V.path, c->s.path)) {
        static bool init;
        if (!init) { pthread_mutex_init(&V.mu, NULL); init = true; }
        open_case(c);
    }
    poll_load();
    if (V.mOk && !V.loaded) after_mesh();
    if (V.mLoading || (!V.mOk && !V.mErr[0])) {
        ui_spinner(x + 20, y + 30, 9, TH.accent);
        ui_text(F_REG, 14, "Reading the mesh ...", x + 40, y + 21, TH.text2);
        return 80;
    }
    if (!V.mOk) {
        char fp[PL_PATH]; pl_join(fp, sizeof fp, V.path, "constant/polyMesh/faces");
        if (!pl_exists(fp)) {
            const char *t = "No mesh yet: make one on the Mesh page and run the case; its results show here.";
            float ch = callout_height(w, NULL, t);
            callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_INFO, NULL, t);
            return ch + 10;
        }
        float ch = callout_height(w, "The mesh couldn't be read", V.mErr);
        callout((Rectangle){ x, y, w, ch }, TOAST_ERR, ICON_TRIANGLE_ALERT, "The mesh couldn't be read", V.mErr);
        return ch + 10;
    }
    refresh_case();
    if (!V.nTimes) {
        const char *t = "No results yet: run the case and its times appear here (a parallel run's too, straight from its processors).";
        float ch = callout_height(w, NULL, t);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_INFO, NULL, t);
        return ch + 10;
    }
    /* playback */
    worker_tick();
    if (V.playing && !V.rec) {
        if (worker_idle() && ui_time() - V.playAt > 1.0 / fmax(0.5, V.fps)) { V.playAt = ui_time(); set_time((V.ti + 1) % V.nTimes); }
        ui_request_frames(2);
    }
    for (int i = 0; i < V.nd; i++) { bool was = V.d[i].texDirty; refresh_disp(&V.d[i]); if (was) V.dirty[0] = V.dirty[1] = true; }
    record_tick();
    /* ---- the toolbar */
    float tx = x, ty = y;
    {
        const char *items[256]; for (int i = 0; i < V.nTimes; i++) items[i] = V.times[i];
        ui_text(F_REG, 12.5f, "Time", tx, ty + 9, TH.text2);
        int ti = V.ti;
        if (ui_select(ui_id("vz-time"), (Rectangle){ tx + 40, ty, 110, 30 }, items, V.nTimes, &ti) && ti != V.ti) set_time(ti);
        tx += 156;
        if (ui_button(ui_id("vz-prev"), (Rectangle){ tx, ty, 30, 30 }, ICON_CHEVRON_LEFT, NULL, BTN_ICON) && V.ti > 0) set_time(V.ti - 1);
        if (ui_button(ui_id("vz-play"), (Rectangle){ tx + 32, ty, 30, 30 }, V.playing ? ICON_PAUSE : ICON_PLAY, NULL, BTN_ICON)) { V.playing = !V.playing; V.playAt = ui_time(); }
        if (ui_button(ui_id("vz-next"), (Rectangle){ tx + 64, ty, 30, 30 }, ICON_CHEVRON_RIGHT, NULL, BTN_ICON) && V.ti < V.nTimes - 1) set_time(V.ti + 1);
        ui_tooltip((Rectangle){ tx + 32, ty, 30, 30 }, "Play through the times (loops)");
        tx += 104;
        static const char *VIEWS[7] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z", "Iso" };
        for (int k = 0; k < 7; k++) {
            float bw = ui_button_width(NULL, VIEWS[k]);
            if (ui_button(ui_idn(ui_id("vz-sv"), k), (Rectangle){ tx, ty, bw, 30 }, NULL, VIEWS[k], BTN_GHOST)) {
                Orbit *o = &V.O[0];
                o->upAxis = 1; o->spin = -1;
                /* looking along an axis: from the + side for +X (the camera on +x looking back) */
                if (k == 6) orbit_set(o, 0.6f, 0.4f);
                else {
                    int ax = k / 2; float s = k % 2 ? -1.f : 1.f;
                    Vector3 b = { 0, 0, 0 }; if (ax == 0) b.x = s; else if (ax == 1) b.y = s; else b.z = s;
                    o->back = b;
                    o->up = ax == 1 ? (Vector3){ 0, 0, -s } : (Vector3){ 0, 1, 0 };
                }
                V.touched[0] = true;
                if (V.linked && V.nViews == 2) V.O[1] = V.O[0];
                redraw_all();
            }
            tx += bw + 4;
        }
        float fw = ui_button_width(ICON_SQUARE, "Fit");
        if (ui_button(ui_id("vz-fit"), (Rectangle){ tx, ty, fw, 30 }, ICON_SQUARE, "Fit", BTN_GHOST)) { for (int v = 0; v < 2; v++) { V.touched[v] = false; fit_view(v, V.rtH[v] ? (float)V.rtW[v] / V.rtH[v] : 1.6f); } redraw_all(); }
        tx += fw + 10;
        /* saved views */
        const char *svn[14]; int nsvn = 0;
        svn[nsvn++] = "Save this view";
        for (int s = 0; s < V.nsv; s++) svn[nsvn++] = V.sv[s].name;
        if (V.nsv) svn[nsvn++] = "Forget the saved views";
        int pk = -1;
        float vw = ui_button_width(ICON_LIST, "Views");
        if (ui_menu_button(ui_id("vz-views"), (Rectangle){ tx, ty, vw, 30 }, ICON_LIST, "Views", BTN_GHOST, svn, nsvn, &pk) && pk >= 0) {
            if (pk == 0) {
                if (V.nsv < 12) { SavedView *s = &V.sv[V.nsv++]; snprintf(s->name, sizeof s->name, "View %d", V.nsv); s->O = V.O[0]; s->persp = V.persp; ui_toast(TOAST_OK, "Saved as %s", s->name); V.saveDirty = true; }
                else ui_toast(TOAST_WARN, "Twelve saved views at most");
            } else if (V.nsv && pk == V.nsv + 1) { V.nsv = 0; V.saveDirty = true; ui_toast(TOAST_INFO, "Saved views forgotten"); }
            else { V.O[0] = V.sv[pk - 1].O; V.persp = V.sv[pk - 1].persp; V.touched[0] = true; if (V.linked && V.nViews == 2) V.O[1] = V.O[0]; redraw_all(); }
        }
        tx += vw + 10;
        float mw = ui_button_width(ICON_PLAY, "Movie"), sw = ui_button_width(ICON_SAVE, "Image");
        if (tx + 156 + (V.nViews == 2 ? 76 : 0) + mw + sw + 18 > x + w) { tx = x; ty += 40; }   /* too narrow: a second row */
        static const char *LAY[2] = { "1 view", "2 views" };
        int lay = V.nViews - 1;
        if (ui_segmented(ui_id("vz-lay"), (Rectangle){ tx, ty, 150, 30 }, LAY, 2, &lay)) { V.nViews = lay + 1; if (V.nViews == 2 && !V.touched[1]) V.O[1] = V.O[0]; redraw_all(); }
        tx += 156;
        if (V.nViews == 2) { if (ui_checkbox(ui_id("vz-link"), (Rectangle){ tx, ty + 5, 70, 20 }, &V.linked, "Linked")) { if (V.linked) V.O[1] = V.O[0]; redraw_all(); } }
        /* the right end: screenshot and movie */
        static const char *SHOT[3] = { "Screenshot 1x", "Screenshot 2x", "Screenshot 4x" };
        static const char *MOV[3] = { "Movie over time", "Movie: orbit", "Movie: sweep the plane" };
        int sp = -1, mp = -1;
        ui_disable(V.rec != 0);
        if (ui_menu_button(ui_id("vz-shot"), (Rectangle){ x + w - mw - sw - 8, ty, sw, 30 }, ICON_SAVE, "Image", BTN_SECONDARY, SHOT, 3, &sp) && sp >= 0) { V.shotMag = sp == 0 ? 1 : sp == 1 ? 2 : 4; screenshot(); }
        if (ui_menu_button(ui_id("vz-movie"), (Rectangle){ x + w - mw, ty, mw, 30 }, ICON_PLAY, "Movie", BTN_SECONDARY, MOV, 3, &mp) && mp >= 0) start_record(mp + 1);
        ui_disable(false);
        if (V.rec) { char t[64]; snprintf(t, sizeof t, "Recording %d / %d", V.recStep, V.recN); ui_text(F_REG, 12.5f, t, x + w - mw - sw - 140, ty + 9, TH.warn); }
    }
    float bar = ty - y + 40;
    y += bar;
    /* ---- the left panel and the views */
    float lw = fminf(380, w * 0.32f), gap = 14;
    float plotH = V.histOn ? 170 : 0;
    float vh = fmaxf(300, h - bar - (plotH > 0 ? plotH + gap : 0));
    Rectangle lp = { x, y, lw, vh + (plotH > 0 ? plotH + gap : 0) };
    float top = ui_scroll_begin(&V.left, lp), ly = top, lx = x + 2, lww = lw - 16;
    ly += scene_list(lx, ly, lww);
    if (V.sel >= 0 && V.sel < V.nd) {
        Disp *d = &V.d[V.sel];
        ui_divider(lx, ly, lww); ly += 10;
        ui_text(F_SEMI, 13, ui_cap(DK_NAME[d->kind]), lx, ly, head_color()); ly += 24;
        row_label("Name", lx, ly);
        if (ui_input(ui_id("vz-name"), (Rectangle){ lx + 96, ly, lww - 96, 30 }, d->name, sizeof d->name, NULL, 0)) V.saveDirty = true;
        ly += 38;
        ly += kind_settings(c, d, lx, ly, lww);
        ly += colour_settings(d, lx, ly, lww);
        if (d->err[0]) ly += ui_text_wrap(F_REG, 12.5f, d->err, (Rectangle){ lx, ly, lww, 60 }, 18, TH.warn, true) + 8;
    }
    ui_divider(lx, ly, lww); ly += 10;
    ly += options_panel(lx, ly, lww);
    ui_scroll_end(&V.left, ly - top + 20);
    float vx = x + lw + gap, vw = w - lw - gap;
    if (V.nViews == 2) {
        float hw = (vw - gap) / 2;
        draw_view(0, (Rectangle){ vx, y, hw, vh });
        draw_view(1, (Rectangle){ vx + hw + gap, y, hw, vh });
    } else draw_view(0, (Rectangle){ vx, y, vw, vh });
    if (V.probeOk) {
        float sbw = has_field("U") ? ui_button_width(NULL, "Streamlines from here") : 0;
        float pw = fminf(vw - 20 - sbw - 8, ui_text_w(F_MONO, 12, V.probeTxt) + 24);
        Rectangle pr = { vx + 10, y + vh - 40, pw, 30 };
        ui_rect(pr, 8, ui_alpha(TH.surface, 0.9f));
        ui_text_clip(F_MONO, 12, V.probeTxt, pr.x + 12, pr.y + 9, pr.width - 20, TH.text);
        ui_register(ui_id("vz-probe"), pr, V.probeTxt);
        V.probeBox = (Rectangle){ pr.x, pr.y, pr.width + (sbw > 0 ? sbw + 8 : 0), pr.height };
        if (sbw > 0 && ui_button(ui_id("vz-seed-here"), (Rectangle){ pr.x + pr.width + 8, pr.y, sbw, 30 }, NULL, "Streamlines from here", BTN_SECONDARY)) {
            Disp *d = add_disp(DK_STREAM);                   /* a small ball of seeds at the probed point, both ways */
            if (d) {
                d->seeds.kind = VS_POINT; d->seeds.dir = 2; d->seeds.n = 40;
                memcpy(d->seeds.a, V.probeX, sizeof V.probeX); d->seeds.b[0] = 0.04 * diag_of_mesh();
                V.sel = V.nd - 1; redraw_all();
            } else ui_toast(TOAST_WARN, "%d displayers at most", MAX_DISP);
        }
    }
    if (plotH > 0) plot_panel((Rectangle){ vx, y + vh + gap, vw, plotH });
    /* the scene file, a moment after the last change */
    if (V.saveDirty) {                                        /* frames only come with events: keep them coming until it's saved */
        if (ui_time() - V.saveAt > 1.5) { scene_save(); V.saveAt = ui_time(); }
        else ui_request_frames(2);
    }
    return bar + vh + (plotH > 0 ? plotH + gap : 0) + 10;
}
