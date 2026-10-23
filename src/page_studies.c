/* page_studies.c -- Studies: variants of a case with one or two settings changed (a grid study, a parameter sweep, a
 * set of models), queued to run, then compared side by side in a table and a chart, against measurements if you
 * have them. The New study dialog has a Basic mode (this case's usual settings in words, quick picks and ranges) and
 * an Advanced one (any keyword, paired or combined settings). A study is a small file next to its cases: <folder>/<name>.qcstudy */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "fieldinfo.h"

#define MAX_STUDIES 64
#define MAX_VARIANTS 64
#define MAX_OUTPUTS 6

/* key "@refine": the mesh, blockMeshDict's base size divided by the value (2 = twice the cells along each edge) */
typedef struct { char file[256], key[256], tmpl[128], label[96]; char values[MAX_VARIANTS][64]; int nValues; } StudyParam;
typedef struct { char path[PL_PATH]; char v1[64], v2[64]; } Variant;
typedef struct { int kind; char file[256], col[64]; } Output;   /* kind 0 monitor column, 1 run time, 2 latest time, 3 final residual (col = field) */

typedef struct {
    char       file[PL_PATH];            /* the .qcstudy */
    char       name[128];
    char       base[PL_PATH];
    StudyParam p[2]; int nParams;
    Variant    v[MAX_VARIANTS]; int nV;
    Output     out[MAX_OUTPUTS]; int nOut;
    char       exp[PL_PATH];             /* measurements: a CSV with x (the first parameter) and y columns */
} Study;

static Study S[MAX_STUDIES];
static int nS, sel = -1;
static double scannedAt = -100;

/* ============================================================================ file */
static void study_save(const Study *s)
{
    Str b; str_init(&b);
    str_cat(&b, "# Q-Continuum study: variants of one case, compared on the Studies page\n");
    str_catf(&b, "name=%s\nbase=%s\n", s->name, s->base);
    for (int i = 0; i < s->nParams; i++) {
        const StudyParam *p = &s->p[i];
        str_catf(&b, "param=%s|%s|%s|", p->file, p->key, p->tmpl);
        for (int k = 0; k < p->nValues; k++) str_catf(&b, "%s%s", k ? "," : "", p->values[k]);
        str_catf(&b, "|%s\n", p->label);
    }
    for (int i = 0; i < s->nV; i++) str_catf(&b, "case=%s|%s|%s\n", s->v[i].path, s->v[i].v1, s->v[i].v2);
    for (int i = 0; i < s->nOut; i++) str_catf(&b, "output=%d|%s|%s\n", s->out[i].kind, s->out[i].file, s->out[i].col);
    if (s->exp[0]) str_catf(&b, "experiment=%s\n", s->exp);
    pl_write_file(s->file, b.s, b.len);
    str_free(&b);
}

static int split_bar(char *s, char **f, int max)
{
    int n = 0;
    for (int i = 0; i < max; i++) f[i] = NULL;                 /* fields a short line doesn't have */
    for (char *q = s; n < max; ) { f[n++] = q; char *b = strchr(q, '|'); if (!b) break; *b = 0; q = b + 1; }
    return n;
}

static bool study_load(const char *file, Study *s)
{
    char *t = pl_read_file(file, NULL);
    if (!t) return false;
    memset(s, 0, sizeof *s);
    pl_strlcpy(s->file, file, sizeof s->file);
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        char *eq = strchr(l, '=');
        if (!eq || l[0] == '#') continue;
        *eq = 0;
        char *k = l, *v = eq + 1, *f[6];
        if (!strcmp(k, "name")) pl_strlcpy(s->name, v, sizeof s->name);
        else if (!strcmp(k, "base")) pl_strlcpy(s->base, v, sizeof s->base);
        else if (!strcmp(k, "experiment")) pl_strlcpy(s->exp, v, sizeof s->exp);
        else if (!strcmp(k, "param") && s->nParams < 2 && split_bar(v, f, 5) >= 4) {
            StudyParam *p = &s->p[s->nParams++];
            pl_strlcpy(p->file, f[0], sizeof p->file); pl_strlcpy(p->key, f[1], sizeof p->key); pl_strlcpy(p->tmpl, f[2], sizeof p->tmpl);
            pl_strlcpy(p->label, f[4] ? f[4] : "", sizeof p->label);              /* older studies have none */
            char *sv = NULL;
            for (char *x = strtok_r(f[3], ",", &sv); x && p->nValues < MAX_VARIANTS; x = strtok_r(NULL, ",", &sv)) pl_strlcpy(p->values[p->nValues++], x, 64);
        } else if (!strcmp(k, "case") && s->nV < MAX_VARIANTS && split_bar(v, f, 3) >= 2) {
            Variant *x = &s->v[s->nV++];
            pl_strlcpy(x->path, f[0], sizeof x->path); pl_strlcpy(x->v1, f[1], sizeof x->v1);
            pl_strlcpy(x->v2, f[2] ? f[2] : "", sizeof x->v2);
        } else if (!strcmp(k, "output") && s->nOut < MAX_OUTPUTS && split_bar(v, f, 3) == 3) {
            Output *o = &s->out[s->nOut++];
            o->kind = atoi(f[0]); pl_strlcpy(o->file, f[1], sizeof o->file); pl_strlcpy(o->col, f[2], sizeof o->col);
        }
    }
    free(t);
    if (!s->name[0]) pl_strlcpy(s->name, pl_basename(file), sizeof s->name);
    return true;
}

static void scan_dir(const char *dir, int depth)
{
    NameList l = { 0 };
    pl_list_dir(dir, &l, false, false);
    for (int i = 0; i < l.n && nS < MAX_STUDIES; i++) {
        char p[PL_PATH]; pl_join(p, sizeof p, dir, l.names[i]);
        if (pl_ends_with(l.names[i], ".qcstudy") && pl_is_file(p)) { if (study_load(p, &S[nS])) nS++; }
        else if (depth > 0 && pl_is_dir(p) && l.names[i][0] != '.' && !case_is_case(p)) scan_dir(p, depth - 1);
    }
    names_free(&l);
}

/* studies in the workspace (one folder deep) and next to the cases opened from elsewhere */
static void scan(bool force)
{
    if (!force && GetTime() - scannedAt < 5) return;
    scannedAt = GetTime();
    char keep[PL_PATH] = ""; if (sel >= 0 && sel < nS) pl_strlcpy(keep, S[sel].file, sizeof keep);
    nS = 0;
    char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
    scan_dir(ws, 1);
    for (int i = 0; i < A.set.recent.n; i++) {
        char par[PL_PATH]; pl_dirname(par, sizeof par, A.set.recent.names[i]);
        bool seen = !strncmp(par, ws, strlen(ws));
        if (!seen) scan_dir(par, 0);
    }
    for (int i = 0; i < nS; i++) for (int k = i + 1; k < nS; k++) if (!strcmp(S[i].file, S[k].file)) { S[k] = S[--nS]; k--; }
    sel = -1;
    for (int i = 0; i < nS; i++) if (!strcmp(S[i].file, keep)) sel = i;
    if (sel < 0 && nS) sel = 0;
}

/* ============================================================================ making the variants */
static void safe_name(const char *in, char *out, int cap)
{
    int k = 0;
    for (const char *p = in; *p && k < cap - 1; p++) out[k++] = (isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '+') ? *p : '_';
    out[k] = 0;
}

static const char *short_key(const char *key)
{
    if (!strcmp(key, "@refine")) return "mesh";
    const char *s = strrchr(key, '/');
    return s ? s + 1 : key;
}
/* what the table and chart call a setting: its label, else the keyword */
static const char *param_name(const StudyParam *p) { return p->label[0] ? p->label : short_key(p->key); }

/* the blockMeshDict scale (convertToMeters or scale) */
static double blocks_scale(FDFile *bm)
{
    char sv[64] = "1";
    if (!fd_get(bm, fd_find(bm, "scale") ? "scale" : "convertToMeters", sv, sizeof sv)) return 1;
    double v = atof(sv);
    return v == 0 ? 1 : v;
}

/* the setup and mesh of base, without results, logs, processor folders or our own history */
static bool copy_setup(const char *base, const char *dst, double startTime, char *err, int cap)
{
    Str ex; str_init(&ex);
    str_cat(&ex, "--exclude=./processor* --exclude=./log.* --exclude=./postProcessing --exclude=./.qc-* --exclude=./movie "
                 "--exclude=./*.foam --exclude=./.q-continuum.ini ");
    NameList l = { 0 };
    pl_list_dir(base, &l, true, false);
    for (int i = 0; i < l.n; i++) if (pl_is_number(l.names[i]) && atof(l.names[i]) > startTime + 1e-12) str_catf(&ex, "--exclude=./%s ", l.names[i]);
    names_free(&l);
    char qb[PL_PATH + 8], qd[PL_PATH + 8];
    pl_shell_quote(qb, sizeof qb, base); pl_shell_quote(qd, sizeof qd, dst);
    Str cmd; str_init(&cmd);
    str_catf(&cmd, "mkdir -p %s && cd %s && tar %s -cf - . | (cd %s && tar xf -) 2>&1", qd, qb, ex.s, qd);
    char out[400] = "";
    int rc = pl_run_wait(cmd.s, out, sizeof out);
    str_free(&ex); str_free(&cmd);
    if (rc != 0) { snprintf(err, (size_t)cap, "Copying the case failed: %s", out); return false; }
    return true;
}

static bool set_value(const char *casePath, const StudyParam *p, const char *value, char *err, int cap)
{
    char path[PL_PATH]; pl_join(path, sizeof path, casePath, p->file);
    if (!strcmp(p->key, "@refine")) {                          /* the mesh: base size / factor, and the copied mesh goes */
        FDFile *bm = fd_load(path);
        if (!bm) { snprintf(err, (size_t)cap, "%s is missing in the copy", p->file); return false; }
        double f = atof(value), cur = 0, sc = blocks_scale(bm); long cells = 0; char why[160] = "";
        bool ok = f > 0 && blocks_base_size(bm, sc, 0, false, &cur, &cells, why, sizeof why) && cur > 0 &&
                  blocks_base_size(bm, sc, cur / f, true, NULL, &cells, why, sizeof why) && fd_save(bm);
        fd_free(bm);
        if (!ok) { pl_fmt_ui(err, (size_t)cap, "Could not refine the mesh by %s: %s", value, why[0] ? why : "not a number above 0"); return false; }
        char pm[PL_PATH]; pl_join(pm, sizeof pm, casePath, "constant/polyMesh");
        if (pl_is_dir(pm)) pl_remove_tree(pm);                  /* the copy's old mesh: Run meshes it again */
        return true;
    }
    FDFile *f = fd_load(path);
    if (!f) { snprintf(err, (size_t)cap, "%s is missing in the copy", p->file); return false; }
    char v[400];
    const char *at = strstr(p->tmpl, "{}");
    if (at) snprintf(v, sizeof v, "%.*s%s%s", (int)(at - p->tmpl), p->tmpl, value, at + 2);
    else snprintf(v, sizeof v, "%s", value);
    bool ok = fd_put(f, p->key, v) && fd_save(f);
    fd_free(f);
    if (!ok) pl_fmt_ui(err, (size_t)cap, "Could not set %s in %s", p->key, p->file);
    return ok;
}

/* ============================================================================ the New study dialog */
/* Basic: the settings people vary, found in this case and named in words (time step, viscosity, the velocity on a
   patch, the turbulence model, the mesh), values from quick picks or a range. Advanced: any file and keyword, with a
   keyword list, the same value tools, and two settings either in every combination or paired up. */
#define MAX_KNOBS 24
enum { KN_NUMBER = 0, KN_CHOICE, KN_REFINE };
typedef struct {
    char label[96], unit[24], file[160], key[256], tmpl[128], cur[64], note[160], defaults[160];
    int  kind;
    const char *const *choices; int nChoices; bool choiceOk[16]; char choiceWhy[16][48];
} Knob;

static struct {
    int  mode;                                   /* 0 Basic, 1 Advanced */
    char name[128]; bool autoName;
    int  knob[2];                                /* Basic: the setting in each slot, -1 none */
    char file[2][256], key[2][256], tmpl[2][128], label[2][96], values[2][400];
    char from[2][32], to[2][32], count[2][8]; int spacing[2];   /* the range builder: 0 even steps, 1 log */
    bool pick[2][16];                            /* Basic: the ticked choices */
    bool two, queue; int combine;                /* 0 every combination, 1 pair them up */
    char err[300];
    char forCase[PL_PATH];
    Knob k[MAX_KNOBS]; int nk;
    char keysFor[2][256]; char keys[2][160][160]; int nKeys[2];   /* Advanced: the keywords of the chosen file */
    UiScroll scroll;
} N;

static int dict_files(const Case *c, char out[][160], int max)
{
    const char *dirs[] = { "system", "constant", c->s.hasZeroOrig ? "0.orig" : "0", NULL };
    int n = 0;
    for (int d = 0; dirs[d]; d++) {
        char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, dirs[d]);
        NameList l = { 0 };
        pl_list_dir(p, &l, false, true);
        names_sort(&l);
        for (int i = 0; i < l.n && n < max; i++) {
            if (l.names[i][0] == '.' || pl_ends_with(l.names[i], "~") || pl_ends_with(l.names[i], ".gz")) continue;
            snprintf(out[n++], 160, "%s/%s", dirs[d], l.names[i]);
        }
        names_free(&l);
    }
    return n;
}

static void current_value(const Case *c, const char *file, const char *key, char *out, int cap)
{
    out[0] = 0;
    if (!file[0] || !key[0]) return;
    if (!strcmp(key, "@refine")) { snprintf(out, (size_t)cap, "1"); return; }
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, file);
    FDFile *f = fd_load(p);
    if (!f) return;
    if (!fd_get(f, key, out, cap)) snprintf(out, (size_t)cap, "(not set in this file)");
    fd_free(f);
}

/* "nu [0 2 -1 0 0 0 0] 1e-05" -> template "nu [0 2 -1 0 0 0 0] {}" and number "1e-05": the last plain number */
static bool num_template(const char *cur, char *tmpl, int tcap, char *num, int ncap)
{
    int a, b;
    if (!fd_number_span(cur, &a, &b)) return false;
    snprintf(tmpl, (size_t)tcap, "%.*s{}%s", a, cur, cur + b);
    snprintf(num, (size_t)ncap, "%.*s", b - a, cur + a);
    return true;
}

static Knob *knob_add(const char *label, const char *unit, const char *file, const char *key, int kind)
{
    if (N.nk >= MAX_KNOBS) return NULL;
    Knob *k = &N.k[N.nk++];
    memset(k, 0, sizeof *k);
    pl_strlcpy(k->label, label, sizeof k->label); pl_strlcpy(k->unit, unit, sizeof k->unit);
    pl_strlcpy(k->file, file, sizeof k->file); pl_strlcpy(k->key, key, sizeof k->key);
    k->kind = kind;
    return k;
}

static Knob *knob_num(const Case *c, const char *label, const char *unit, const char *file, const char *key)
{
    char cur[200]; current_value(c, file, key, cur, sizeof cur);
    if (!cur[0] || pl_starts_with(cur, "(not")) return NULL;
    char t[128], n[64];
    if (!num_template(cur, t, sizeof t, n, sizeof n)) return NULL;
    Knob *k = knob_add(label, unit, file, key, KN_NUMBER);
    if (k) { pl_strlcpy(k->tmpl, t, sizeof k->tmpl); pl_strlcpy(k->cur, n, sizeof k->cur); }
    return k;
}

/* fixed values on patches: "Velocity on inlet" (the largest component), "Temperature on hotWall" */
static void knob_patches(const Case *c, const char *field, const char *what, const char *unit, int max)
{
    char rel[160]; snprintf(rel, sizeof rel, "%s/%s", c->s.hasZeroOrig ? "0.orig" : "0", field);
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, rel);
    FDFile *f = fd_load(p);
    if (!f) return;
    FDEntry *bf = fd_find(f, "boundaryField");
    int n = 0;
    for (int i = 0; bf && i < bf->nChild && n < max; i++) {
        FDEntry *e = &bf->child[i];
        if (e->kind != FD_DICT || !e->key || strpbrk(e->key, "/\"(|*")) continue;     /* plain patch names only */
        char key[256], type[64] = "", val[200] = "";
        snprintf(key, sizeof key, "boundaryField/%s/type", e->key);
        if (!fd_get(f, key, type, sizeof type) || strcmp(type, "fixedValue")) continue;
        snprintf(key, sizeof key, "boundaryField/%s/value", e->key);
        if (!fd_get(f, key, val, sizeof val) || strncmp(val, "uniform", 7)) continue;
        char label[96]; snprintf(label, sizeof label, "%s on %s", what, e->key);
        double v[3];
        const char *par = strchr(val, '(');
        if (par && sscanf(par, "( %lf %lf %lf )", &v[0], &v[1], &v[2]) == 3) {
            int big = 0;
            for (int k = 1; k < 3; k++) if (fabs(v[k]) > fabs(v[big])) big = k;
            if (v[big] == 0) continue;                               /* a wall at rest: nothing to vary */
            Knob *k = knob_add(label, unit, rel, key, KN_NUMBER);
            if (!k) break;
            char comp[3][32];
            for (int j = 0; j < 3; j++) snprintf(comp[j], sizeof comp[j], "%g", v[j]);
            snprintf(comp[big], sizeof comp[big], "{}");
            snprintf(k->tmpl, sizeof k->tmpl, "uniform (%s %s %s)", comp[0], comp[1], comp[2]);
            snprintf(k->cur, sizeof k->cur, "%g", v[big]);
            snprintf(k->note, sizeof k->note, "its %c component; the others stay %s", "xyz"[big], big == 0 ? "as they are" : "as they are");
            n++;
        } else if (!par) {
            char t[128], num[64];
            if (!num_template(val, t, sizeof t, num, sizeof num)) continue;
            Knob *k = knob_add(label, unit, rel, key, KN_NUMBER);
            if (!k) break;
            pl_strlcpy(k->tmpl, t, sizeof k->tmpl); pl_strlcpy(k->cur, num, sizeof k->cur);
            n++;
        }
    }
    fd_free(f);
}

static const char *const RAS_MODELS[] = { "kOmegaSST", "kEpsilon", "realizableKE", "RNGkEpsilon", "kOmega", "SpalartAllmaras", "LaunderSharmaKE" };
static const char *const RAS_NEEDS[]  = { "k omega",   "k epsilon", "k epsilon",   "k epsilon",   "k omega", "nuTilda",         "k epsilon" };

static void build_knobs(const Case *c)
{
    N.nk = 0;
    knob_num(c, "Time step", "s", "system/controlDict", "deltaT");
    knob_num(c, "End time", "s", "system/controlDict", "endTime");
    knob_num(c, "Largest Courant number", "", "system/controlDict", "maxCo");
    const char *tp = NULL;
    char p[PL_PATH];
    static const char *const TP[] = { "constant/transportProperties", "constant/physicalProperties", NULL };
    for (int i = 0; TP[i] && !tp; i++) { pl_join(p, sizeof p, c->s.path, TP[i]); if (pl_exists(p)) tp = TP[i]; }
    if (tp) knob_num(c, "Viscosity ν", "m²/s", tp, "nu");
    knob_patches(c, "U", "Velocity", "m/s", 3);
    knob_patches(c, "T", "Temperature", "K", 3);
    /* the turbulence model, among those whose fields the case has */
    static const char *const TU[] = { "constant/turbulenceProperties", "constant/momentumTransport", NULL };
    for (int i = 0; TU[i]; i++) {
        char st[64]; current_value(c, TU[i], "simulationType", st, sizeof st);
        if (strcmp(st, "RAS")) continue;
        char cur[64]; const char *key = "RAS/model";
        current_value(c, TU[i], key, cur, sizeof cur);
        if (!cur[0] || pl_starts_with(cur, "(not")) { key = "RAS/RASModel"; current_value(c, TU[i], key, cur, sizeof cur); }
        if (!cur[0] || pl_starts_with(cur, "(not")) continue;
        Knob *k = knob_add("Turbulence model", "", TU[i], key, KN_CHOICE);
        if (!k) break;
        pl_strlcpy(k->tmpl, "{}", sizeof k->tmpl); pl_strlcpy(k->cur, cur, sizeof k->cur);
        k->choices = RAS_MODELS; k->nChoices = (int)(sizeof RAS_MODELS / sizeof RAS_MODELS[0]);
        for (int m = 0; m < k->nChoices; m++) {
            k->choiceOk[m] = true;
            char needs[64]; pl_strlcpy(needs, RAS_NEEDS[m], sizeof needs);
            char *sv = NULL;
            for (char *fld = strtok_r(needs, " ", &sv); fld; fld = strtok_r(NULL, " ", &sv)) {
                char fp[PL_PATH], rel[160]; snprintf(rel, sizeof rel, "%s/%s", c->s.hasZeroOrig ? "0.orig" : "0", fld);
                pl_join(fp, sizeof fp, c->s.path, rel);
                if (!pl_exists(fp)) { k->choiceOk[m] = false; pl_fmt_ui(k->choiceWhy[m], sizeof k->choiceWhy[m], "needs %s", rel); break; }
            }
        }
        break;
    }
    Knob *r = knob_num(c, "Relaxation of p", "", "system/fvSolution", "relaxationFactors/fields/p");
    if (r) pl_strlcpy(r->defaults, "0.2, 0.3, 0.5", sizeof r->defaults);
    r = knob_num(c, "Relaxation of U", "", "system/fvSolution", "relaxationFactors/equations/U");
    if (r) pl_strlcpy(r->defaults, "0.5, 0.7, 0.9", sizeof r->defaults);
    /* the mesh, through the base size */
    FDFile *bm = case_dict((Case *)c, "system/blockMeshDict");
    double cur = 0; long cells = 0; char why[160] = "";
    if (bm && blocks_base_size(bm, blocks_scale(bm), 0, false, &cur, &cells, why, sizeof why) && cur > 0) {
        Knob *k = knob_add("Mesh refinement", "×", "system/blockMeshDict", "@refine", KN_REFINE);
        if (k) {
            pl_strlcpy(k->tmpl, "{}", sizeof k->tmpl); pl_strlcpy(k->cur, "1", sizeof k->cur);
            pl_strlcpy(k->defaults, "0.5, 1, 2", sizeof k->defaults);
            snprintf(k->note, sizeof k->note, "%ld cells now; 2 = twice the cells along each edge, 0.5 = half", cells);
        }
    }
    knob_num(c, "Write interval", "", "system/controlDict", "writeInterval");
}

static int count_values(const char *s)
{
    char t[400]; pl_strlcpy(t, s, sizeof t);
    int n = 0; char *sv = NULL;
    for (char *x = strtok_r(t, ",;", &sv); x; x = strtok_r(NULL, ",;", &sv)) { pl_trim_inplace(x); if (*x) n++; }
    return n;
}

static void fmt_list(char *out, int cap, const double *v, int n)
{
    int k = 0; out[0] = 0;
    for (int i = 0; i < n && k < cap - 24; i++) k += snprintf(out + k, (size_t)(cap - k), "%s%.4g", i ? ", " : "", v[i]);
}

static void scaled_list(char *out, int cap, double cur, const double *f, int n)
{
    double v[16] = { 0 }; for (int i = 0; i < n && i < 16; i++) v[i] = cur * f[i];
    fmt_list(out, cap, v, n < 16 ? n : 16);
}

/* put Basic's setting k into slot i: its file, keyword and template, and a first set of values */
static void use_knob(const Case *c, int i, int ki)
{
    N.knob[i] = ki;
    if (ki < 0 || ki >= N.nk) return;
    const Knob *k = &N.k[ki];
    pl_strlcpy(N.file[i], k->file, sizeof N.file[i]); pl_strlcpy(N.key[i], k->key, sizeof N.key[i]);
    pl_strlcpy(N.tmpl[i], k->tmpl, sizeof N.tmpl[i]); pl_strlcpy(N.label[i], k->label, sizeof N.label[i]);
    double cur = atof(k->cur);
    if (k->kind == KN_CHOICE) {
        memset(N.pick[i], 0, sizeof N.pick[i]);
        int other = -1;
        for (int m = 0; m < k->nChoices; m++) {
            if (!strcmp(k->choices[m], k->cur)) N.pick[i][m] = true;
            else if (other < 0 && k->choiceOk[m]) other = m;
        }
        if (other >= 0) N.pick[i][other] = true;
    } else if (k->defaults[0]) pl_strlcpy(N.values[i], k->defaults, sizeof N.values[i]);
    else { static const double F[3] = { 0.5, 1, 2 }; scaled_list(N.values[i], sizeof N.values[i], cur, F, 3); }
    snprintf(N.from[i], sizeof N.from[i], "%.4g", k->kind == KN_REFINE ? 0.5 : cur * 0.5);
    snprintf(N.to[i], sizeof N.to[i], "%.4g", k->kind == KN_REFINE ? 2 : cur * 2);
    snprintf(N.count[i], sizeof N.count[i], "3");
    N.spacing[i] = 1;
    if (i == 0 && N.autoName) pl_fmt_ui(N.name, sizeof N.name, "%s %s study", c->s.name, k->label);
}

/* the keywords of a dictionary, as a/b/c paths of its values, for Advanced */
static void collect_keys(const FDEntry *e, const char *prefix, int depth, char (*out)[160], int *n, int max)
{
    for (int i = 0; i < e->nChild && *n < max; i++) {
        const FDEntry *c = &e->child[i];
        if (!c->key || c->kind == FD_DIRECTIVE || !strcmp(c->key, "FoamFile")) continue;
        char path[160];
        if (prefix[0]) snprintf(path, sizeof path, "%s/%s", prefix, c->key); else snprintf(path, sizeof path, "%s", c->key);
        if (c->kind == FD_VALUE) pl_strlcpy(out[(*n)++], path, 160);
        else if (depth < 4) collect_keys(c, path, depth + 1, out, n, max);
    }
}

static void keys_for(const Case *c, int i)
{
    if (!strcmp(N.keysFor[i], N.file[i])) return;
    pl_strlcpy(N.keysFor[i], N.file[i], sizeof N.keysFor[i]);
    N.nKeys[i] = 0;
    if (!N.file[i][0]) return;
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, N.file[i]);
    FDFile *f = fd_load(p);
    if (!f) return;
    collect_keys(&f->root, "", 0, N.keys[i], &N.nKeys[i], 160);
    fd_free(f);
}

/* values for slot i: a list you can type, quick picks around the present value, and a range */
static float values_editor(const Case *c, int i, const Knob *k, float x, float y, float w)
{
    if (k && k->kind == KN_CHOICE) {
        ui_text(F_REG, 12.5f, "Run one case with each ticked model", x, y, TH.text3);
        y += 22;
        Str v; str_init(&v);
        for (int m = 0; m < k->nChoices; m++) {
            // NOLINTNEXTLINE(bugprone-integer-division): two columns, whole rows
            float cx = x + (m % 2) * (w / 2), cy = y + (m / 2) * 30;
            ui_disable(!k->choiceOk[m]);
            char lab[96]; snprintf(lab, sizeof lab, "%s%s%s%s", k->choices[m], !strcmp(k->choices[m], k->cur) ? "  (now)" : "",
                                   k->choiceOk[m] ? "" : "  ", k->choiceOk[m] ? "" : k->choiceWhy[m]);
            ui_checkbox(ui_idn(ui_idn(ui_id("st-choice"), i), m), (Rectangle){ cx, cy, w / 2 - 10, 24 }, &N.pick[i][m], lab);
            ui_disable(false);
            if (N.pick[i][m] && k->choiceOk[m]) { if (v.len) str_cat(&v, ", "); str_cat(&v, k->choices[m]); }
        }
        pl_strlcpy(N.values[i], v.s ? v.s : "", sizeof N.values[i]);
        str_free(&v);
        // NOLINTNEXTLINE(bugprone-integer-division): whole rows
        return y + ((k->nChoices + 1) / 2) * 30 + 8;
    }
    /* the present number, for the quick picks */
    double cur = k ? atof(k->cur) : 0;
    if (!k) {
        char cv[200], t[128], n[64]; current_value(c, N.file[i], N.key[i], cv, sizeof cv);
        if (num_template(cv, t, sizeof t, n, sizeof n)) cur = atof(n);
    }
    bool refine = k && k->kind == KN_REFINE;
    ui_text(F_REG, 12, "Values, separated by commas", x, y, TH.text3);
    char nv[48]; int cnt = count_values(N.values[i]); snprintf(nv, sizeof nv, "%d value%s", cnt, cnt == 1 ? "" : "s");
    ui_text(F_REG, 12, nv, x + w - ui_text_w(F_REG, 12, nv), y, cnt ? TH.text2 : TH.warn);
    y += 18;
    ui_input(ui_idn(ui_id("st-vals"), i), (Rectangle){ x, y, w, 34 }, N.values[i], sizeof N.values[i], "e.g. 1e-5, 2e-5, 5e-5", INPUT_MONO | INPUT_LIVE);
    y += 42;
    /* quick picks */
    static const double Q1[3] = { 0.5, 1, 2 }, Q2[3] = { 0.1, 1, 10 }, Q3[5] = { 0.8, 0.9, 1, 1.1, 1.2 }, R2[3] = { 1, 2, 4 }, R3[4] = { 1, 1.5, 2, 3 };
    struct { const char *label; const double *f; int n; } q[3];
    int nq = 0;
    if (refine) { q[0] = (typeof(q[0])){ "½  1  2", Q1, 3 }; q[1] = (typeof(q[0])){ "1  2  4", R2, 3 }; q[2] = (typeof(q[0])){ "1  1.5  2  3", R3, 4 }; nq = 3; }
    else if (cur != 0) { q[0] = (typeof(q[0])){ "×½  ×1  ×2", Q1, 3 }; q[1] = (typeof(q[0])){ "×0.1  ×1  ×10", Q2, 3 }; q[2] = (typeof(q[0])){ "±10 %, ±20 %", Q3, 5 }; nq = 3; }
    if (nq) {
        ui_text(F_REG, 12, refine ? "Quick" : "Around now", x, y + 8, TH.text3);
        float qx = x + 90;
        for (int j = 0; j < nq; j++) {
            float bw = ui_button_width(NULL, q[j].label) - 6;
            if (ui_button(ui_idn(ui_idn(ui_id("st-quick"), i), j), (Rectangle){ qx, y, bw, 30 }, NULL, q[j].label, BTN_SECONDARY))
                scaled_list(N.values[i], sizeof N.values[i], refine ? 1 : cur, q[j].f, q[j].n);
            qx += bw + 6;
        }
        y += 38;
    }
    /* a range */
    ui_text(F_REG, 12, "Range", x, y + 8, TH.text3);
    float rx = x + 90;
    ui_text(F_REG, 12, "from", rx, y + 8, TH.text3);
    ui_input(ui_idn(ui_id("st-from"), i), (Rectangle){ rx + 34, y, 96, 30 }, N.from[i], sizeof N.from[i], "0", INPUT_MONO | INPUT_LIVE);
    ui_text(F_REG, 12, "to", rx + 140, y + 8, TH.text3);
    ui_input(ui_idn(ui_id("st-to"), i), (Rectangle){ rx + 160, y, 96, 30 }, N.to[i], sizeof N.to[i], "1", INPUT_MONO | INPUT_LIVE);
    ui_input(ui_idn(ui_id("st-count"), i), (Rectangle){ rx + 266, y, 46, 30 }, N.count[i], sizeof N.count[i], "3", INPUT_MONO | INPUT_LIVE | INPUT_NUMERIC);
    ui_text(F_REG, 12, "values", rx + 318, y + 8, TH.text3);
    static const char *sp[] = { "Even", "Log" };
    ui_segmented(ui_idn(ui_id("st-spacing"), i), (Rectangle){ rx + 366, y, 120, 30 }, sp, 2, &N.spacing[i]);
    if (ui_button(ui_idn(ui_id("st-fill"), i), (Rectangle){ rx + 494, y, 64, 30 }, NULL, "Fill", BTN_SECONDARY)) {
        double a = atof(N.from[i]), b = atof(N.to[i]); int n = atoi(N.count[i]);
        if (n < 2 || n > 16) ui_toast(TOAST_WARN, "Use 2 to 16 values");
        else if (N.spacing[i] == 1 && (a <= 0 || b <= 0)) ui_toast(TOAST_WARN, "Log spacing needs both ends above 0");
        else {
            double v[16] = { 0 };
            for (int j = 0; j < n; j++) v[j] = N.spacing[i] == 1 ? a * pow(b / a, (double)j / (n - 1)) : a + (b - a) * j / (n - 1);
            fmt_list(N.values[i], sizeof N.values[i], v, n);
        }
    }
    return y + 42;
}

static int variant_count(void)
{
    int n1 = count_values(N.values[0]), n2 = N.two ? count_values(N.values[1]) : 1;
    return N.two && N.combine == 1 ? (n1 == n2 ? n1 : -1) : n1 * n2;
}

static void make_study(Case *c)
{
    N.err[0] = 0;
    pl_trim_inplace(N.name);
    if (!N.name[0] || strchr(N.name, '/')) { snprintf(N.err, sizeof N.err, "Give the study a name (no '/')."); return; }
    if (N.mode == 0 && N.knob[0] < 0) { snprintf(N.err, sizeof N.err, "Pick what to vary."); return; }
    if (N.mode == 0 && N.two && N.knob[1] < 0) { snprintf(N.err, sizeof N.err, "Pick the second setting, or untick it."); return; }
    Study *s = calloc(1, sizeof *s);
    char par[PL_PATH]; pl_dirname(par, sizeof par, c->s.path);
    char sn[128]; safe_name(N.name, sn, sizeof sn);
    char fn[200]; snprintf(fn, sizeof fn, "%s.qcstudy", sn);
    pl_join(s->file, sizeof s->file, par, fn);
    if (pl_exists(s->file)) { snprintf(N.err, sizeof N.err, "A study called %s already exists next to this case.", sn); free(s); return; }
    pl_strlcpy(s->name, N.name, sizeof s->name);
    pl_strlcpy(s->base, c->s.path, sizeof s->base);
    s->nParams = N.two ? 2 : 1;
    for (int i = 0; i < s->nParams; i++) {
        StudyParam *p = &s->p[i];
        if (!N.file[i][0] || !N.key[i][0]) { snprintf(N.err, sizeof N.err, "Setting %d needs a file and a keyword.", i + 1); free(s); return; }
        pl_strlcpy(p->file, N.file[i], sizeof p->file); pl_strlcpy(p->key, N.key[i], sizeof p->key);
        pl_strlcpy(p->tmpl, N.tmpl[i][0] ? N.tmpl[i] : "{}", sizeof p->tmpl); pl_strlcpy(p->label, N.label[i], sizeof p->label);
        char tmp[400]; pl_strlcpy(tmp, N.values[i], sizeof tmp);
        char *sv;
        for (char *x = strtok_r(tmp, ",;", &sv); x && p->nValues < MAX_VARIANTS; x = strtok_r(NULL, ",;", &sv)) {
            pl_trim_inplace(x);
            if (*x) pl_strlcpy(p->values[p->nValues++], x, 64);
        }
        if (p->nValues == 0) { snprintf(N.err, sizeof N.err, "Setting %d needs values.", i + 1); free(s); return; }
        for (int k = 0; k < p->nValues; k++)                       /* the same value twice makes the same case twice (H-12) */
            for (int j = 0; j < k; j++) {
                char a1[64], a2[64]; safe_name(p->values[j], a1, sizeof a1); safe_name(p->values[k], a2, sizeof a2);
                if (!strcmp(a1, a2)) { pl_fmt_ui(N.err, sizeof N.err, "%s is in the list twice.", p->values[k]); free(s); return; }
            }
        if (strcmp(p->key, "@refine")) {
            /* the keyword must exist in the base case: a typo would otherwise make variants that differ in nothing */
            char cur[200]; current_value(c, p->file, p->key, cur, sizeof cur);
            if (!cur[0] || pl_starts_with(cur, "(not set")) { pl_fmt_ui(N.err, sizeof N.err, "%s has no entry %s.", p->file, p->key); free(s); return; }
            /* each value as it will be written, checked like a typed one (H-02, H-12); values that are whole entries
               already ("uniform (2 0 0)" for a "uniform ({} 0 0)" template) are written as they are */
            char cls[64] = "", bp[PL_PATH]; pl_join(bp, sizeof bp, c->s.path, p->file);
            FDFile *bf = fd_load(bp); if (bf) { fd_get(bf, "FoamFile/class", cls, sizeof cls); fd_free(bf); }
            bool whole = strcmp(p->tmpl, "{}") != 0;
            for (int k = 0; k < p->nValues && whole; k++) {
                char filled[400]; const char *at = strstr(p->tmpl, "{}");
                if (at) snprintf(filled, sizeof filled, "%.*s%s%s", (int)(at - p->tmpl), p->tmpl, p->values[k], at + 2);
                else pl_strlcpy(filled, p->values[k], sizeof filled);
                if (!value_problem(p->key, cur, filled, cls) || value_problem(p->key, cur, p->values[k], cls)) whole = false;
            }
            if (whole) pl_strlcpy(p->tmpl, "{}", sizeof p->tmpl);
            for (int k = 0; k < p->nValues; k++) {
                char filled[400]; const char *at = strstr(p->tmpl, "{}");
                if (at) snprintf(filled, sizeof filled, "%.*s%s%s", (int)(at - p->tmpl), p->tmpl, p->values[k], at + 2);
                else pl_strlcpy(filled, p->values[k], sizeof filled);
                const char *why = value_problem(p->key, cur, filled, cls);
                if (why) { pl_fmt_ui(N.err, sizeof N.err, "%s = %s won't do: %s", short_key(p->key), filled, why); free(s); return; }
            }
        } else for (int k = 0; k < p->nValues; k++) if (atof(p->values[k]) <= 0) {
            pl_fmt_ui(N.err, sizeof N.err, "Mesh refinement %s: use factors above 0 (1 = as now).", p->values[k]); free(s); return;
        }
    }
    bool pair = s->nParams > 1 && N.combine == 1;
    int n1 = s->p[0].nValues, n2 = s->nParams > 1 ? s->p[1].nValues : 1;
    if (pair && n1 != n2) { snprintf(N.err, sizeof N.err, "Pairing needs as many values of each: %d and %d.", n1, n2); free(s); return; }
    if ((pair ? n1 : n1 * n2) > MAX_VARIANTS) { snprintf(N.err, sizeof N.err, "%d variants is too many (at most %d).", pair ? n1 : n1 * n2, MAX_VARIANTS); free(s); return; }
    if (!app_save_all(true)) { free(s); return; }
    extern int run_procs_for_check(void);
    int procs = run_procs_for_check();
    for (int a = 0; a < n1; a++)                                    /* every name free before the first copy (H-12) */
        for (int b = pair ? a : 0; b < (pair ? a + 1 : n2); b++) {
            char v1[64], v2[64] = "", dn[400], dst[PL_PATH];
            safe_name(s->p[0].values[a], v1, sizeof v1);
            if (s->nParams > 1) safe_name(s->p[1].values[b], v2, sizeof v2);
            bool fits = s->nParams > 1 ? pl_fmt(dn, sizeof dn, "%s_%s-%s_%s-%s", c->s.name, short_key(s->p[0].key), v1, short_key(s->p[1].key), v2)
                                       : pl_fmt(dn, sizeof dn, "%s_%s-%s", c->s.name, short_key(s->p[0].key), v1);
            if (!fits) { pl_fmt_ui(N.err, sizeof N.err, "The variant names get too long: shorten the case name or the keyword."); free(s); return; }
            pl_join(dst, sizeof dst, par, dn);
            if (pl_exists(dst)) { pl_fmt_ui(N.err, sizeof N.err, "%s already exists; remove it or choose other values.", dn); free(s); return; }
        }
    for (int a = 0; a < n1; a++)
        for (int b = pair ? a : 0; b < (pair ? a + 1 : n2); b++) {
            char v1[64], v2[64] = "", dn[400];
            safe_name(s->p[0].values[a], v1, sizeof v1);
            if (s->nParams > 1) safe_name(s->p[1].values[b], v2, sizeof v2);
            bool fits = s->nParams > 1 ? pl_fmt(dn, sizeof dn, "%s_%s-%s_%s-%s", c->s.name, short_key(s->p[0].key), v1, short_key(s->p[1].key), v2)
                                       : pl_fmt(dn, sizeof dn, "%s_%s-%s", c->s.name, short_key(s->p[0].key), v1);
            if (!fits) { pl_fmt_ui(N.err, sizeof N.err, "The variant names get too long: shorten the case name or the keyword."); free(s); return; }
            char dst[PL_PATH]; pl_join(dst, sizeof dst, par, dn);
            char err[300];
            if (pl_exists(dst)) { pl_fmt_ui(N.err, sizeof N.err, "%s already exists; remove it or choose other values.", dn); free(s); return; }
            if (!copy_setup(c->s.path, dst, c->s.startTime, err, sizeof err) || !set_value(dst, &s->p[0], s->p[0].values[a], err, sizeof err) ||
                (s->nParams > 1 && !set_value(dst, &s->p[1], s->p[1].values[b], err, sizeof err))) {
                /* nothing half made: the copies this click made go again (they are new, nothing of the user's is in them) */
                pl_remove_tree(dst);
                for (int i = 0; i < s->nV; i++) pl_remove_tree(s->v[i].path);
                pl_strlcpy(N.err, err, sizeof N.err); free(s); return;
            }
            Variant *x = &s->v[s->nV++];
            pl_strlcpy(x->path, dst, sizeof x->path);
            pl_strlcpy(x->v1, s->p[0].values[a], sizeof x->v1);
            if (s->nParams > 1) pl_strlcpy(x->v2, s->p[1].values[b], sizeof x->v2);
            settings_add_recent(&A.set, dst);
        }
    /* compare by: the first monitor column of the base case, if it writes any; and the run time */
    if (A.mons.n && A.mons.f[0].nCols) { Output *o = &s->out[s->nOut++]; o->kind = 0; pl_strlcpy(o->file, A.mons.f[0].key, sizeof o->file); pl_strlcpy(o->col, A.mons.f[0].col[0], sizeof o->col); }
    { Output *o = &s->out[s->nOut++]; o->kind = 1; }
    study_save(s);
    settings_save(&A.set);
    if (N.queue) for (int i = 0; i < s->nV; i++) app_queue_add(s->v[i].path, -1, procs, false, false);
    ui_toast(TOAST_OK, "Made %d variant%s of %s%s", s->nV, s->nV == 1 ? "" : "s", c->s.name, N.queue ? ", queued to run" : "");
    free(s);
    A.modal = MD_NONE;
    app_rescan_cases(true);
    scan(true);
    for (int i = 0; i < nS; i++) if (!strcmp(S[i].name, N.name)) sel = i;
    app_go(PG_STUDIES);
}

/* Basic's picker: the settings this case has, with their present values */
static float knob_picker(const Case *c, int i, float x, float y, float w)
{
    const char *items[MAX_KNOBS] = { 0 }; char texts[MAX_KNOBS][200];
    for (int k = 0; k < N.nk; k++) {
        pl_fmt_ui(texts[k], sizeof texts[k], "%s   (now %s%s%s)", N.k[k].label, N.k[k].kind == KN_REFINE ? "×" : "", N.k[k].cur,
                  N.k[k].unit[0] && N.k[k].kind != KN_REFINE ? TextFormat(" %s", N.k[k].unit) : "");
        items[k] = texts[k];
    }
    char pick[200] = "";
    const char *cur = N.knob[i] >= 0 ? texts[N.knob[i]] : "";
    if (ui_dropdown(ui_idn(ui_id("st-knob"), i), (Rectangle){ x, y, w, 36 }, cur, items, N.nk, 0, pick, sizeof pick, "Pick a setting"))
        for (int k = 0; k < N.nk; k++) if (!strcmp(pick, texts[k])) use_knob(c, i, k);
    y += 44;
    if (N.knob[i] >= 0) {
        const Knob *k = &N.k[N.knob[i]];
        char t[400];
        pl_fmt_ui(t, sizeof t, "%s  %s%s%s", k->file, k->kind == KN_REFINE ? "(base size)" : k->key, k->note[0] ? "  ·  " : "", k->note);
        ui_text_clip(F_REG, 12, t, x, y, w, TH.text3);
        y += 22;
    }
    return y;
}

void study_new_modal(bool *close)
{
    Case *c = A.cs;
    if (!c) { *close = true; return; }
    if (strcmp(N.forCase, c->s.path)) {
        memset(&N, 0, sizeof N);
        pl_strlcpy(N.forCase, c->s.path, sizeof N.forCase);
        pl_fmt_ui(N.name, sizeof N.name, "%s study", c->s.name);
        N.autoName = true;
        N.queue = true;
        N.knob[0] = N.knob[1] = -1;
        pl_strlcpy(N.tmpl[0], "{}", sizeof N.tmpl[0]); pl_strlcpy(N.tmpl[1], "{}", sizeof N.tmpl[1]);
        build_knobs(c);
        if (N.nk) use_knob(c, 0, 0);
    }
    char sub[400]; pl_fmt_ui(sub, sizeof sub, "Copies of %s (setup and mesh, no results) next to it, one per value, queued to run and "
                            "compared on the Studies page.", c->s.name);
    Rectangle b = ui_modal_begin("New study", sub, 800, 760, close);
    float x = b.x, w = b.width;
    static const char *modes[] = { "Basic", "Advanced" };
    int om = N.mode;
    ui_segmented(ui_id("st-mode"), (Rectangle){ x, b.y, 240, 32 }, modes, 2, &N.mode);
    if (om != N.mode && N.mode == 0) for (int i = 0; i < 2; i++) if (N.knob[i] >= 0) use_knob(c, i, N.knob[i]);
    ui_text(F_REG, 12.5f, N.mode == 0 ? "The usual settings of this case, in words." : "Any file and keyword, and how two settings combine.",
            x + 254, b.y + 9, TH.text3);
    Rectangle body = { x, b.y + 44, w, b.height - 44 - 52 };
    float top = ui_scroll_begin(&N.scroll, body), y = top + 4;
    ui_text(F_MED, 13, "Name", x, y + 9, TH.text2);
    char before[128]; pl_strlcpy(before, N.name, sizeof before);
    ui_input(ui_id("st-name"), (Rectangle){ x + 110, y, w - 110, 34 }, N.name, sizeof N.name, NULL, 0);
    if (strcmp(before, N.name)) N.autoName = false;
    y += 48;
    static char files[160][160]; int nf = dict_files(c, files, 160);
    const char *fl[160]; for (int i = 0; i < nf; i++) fl[i] = files[i];
    for (int i = 0; i < (N.two ? 2 : 1); i++) {
        char h[64]; snprintf(h, sizeof h, N.two ? "Setting %d" : "What to vary", i + 1);
        ui_text(F_SEMI, 14, h, x, y, head_color());
        y += 26;
        const Knob *k = NULL;
        if (N.mode == 0) {
            if (!N.nk) { ui_text(F_REG, 13, "This case has none of the usual settings: use Advanced.", x, y, TH.warn); y += 30; continue; }
            y = knob_picker(c, i, x, y, w);
            if (N.knob[i] < 0) continue;
            k = &N.k[N.knob[i]];
        } else {
            float c1 = w * 0.42f;
            ui_text(F_REG, 12, "File", x, y, TH.text3);
            ui_text(F_REG, 12, "Keyword (a/b for nested, e.g. boundaryField/inlet/value)", x + c1 + 10, y, TH.text3);
            y += 18;
            char pick[160] = "";
            if (ui_dropdown(ui_idn(ui_id("st-file"), i), (Rectangle){ x, y, c1, 34 }, N.file[i], fl, nf, DD_MONO | DD_FILTER, pick, sizeof pick, "file"))
                { pl_strlcpy(N.file[i], pick, sizeof N.file[i]); N.label[i][0] = 0; N.knob[i] = -1; }
            keys_for(c, i);
            const char *kl[160] = { 0 }; for (int j = 0; j < N.nKeys[i]; j++) kl[j] = N.keys[i][j];
            float kw = w - c1 - 10 - 120;
            char before2[256]; pl_strlcpy(before2, N.key[i], sizeof before2);
            ui_input(ui_idn(ui_id("st-key"), i), (Rectangle){ x + c1 + 10, y, kw, 34 }, N.key[i], sizeof N.key[i], "e.g. nu, deltaT, RAS/model", INPUT_MONO | INPUT_LIVE);
            char kp[160] = "";
            if (ui_dropdown(ui_idn(ui_id("st-keylist"), i), (Rectangle){ x + c1 + 18 + kw, y, 112, 34 }, "", kl, N.nKeys[i], DD_MONO | DD_FILTER, kp, sizeof kp, "Keywords"))
                pl_strlcpy(N.key[i], kp, sizeof N.key[i]);
            if (strcmp(before2, N.key[i]) && strcmp(N.key[i], "@refine")) {
                /* a new keyword: its template follows its present value (dimensions and vectors kept) */
                char cv[200], t[128], n[64]; current_value(c, N.file[i], N.key[i], cv, sizeof cv);
                if (num_template(cv, t, sizeof t, n, sizeof n)) pl_strlcpy(N.tmpl[i], t, sizeof N.tmpl[i]); else pl_strlcpy(N.tmpl[i], "{}", sizeof N.tmpl[i]);
                N.label[i][0] = 0; N.knob[i] = -1;
            }
            y += 40;
            char cur[200]; current_value(c, N.file[i], N.key[i], cur, sizeof cur);
            char t[300]; snprintf(t, sizeof t, "Now: %s", cur[0] ? cur : "-");
            ui_text_clip(F_MONO, 12, t, x, y, c1, pl_starts_with(cur, "(not") ? TH.warn : TH.text3);
            ui_text(F_REG, 12, "Written as ({} = the value)", x + c1 + 10, y, TH.text3);
            ui_input(ui_idn(ui_id("st-tmpl"), i), (Rectangle){ x + c1 + 180, y - 8, w - c1 - 180, 30 }, N.tmpl[i], sizeof N.tmpl[i], "{}  or  uniform ({} 0 0)", INPUT_MONO | INPUT_LIVE);
            y += 32;
            if (N.knob[i] >= 0) k = &N.k[N.knob[i]];
        }
        y = values_editor(c, i, k, x, y, w) + 6;
    }
    /* a second setting; how the two combine (Advanced) */
    bool was = N.two;
    ui_checkbox(ui_id("st-two"), (Rectangle){ x, y, w / 2, 24 }, &N.two, "Change a second setting too");
    if (N.two && !was && N.mode == 0 && N.knob[1] < 0 && N.nk > 1) use_knob(c, 1, N.knob[0] == 1 ? 0 : 1);
    if (N.two && N.mode == 1) {
        static const char *cb[] = { "Every combination", "Pair them up" };
        ui_segmented(ui_id("st-combine"), (Rectangle){ x + w / 2, y - 4, w / 2, 32 }, cb, 2, &N.combine);
    }
    y += 40;
    /* what it will make */
    int nv = variant_count();
    char pv[200];
    if (nv < 0) pl_fmt_ui(pv, sizeof pv, "Pairing needs as many values of each setting");
    else pl_fmt_ui(pv, sizeof pv, "Makes %d case%s%s", nv, nv == 1 ? "" : "s", nv > MAX_VARIANTS ? TextFormat(": too many (at most %d)", MAX_VARIANTS) : ":");
    ui_text(F_SEMI, 13.5f, pv, x, y, nv < 0 || nv > MAX_VARIANTS ? TH.err : TH.text);
    y += 24;
    if (nv > 0 && nv <= MAX_VARIANTS) {
        char a[16][64], bb[16][64]; int na = 0, nb = 0;
        for (int i = 0; i < (N.two ? 2 : 1); i++) {
            char tmp[400]; pl_strlcpy(tmp, N.values[i], sizeof tmp); char *sv = NULL;
            for (char *q = strtok_r(tmp, ",;", &sv); q; q = strtok_r(NULL, ",;", &sv)) {
                pl_trim_inplace(q);
                if (!*q) continue;
                if (i == 0 && na < 16) pl_strlcpy(a[na++], q, 64);
                if (i == 1 && nb < 16) pl_strlcpy(bb[nb++], q, 64);
            }
        }
        FDFile *bm = case_dict(c, "system/blockMeshDict");
        double bs = 0; long cells0 = 0; char why[160];
        bool refine0 = !strcmp(N.key[0], "@refine"), refine1 = N.two && !strcmp(N.key[1], "@refine");
        if ((refine0 || refine1) && bm) blocks_base_size(bm, blocks_scale(bm), 0, false, &bs, &cells0, why, sizeof why);
        int shown = 0;
        for (int ia = 0; ia < na && shown < 8; ia++)
            for (int ib = (N.two && N.combine == 1) ? ia : 0; ib < (N.two ? (N.combine == 1 ? ia + 1 : nb) : 1) && shown < 8; ib++, shown++) {
                char va[64], vb[64] = "", line[400];
                safe_name(a[ia], va, sizeof va);
                if (N.two) safe_name(bb[ib], vb, sizeof vb);
                if (N.two) pl_fmt_ui(line, sizeof line, "%s_%s-%s_%s-%s", c->s.name, short_key(N.key[0]), va, short_key(N.key[1]), vb);
                else pl_fmt_ui(line, sizeof line, "%s_%s-%s", c->s.name, short_key(N.key[0]), va);
                ui_text_clip(F_MONO, 12, line, x + 12, y, w * 0.68f, TH.text2);
                double f = (refine0 ? atof(a[ia]) : 1) * (refine1 ? atof(bb[ib]) : 1);
                long cells = 0;
                if ((refine0 || refine1) && bm && bs > 0 && f > 0 && blocks_base_size(bm, blocks_scale(bm), bs / f, false, NULL, &cells, why, sizeof why)) {
                    char ct[48]; snprintf(ct, sizeof ct, "%ld cells", cells);
                    ui_text(F_REG, 12, ct, x + w - ui_text_w(F_REG, 12, ct), y, cells > 5000000 ? TH.warn : TH.text3);
                }
                y += 20;
            }
        if (nv > shown) { char m[48]; snprintf(m, sizeof m, "and %d more", nv - shown); ui_text(F_REG, 12, m, x + 12, y, TH.text3); y += 20; }
    }
    y += 10;
    ui_checkbox(ui_id("st-queue"), (Rectangle){ x, y, w / 2, 24 }, &N.queue, "Queue them to run");
    y += 34;
    ui_scroll_end(&N.scroll, y - top);
    if (N.err[0]) ui_text_clip(F_REG, 12.5f, N.err, x + 110, b.y + b.height - 28, w - 110 - 200, TH.err);
    float by = b.y + b.height - 38;
    if (ui_button(ui_id("st-cancel"), (Rectangle){ x, by, 100, 38 }, NULL, "Cancel", BTN_GHOST)) *close = true;
    float bw = ui_button_width(ICON_GIT_FORK, "Make the variants");
    ui_disable(nv <= 0 || nv > MAX_VARIANTS);
    if (ui_button(ui_id("st-go"), (Rectangle){ x + w - bw, by, bw, 38 }, ICON_GIT_FORK, "Make the variants", BTN_PRIMARY)) make_study(c);
    ui_disable(false);
}

/* ============================================================================ results */
typedef struct { char path[PL_PATH]; double at; CaseSummary s; MonSet *ms; double clock; char resid[16][24]; double residV[16]; int nResid; } RowData;
static RowData R[MAX_VARIANTS];
static int nR;

/* the end of the solver log: ClockTime and the last initial residual of each field */
static void log_tail(RowData *d)
{
    d->clock = -1; d->nResid = 0;
    char lp[PL_PATH], n[96]; snprintf(n, sizeof n, "log.%s", d->s.app);
    pl_join(lp, sizeof lp, d->path, n);
    long long sz = pl_file_size(lp);
    if (sz <= 0) return;
    FILE *f = fopen(lp, "rb");
    if (!f) return;
    long take = sz > 200000 ? 200000 : (long)sz;
    fseek(f, -take, SEEK_END);
    char *b = malloc((size_t)take + 1);
    size_t r = fread(b, 1, (size_t)take, f);
    fclose(f);
    b[r] = 0;
    for (char *l = strtok(b, "\n"); l; l = strtok(NULL, "\n")) {
        char *ct = strstr(l, "ClockTime = ");
        if (ct) d->clock = atof(ct + 12);
        char *sf = strstr(l, "Solving for ");
        char *ir = sf ? strstr(sf, "Initial residual = ") : NULL;
        if (sf && ir) {
            char nm[24]; int k = 0; const char *q = sf + 12;
            while (*q && *q != ',' && k < 23) nm[k++] = *q++;
            nm[k] = 0;
            int i = 0; while (i < d->nResid && strcmp(d->resid[i], nm)) i++;
            if (i == d->nResid && d->nResid < 16) pl_strlcpy(d->resid[d->nResid++], nm, 24);
            if (i < 16) d->residV[i] = atof(ir + 19);
        }
    }
    free(b);
}

static RowData *row_data(const char *path)
{
    RowData *d = NULL;
    for (int i = 0; i < nR; i++) if (!strcmp(R[i].path, path)) d = &R[i];
    if (!d) {
        if (nR >= MAX_VARIANTS) { if (R[0].ms) { monset_close(R[0].ms); free(R[0].ms); } memmove(&R[0], &R[1], sizeof R[0] * (MAX_VARIANTS - 1)); nR--; }
        d = &R[nR++];
        memset(d, 0, sizeof *d);
        pl_strlcpy(d->path, path, sizeof d->path);
    }
    if (GetTime() - d->at > 4 || d->at == 0) {
        d->at = GetTime();
        case_summarize(path, &d->s);
        for (int k = 0; k <= MAX_JOBS; k++) if (A.jobs[k].active && !strcmp(A.jobs[k].casePath, path)) d->s.status = CS_RUNNING;
        if (d->s.status != CS_RUNNING && app_queue_has(path)) d->s.status = CS_QUEUED;
        if (!d->ms) { d->ms = calloc(1, sizeof(MonSet)); monset_open(d->ms, path); }
        else monset_poll(d->ms, GetTime());
        log_tail(d);
    }
    return d;
}

/* an output's value for a case: NAN when it isn't there (yet) */
static double output_value(RowData *d, const Output *o)
{
    if (o->kind == 1) return d->clock;
    if (o->kind == 2) { const char *lt = case_latest_result(&d->s); return lt[0] ? atof(lt) : NAN; }
    if (o->kind == 3) { for (int i = 0; i < d->nResid; i++) if (!strcmp(d->resid[i], o->col)) return d->residV[i]; return NAN; }
    int fi = monset_find(d->ms, o->file);
    if (fi < 0) return NAN;
    MonFile *f = &d->ms->f[fi];
    for (int c = 0; c < f->nCols; c++)
        if (!strcmp(f->col[c], o->col)) { for (int k = f->n - 1; k >= 0; k--) if (!isnan(f->v[c][k])) return f->v[c][k]; }
    return NAN;
}

static void output_label(const Output *o, char *b, int cap)
{
    if (o->kind == 1) snprintf(b, (size_t)cap, "run time (s)");
    else if (o->kind == 2) snprintf(b, (size_t)cap, "latest time");
    else if (o->kind == 3) snprintf(b, (size_t)cap, "%s residual", o->col);
    else snprintf(b, (size_t)cap, "%s", o->col);
}

/* measurements: the first two numeric columns of a CSV (or whitespace-separated) file */
static int read_csv(const char *path, double *x, double *y, int max)
{
    char *t = pl_read_file(path, NULL);
    if (!t) return 0;
    int n = 0;
    for (char *l = strtok(t, "\n"); l && n < max; l = strtok(NULL, "\n")) {
        for (char *q = l; *q; q++) if (*q == ',' || *q == ';' || *q == '\t') *q = ' ';
        double a, b;
        if (sscanf(l, "%lf %lf", &a, &b) == 2) { x[n] = a; y[n] = b; n++; }
    }
    free(t);
    return n;
}

/* ============================================================================ page */
static double nice(double range)
{
    double raw = range / 5, mag = pow(10, floor(log10(raw))), n = raw / mag;
    return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag;
}

static void study_chart(Rectangle r, Study *s, int oi)
{
    ui_rect(r, 12, TH.chartBg);
    ui_rect_lines(r, 12, 1, TH.lineSoft);
    if (oi < 0 || oi >= s->nOut || s->nV == 0) return;
    const Output *o = &s->out[oi];
    /* x: the first setting's value (numeric); one line per value of the second */
    double xs[MAX_VARIANTS], ys[MAX_VARIANTS]; int grp[MAX_VARIANTS], n = 0;
    char groups[MAX_VARIANTS][64]; int ng = 0;
    for (int i = 0; i < s->nV; i++) {
        char *e; double xv = strtod(s->v[i].v1, &e);
        if (e == s->v[i].v1) continue;
        double yv = output_value(row_data(s->v[i].path), o);
        if (isnan(yv)) continue;
        int g = 0; while (g < ng && strcmp(groups[g], s->v[i].v2)) g++;
        if (g == ng) pl_strlcpy(groups[ng++], s->v[i].v2, 64);
        xs[n] = xv; ys[n] = yv; grp[n] = g; n++;
    }
    static double ex[4096], ey[4096];
    int ne = s->exp[0] ? read_csv(s->exp, ex, ey, 4096) : 0;
    char lab[96]; output_label(o, lab, sizeof lab);
    char t[200]; pl_fmt_ui(t, sizeof t, "%s against %s", lab, param_name(&s->p[0]));
    ui_text(F_MED, 13, t, r.x + 16, r.y + 12, TH.text);
    if (n + ne == 0) { ui_text(F_REG, 12.5f, "No numbers yet: the chart fills in as the variants finish.", r.x + 16, r.y + 40, TH.text3); return; }
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (int i = 0; i < n; i++) { x0 = fmin(x0, xs[i]); x1 = fmax(x1, xs[i]); y0 = fmin(y0, ys[i]); y1 = fmax(y1, ys[i]); }
    for (int i = 0; i < ne; i++) { x0 = fmin(x0, ex[i]); x1 = fmax(x1, ex[i]); y0 = fmin(y0, ey[i]); y1 = fmax(y1, ey[i]); }
    if (x1 - x0 < 1e-300) { x0 -= 1; x1 += 1; }
    if (y1 - y0 < 1e-300) { double m = fabs(y1) > 0 ? fabs(y1) * 0.1 : 1; y0 -= m; y1 += m; }
    double py = (y1 - y0) * 0.08; y0 -= py; y1 += py;
    double px = (x1 - x0) * 0.04; x0 -= px; x1 += px;
    Rectangle p = { r.x + 72, r.y + 44, r.width - 72 - 24, r.height - 44 - 36 };
    double sy = nice(y1 - y0), sx = nice(x1 - x0);
    for (double v = ceil(y0 / sy) * sy; v <= y1; v += sy) {
        float yy = p.y + p.height - (float)((v - y0) / (y1 - y0)) * p.height;
        DrawRectangleRec((Rectangle){ p.x, yy, p.width, 1 }, TH.grid);
        char b[32]; snprintf(b, sizeof b, "%.4g", fabs(v) < sy * 1e-6 ? 0.0 : v);
        ui_text(F_MONO, 11.5f, b, p.x - 10 - ui_text_w(F_MONO, 11.5f, b), yy - 7, TH.text3);
    }
    for (double v = ceil(x0 / sx) * sx; v <= x1; v += sx) {
        float xx = p.x + (float)((v - x0) / (x1 - x0)) * p.width;
        char b[32]; snprintf(b, sizeof b, "%.4g", fabs(v) < sx * 1e-6 ? 0.0 : v);
        ui_text(F_REG, 11.5f, b, xx - ui_text_w(F_REG, 11.5f, b) / 2, p.y + p.height + 8, TH.text3);
    }
#define PX(v) (p.x + (float)(((v) - x0) / (x1 - x0)) * p.width)
#define PY(v) (p.y + p.height - (float)(((v) - y0) / (y1 - y0)) * p.height)
    for (int g = 0; g < ng; g++) {                              /* each group's points in x order, joined */
        int idx[MAX_VARIANTS], m = 0;
        for (int i = 0; i < n; i++) if (grp[i] == g) idx[m++] = i;
        for (int a = 0; a < m; a++) for (int b = a + 1; b < m; b++) if (xs[idx[b]] < xs[idx[a]]) { int t2 = idx[a]; idx[a] = idx[b]; idx[b] = t2; }
        Color col = TH.series[g % 10];
        for (int a = 0; a + 1 < m; a++) DrawLineEx((Vector2){ PX(xs[idx[a]]), PY(ys[idx[a]]) }, (Vector2){ PX(xs[idx[a + 1]]), PY(ys[idx[a + 1]]) }, 2, col);
        for (int a = 0; a < m; a++) DrawCircleV((Vector2){ PX(xs[idx[a]]), PY(ys[idx[a]]) }, 5, col);
        if (s->nParams > 1) {
            char gl[96]; pl_fmt_ui(gl, sizeof gl, "%s = %s", param_name(&s->p[1]), groups[g]);
            ui_text(F_MED, 12, gl, r.x + r.width - 220, r.y + 12 + g * 18, col);
        }
    }
    for (int i = 0; i < ne; i++) {                              /* measurements: hollow squares */
        Rectangle q = { PX(ex[i]) - 4, PY(ey[i]) - 4, 8, 8 };
        DrawRectangleLinesEx(q, 1.5f, TH.text);
    }
    if (ne) ui_text(F_REG, 12, "squares: measurements", r.x + 32 + ui_text_w(F_MED, 13, t), r.y + 13, TH.text2);
#undef PX
#undef PY
}

static int chartOut;

void page_studies(Rectangle r)
{
    scan(false);
    UiScroll *sc = &A.scr[19];
    float top = ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = fminf(r.width - 2 * PAGE_PAD, CONTENT_MAX + 200), y = top + 30;
    y += page_header(x, y, w, "Studies", "Variants of a case with one or two settings changed, run from the queue and compared here: a mesh "
                     "study, a sweep of a parameter, a set of models.");
    float bw;
    if (A.cs) {
        char lab[300]; snprintf(lab, sizeof lab, "New study from %s", A.cs->s.name);
        bw = ui_button_width(ICON_GIT_FORK, lab);
        if (ui_button(ui_id("st-new"), (Rectangle){ x, y, bw, 36 }, ICON_GIT_FORK, lab, BTN_PRIMARY)) A.modal = MD_STUDY_NEW;
    } else {
        ui_text(F_REG, 13, "Open a case to make a study from it.", x, y + 9, TH.text3);
        float ow = ui_button_width(ICON_LIBRARY, "Open a case");
        float tx = x + ui_text_w(F_REG, 13, "Open a case to make a study from it.") + 14;
        if (ui_button(ui_id("st-open-case"), (Rectangle){ tx, y, ow, 36 }, ICON_LIBRARY, "Open a case", BTN_SECONDARY)) { app_rescan_cases(true); app_go(PG_CASES); }
    }
    y += 56;
    if (nS == 0) { ui_text(F_REG, 13, "No studies yet.", x, y, TH.text3); ui_scroll_end(sc, y - top + 40); return; }
    /* study pills */
    float lx = x;
    for (int i = 0; i < nS; i++) {
        float pw = ui_text_w(F_MED, 13, S[i].name) + 30;
        if (lx + pw > x + w) { lx = x; y += 38; }
        Rectangle pr = { lx, y, pw, 30 };
        bool on = i == sel, hov = ui_hovered(pr);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        ui_rect(pr, 15, on ? TH.accent : hov ? TH.hover : TH.surface);
        ui_text(F_MED, 13, S[i].name, lx + 15, y + 7, on ? TH.textOnAccent : TH.text2);
        if (ui_clicked(pr)) { sel = i; chartOut = 0; }
        lx += pw + 8;
    }
    y += 50;
    if (sel < 0 || sel >= nS) { ui_scroll_end(sc, y - top + 40); return; }
    Study *s = &S[sel];
    char info[PL_PATH + 200];
    snprintf(info, sizeof info, "%d variant%s of %s  ·  %s", s->nV, s->nV == 1 ? "" : "s", pl_basename(s->base), s->file);
    ui_text_clip(F_MONO, 12, info, x, y, w, TH.text3);
    y += 26;
    /* actions */
    int waiting = 0;
    for (int i = 0; i < s->nV; i++) { RowData *d = row_data(s->v[i].path); if (d->s.status != CS_DONE && d->s.status != CS_RUNNING && d->s.status != CS_QUEUED) waiting++; }
    {
        float bx = x;
        char ql[64]; snprintf(ql, sizeof ql, "Queue the %d not run", waiting);
        bw = ui_button_width(ICON_LIST, ql);
        ui_disable(waiting == 0);
        if (ui_button(ui_id("st-queue-all"), (Rectangle){ bx, y, bw, 34 }, ICON_LIST, ql, BTN_SECONDARY)) {
            extern int run_procs_for_check(void);
            for (int i = 0; i < s->nV; i++) {
                RowData *d = row_data(s->v[i].path);
                if (d->s.status == CS_DONE || d->s.status == CS_RUNNING || d->s.status == CS_QUEUED) continue;
                app_queue_add(s->v[i].path, -1, A.cs ? run_procs_for_check() : A.set.nProcs, false, case_has_results(&d->s));
                d->at = 0;
            }
        }
        ui_disable(false);
        bx += bw + 10;
        bw = ui_button_width(ICON_PLUS, "Compare by ...");
        if (ui_button(ui_id("st-add-out"), (Rectangle){ bx, y, bw, 34 }, ICON_PLUS, "Compare by ...", BTN_SECONDARY)) A.modal = MD_STUDY_OUT;
        bx += bw + 10;
        bw = ui_button_width(ICON_CHART_LINE, s->exp[0] ? "Measurements: change" : "Add measurements (CSV)");
        if (ui_button(ui_id("st-exp"), (Rectangle){ bx, y, bw, 34 }, ICON_CHART_LINE, s->exp[0] ? "Measurements: change" : "Add measurements (CSV)", BTN_GHOST)) {
            A.modal = MD_STUDY_EXP; pl_strlcpy(A.mText, s->exp, sizeof A.mText); A.mErr[0] = 0;
        }
    }
    y += 50;
    /* the table */
    int nc = s->nParams + 1 + s->nOut;
    float cw[12] = { 0 }; float tw = w - 90;
    for (int i = 0; i < nc; i++) cw[i] = tw / nc;
    float rowH = 38;
    ui_rect((Rectangle){ x, y, w, rowH }, 8, TH.surface2);
    float cx = x + 14;
    for (int i = 0; i < s->nParams; i++) { ui_text_clip(F_MED, 12.5f, param_name(&s->p[i]), cx, y + 11, cw[0] - 10, TH.text2); cx += cw[0]; }
    ui_text(F_MED, 12.5f, "status", cx, y + 11, TH.text2); cx += cw[0];
    int delOut = -1;
    for (int i = 0; i < s->nOut; i++) {
        char lab[96]; output_label(&s->out[i], lab, sizeof lab);
        Rectangle hr = { cx - 4, y + 4, cw[0] - 6, rowH - 8 };
        bool on = chartOut == i;
        if (on) ui_rect(hr, 6, ui_alpha(TH.accent, 0.18f));
        if (ui_hovered(hr)) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (ui_clicked(hr)) chartOut = i;
        ui_text_clip(F_MED, 12.5f, lab, cx, y + 11, cw[0] - 30, on ? TH.accent : TH.text2);
        if (ui_button(ui_idn(ui_id("st-out-x"), i), (Rectangle){ cx + cw[0] - 30, y + 7, 24, 24 }, ICON_X, NULL, BTN_ICON)) delOut = i;
        cx += cw[0];
    }
    y += rowH + 4;
    for (int i = 0; i < s->nV; i++) {
        RowData *d = row_data(s->v[i].path);
        Rectangle row = { x, y, w, rowH };
        if (ui_row(ui_idn(ui_id("st-row"), i), row, A.cs && !strcmp(A.cs->s.path, s->v[i].path))) app_open_case(s->v[i].path, PG_RUN);
        cx = x + 14;
        ui_text_clip(F_MONO, 13, s->v[i].v1, cx, y + 11, cw[0] - 10, TH.text); cx += cw[0];
        if (s->nParams > 1) { ui_text_clip(F_MONO, 13, s->v[i].v2, cx, y + 11, cw[0] - 10, TH.text); cx += cw[0]; }
        const char *stn = pl_exists(s->v[i].path) ? status_name(d->s.status) : "missing";
        ui_text(F_MED, 12.5f, stn, cx, y + 11, status_color(d->s.status));
        cx += cw[0];
        for (int k = 0; k < s->nOut; k++) {
            double v = output_value(d, &s->out[k]);
            char b[48];
            if (isnan(v) || (s->out[k].kind == 1 && v < 0)) snprintf(b, sizeof b, "-");
            else if (s->out[k].kind == 1) pl_duration(b, sizeof b, v);
            else snprintf(b, sizeof b, "%.5g", v);
            ui_text(F_MONO, 13, b, cx, y + 11, TH.text);
            cx += cw[0];
        }
        ui_icon(ICON_CHEVRON_RIGHT, x + w - 20, y + rowH / 2, 14, TH.text3);
        y += rowH + 2;
    }
    if (delOut >= 0) { memmove(&s->out[delOut], &s->out[delOut + 1], sizeof s->out[0] * (size_t)(s->nOut - delOut - 1)); s->nOut--; study_save(s); if (chartOut >= s->nOut) chartOut = 0; }
    y += 20;
    if (s->nOut) { study_chart((Rectangle){ x, y, w, 340 }, s, chartOut); y += 360; }
    ui_scroll_end(sc, y - top + 40);
}

/* "Compare by": a monitor column (from the first variant that has one), run time, latest time or a final residual */
void study_out_modal(bool *close)
{
    if (sel < 0 || sel >= nS) { *close = true; return; }
    Study *s = &S[sel];
    Rectangle b = ui_modal_begin("Compare by", "A value read from every variant: the last value of a monitor column, the final residual "
                                 "of a field, the run time, or how far it got.", 640, 520, close);
    static char opts[400][320]; static Output oo[400]; int n = 0;
    oo[n].kind = 1; snprintf(opts[n++], 320, "Run time (wall clock)");
    oo[n].kind = 2; snprintf(opts[n++], 320, "Latest time written");
    for (int i = 0; i < s->nV && n < 380; i++) {
        RowData *d = row_data(s->v[i].path);
        for (int k = 0; k < d->nResid && n < 380; k++) {
            bool have = false; for (int j = 0; j < n; j++) if (oo[j].kind == 3 && !strcmp(oo[j].col, d->resid[k])) have = true;
            if (have) continue;
            oo[n].kind = 3; pl_strlcpy(oo[n].col, d->resid[k], sizeof oo[n].col);
            snprintf(opts[n++], 320, "Final residual of %s", d->resid[k]);
        }
        for (int f = 0; d->ms && f < d->ms->n; f++)
            for (int c = 0; c < d->ms->f[f].nCols && n < 380; c++) {
                bool have = false;
                for (int j = 0; j < n; j++) if (oo[j].kind == 0 && !strcmp(oo[j].file, d->ms->f[f].key) && !strcmp(oo[j].col, d->ms->f[f].col[c])) have = true;
                if (have) continue;
                oo[n].kind = 0; pl_strlcpy(oo[n].file, d->ms->f[f].key, sizeof oo[n].file); pl_strlcpy(oo[n].col, d->ms->f[f].col[c], sizeof oo[n].col);
                snprintf(opts[n++], 320, "%s : %s", d->ms->f[f].key, d->ms->f[f].col[c]);
            }
    }
    UiScroll *sc = &A.scr[18];
    Rectangle lr = { b.x, b.y, b.width, b.height - 52 };
    float top = ui_scroll_begin(sc, lr);
    float y = top;
    for (int i = 0; i < n; i++) {
        Rectangle row = { b.x, y, b.width - 12, 30 };
        if (ui_row(ui_idn(ui_id("so-row"), i), row, false) && s->nOut < MAX_OUTPUTS) {
            s->out[s->nOut++] = oo[i];
            study_save(s);
            chartOut = s->nOut - 1;
            *close = true;
        }
        ui_text_clip(oo[i].kind == 0 ? F_MONO : F_REG, 13, opts[i], b.x + 10, y + 7, b.width - 30, TH.text);
        y += 32;
    }
    ui_scroll_end(sc, y - top);
    if (ui_button(ui_id("so-cancel"), (Rectangle){ b.x, b.y + b.height - 38, 100, 38 }, NULL, "Cancel", BTN_GHOST)) *close = true;
}

void study_exp_modal(bool *close)
{
    if (sel < 0 || sel >= nS) { *close = true; return; }
    Study *s = &S[sel];
    Rectangle b = ui_modal_begin("Measurements", "A CSV (or space-separated) file: the first column is the first setting's value, the "
                                 "second the measured value. Shown as squares on the study's chart.", 640, 250, close);
    ui_input(ui_id("se-path"), (Rectangle){ b.x, b.y + 4, b.width, 38 }, A.mText, sizeof A.mText, "~/data/measured.csv", INPUT_MONO | INPUT_LIVE);
    if (A.mErr[0]) ui_text(F_REG, 12.5f, A.mErr, b.x, b.y + 50, TH.err);
    if (ui_button(ui_id("se-cancel"), (Rectangle){ b.x, b.y + b.height - 38, 100, 38 }, NULL, "Cancel", BTN_GHOST)) *close = true;
    if (s->exp[0] && ui_button(ui_id("se-clear"), (Rectangle){ b.x + 110, b.y + b.height - 38, 110, 38 }, NULL, "Remove", BTN_GHOST)) { s->exp[0] = 0; study_save(s); *close = true; }
    if (ui_button(ui_id("se-go"), (Rectangle){ b.x + b.width - 110, b.y + b.height - 38, 110, 38 }, NULL, "Use it", BTN_PRIMARY)) {
        char p[PL_PATH]; pl_expand_home(p, sizeof p, A.mText);
        double xx[4], yy[4];
        if (!pl_is_file(p)) snprintf(A.mErr, sizeof A.mErr, "No such file.");
        else if (read_csv(p, xx, yy, 4) == 0) snprintf(A.mErr, sizeof A.mErr, "No rows with two numbers in that file.");
        else { pl_strlcpy(s->exp, p, sizeof s->exp); study_save(s); *close = true; }
    }
}
