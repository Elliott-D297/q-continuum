/* festudy.c -- see festudy.h */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "festudy.h"
#include "ccxmodel.h"
#include "pl_util.h"

const char *const FE_FX_NAME[FX__N] = { "fixed", "x", "y", "z" };
const char *const FE_LD_NAME[LD__N] = { "force", "pressure" };
const char *const FE_DIR_NAME[DIR__N] = { "normal", "+x", "+y", "+z", "-x", "-y", "-z" };

void fe_study_default(FeStudy *s)
{
    memset(s, 0, sizeof *s);
    s->scale = 1; s->order = 2;
    pl_strlcpy(s->material, CCX_LIB[0].name, sizeof s->material);
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
    enum { S_NONE, S_PART, S_MAT, S_FIX, S_LOAD } sec = S_NONE;
    for (char *l = t; l && *l; ) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        char *line = trim(l);
        l = e ? e + 1 : NULL;
        if (!*line || *line == '#' || *line == ';') continue;
        if (*line == '[') {
            if (!strncasecmp(line, "[part]", 6)) sec = S_PART;
            else if (!strncasecmp(line, "[material]", 10)) sec = S_MAT;
            else if (!strncasecmp(line, "[fixture]", 9) && s->nFix < FE_MAXITEM) { sec = S_FIX; s->nFix++; }
            else if (!strncasecmp(line, "[load]", 6) && s->nLoad < FE_MAXITEM) { sec = S_LOAD; s->nLoad++; s->load[s->nLoad - 1].dir = DIR_NORMAL; }
            else sec = S_NONE;
            continue;
        }
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char *k = trim(line), *v = trim(eq + 1);
        if (sec == S_PART) {
            if (!strcmp(k, "file")) pl_strlcpy(s->part, v, sizeof s->part);
            else if (!strcmp(k, "scale")) s->scale = atof(v) > 0 ? atof(v) : 1;
            else if (!strcmp(k, "size")) s->size = atof(v);
            else if (!strcmp(k, "order")) s->order = atoi(v) == 1 ? 1 : 2;
        } else if (sec == S_MAT) {
            if (!strcmp(k, "name")) pl_strlcpy(s->material, v, sizeof s->material);
        } else if (sec == S_FIX) {
            FeFixture *f = &s->fix[s->nFix - 1];
            if (!strcmp(k, "kind")) { int i = name_index(FE_FX_NAME, FX__N, v); f->kind = i < 0 ? FX_FIXED : i; }
            else if (!strcmp(k, "faces")) f->nf = faces_parse(v, f->face, FE_MAXFACES);
        } else if (sec == S_LOAD) {
            FeLoad *d = &s->load[s->nLoad - 1];
            if (!strcmp(k, "kind")) { int i = name_index(FE_LD_NAME, LD__N, v); d->kind = i < 0 ? LD_FORCE : i; }
            else if (!strcmp(k, "faces")) d->nf = faces_parse(v, d->face, FE_MAXFACES);
            else if (!strcmp(k, "value")) d->value = atof(v);
            else if (!strcmp(k, "dir")) { int i = name_index(FE_DIR_NAME, DIR__N, v); d->dir = i < 0 ? DIR_NORMAL : i; }
        }
    }
    free(t);
    return true;
}

static void faces_cat(Str *o, char f[][64], int n)
{
    for (int i = 0; i < n; i++) str_catf(o, "%s%s", i ? ", " : "", f[i]);
    str_cat(o, "\n");
}
bool fe_study_save(const char *proj, const FeStudy *s)
{
    Str o; str_init(&o);
    str_cat(&o, "# A part study for CalculiX, written by Q-Continuum (Setup edits it; study.inp is written from it at each run).\n"
                "# Lengths in mm, forces in N, pressures in MPa. Faces by name, as the part's mesh names them.\n");
    str_catf(&o, "[part]\nfile = %s\nscale = %g\nsize = %g\norder = %d\n\n[material]\nname = %s\n", s->part, s->scale, s->size, s->order, s->material);
    for (int i = 0; i < s->nFix; i++) { str_catf(&o, "\n[fixture]\nkind = %s\nfaces = ", FE_FX_NAME[s->fix[i].kind]); faces_cat(&o, (char (*)[64])s->fix[i].face, s->fix[i].nf); }
    for (int i = 0; i < s->nLoad; i++) {
        const FeLoad *d = &s->load[i];
        str_catf(&o, "\n[load]\nkind = %s\nvalue = %g\ndir = %s\nfaces = ", FE_LD_NAME[d->kind], d->value, FE_DIR_NAME[d->dir]);
        faces_cat(&o, (char (*)[64])d->face, d->nf);
    }
    char p[PL_PATH]; pl_join(p, sizeof p, proj, "study.ini");
    bool ok = pl_write_file(p, o.s, o.len);
    str_free(&o);
    return ok;
}

int fe_group(const FeMesh *m, const char *name)
{
    for (int g = 0; g < m->ng; g++) if (!strcasecmp(m->gname[g], name)) return g;
    return -1;
}

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

/* the nodal forces of every load (3 per node; NULL when nothing is loaded) */
static double *loads_of(const FeStudy *s, const FeMesh *m, int *unknown)
{
    double *F = calloc(3 * (size_t)m->n, sizeof(double));
    char *in = malloc((size_t)m->ng + 1);
    for (int i = 0; i < s->nLoad; i++) {
        const FeLoad *d = &s->load[i];
        memset(in, 0, (size_t)m->ng + 1);
        double At = 0;
        for (int j = 0; j < d->nf; j++) { int g = fe_group(m, d->face[j]); if (g < 0) { if (unknown) (*unknown)++; continue; } if (!in[g]) { in[g] = 1; At += m->garea[g]; } }
        if (At <= 0) continue;
        for (int f = 0; f < m->nf; f++) {
            if (!in[m->fg[f]]) continue;
            double n[3], t[3] = { 0, 0, 0 }; femesh_face_area(m, f, n);
            if (d->kind == LD_PRESSURE) for (int q = 0; q < 3; q++) t[q] = -d->value * n[q];
            else if (d->dir == DIR_NORMAL) for (int q = 0; q < 3; q++) t[q] = -d->value / At * n[q];
            else { int ax = (d->dir - 1) % 3; t[ax] = (d->dir <= DIR_PZ ? 1 : -1) * d->value / At; }
            face_load(m, f, t, F);
        }
    }
    free(in);
    return F;
}

/* the nodes of fixture i (a flag per node) */
static int fixed_nodes(const FeStudy *s, const FeMesh *m, int i, char *flag, int *unknown)
{
    memset(flag, 0, (size_t)m->n);
    char *in = calloc((size_t)m->ng + 1, 1);
    for (int j = 0; j < s->fix[i].nf; j++) { int g = fe_group(m, s->fix[i].face[j]); if (g < 0) { if (unknown) (*unknown)++; } else in[g] = 1; }
    int n = 0;
    for (int f = 0; f < m->nf; f++) if (in[m->fg[f]]) for (int j = 0; j < m->fk[f]; j++) { int v = m->fn[8 * f + j]; if (!flag[v]) { flag[v] = 1; n++; } }
    free(in);
    return n;
}

void fe_study_summary(const FeStudy *s, const FeMesh *m, FeSummary *o)
{
    memset(o, 0, sizeof *o);
    char *flag = malloc((size_t)m->n), *any = calloc((size_t)m->n, 1);
    bool held[3] = { false, false, false };
    for (int i = 0; i < s->nFix; i++) {
        if (fixed_nodes(s, m, i, flag, &o->unknownFaces) > 0) {
            for (int k = 0; k < 3; k++) if (s->fix[i].kind == FX_FIXED || s->fix[i].kind == FX_X + k) held[k] = true;
        }
        for (int v = 0; v < m->n; v++) any[v] |= flag[v];
    }
    for (int v = 0; v < m->n; v++) o->fixedNodes += any[v];
    double *F = loads_of(s, m, &o->unknownFaces);
    for (int v = 0; v < m->n; v++) { bool l = false; for (int q = 0; q < 3; q++) { o->force[q] += F[3 * v + q]; if (F[3 * v + q] != 0) l = true; } o->loadedNodes += l; }
    free(F); free(flag); free(any);
    if (!s->nFix || !o->fixedNodes) snprintf(o->problem, sizeof o->problem, "Nothing holds the part: add a fixture (pick the faces it is bolted or welded at)");
    else if (!held[0] || !held[1] || !held[2]) snprintf(o->problem, sizeof o->problem, "The part is free to move along %s: add a fixture that holds it that way",
                                                       !held[0] ? "x" : !held[1] ? "y" : "z");
    else if (!s->nLoad || !o->loadedNodes) snprintf(o->problem, sizeof o->problem, "No load: add a force or a pressure on the faces it acts on");
    else if (o->unknownFaces) snprintf(o->problem, sizeof o->problem, "%d face name%s the mesh doesn't have (meshed again with other faces?): pick them again",
                                      o->unknownFaces, o->unknownFaces == 1 ? "" : "s");
}

bool fe_study_write(const FeStudy *s, const FeMesh *m, const char *proj, char *err, int cap)
{
    err[0] = 0;
    int lib = -1; for (int i = 0; i < CCX_LIB_N; i++) if (!strcasecmp(CCX_LIB[i].name, s->material)) lib = i;
    if (lib < 0) lib = 0;
    const CcxLibMat *mt = &CCX_LIB[lib];
    /* the mesh: nodes, elements by type, each fixture's nodes */
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
    for (int i = 0; i < s->nFix; i++) {
        if (!fixed_nodes(s, m, i, flag, NULL)) continue;
        str_catf(&o, "*NSET, NSET=FIX%d\n", i + 1);
        int col = 0;
        for (int v = 0; v < m->n; v++) if (flag[v]) { str_catf(&o, "%d,%s", m->id[v], ++col % 12 ? " " : "\n"); }
        if (col % 12) str_cat(&o, "\n");
    }
    char p[PL_PATH]; pl_join(p, sizeof p, proj, "study_mesh.inp");
    bool ok = pl_write_file(p, o.s, o.len);
    str_free(&o);
    if (!ok) { snprintf(err, (size_t)cap, "Could not write %s", p); free(flag); return false; }
    /* the study */
    str_init(&o);
    str_catf(&o, "** %s: a static study of %s, written by Q-Continuum from study.ini (Setup). Edits here are overwritten at the next run\n"
                 "** from Setup; to keep a hand-edited deck, save it under another name.\n"
                 "** Units: mm, N, MPa, tonnes/mm³. Loads are consistent nodal forces on the faces picked.\n", "study.inp", s->part);
    str_cat(&o, "*INCLUDE, INPUT=study_mesh.inp\n");
    str_catf(&o, "*MATERIAL, NAME=%s\n*ELASTIC\n%.6g, %.4g\n*DENSITY\n%.6g\n", mt->name, mt->E / 1e6, mt->nu, mt->rho * 1e-12);
    if (mt->alpha > 0) str_catf(&o, "*EXPANSION\n%.6g\n", mt->alpha);
    str_cat(&o, "*SOLID SECTION, ELSET=EALL, MATERIAL=");
    str_catf(&o, "%s\n*STEP\n*STATIC\n*BOUNDARY\n", mt->name);
    for (int i = 0; i < s->nFix; i++) {
        if (!fixed_nodes(s, m, i, flag, NULL)) continue;
        int k = s->fix[i].kind;
        if (k == FX_FIXED) str_catf(&o, "FIX%d, 1, 3\n", i + 1);
        else str_catf(&o, "FIX%d, %d, %d\n", i + 1, k - FX_X + 1, k - FX_X + 1);
    }
    double *F = loads_of(s, m, NULL);
    bool head = false;
    for (int v = 0; v < m->n; v++) for (int q = 0; q < 3; q++) if (F[3 * v + q] != 0) {
        if (!head) { str_cat(&o, "*CLOAD\n"); head = true; }
        str_catf(&o, "%d, %d, %.9g\n", m->id[v], q + 1, F[3 * v + q]);
    }
    free(F); free(flag);
    str_cat(&o, "*NODE FILE\nU, RF\n*EL FILE\nS\n*END STEP\n");
    pl_join(p, sizeof p, proj, "study.inp");
    ok = pl_write_file(p, o.s, o.len);
    str_free(&o);
    if (!ok) snprintf(err, (size_t)cap, "Could not write %s", p);
    return ok;
}
