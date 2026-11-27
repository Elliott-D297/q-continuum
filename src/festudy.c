/* festudy.c -- see festudy.h */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "festudy.h"
#include "ccxmodel.h"
#include "pl_util.h"

const char *const FE_AN_NAME[AN__N] = { "Static stress", "Nonlinear (large deflection, yielding)", "Natural frequencies", "Buckling",
                                        "Heat conduction", "Thermal stress", "Dynamic response (in time)", "Frequency response (harmonic)" };
const char *const FE_AN_WORD[AN__N] = { "static", "nonlinear", "frequency", "buckling", "heat", "thermalstress", "dynamic", "harmonic" };
const char *const FE_FX_NAME[FX__N] = { "fixed", "x", "y", "z", "roller", "pin", "displacement" };
const char *const FE_LD_NAME[LD__N] = { "force", "pressure", "gravity", "rotation", "bearing", "torque" };
const char *const FE_TH_NAME[TH__N] = { "temperature", "flux", "convection", "radiation", "heatgen" };
const char *const FE_DIR_NAME[DIR__N] = { "normal", "+x", "+y", "+z", "-x", "-y", "-z" };
const char *const FE_AMP_NAME[AMP__N] = { "step", "ramp", "pulse" };

#define SIGMA_MM 5.670374e-11        /* Stefan-Boltzmann in mW/(mm² K⁴)                                  */

void fe_study_default(FeStudy *s)
{
    memset(s, 0, sizeof *s);
    s->scale = 1; s->order = 2;
    pl_strlcpy(s->material, CCX_LIB[0].name, sizeof s->material);
    s->time = 1; s->modes = 10; s->fLo = 10; s->fHi = 1000; s->nPts = 20; s->damping = 0.02; s->amp = AMP_STEP; s->ampT = 0.001;
    s->T0 = 293.15;
}

bool fe_an_mechanical(int an) { return an != AN_HEAT; }
bool fe_an_thermal(int an) { return an == AN_HEAT || an == AN_THERMSTRESS; }

void fe_study_props(const FeStudy *s, FeMatProps *p)
{
    int lib = 0;
    for (int i = 0; i < CCX_LIB_N; i++) if (!strcasecmp(CCX_LIB[i].name, s->material)) lib = i;
    const CcxLibMat *m = &CCX_LIB[lib];
    *p = (FeMatProps){ m->E, m->nu, m->rho, m->yield, m->uts, m->elong, m->k, m->cp, m->alpha };
    if (s->custom) *p = s->props;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}
static int faces_parse(const char *v, char out[][64], int max)
{
    int n = 0; char buf[4096]; pl_strlcpy(buf, v, sizeof buf);
    for (char *tok = strtok(buf, ","); tok && n < max; tok = strtok(NULL, ",")) { char *t = trim(tok); if (*t) pl_strlcpy(out[n++], t, 64); }
    return n;
}
static int name_index(const char *const *names, int n, const char *v) { for (int i = 0; i < n; i++) if (!strcasecmp(names[i], v)) return i; return -1; }

bool fe_study_load(const char *proj, FeStudy *s)
{
    fe_study_default(s);
    char p[PL_PATH]; pl_join(p, sizeof p, proj, "study.ini");
    char *t = pl_read_file(p, NULL);
    if (!t) return false;
    enum { S_NONE, S_PART, S_MAT, S_STUDY, S_FIX, S_LOAD, S_TH, S_REF } sec = S_NONE;
    for (char *l = t; l && *l; ) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        char *line = trim(l);
        l = e ? e + 1 : NULL;
        if (!*line || *line == '#' || *line == ';') continue;
        if (*line == '[') {
            if (!strncasecmp(line, "[part]", 6)) sec = S_PART;
            else if (!strncasecmp(line, "[material]", 10)) sec = S_MAT;
            else if (!strncasecmp(line, "[study]", 7)) sec = S_STUDY;
            else if (!strncasecmp(line, "[fixture]", 9) && s->nFix < FE_MAXITEM) { sec = S_FIX; s->nFix++; }
            else if (!strncasecmp(line, "[load]", 6) && s->nLoad < FE_MAXITEM) { sec = S_LOAD; s->nLoad++; s->load[s->nLoad - 1].dir = DIR_NORMAL; }
            else if (!strncasecmp(line, "[thermal]", 9) && s->nTh < FE_MAXITEM) { sec = S_TH; s->nTh++; s->th[s->nTh - 1].value2 = 293.15; }
            else if (!strncasecmp(line, "[refine]", 8) && s->nRef < FE_MAXREF) { sec = S_REF; s->nRef++; s->ref[s->nRef - 1].size = 1; }
            else sec = S_NONE;
            continue;
        }
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char *k = trim(line), *v = trim(eq + 1);
        double d = atof(v);
        if (sec == S_PART) {
            if (!strcmp(k, "file")) pl_strlcpy(s->part, v, sizeof s->part);
            else if (!strcmp(k, "scale")) s->scale = d > 0 ? d : 1;
            else if (!strcmp(k, "size")) s->size = d;
            else if (!strcmp(k, "order")) s->order = atoi(v) == 1 ? 1 : 2;
        } else if (sec == S_MAT) {
            FeMatProps *m = &s->props;
            if (!strcmp(k, "name")) pl_strlcpy(s->material, v, sizeof s->material);
            else if (!strcmp(k, "custom")) s->custom = atoi(v) != 0;
            else if (!strcmp(k, "E")) m->E = d; else if (!strcmp(k, "nu")) m->nu = d; else if (!strcmp(k, "rho")) m->rho = d;
            else if (!strcmp(k, "yield")) m->yield = d; else if (!strcmp(k, "uts")) m->uts = d; else if (!strcmp(k, "elong")) m->elong = d;
            else if (!strcmp(k, "k")) m->k = d; else if (!strcmp(k, "cp")) m->cp = d; else if (!strcmp(k, "alpha")) m->alpha = d;
        } else if (sec == S_STUDY) {
            if (!strcmp(k, "analysis")) { int i = name_index(FE_AN_WORD, AN__N, v); s->analysis = i < 0 ? AN_STATIC : i; }
            else if (!strcmp(k, "transient")) s->transient = atoi(v) != 0;
            else if (!strcmp(k, "time")) s->time = d > 0 ? d : 1;
            else if (!strcmp(k, "inc")) s->inc = d;
            else if (!strcmp(k, "modes")) s->modes = atoi(v) > 0 ? atoi(v) : 10;
            else if (!strcmp(k, "fLo")) s->fLo = d; else if (!strcmp(k, "fHi")) s->fHi = d;
            else if (!strcmp(k, "points")) s->nPts = atoi(v) > 1 ? atoi(v) : 20;
            else if (!strcmp(k, "damping")) s->damping = d;
            else if (!strcmp(k, "amplitude")) { int i = name_index(FE_AMP_NAME, AMP__N, v); s->amp = i < 0 ? AMP_STEP : i; }
            else if (!strcmp(k, "ampTime")) s->ampT = d;
            else if (!strcmp(k, "T0")) s->T0 = d;
        } else if (sec == S_FIX) {
            FeFixture *f = &s->fix[s->nFix - 1];
            if (!strcmp(k, "kind")) { int i = name_index(FE_FX_NAME, FX__N, v); f->kind = i < 0 ? FX_FIXED : i; }
            else if (!strcmp(k, "faces")) f->nf = faces_parse(v, f->face, FE_MAXFACES);
            else if (!strcmp(k, "value")) f->value = d;
            else if (!strcmp(k, "dir")) { int i = name_index(FE_DIR_NAME, DIR__N, v); f->dir = i < 0 ? DIR_PX : i; }
        } else if (sec == S_LOAD) {
            FeLoad *ld = &s->load[s->nLoad - 1];
            if (!strcmp(k, "kind")) { int i = name_index(FE_LD_NAME, LD__N, v); ld->kind = i < 0 ? LD_FORCE : i; }
            else if (!strcmp(k, "faces")) ld->nf = faces_parse(v, ld->face, FE_MAXFACES);
            else if (!strcmp(k, "value")) ld->value = d;
            else if (!strcmp(k, "dir")) { int i = name_index(FE_DIR_NAME, DIR__N, v); ld->dir = i < 0 ? DIR_NORMAL : i; }
            else if (!strcmp(k, "point")) sscanf(v, "%lf %lf %lf", &ld->pt[0], &ld->pt[1], &ld->pt[2]);
        } else if (sec == S_TH) {
            FeThermal *th = &s->th[s->nTh - 1];
            if (!strcmp(k, "kind")) { int i = name_index(FE_TH_NAME, TH__N, v); th->kind = i < 0 ? TH_TEMP : i; }
            else if (!strcmp(k, "faces")) th->nf = faces_parse(v, th->face, FE_MAXFACES);
            else if (!strcmp(k, "value")) th->value = d;
            else if (!strcmp(k, "ambient")) th->value2 = d;
        } else if (sec == S_REF) {
            FeRefine *r = &s->ref[s->nRef - 1];
            if (!strcmp(k, "faces")) r->nf = faces_parse(v, r->face, FE_MAXFACES);
            else if (!strcmp(k, "size")) r->size = d > 0 ? d : 1;
        }
    }
    free(t);
    return true;
}

static void faces_cat(Str *o, const char f[][64], int n)
{
    for (int i = 0; i < n; i++) str_catf(o, "%s%s", i ? ", " : "", f[i]);
    str_cat(o, "\n");
}
bool fe_study_save(const char *proj, const FeStudy *s)
{
    Str o; str_init(&o);
    str_cat(&o, "# A part study for CalculiX, written by Q-Continuum (Setup edits it; study.inp is written from it at each run).\n"
                "# Units: mm, N, MPa, N m (torque), m/s² (gravity), rpm, K, W, W/m², W/(m² K), Hz, s. Faces by name, as the part's mesh names them.\n");
    str_catf(&o, "[part]\nfile = %s\nscale = %g\nsize = %g\norder = %d\n\n[material]\nname = %s\n", s->part, s->scale, s->size, s->order, s->material);
    if (s->custom) {
        const FeMatProps *m = &s->props;
        str_catf(&o, "custom = 1\nE = %.9g\nnu = %.9g\nrho = %.9g\nyield = %.9g\nuts = %.9g\nelong = %.9g\nk = %.9g\ncp = %.9g\nalpha = %.9g\n",
                 m->E, m->nu, m->rho, m->yield, m->uts, m->elong, m->k, m->cp, m->alpha);
    }
    str_catf(&o, "\n[study]\nanalysis = %s\ntransient = %d\ntime = %.9g\ninc = %.9g\nmodes = %d\nfLo = %.9g\nfHi = %.9g\npoints = %d\ndamping = %.9g\n"
                 "amplitude = %s\nampTime = %.9g\nT0 = %.9g\n", FE_AN_WORD[s->analysis], s->transient, s->time, s->inc, s->modes, s->fLo, s->fHi, s->nPts,
             s->damping, FE_AMP_NAME[s->amp], s->ampT, s->T0);
    for (int i = 0; i < s->nFix; i++) {
        const FeFixture *f = &s->fix[i];
        str_catf(&o, "\n[fixture]\nkind = %s\n", FE_FX_NAME[f->kind]);
        if (f->kind == FX_DISP) str_catf(&o, "value = %.9g\ndir = %s\n", f->value, FE_DIR_NAME[f->dir]);
        str_cat(&o, "faces = "); faces_cat(&o, f->face, f->nf);
    }
    for (int i = 0; i < s->nLoad; i++) {
        const FeLoad *d = &s->load[i];
        str_catf(&o, "\n[load]\nkind = %s\nvalue = %.9g\ndir = %s\n", FE_LD_NAME[d->kind], d->value, FE_DIR_NAME[d->dir]);
        if (d->kind == LD_ROTATION) str_catf(&o, "point = %.9g %.9g %.9g\n", d->pt[0], d->pt[1], d->pt[2]);
        str_cat(&o, "faces = "); faces_cat(&o, d->face, d->nf);
    }
    for (int i = 0; i < s->nTh; i++) {
        const FeThermal *t = &s->th[i];
        str_catf(&o, "\n[thermal]\nkind = %s\nvalue = %.9g\nambient = %.9g\nfaces = ", FE_TH_NAME[t->kind], t->value, t->value2);
        faces_cat(&o, t->face, t->nf);
    }
    for (int i = 0; i < s->nRef; i++) { str_catf(&o, "\n[refine]\nsize = %.9g\nfaces = ", s->ref[i].size); faces_cat(&o, s->ref[i].face, s->ref[i].nf); }
    char p[PL_PATH]; pl_join(p, sizeof p, proj, "study.ini");
    bool ok = pl_write_file(p, o.s, o.len);
    str_free(&o);
    return ok;
}

static void face_centre(const FeMesh *m, int f, double c[3]);

bool fe_refine_write(const FeStudy *s, const FeMesh *m, const char *proj)
{
    char p[PL_PATH]; pl_join(p, sizeof p, proj, "refine.txt");
    Str o; str_init(&o);
    int pts = 0;
    for (int i = 0; i < s->nRef; i++) {
        const FeRefine *r = &s->ref[i];
        for (int j = 0; j < r->nf; j++) str_catf(&o, "face %s = %.9g\n", r->face[j], r->size);   /* a CAD part: the face by name */
        if (!m || !m->n) continue;
        for (int j = 0; j < r->nf; j++) {
            int g = fe_group(m, r->face[j]);
            if (g < 0) continue;
            /* the faces' corners and centres, at most 4000 points per face group (every k-th face) */
            int nfg = 0; for (int f = 0; f < m->nf; f++) nfg += m->fg[f] == g;
            int every = nfg > 4000 ? nfg / 4000 + 1 : 1, k = 0;
            for (int f = 0; f < m->nf; f++) {
                if (m->fg[f] != g || k++ % every) continue;
                double c[3]; face_centre(m, f, c);
                str_catf(&o, "%.6g %.6g %.6g %.6g\n", c[0], c[1], c[2], r->size);
                int v = m->fn[8 * f];
                str_catf(&o, "%.6g %.6g %.6g %.6g\n", m->x[3 * v], m->x[3 * v + 1], m->x[3 * v + 2], r->size);
                pts += 2;
            }
        }
    }
    if (!o.len) { remove(p); str_free(&o); return false; }
    bool ok = pl_write_file(p, o.s, o.len);
    str_free(&o);
    return ok && (pts > 0 || s->nRef > 0);
}

int fe_group(const FeMesh *m, const char *name)
{
    for (int g = 0; g < m->ng; g++) if (!strcasecmp(m->gname[g], name)) return g;
    return -1;
}

/* ---------------------------------------------------------------------------------------------------- geometry */
static void dir_vec(int dir, double v[3])
{
    v[0] = v[1] = v[2] = 0;
    if (dir >= DIR_PX && dir <= DIR_MZ) v[(dir - 1) % 3] = dir <= DIR_PZ ? 1 : -1;
}
static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void face_centre(const FeMesh *m, int f, double c[3])
{
    int k = m->fk[f] == 4 || m->fk[f] == 8 ? 4 : 3;
    c[0] = c[1] = c[2] = 0;
    for (int j = 0; j < k; j++) for (int q = 0; q < 3; q++) c[q] += m->x[3 * m->fn[8 * f + j] + q] / k;
}

/* the smallest-eigenvalue eigenvector of a symmetric 3x3 (Jacobi rotations) */
static void eig3(double A[3][3], double val[3], double vec[3][3])
{
    double V[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int sweep = 0; sweep < 50; sweep++) {
        double off = fabs(A[0][1]) + fabs(A[0][2]) + fabs(A[1][2]);
        if (off < 1e-14 * (fabs(A[0][0]) + fabs(A[1][1]) + fabs(A[2][2]) + 1e-300)) break;
        for (int p = 0; p < 2; p++) for (int q = p + 1; q < 3; q++) {
            if (fabs(A[p][q]) < 1e-300) continue;
            double th = (A[q][q] - A[p][p]) / (2 * A[p][q]);
            double t = (th >= 0 ? 1 : -1) / (fabs(th) + sqrt(th * th + 1)), c = 1 / sqrt(t * t + 1), s = t * c;
            for (int k = 0; k < 3; k++) { double akp = A[k][p], akq = A[k][q]; A[k][p] = c * akp - s * akq; A[k][q] = s * akp + c * akq; }
            for (int k = 0; k < 3; k++) { double apk = A[p][k], aqk = A[q][k]; A[p][k] = c * apk - s * aqk; A[q][k] = s * apk + c * aqk; }
            for (int k = 0; k < 3; k++) { double vkp = V[k][p], vkq = V[k][q]; V[k][p] = c * vkp - s * vkq; V[k][q] = s * vkp + c * vkq; }
        }
    }
    for (int i = 0; i < 3; i++) { val[i] = A[i][i]; for (int k = 0; k < 3; k++) vec[i][k] = V[k][i]; }
}

bool fe_cylinder(const FeMesh *m, int g, double axis[3], double ctr[3], double *radius)
{
    double M[3][3] = { { 0 } }, Atot = 0;
    for (int f = 0; f < m->nf; f++) {
        if (m->fg[f] != g) continue;
        double n[3], A = femesh_face_area(m, f, n);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i][j] += A * n[i] * n[j];
        Atot += A;
    }
    if (Atot <= 0) return false;
    double val[3], vec[3][3];
    eig3(M, val, vec);
    int lo = 0, hi = 0;
    for (int i = 1; i < 3; i++) { if (val[i] < val[lo]) lo = i; if (val[i] > val[hi]) hi = i; }
    int mid = 3 - lo - hi;
    /* normals all across one direction (lo ~ 0), spread over a plane (mid not ~ 0): a cylinder, or an arc of one over 30 degrees */
    if (val[lo] > 0.02 * Atot || val[mid] < 0.03 * Atot) return false;
    for (int k = 0; k < 3; k++) axis[k] = vec[lo][k];
    /* the axis's point: the least-squares meeting point of the normal lines, in the plane across the axis */
    double S[3][3] = { { 0 } }, b[3] = { 0, 0, 0 };
    for (int f = 0; f < m->nf; f++) {
        if (m->fg[f] != g) continue;
        double n[3], c[3], A = femesh_face_area(m, f, n);
        face_centre(m, f, c);
        double d[3], dn = dot3(n, axis);
        for (int q = 0; q < 3; q++) d[q] = n[q] - dn * axis[q];
        double dl = sqrt(dot3(d, d)); if (dl < 1e-12) continue;
        for (int q = 0; q < 3; q++) d[q] /= dl;
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
            double P = (i == j) - d[i] * d[j] - axis[i] * axis[j];
            S[i][j] += A * P; b[i] += A * P * c[j];
        }
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) S[i][j] += Atot * axis[i] * axis[j];   /* along the axis: pinned at 0 */
    /* solve S x = b (Cramer) */
    double det = S[0][0] * (S[1][1] * S[2][2] - S[1][2] * S[2][1]) - S[0][1] * (S[1][0] * S[2][2] - S[1][2] * S[2][0]) + S[0][2] * (S[1][0] * S[2][1] - S[1][1] * S[2][0]);
    if (fabs(det) < 1e-300) return false;
    for (int c = 0; c < 3; c++) {
        double T[3][3]; memcpy(T, S, sizeof T);
        for (int r = 0; r < 3; r++) T[r][c] = b[r];
        ctr[c] = (T[0][0] * (T[1][1] * T[2][2] - T[1][2] * T[2][1]) - T[0][1] * (T[1][0] * T[2][2] - T[1][2] * T[2][0]) + T[0][2] * (T[1][0] * T[2][1] - T[1][1] * T[2][0])) / det;
    }
    double rs = 0; int nr = 0;
    for (int f = 0; f < m->nf; f++) {
        if (m->fg[f] != g) continue;
        double c[3], d[3]; face_centre(m, f, c);
        for (int q = 0; q < 3; q++) d[q] = c[q] - ctr[q];
        double al = dot3(d, axis);
        for (int q = 0; q < 3; q++) d[q] -= al * axis[q];
        rs += sqrt(dot3(d, d)); nr++;
    }
    if (radius) *radius = nr ? rs / nr : 0;
    return true;
}

/* the part's volume, mm³ (the divergence theorem over its boundary faces) */
static double mesh_volume(const FeMesh *m)
{
    double V = 0;
    for (int f = 0; f < m->nf; f++) { double n[3], c[3], A = femesh_face_area(m, f, n); face_centre(m, f, c); V += dot3(c, n) * A / 3; }
    return V;
}

/* ---------------------------------------------------------------------------------------------------- loads */
/* consistent nodal forces of a uniform traction t (N/mm²) over boundary face f, added into F (3 per node) */
static void face_load(const FeMesh *m, int f, const double t[3], double *F)
{
    double n[3], A = femesh_face_area(m, f, n);
    const int *v = &m->fn[8 * f];
    int k = m->fk[f];
    double wc, wm; int c;
    switch (k) {
    case 3: c = 3; wc = A / 3; wm = 0; break;
    case 6: c = 3; wc = 0; wm = A / 3; break;
    case 4: c = 4; wc = A / 4; wm = 0; break;
    default: c = 4; wc = -A / 12; wm = A / 3; break;           /* 8-node quadrangle */
    }
    for (int j = 0; j < c; j++) for (int q = 0; q < 3; q++) F[3 * v[j] + q] += wc * t[q];
    if (k > c) for (int j = 0; j < c; j++) for (int q = 0; q < 3; q++) F[3 * v[c + j] + q] += wm * t[q];
}

/* the groups of a face list, as flags (in[g]); their area; unknown names counted */
static double groups_of(const FeMesh *m, char faces[][64], int nf, char *in, int *unknown)
{
    memset(in, 0, (size_t)m->ng + 1);
    double At = 0;
    for (int j = 0; j < nf; j++) {
        int g = fe_group(m, faces[j]);
        if (g < 0) { if (unknown) (*unknown)++; continue; }
        if (!in[g]) { in[g] = 1; At += m->garea[g]; }
    }
    return At;
}

/* the nodal forces of the face loads (force, pressure, bearing, torque), 3 per node; NULL only when memory ran out */
static double *loads_of(const FeStudy *s, const FeMesh *m, int *unknown)
{
    double *F = calloc(3 * (size_t)m->n, sizeof(double));
    char *in = malloc((size_t)m->ng + 1);
    if (!F || !in) { free(F); free(in); return NULL; }
    for (int i = 0; i < s->nLoad; i++) {
        const FeLoad *d = &s->load[i];
        if (d->kind == LD_GRAVITY || d->kind == LD_ROTATION) continue;   /* body loads: *DLOAD */
        double At = groups_of(m, (char (*)[64])d->face, d->nf, in, unknown);
        if (At <= 0) continue;
        double dv[3]; dir_vec(d->dir, dv);
        if (d->kind == LD_BEARING) {
            /* pressure p0 (-n.d) where the faces face against d: its resultant is the force along d */
            double D = 0;
            for (int f = 0; f < m->nf; f++) {
                if (!in[m->fg[f]]) continue;
                double n[3], A = femesh_face_area(m, f, n), nd = dot3(n, dv);
                if (nd < 0) D += A * nd * nd;
            }
            if (D <= 0) continue;
            double p0 = d->value / D;
            for (int f = 0; f < m->nf; f++) {
                if (!in[m->fg[f]]) continue;
                double n[3], t[3]; femesh_face_area(m, f, n);
                double nd = dot3(n, dv);
                if (nd >= 0) continue;
                for (int q = 0; q < 3; q++) t[q] = p0 * nd * n[q];         /* -p n with p = p0 (-n.d) */
                face_load(m, f, t, F);
            }
            continue;
        }
        if (d->kind == LD_TORQUE) {
            /* tangential traction T r / J about the faces' cylinder axis (the first face's group) */
            int g0 = -1; for (int g = 0; g < m->ng && g0 < 0; g++) if (in[g]) g0 = g;
            double ax[3], c[3], R;
            if (g0 < 0 || !fe_cylinder(m, g0, ax, c, &R)) continue;
            double J = 0, T = d->value * 1000;                              /* N m -> N mm */
            for (int pass = 0; pass < 2; pass++)
                for (int f = 0; f < m->nf; f++) {
                    if (!in[m->fg[f]]) continue;
                    double n[3], x[3], r[3], A = femesh_face_area(m, f, n);
                    face_centre(m, f, x);
                    for (int q = 0; q < 3; q++) r[q] = x[q] - c[q];
                    double al = dot3(r, ax);
                    for (int q = 0; q < 3; q++) r[q] -= al * ax[q];
                    double rl = sqrt(dot3(r, r));
                    if (pass == 0) { J += A * rl * rl; continue; }
                    if (J <= 0 || rl < 1e-12) continue;
                    double e[3] = { ax[1] * r[2] - ax[2] * r[1], ax[2] * r[0] - ax[0] * r[2], ax[0] * r[1] - ax[1] * r[0] }, t[3];
                    for (int q = 0; q < 3; q++) t[q] = T / J * e[q];        /* |e| = r: T r / J along the tangent */
                    face_load(m, f, t, F);
                }
            continue;
        }
        for (int f = 0; f < m->nf; f++) {
            if (!in[m->fg[f]]) continue;
            double n[3], t[3] = { 0, 0, 0 }; femesh_face_area(m, f, n);
            if (d->kind == LD_PRESSURE) for (int q = 0; q < 3; q++) t[q] = -d->value * n[q];
            else if (d->dir == DIR_NORMAL) for (int q = 0; q < 3; q++) t[q] = -d->value / At * n[q];
            else for (int q = 0; q < 3; q++) t[q] = dv[q] * d->value / At;
            face_load(m, f, t, F);
        }
    }
    free(in);
    return F;
}

/* the nodes on a face list (a flag per node); -1 when memory ran out */
static int nodes_of(const FeMesh *m, char faces[][64], int nf, char *flag, int *unknown)
{
    memset(flag, 0, (size_t)m->n);
    char *in = calloc((size_t)m->ng + 1, 1);
    if (!in) return -1;
    groups_of(m, faces, nf, in, unknown);
    int n = 0;
    for (int f = 0; f < m->nf; f++) if (in[m->fg[f]]) for (int j = 0; j < m->fk[f]; j++) { int v = m->fn[8 * f + j]; if (!flag[v]) { flag[v] = 1; n++; } }
    free(in);
    return n;
}

/* the dof a roller holds when its face is square to an axis (0 x, 1 y, 2 z), else -1 (an equation per node) */
static int roller_axis(const FeMesh *m, const FeFixture *fx, double nrm[3])
{
    nrm[0] = nrm[1] = nrm[2] = 0;
    for (int j = 0; j < fx->nf; j++) { int g = fe_group(m, fx->face[j]); if (g >= 0) for (int q = 0; q < 3; q++) nrm[q] += m->garea[g] * m->gnorm[3 * g + q]; }
    double l = sqrt(dot3(nrm, nrm));
    if (l <= 0) return -1;
    for (int q = 0; q < 3; q++) nrm[q] /= l;
    for (int q = 0; q < 3; q++) if (fabs(nrm[q]) > 0.9998) return q;
    return -1;
}

/* ---------------------------------------------------------------------------------------------------- summary */
void fe_study_summary(const FeStudy *s, const FeMesh *m, FeSummary *o)
{
    memset(o, 0, sizeof *o);
    char *flag = malloc((size_t)m->n), *any = calloc((size_t)m->n, 1);
    double *F = flag && any ? loads_of(s, m, &o->unknownFaces) : NULL;
    if (!F) { free(flag); free(any); snprintf(o->problem, sizeof o->problem, "Not enough memory free to check the study"); return; }
    bool mech = fe_an_mechanical(s->analysis), therm = fe_an_thermal(s->analysis);
    bool held[3] = { false, false, false }, body = false;
    int pins = 0, rollers = 0;
    for (int i = 0; mech && i < s->nFix; i++) {
        const FeFixture *fx = &s->fix[i];
        if (nodes_of(m, (char (*)[64])fx->face, fx->nf, flag, &o->unknownFaces) > 0) {
            if (fx->kind == FX_FIXED) held[0] = held[1] = held[2] = true;
            else if (fx->kind >= FX_X && fx->kind <= FX_Z) held[fx->kind - FX_X] = true;
            else if (fx->kind == FX_DISP && fx->dir >= DIR_PX) held[(fx->dir - 1) % 3] = true;
            else if (fx->kind == FX_ROLLER) { double n[3]; int ax = roller_axis(m, fx, n); if (ax >= 0) held[ax] = true; else rollers++; }
            else if (fx->kind == FX_PIN) pins++;
        }
        for (int v = 0; v < m->n; v++) any[v] |= flag[v];
    }
    /* a pin holds round and along its axis: two pins, or a pin and a roller across, hold it (free turning aside) */
    if (pins) { held[0] = held[1] = held[2] = true; }
    if (rollers >= 3) held[0] = held[1] = held[2] = true;
    for (int v = 0; v < m->n; v++) o->fixedNodes += any[v];
    for (int v = 0; v < m->n; v++) { bool l = false; for (int q = 0; q < 3; q++) { o->force[q] += F[3 * v + q]; if (F[3 * v + q] != 0) l = true; } o->loadedNodes += l; }
    for (int i = 0; i < s->nLoad; i++) if (s->load[i].kind == LD_GRAVITY || s->load[i].kind == LD_ROTATION) body = true;
    int temps = 0, heatIn = 0;
    for (int i = 0; therm && i < s->nTh; i++) {
        const FeThermal *t = &s->th[i];
        if (t->kind == TH_HEATGEN) { heatIn++; continue; }
        if (nodes_of(m, (char (*)[64])t->face, t->nf, flag, &o->unknownFaces) <= 0) continue;
        if (t->kind == TH_TEMP || t->kind == TH_CONVECT || t->kind == TH_RADIATE) temps++;
        if (t->kind == TH_FLUX) heatIn++;
    }
    free(F); free(flag); free(any);
    FeFree fr; fr.n = 0;
    (void)held;
    bool needsLoad = s->analysis == AN_STATIC || s->analysis == AN_NONLINEAR || s->analysis == AN_BUCKLE || s->analysis == AN_DYNAMIC || s->analysis == AN_HARMONIC;
    if (o->unknownFaces) snprintf(o->problem, sizeof o->problem, "%d face name%s the mesh doesn't have (meshed again with other faces?): pick them again",
                                  o->unknownFaces, o->unknownFaces == 1 ? "" : "s");
    else if (mech && (!s->nFix || !o->fixedNodes) && s->analysis != AN_FREQUENCY)
        snprintf(o->problem, sizeof o->problem, "Nothing holds the part: add a support (pick the faces it is bolted, welded or resting at)");
    else if (mech && s->analysis != AN_FREQUENCY && (fe_free_motions(s, m, &fr), fr.n > 0))
        snprintf(o->problem, sizeof o->problem, fr.m[0].rotation ? "The supports leave the part free to turn about an axis: add a support away from it (Checks says where)"
                                                                 : "The supports leave the part free to slide: add a support that holds it that way (Checks says which way)");
    else if (needsLoad && !o->loadedNodes && !body)
        snprintf(o->problem, sizeof o->problem, "No load: add a force, a pressure, gravity or another load");
    else if (therm && !temps && !s->transient)
        snprintf(o->problem, sizeof o->problem, "Nothing sets a temperature: hold a face at a temperature, or give it convection or radiation to its surroundings "
                                                "(steady, heat must have a way out)");
    else if (s->analysis == AN_HEAT && !heatIn && !temps)
        snprintf(o->problem, sizeof o->problem, "No heat goes in or out: add a temperature, a heat flux or heat made inside");
    else if (s->analysis == AN_HARMONIC && !(s->fHi > s->fLo && s->fLo >= 0))
        snprintf(o->problem, sizeof o->problem, "Give the frequency range: from a lower to a higher frequency (Hz)");
}

/* ---------------------------------------------------------------------------------------------------- writing */
static void nset_write(Str *o, const char *name, const FeMesh *m, const char *flag)
{
    str_catf(o, "*NSET, NSET=%s\n", name);
    int col = 0;
    for (int v = 0; v < m->n; v++) if (flag[v]) str_catf(o, "%d,%s", m->id[v], ++col % 12 ? " " : "\n");
    if (col % 12) str_cat(o, "\n");
}

typedef struct { Str *o; const FeStudy *s; const FeMesh *m; char *flag; double *F; unsigned char *held; } Wr;

/* supports into the step: *BOUNDARY lines; rollers off an axis as *EQUATION in the model (written before the step) */
static void supports(Wr *w, Str *model, Str *step)
{
    const FeStudy *s = w->s; const FeMesh *m = w->m;
    memset(w->held, 0, (size_t)m->n);
    /* pins: a cylindrical coordinate system on their nodes, radial (1) and axial (3) held */
    for (int i = 0; i < s->nFix; i++) {
        const FeFixture *fx = &s->fix[i];
        if (nodes_of(m, (char (*)[64])fx->face, fx->nf, w->flag, NULL) <= 0) continue;
        char nm[16]; snprintf(nm, sizeof nm, "FIX%d", i + 1);
        switch (fx->kind) {
        case FX_FIXED: str_catf(step, "%s, 1, 3\n", nm); for (int v = 0; v < m->n; v++) if (w->flag[v]) w->held[v] |= 7; break;
        case FX_X: case FX_Y: case FX_Z: {
            int k = fx->kind - FX_X + 1; str_catf(step, "%s, %d, %d\n", nm, k, k);
            for (int v = 0; v < m->n; v++) if (w->flag[v]) w->held[v] |= (unsigned char)(1 << (k - 1));
            break;
        }
        case FX_DISP: {
            int k = fx->dir >= DIR_PX ? (fx->dir - 1) % 3 + 1 : 1;
            double val = fx->dir >= DIR_MX ? -fx->value : fx->value;
            str_catf(step, "%s, %d, %d, %.9g\n", nm, k, k, val);
            for (int v = 0; v < m->n; v++) if (w->flag[v]) w->held[v] |= (unsigned char)(1 << (k - 1));
            break;
        }
        case FX_PIN: {
            int g = fe_group(m, fx->face[0]); double ax[3], c[3], R;
            if (g < 0 || !fe_cylinder(m, g, ax, c, &R)) break;
            str_catf(model, "*TRANSFORM, TYPE=C, NSET=%s\n%.9g, %.9g, %.9g, %.9g, %.9g, %.9g\n", nm, c[0], c[1], c[2], c[0] + ax[0] * 100, c[1] + ax[1] * 100, c[2] + ax[2] * 100);
            str_catf(step, "%s, 1, 1\n%s, 3, 3\n", nm, nm);
            for (int v = 0; v < m->n; v++) if (w->flag[v]) w->held[v] |= 7;   /* its local dofs: nothing else may hold them */
            break;
        }
        default: break;
        }
    }
    /* rollers last: square to an axis, that dof; else n.u = 0 per node, its dependent dof one not held yet */
    for (int i = 0; i < s->nFix; i++) {
        const FeFixture *fx = &s->fix[i];
        if (fx->kind != FX_ROLLER || nodes_of(m, (char (*)[64])fx->face, fx->nf, w->flag, NULL) <= 0) continue;
        double n[3]; int ax = roller_axis(m, fx, n);
        char nm[16]; snprintf(nm, sizeof nm, "FIX%d", i + 1);
        if (ax >= 0) { str_catf(step, "%s, %d, %d\n", nm, ax + 1, ax + 1); for (int v = 0; v < m->n; v++) if (w->flag[v]) w->held[v] |= (unsigned char)(1 << ax); continue; }
        for (int v = 0; v < m->n; v++) {
            if (!w->flag[v] || w->held[v] == 7) continue;
            int terms[3], nt = 0, dep = -1;
            for (int q = 0; q < 3; q++) if (!(w->held[v] & (1 << q)) && fabs(n[q]) > 1e-9) terms[nt++] = q;
            if (!nt) continue;
            for (int k = 0; k < nt; k++) if (dep < 0 || fabs(n[terms[k]]) > fabs(n[dep])) dep = terms[k];
            str_catf(model, "*EQUATION\n%d\n%d, %d, %.9g", nt, m->id[v], dep + 1, n[dep]);
            for (int k = 0; k < nt; k++) if (terms[k] != dep) str_catf(model, ", %d, %d, %.9g", m->id[v], terms[k] + 1, n[terms[k]]);
            str_cat(model, "\n");
            w->held[v] |= (unsigned char)(1 << dep);
        }
    }
}

static void cloads(Wr *w, Str *step, const char *amp)
{
    bool head = false;
    for (int v = 0; v < w->m->n; v++) for (int q = 0; q < 3; q++) if (w->F[3 * v + q] != 0) {
        if (!head) { str_catf(step, "*CLOAD%s%s\n", amp ? ", AMPLITUDE=" : "", amp ? amp : ""); head = true; }
        str_catf(step, "%d, %d, %.9g\n", w->m->id[v], q + 1, w->F[3 * v + q]);
    }
}

static void body_loads(const FeStudy *s, Str *step, const char *amp)
{
    for (int i = 0; i < s->nLoad; i++) {
        const FeLoad *d = &s->load[i];
        double dv[3]; dir_vec(d->dir, dv);
        if (d->kind == LD_GRAVITY && (dv[0] || dv[1] || dv[2]))
            str_catf(step, "*DLOAD%s%s\nEALL, GRAV, %.9g, %g, %g, %g\n", amp ? ", AMPLITUDE=" : "", amp ? amp : "", d->value * 1000, dv[0], dv[1], dv[2]);
        else if (d->kind == LD_ROTATION && (dv[0] || dv[1] || dv[2])) {
            double w = d->value * 2 * M_PI / 60;                               /* rpm -> rad/s */
            str_catf(step, "*DLOAD%s%s\nEALL, CENTRIF, %.9g, %.9g, %.9g, %.9g, %g, %g, %g\n", amp ? ", AMPLITUDE=" : "", amp ? amp : "", w * w,
                     d->pt[0], d->pt[1], d->pt[2], fabs(dv[0]), fabs(dv[1]), fabs(dv[2]));
        }
    }
}

/* thermal conditions into the step; element faces by their CalculiX number */
static void thermal(Wr *w, Str *step, double volume)
{
    const FeStudy *s = w->s; const FeMesh *m = w->m;
    char *in = malloc((size_t)m->ng + 1);
    if (!in) return;
    for (int i = 0; i < s->nTh; i++) {
        const FeThermal *t = &s->th[i];
        if (t->kind == TH_HEATGEN) {
            if (volume > 0) str_catf(step, "*DFLUX\nEALL, BF, %.9g\n", t->value * 1000 / volume);   /* W -> mW, per mm³ */
            continue;
        }
        if (t->kind == TH_TEMP) {
            if (nodes_of(m, (char (*)[64])t->face, t->nf, w->flag, NULL) > 0) str_catf(step, "*BOUNDARY\nTH%d, 11, 11, %.9g\n", i + 1, t->value);
            continue;
        }
        groups_of(m, (char (*)[64])t->face, t->nf, in, NULL);
        const char *kw = t->kind == TH_FLUX ? "*DFLUX" : t->kind == TH_CONVECT ? "*FILM" : "*RADIATE";
        bool head = false;
        for (int f = 0; f < m->nf; f++) {
            if (!in[m->fg[f]]) continue;
            if (!head) { str_catf(step, "%s\n", kw); head = true; }
            int e = m->fe[f] + 1, k = m->fl[f];
            if (t->kind == TH_FLUX) str_catf(step, "%d, S%d, %.9g\n", e, k, t->value * 1e-3);                 /* W/m² -> mW/mm² */
            else if (t->kind == TH_CONVECT) str_catf(step, "%d, F%d, %.9g, %.9g\n", e, k, t->value2, t->value * 1e-3);
            else str_catf(step, "%d, R%d, %.9g, %.9g\n", e, k, t->value2, t->value);
        }
    }
    free(in);
}

static void amplitude(const FeStudy *s, Str *model)
{
    double T = s->time > 0 ? s->time : 1, a = s->ampT > 0 ? fmin(s->ampT, T) : T / 100;
    str_cat(model, "*AMPLITUDE, NAME=LOADT\n");
    if (s->amp == AMP_STEP) str_catf(model, "0., 1., %.9g, 1.\n", T);
    else if (s->amp == AMP_RAMP) str_catf(model, "0., 0., %.9g, 1., %.9g, 1.\n", a, fmax(T, a * 1.001));
    else {                                                               /* half a sine over the pulse, then nothing */
        for (int k = 0; k <= 20; k++) str_catf(model, "%.9g, %.9g%s", a * k / 20, sin(M_PI * k / 20), k % 4 == 3 ? ",\n" : ", ");
        str_catf(model, "%.9g, 0., %.9g, 0.\n", a * 1.0001, fmax(T, a * 1.01));
    }
}

bool fe_study_write(const FeStudy *s, const FeMesh *m, const char *proj, char *err, int cap)
{
    err[0] = 0;
    FeMatProps mp; fe_study_props(s, &mp);
    char matName[80]; snprintf(matName, sizeof matName, "%s", s->custom ? "CUSTOM" : s->material);
    for (char *q = matName; *q; q++) if (*q == ',' || *q == '=') *q = ' ';
    int an = s->analysis;
    bool mech = fe_an_mechanical(an), therm = fe_an_thermal(an);
    /* the mesh: nodes, elements by type, each support's and temperature's nodes */
    Str o; str_init(&o);
    str_cat(&o, "** The part's mesh, written by Q-Continuum from part.qcmesh (millimetres). Meshing again rewrites it.\n*NODE, NSET=NALL\n");
    for (int i = 0; i < m->n; i++) str_catf(&o, "%d, %.9g, %.9g, %.9g\n", m->id[i], m->x[3 * i], m->x[3 * i + 1], m->x[3 * i + 2]);
    static const int KS[] = { 10, 4, 20, 8, 15, 6 };
    for (size_t t = 0; t < sizeof KS / sizeof KS[0]; t++) {
        bool any = false;
        for (int e = 0; e < m->ne; e++) {
            if (m->ek[e] != KS[t]) continue;
            if (!any) { str_catf(&o, "*ELEMENT, TYPE=%s, ELSET=EALL\n", femesh_ccx_type(KS[t])); any = true; }
            str_catf(&o, "%d", e + 1);
            for (int i = 0; i < KS[t]; i++) str_catf(&o, i == 15 ? ",\n%d" : ", %d", m->id[m->en[FE_MAXK * e + i]]);
            str_cat(&o, "\n");
        }
    }
    char *flag = malloc((size_t)m->n);
    unsigned char *held = calloc((size_t)m->n, 1);
    double *F = flag && held ? loads_of(s, m, NULL) : NULL;
    if (!F) { free(flag); free(held); str_free(&o); snprintf(err, (size_t)cap, "Not enough memory free to write the study"); return false; }
    for (int i = 0; mech && i < s->nFix; i++) {
        int n = nodes_of(m, (char (*)[64])s->fix[i].face, s->fix[i].nf, flag, NULL);
        if (n < 0) { free(flag); free(held); free(F); str_free(&o); snprintf(err, (size_t)cap, "Not enough memory free to write the study"); return false; }
        char nm[16]; snprintf(nm, sizeof nm, "FIX%d", i + 1);
        if (n) nset_write(&o, nm, m, flag);
    }
    for (int i = 0; therm && i < s->nTh; i++) {
        if (s->th[i].kind != TH_TEMP) continue;
        if (nodes_of(m, (char (*)[64])s->th[i].face, s->th[i].nf, flag, NULL) > 0) { char nm[16]; snprintf(nm, sizeof nm, "TH%d", i + 1); nset_write(&o, nm, m, flag); }
    }
    char p[PL_PATH]; pl_join(p, sizeof p, proj, "study_mesh.inp");
    bool ok = pl_write_file(p, o.s, o.len);
    str_free(&o);
    if (!ok) { snprintf(err, (size_t)cap, "Could not write %s", p); free(flag); free(held); free(F); return false; }

    /* the study: model data, then the step(s) */
    Wr w = { NULL, s, m, flag, F, held };
    Str model, step; str_init(&model); str_init(&step);
    str_catf(&model, "** %s: %s of %s, written by Q-Continuum from study.ini (Setup). Edits here are overwritten at the next run\n"
                     "** from Setup; to keep a hand-edited deck, save it under another name.\n"
                     "** Units: mm, N, MPa, tonnes/mm³, s, K, mJ, mW. Loads are consistent nodal forces on the faces picked.\n"
                     "*INCLUDE, INPUT=study_mesh.inp\n", "study.inp", FE_AN_NAME[an], s->part);
    str_catf(&model, "*MATERIAL, NAME=%s\n*ELASTIC\n%.6g, %.4g\n*DENSITY\n%.6g\n", matName, mp.E / 1e6, mp.nu, mp.rho * 1e-12);
    if (an == AN_NONLINEAR && mp.yield > 0) {
        /* bilinear: yield at no plastic strain, the ultimate strength at the elongation's plastic part */
        double ep = mp.elong - mp.uts / mp.E;
        str_catf(&model, "*PLASTIC\n%.6g, 0.\n", mp.yield / 1e6);
        if (mp.uts > mp.yield && ep > 0) str_catf(&model, "%.6g, %.6g\n", mp.uts / 1e6, ep);
    }
    if (an == AN_THERMSTRESS) str_catf(&model, "*EXPANSION, ZERO=%.9g\n%.6g\n", s->T0, mp.alpha);   /* stress-free at T0 */
    if (therm) str_catf(&model, "*CONDUCTIVITY\n%.6g\n*SPECIFIC HEAT\n%.6g\n", mp.k, mp.cp * 1e6);   /* W/(m K) = mW/(mm K); J/(kg K) -> mJ/(t K) */
    str_catf(&model, "*SOLID SECTION, ELSET=EALL, MATERIAL=%s\n", matName);
    if (therm) str_catf(&model, "*PHYSICAL CONSTANTS, ABSOLUTE ZERO=0., STEFAN BOLTZMANN=%.7g\n", SIGMA_MM);   /* kelvin; radiation */
    if (therm) str_catf(&model, "*INITIAL CONDITIONS, TYPE=TEMPERATURE\nNALL, %.9g\n", s->T0);
    Str bnd; str_init(&bnd);
    if (mech) supports(&w, &model, &bnd);
    if (an == AN_DYNAMIC) amplitude(s, &model);

    const char *out_mech = "*NODE FILE\nU, RF\n*EL FILE\nS, E\n";
    switch (an) {
    case AN_STATIC:
        str_cat(&step, "*STEP\n*STATIC\n");
        if (bnd.s) { str_cat(&step, "*BOUNDARY\n"); str_cat(&step, bnd.s); }
        cloads(&w, &step, NULL); body_loads(s, &step, NULL);
        str_cat(&step, out_mech);
        str_cat(&step, "*END STEP\n");
        break;
    case AN_NONLINEAR: {
        double inc = s->inc > 0 ? s->inc : 0.1;
        str_catf(&step, "*STEP, NLGEOM, INC=1000\n*STATIC\n%.6g, 1., %.6g, %.6g\n", inc, fmin(inc, 1e-5), fmin(inc * 2, 0.25));
        if (bnd.s) { str_cat(&step, "*BOUNDARY\n"); str_cat(&step, bnd.s); }
        cloads(&w, &step, NULL); body_loads(s, &step, NULL);
        str_cat(&step, "*NODE FILE\nU, RF\n*EL FILE\nS, E, PEEQ\n*END STEP\n");
        break;
    }
    case AN_FREQUENCY:
        str_catf(&step, "*STEP\n*FREQUENCY, STORAGE=YES\n%d\n", s->modes > 0 ? s->modes : 10);
        if (bnd.s) { str_cat(&step, "*BOUNDARY\n"); str_cat(&step, bnd.s); }
        str_cat(&step, "*NODE FILE\nU\n*EL FILE\nS\n*END STEP\n");
        break;
    case AN_BUCKLE:
        str_catf(&step, "*STEP\n*BUCKLE\n%d, 0.01\n", s->modes > 0 ? (s->modes < 10 ? s->modes : 5) : 5);
        if (bnd.s) { str_cat(&step, "*BOUNDARY\n"); str_cat(&step, bnd.s); }
        cloads(&w, &step, NULL); body_loads(s, &step, NULL);
        str_cat(&step, "*NODE FILE\nU\n*END STEP\n");
        break;
    case AN_HEAT:
    case AN_THERMSTRESS: {
        double T = s->time > 0 ? s->time : 1, inc = s->inc > 0 ? s->inc : T / 50;
        if (an == AN_HEAT) {
            if (s->transient) str_catf(&step, "*STEP, INC=10000\n*HEAT TRANSFER\n%.6g, %.6g\n", inc, T);
            else str_cat(&step, "*STEP\n*HEAT TRANSFER, STEADY STATE\n1., 1.\n");
        } else {
            if (s->transient) str_catf(&step, "*STEP, INC=10000\n*COUPLED TEMPERATURE-DISPLACEMENT\n%.6g, %.6g\n", inc, T);
            else str_cat(&step, "*STEP\n*COUPLED TEMPERATURE-DISPLACEMENT, STEADY STATE\n1., 1.\n");
            if (bnd.s) { str_cat(&step, "*BOUNDARY\n"); str_cat(&step, bnd.s); }
            cloads(&w, &step, NULL); body_loads(s, &step, NULL);
        }
        thermal(&w, &step, mesh_volume(m));
        if (an == AN_HEAT) str_cat(&step, "*NODE FILE\nNT, RFL\n*EL FILE\nHFL\n*END STEP\n");
        else str_cat(&step, "*NODE FILE\nU, RF, NT\n*EL FILE\nS, E, HFL\n*END STEP\n");
        break;
    }
    case AN_DYNAMIC:
    case AN_HARMONIC: {
        int nm = s->modes > 0 ? s->modes : 10;
        str_catf(&step, "*STEP\n*FREQUENCY, STORAGE=YES\n%d\n", nm);
        if (bnd.s) { str_cat(&step, "*BOUNDARY\n"); str_cat(&step, bnd.s); }
        str_cat(&step, "*NODE FILE\nU\n*END STEP\n");
        if (an == AN_DYNAMIC) {
            double T = s->time > 0 ? s->time : 0.01, inc = s->inc > 0 ? s->inc : T / 400;
            int every = (int)ceil(T / inc / 200);                               /* about 200 frames written */
            str_catf(&step, "*STEP, INC=1000000\n*MODAL DYNAMIC\n%.6g, %.6g\n", inc, T);
            str_catf(&step, "*MODAL DAMPING\n1, %d, %.6g\n", nm, s->damping);
            cloads(&w, &step, "LOADT"); body_loads(s, &step, "LOADT");
            str_catf(&step, "*NODE FILE, FREQUENCY=%d\nU\n*EL FILE, FREQUENCY=%d\nS\n*END STEP\n", every < 1 ? 1 : every, every < 1 ? 1 : every);
        } else {
            str_catf(&step, "*STEP\n*STEADY STATE DYNAMICS\n%.6g, %.6g, %d\n", s->fLo, s->fHi, s->nPts > 1 ? s->nPts : 20);
            str_catf(&step, "*MODAL DAMPING\n1, %d, %.6g\n", nm, s->damping);
            cloads(&w, &step, NULL); body_loads(s, &step, NULL);
            str_cat(&step, "*NODE FILE\nU\n*EL FILE\nS\n*END STEP\n");
        }
        break;
    }
    default: break;
    }
    free(F); free(flag); free(held); str_free(&bnd);
    Str all; str_init(&all);
    str_cat(&all, model.s ? model.s : ""); str_cat(&all, step.s ? step.s : "");
    str_free(&model); str_free(&step);
    pl_join(p, sizeof p, proj, "study.inp");
    ok = pl_write_file(p, all.s, all.len);
    str_free(&all);
    if (!ok) snprintf(err, (size_t)cap, "Could not write %s", p);
    return ok;
}

/* ---------------------------------------------------------------------------------------------------- plan B4: checks */
static int edcmp(const void *x, const void *y)
{
    const int *p = x, *q = y;
    return p[0] != q[0] ? (p[0] > q[0]) - (p[0] < q[0]) : (p[1] > q[1]) - (p[1] < q[1]);
}

static void add_row(double G[6][6], const double c[3], const double r[3])
{
    double a[6] = { c[0], c[1], c[2], r[1] * c[2] - r[2] * c[1], r[2] * c[0] - r[0] * c[2], r[0] * c[1] - r[1] * c[0] };
    for (int i = 0; i < 6; i++) for (int j = 0; j < 6; j++) G[i][j] += a[i] * a[j];
}

/* the eigenvalues and vectors of a symmetric 6x6 (Jacobi) */
static void eig6(double A[6][6], double val[6], double vec[6][6])
{
    double V[6][6] = { { 0 } };
    for (int i = 0; i < 6; i++) V[i][i] = 1;
    for (int sweep = 0; sweep < 100; sweep++) {
        double off = 0, dia = 0;
        for (int i = 0; i < 6; i++) for (int j = 0; j < 6; j++) { if (i != j) off += fabs(A[i][j]); else dia += fabs(A[i][i]); }
        if (off < 1e-15 * (dia + 1e-300)) break;
        for (int p = 0; p < 5; p++) for (int q = p + 1; q < 6; q++) {
            if (fabs(A[p][q]) < 1e-300) continue;
            double th = (A[q][q] - A[p][p]) / (2 * A[p][q]);
            double t = (th >= 0 ? 1 : -1) / (fabs(th) + sqrt(th * th + 1)), c = 1 / sqrt(t * t + 1), sn = t * c;
            for (int k = 0; k < 6; k++) { double akp = A[k][p], akq = A[k][q]; A[k][p] = c * akp - sn * akq; A[k][q] = sn * akp + c * akq; }
            for (int k = 0; k < 6; k++) { double apk = A[p][k], aqk = A[q][k]; A[p][k] = c * apk - sn * aqk; A[q][k] = sn * apk + c * aqk; }
            for (int k = 0; k < 6; k++) { double vkp = V[k][p], vkq = V[k][q]; V[k][p] = c * vkp - sn * vkq; V[k][q] = sn * vkp + c * vkq; }
        }
    }
    for (int i = 0; i < 6; i++) { val[i] = A[i][i]; for (int k = 0; k < 6; k++) vec[i][k] = V[k][i]; }
}

void fe_free_motions(const FeStudy *s, const FeMesh *m, FeFree *o)
{
    memset(o, 0, sizeof *o);
    if (!m->n) return;
    /* about the part's centre, in units of its size, so translations and rotations weigh alike */
    double c0[3], L = 0;
    for (int k = 0; k < 3; k++) { c0[k] = (m->lo[k] + m->hi[k]) / 2; L = fmax(L, m->hi[k] - m->lo[k]); }
    if (L <= 0) L = 1;
    double G[6][6] = { { 0 } };
    char *flag = malloc((size_t)m->n);
    if (!flag) return;
    int rows = 0;
    for (int i = 0; i < s->nFix; i++) {
        const FeFixture *fx = &s->fix[i];
        if (nodes_of(m, (char (*)[64])fx->face, fx->nf, flag, NULL) <= 0) continue;
        double nrm[3] = { 0, 0, 0 }, ax[3] = { 0, 0, 1 }, cc[3] = { 0, 0, 0 }, R = 0;
        bool pin = false;
        if (fx->kind == FX_ROLLER) roller_axis(m, fx, nrm);
        if (fx->kind == FX_PIN) { int g = fe_group(m, fx->face[0]); pin = g >= 0 && fe_cylinder(m, g, ax, cc, &R); if (!pin) continue; }
        int cnt = 0, every = 1;
        for (int v = 0; v < m->n; v++) cnt += flag[v];
        if (cnt > 400) every = cnt / 400 + 1;
        int k = 0;
        for (int v = 0; v < m->n; v++) {
            if (!flag[v] || k++ % every) continue;
            double r[3]; for (int q = 0; q < 3; q++) r[q] = (m->x[3 * v + q] - c0[q]) / L;
            static const double E[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            switch (fx->kind) {
            case FX_FIXED: for (int q = 0; q < 3; q++) add_row(G, E[q], r); break;
            case FX_X: case FX_Y: case FX_Z: add_row(G, E[fx->kind - FX_X], r); break;
            case FX_DISP: add_row(G, E[fx->dir >= DIR_PX ? (fx->dir - 1) % 3 : 0], r); break;
            case FX_ROLLER: add_row(G, nrm, r); break;
            case FX_PIN: {
                double d[3], rad[3];
                for (int q = 0; q < 3; q++) d[q] = m->x[3 * v + q] - cc[q];
                double al = dot3(d, ax);
                for (int q = 0; q < 3; q++) rad[q] = d[q] - al * ax[q];
                double rl = sqrt(dot3(rad, rad));
                if (rl > 1e-12) { for (int q = 0; q < 3; q++) rad[q] /= rl; add_row(G, rad, r); }
                add_row(G, ax, r);
                break;
            }
            default: break;
            }
            rows++;
        }
    }
    free(flag);
    if (!rows) {                                            /* nothing held: all six */
        o->n = 6;
        for (int q = 0; q < 6; q++) { o->m[q].rotation = q >= 3; memset(o->m[q].axis, 0, sizeof o->m[q].axis); o->m[q].axis[q % 3] = 1; memcpy(o->m[q].pt, c0, sizeof c0); }
        return;
    }
    double val[6], vec[6][6], mx = 0;
    eig6(G, val, vec);
    for (int i = 0; i < 6; i++) mx = fmax(mx, val[i]);
    for (int i = 0; i < 6 && o->n < 6; i++) {
        if (val[i] > 1e-8 * mx) continue;
        double *t = vec[i], *w = vec[i] + 3, wl = sqrt(dot3(w, w)), tl = sqrt(dot3(t, t));
        int k = o->n++;
        if (wl > 0.3 * tl) {                                /* a rotation: its axis w, through the point p with t = p x w ... */
            o->m[k].rotation = true;
            for (int q = 0; q < 3; q++) o->m[k].axis[q] = w[q] / wl;
            /* the motion at point r is t + w x r: zero on the axis; p = (w x t) / |w|^2 (in the scaled frame) */
            double p[3] = { (w[1] * t[2] - w[2] * t[1]) / (wl * wl), (w[2] * t[0] - w[0] * t[2]) / (wl * wl), (w[0] * t[1] - w[1] * t[0]) / (wl * wl) };
            for (int q = 0; q < 3; q++) o->m[k].pt[q] = c0[q] + p[q] * L;
        } else {
            o->m[k].rotation = false;
            for (int q = 0; q < 3; q++) o->m[k].axis[q] = t[q] / (tl > 0 ? tl : 1);
            memcpy(o->m[k].pt, c0, sizeof c0);
        }
    }
}

static const char *axis_word(const double a[3])
{
    static char b[64];
    for (int q = 0; q < 3; q++) if (fabs(a[q]) > 0.98) { snprintf(b, sizeof b, "%s", q == 0 ? "x" : q == 1 ? "y" : "z"); return b; }
    snprintf(b, sizeof b, "(%.2f, %.2f, %.2f)", a[0], a[1], a[2]);
    return b;
}

static void pf_add(FePreflight *o, int sev, int fix, const char *fmt, ...)
{
    if (o->n >= FE_MAXPF) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(o->it[o->n].text, sizeof o->it[o->n].text, fmt, ap);
    va_end(ap);
    o->it[o->n].sev = sev; o->it[o->n].fix = fix; o->n++;
}

void fe_preflight(const FeStudy *s, const FeMesh *m, double memFreeMB, FePreflight *o)
{
    memset(o, 0, sizeof *o);
    if (!m->n) return;
    double L = 0; for (int k = 0; k < 3; k++) L = fmax(L, m->hi[k] - m->lo[k]);
    int an = s->analysis;
    bool mech = fe_an_mechanical(an);
    /* the unit: a part under 2 mm or over 5 m across is most likely in the wrong one */
    if (L < 2) pf_add(o, 1, s->scale == 1 ? PF_UNIT_M : PF_UNIT_MM, "The part is %.3g mm across: is its file in metres? (Then it is %.3g m.)", L, L);
    else if (L > 5000) pf_add(o, 1, PF_UNIT_MM, "The part is %.4g m across: is its file in millimetres?", L / 1000);
    /* free motions: what the supports leave the part able to do */
    if (mech && an != AN_FREQUENCY) {
        FeFree fr; fe_free_motions(s, m, &fr);
        for (int i = 0; i < fr.n && i < 2; i++) {
            if (fr.m[i].rotation)
                pf_add(o, 2, PF_ADD_SUPPORT, "The supports leave it free to turn about %s through (%.3g, %.3g, %.3g) mm: add a support away from that axis "
                       "(or hold a flat face), or the solver can't find an answer.", axis_word(fr.m[i].axis), fr.m[i].pt[0], fr.m[i].pt[1], fr.m[i].pt[2]);
            else pf_add(o, 2, PF_ADD_SUPPORT, "The supports leave it free to slide along %s: add a support that holds it that way.", axis_word(fr.m[i].axis));
        }
        if (fr.n > 2) pf_add(o, 2, PF_ADD_SUPPORT, "...and %d more ways it can move freely.", fr.n - 2);
    } else if (an == AN_FREQUENCY) {
        FeFree fr; fe_free_motions(s, m, &fr);
        if (fr.n) pf_add(o, 0, PF_NONE, "Held so it can still move %d way%s: the first %d frequencies will be (about) 0 Hz, its free movements, then the real ones.",
                         fr.n, fr.n == 1 ? "" : "s", fr.n);
    }
    /* point-like supports and loads: tiny faces make stresses there that aren't real (a singularity) */
    double Atot = 0; for (int g = 0; g < m->ng; g++) Atot += m->garea[g];
    for (int i = 0; mech && i < s->nFix + s->nLoad; i++) {
        bool isFix = i < s->nFix;
        const char (*faces)[64] = isFix ? (const char (*)[64])s->fix[i].face : (const char (*)[64])s->load[i - s->nFix].face;
        int nf = isFix ? s->fix[i].nf : s->load[i - s->nFix].nf;
        if (!isFix && (s->load[i - s->nFix].kind == LD_GRAVITY || s->load[i - s->nFix].kind == LD_ROTATION)) continue;
        double A = 0; for (int j = 0; j < nf; j++) { int g = fe_group(m, faces[j]); if (g >= 0) A += m->garea[g]; }
        if (nf && A > 0 && A < 2e-4 * Atot)
            pf_add(o, 1, PF_NONE, "%s %d sits on a very small area (%.3g mm²): close to it the stress is far higher than in the real part "
                   "(a point load or support makes a singularity). Spread it over the face it really acts on, or read stresses a little away from it.",
                   isFix ? "Support" : "Load", isFix ? i + 1 : i - s->nFix + 1, A);
    }
    bool anyFixed = false; for (int i = 0; i < s->nFix; i++) if (s->fix[i].kind == FX_FIXED) anyFixed = true;
    if (mech && anyFixed && (an == AN_STATIC || an == AN_NONLINEAR))
        pf_add(o, 0, PF_NONE, "A fixed face stops the material at its edges completely, which no real bolt or weld does: the stress right at those edges "
               "is higher than reality and grows with a finer mesh. Judge the part by the stress a little away from them.");
    /* sharp inside corners: two boundary faces of different groups sharing an edge, meeting concave at more than 30 degrees
       (no fillet): each face's edges sorted by their corner nodes, so neighbours sit side by side */
    if (mech && m->nf > 0) {
        typedef struct { int a, b, f; } Ed;
        Ed *e = malloc(sizeof(Ed) * 4 * (size_t)m->nf);
        int ne = 0, concave = 0; double at[3] = { 0, 0, 0 };
        if (e) {
            for (int f = 0; f < m->nf; f++) {
                int k = m->fk[f] == 4 || m->fk[f] == 8 ? 4 : 3;
                for (int j = 0; j < k; j++) { int u = m->fn[8 * f + j], v = m->fn[8 * f + (j + 1) % k]; e[ne++] = (Ed){ u < v ? u : v, u < v ? v : u, f }; }
            }
            qsort(e, (size_t)ne, sizeof(Ed), edcmp);
            for (int i = 0; i + 1 < ne; i++) {
                if (e[i].a != e[i + 1].a || e[i].b != e[i + 1].b) continue;
                int f = e[i].f, h = e[i + 1].f;
                if (m->fg[f] == m->fg[h]) continue;
                double n1[3], c1[3], n2[3], c2[3], d[3];
                femesh_face_area(m, f, n1); face_centre(m, f, c1); femesh_face_area(m, h, n2); face_centre(m, h, c2);
                for (int q = 0; q < 3; q++) d[q] = c2[q] - c1[q];
                if (dot3(d, n1) > 1e-9 * L && dot3(n1, n2) < cos(30 * M_PI / 180)) { if (!concave) memcpy(at, c1, sizeof at); concave++; }
            }
            free(e);
        }
        if (concave)
            pf_add(o, 0, PF_NONE, "Sharp inside corners (near (%.3g, %.3g, %.3g) mm): a corner with no fillet concentrates stress without limit in theory, so "
                   "the peak there rises as the mesh gets finer. Real parts have a radius: model it, or judge by the stress beside the corner.", at[0], at[1], at[2]);
    }
    /* memory: the estimate against what is free (an upper bound for a compact part, calibrated on CalculiX) */
    double dof = 3.0 * m->n, mb = 124 * pow(dof / 27678, 1.33);
    if (memFreeMB > 0 && mb > 0.9 * memFreeMB)
        pf_add(o, mb > 2 * memFreeMB ? 2 : 1, PF_FINER, "Solving may need up to %.0f MB and %.0f MB is free: use a coarser mesh, or close other programs.", mb, memFreeMB);
}
