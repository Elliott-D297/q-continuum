/* ccxmodel.c -- a CalculiX deck in engineering terms (see ccxmodel.h). A line-by-line reader of the keywords that say what
 * the model is: *NODE, *ELEMENT, *NSET (with GENERATE), *MATERIAL with *ELASTIC / *DENSITY / *CONDUCTIVITY / *SPECIFIC
 * HEAT / *EXPANSION, *BOUNDARY, *CLOAD, *DLOAD, *STEP and its procedure; *INCLUDE files are read where they stand. The
 * Setup page edits the input's own data lines through params.c; this only reads. */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ccxmodel.h"
#include "pl_util.h"

const CcxLibMat CCX_LIB[] = {
    { "Structural steel",          210e9,   0.30,  7850, 50,   460, 12e-6,   250e6 },
    { "Stainless steel 304",       193e9,   0.29,  8000, 16.2, 500, 17.3e-6, 215e6 },
    { "Aluminium 6061-T6",         68.9e9,  0.33,  2700, 167,  896, 23.6e-6, 276e6 },
    { "Titanium Ti-6Al-4V",        113.8e9, 0.342, 4430, 6.7,  526, 8.6e-6,  880e6 },
    { "Copper",                    117e9,   0.34,  8960, 401,  385, 16.5e-6, 70e6 },
};
const int CCX_LIB_N = (int)(sizeof CCX_LIB / sizeof CCX_LIB[0]);

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t k = 0; while (s[k] && isspace((unsigned char)s[k])) k++;
    if (k) memmove(s, s + k, n - k + 1);
}
/* a keyword line's parameter: "*NSET, NSET=TIP, GENERATE" -> "TIP" for "NSET" */
static bool kw_param(const char *line, const char *name, char *out, int cap)
{
    size_t n = strlen(name);
    for (const char *p = line; (p = strchr(p, ',')); ) {
        p++;
        while (*p == ' ') p++;
        if (!strncasecmp(p, name, n)) {
            const char *q = p + n; while (*q == ' ') q++;
            if (*q != '=') continue;
            q++; while (*q == ' ') q++;
            int k = 0; while (*q && *q != ',' && *q != '\n' && *q != '\r' && k < cap - 1) out[k++] = *q++;
            out[k] = 0; trim(out);
            return true;
        }
    }
    return false;
}
static bool kw_flag(const char *line, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = line; (p = strchr(p, ',')); ) {
        p++; while (*p == ' ') p++;
        if (!strncasecmp(p, name, n) && (p[n] == 0 || p[n] == ',' || isspace((unsigned char)p[n]))) return true;
    }
    return false;
}
static int split(char *s, char **f, int max)          /* comma-separated, trimmed */
{
    int n = 0;
    for (char *p = s; n < max; ) {
        f[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = 0; p = c + 1;
    }
    for (int i = 0; i < n; i++) trim(f[i]);
    if (n && !f[n - 1][0]) n--;
    return n;
}

static CcxSet *set_get(CcxModel *m, const char *name)
{
    for (int i = 0; i < m->nSets; i++) if (!strcasecmp(m->set[i].name, name)) return &m->set[i];
    if (m->nSets >= CCX_MAX_SETS) return NULL;
    CcxSet *s = &m->set[m->nSets++];
    pl_strlcpy(s->name, name, sizeof s->name); s->nNodes = 0;
    return s;
}

enum { K_NONE = 0, K_NODE, K_ELEMENT, K_NSET, K_ELASTIC, K_DENSITY, K_COND, K_CP, K_EXP, K_BOUNDARY, K_CLOAD, K_DLOAD, K_PROC };

static const char *gMainText;   /* the input as the Setup page holds it (unsaved edits), instead of the file */
static void read_file(const char *dir, const char *rel, CcxModel *m, bool main, int depth)
{
    if (depth > 4) return;
    char p[PL_PATH]; pl_join(p, sizeof p, dir, rel);
    char *t = main && gMainText ? strdup(gMainText) : pl_read_file(p, NULL);
    if (!t) return;
    int mode = K_NONE, lineNo = 0;
    bool gen = false, firstStep = m->analysis == CCX_NONE;
    CcxSet *cur = NULL;
    CcxMat *mat = m->nMat ? &m->mat[m->nMat - 1] : NULL;
    for (char *l = t; l && *l; ) {
        char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);
        char line[1024]; snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), l);
        lineNo++;
        l = e ? e + 1 : NULL;
        trim(line);
        if (!line[0] || (line[0] == '*' && line[1] == '*')) continue;
        if (line[0] == '*') {
            mode = K_NONE; gen = false;
            char kw[64]; int k = 0;
            for (const char *q = line; *q && *q != ',' && k < 63; q++) kw[k++] = (char)toupper((unsigned char)*q);
            kw[k] = 0; trim(kw);
            char v[256];
            if (!strcmp(kw, "*INCLUDE") && kw_param(line, "INPUT", v, sizeof v)) { read_file(dir, v, m, false, depth + 1); continue; }
            if (!strcmp(kw, "*NODE")) { mode = K_NODE; cur = kw_param(line, "NSET", v, sizeof v) ? set_get(m, v) : NULL; }
            else if (!strcmp(kw, "*ELEMENT")) { mode = K_ELEMENT; if (kw_param(line, "TYPE", v, sizeof v)) pl_strlcpy(m->elType, v, sizeof m->elType); }
            else if (!strcmp(kw, "*NSET")) { mode = K_NSET; gen = kw_flag(line, "GENERATE"); cur = kw_param(line, "NSET", v, sizeof v) ? set_get(m, v) : NULL; }
            else if (!strcmp(kw, "*MATERIAL")) {
                if (m->nMat < CCX_MAX_MAT) { mat = &m->mat[m->nMat++]; memset(mat, 0, sizeof *mat); kw_param(line, "NAME", mat->name, sizeof mat->name); }
            }
            else if (!strcmp(kw, "*ELASTIC")) mode = K_ELASTIC;
            else if (!strcmp(kw, "*DENSITY")) mode = K_DENSITY;
            else if (!strcmp(kw, "*CONDUCTIVITY")) mode = K_COND;
            else if (!strcmp(kw, "*SPECIFIC HEAT")) mode = K_CP;
            else if (!strcmp(kw, "*EXPANSION")) mode = K_EXP;
            else if (!strcmp(kw, "*BOUNDARY")) mode = K_BOUNDARY;
            else if (!strcmp(kw, "*CLOAD")) mode = K_CLOAD;
            else if (!strcmp(kw, "*DLOAD")) mode = K_DLOAD;
            else if (!strcmp(kw, "*STEP")) { if (kw_flag(line, "NLGEOM")) m->nonlinear = true; }
            else if (firstStep && m->analysis == CCX_NONE) {
                int a = !strcmp(kw, "*STATIC") ? CCX_STATIC : !strcmp(kw, "*HEAT TRANSFER") ? CCX_HEAT : !strcmp(kw, "*FREQUENCY") ? CCX_FREQUENCY
                      : !strcmp(kw, "*DYNAMIC") || !strcmp(kw, "*MODAL DYNAMIC") ? CCX_DYNAMIC
                      : strstr(kw, "TEMPERATURE-DISPLACEMENT") ? CCX_COUPLED : -1;
                if (a >= 0) m->analysis = a;
            }
            continue;
        }
        char buf[1024]; pl_strlcpy(buf, line, sizeof buf);
        char *f[16]; int n = split(buf, f, 16);
        if (!n) continue;
        switch (mode) {
        case K_NODE:
            if (n >= 3) {
                double x[3] = { atof(f[1]), n > 2 ? atof(f[2]) : 0, n > 3 ? atof(f[3]) : 0 };
                for (int k = 0; k < 3; k++) {
                    if (!m->nNodes || x[k] < m->lo[k]) m->lo[k] = x[k];
                    if (!m->nNodes || x[k] > m->hi[k]) m->hi[k] = x[k];
                }
                m->nNodes++;
                if (cur) cur->nNodes++;
            }
            break;
        case K_ELEMENT: m->nElems++; break;
        case K_NSET:
            if (!cur) break;
            if (gen && n >= 2) { long a = atol(f[0]), b = atol(f[1]), s = n > 2 ? atol(f[2]) : 1; if (s <= 0) s = 1; if (b >= a) cur->nNodes += (int)((b - a) / s + 1); }
            else for (int i = 0; i < n; i++) {
                if (isdigit((unsigned char)f[i][0])) cur->nNodes++;
                else { const CcxSet *o = NULL; for (int k = 0; k < m->nSets; k++) if (!strcasecmp(m->set[k].name, f[i])) o = &m->set[k]; if (o && o != cur) cur->nNodes += o->nNodes; }
            }
            break;
        case K_ELASTIC: if (mat && n >= 1) { mat->E = atof(f[0]); mat->hasE = true; if (n >= 2) mat->nu = atof(f[1]); mat->lineE = main ? lineNo : 0; } mode = K_NONE; break;
        case K_DENSITY: if (mat) { mat->rho = atof(f[0]); mat->hasRho = true; mat->lineRho = main ? lineNo : 0; } mode = K_NONE; break;
        case K_COND: if (mat) { mat->k = atof(f[0]); mat->hasK = true; } mode = K_NONE; break;
        case K_CP: if (mat) { mat->cp = atof(f[0]); mat->hasCp = true; } mode = K_NONE; break;
        case K_EXP: if (mat) { mat->alpha = atof(f[0]); mat->hasAlpha = true; } mode = K_NONE; break;
        case K_BOUNDARY:
            if (m->nBc < CCX_MAX_BC && n >= 2) {
                CcxBc *b = &m->bc[m->nBc++];
                pl_strlcpy(b->set, f[0], sizeof b->set);
                b->d0 = atoi(f[1]); b->d1 = n > 2 && f[2][0] ? atoi(f[2]) : b->d0;
                b->hasValue = n > 3; b->value = n > 3 ? atof(f[3]) : 0;
                b->line = main ? lineNo : 0;
            }
            break;
        case K_CLOAD:
            if (m->nLoad < CCX_MAX_BC && n >= 3) {
                CcxLoad *c = &m->load[m->nLoad++];
                pl_strlcpy(c->set, f[0], sizeof c->set); c->dof = atoi(f[1]); c->mag = atof(f[2]); c->line = main ? lineNo : 0;
            }
            break;
        case K_DLOAD:
            if (m->nDl < CCX_MAX_BC && n >= 3) {
                CcxDload *d = &m->dl[m->nDl++];
                pl_strlcpy(d->set, f[0], sizeof d->set); pl_strlcpy(d->kind, f[1], sizeof d->kind); d->mag = atof(f[2]);
                for (int k = 0; k < 3; k++) d->dir[k] = n > 3 + k ? atof(f[3 + k]) : 0;
                d->line = main ? lineNo : 0;
            }
            break;
        default: break;
        }
    }
    free(t);
}

bool ccx_model_load(const char *dir, const char *input, CcxModel *m)
{
    memset(m, 0, sizeof *m);
    m->analysis = CCX_NONE;
    char p[PL_PATH]; pl_join(p, sizeof p, dir, input);
    if (!pl_is_file(p)) return false;
    read_file(dir, input, m, true, 0);
    return true;
}

bool ccx_model_load_text(const char *dir, const char *input, const char *text, CcxModel *m)
{
    memset(m, 0, sizeof *m);
    m->analysis = CCX_NONE;
    gMainText = text;
    read_file(dir, input, m, true, 0);
    gMainText = NULL;
    return true;
}

int ccx_set_nodes(const CcxModel *m, const char *set)
{
    if (isdigit((unsigned char)set[0])) return 1;
    for (int i = 0; i < m->nSets; i++) if (!strcasecmp(m->set[i].name, set)) return m->set[i].nNodes;
    return -1;
}

double ccx_extent(const CcxModel *m)
{
    double L = 0;
    for (int k = 0; k < 3 && m->nNodes; k++) L = fmax(L, m->hi[k] - m->lo[k]);
    return L;
}

int ccx_units(const CcxModel *m, char *msg, int cap)
{
    if (msg && cap) msg[0] = 0;
    int u = CCX_UNITS_UNKNOWN;
    double E = 0, rho = 0;
    for (int i = 0; i < m->nMat; i++) { if (m->mat[i].hasE && !E) E = m->mat[i].E; if (m->mat[i].hasRho && !rho) rho = m->mat[i].rho; }
    if (E > 1e8) u = CCX_UNITS_SI;                          /* metals 1e10-1e12 Pa, plastics 1e9 */
    else if (E > 5 && E < 1e7) u = CCX_UNITS_MM;            /* metals 1e4-1e6 MPa, plastics 1e3 */
    else if (rho > 100) u = CCX_UNITS_SI;
    else if (rho > 0 && rho < 1e-6) u = CCX_UNITS_MM;
    if (u == CCX_UNITS_UNKNOWN || !msg) return u;
    double L = ccx_extent(m);
    if (u == CCX_UNITS_SI && L > 50)
        snprintf(msg, (size_t)cap, "Young's modulus is in Pa (metres), but the model is %.4g units across: if that is millimetres, the part is modelled "
                 "%.4g m long. Use one system: metres with Pa and kg/m³, or millimetres with MPa and t/mm³.", L, L);
    else if (u == CCX_UNITS_MM && L > 0 && L < 5)
        snprintf(msg, (size_t)cap, "Young's modulus is in MPa (millimetres), but the model is only %.3g units across: if that is metres, scale the "
                 "coordinates to millimetres or give E in Pa (and the density in kg/m³).", L);
    else if (u == CCX_UNITS_SI && rho > 0 && rho < 1)
        snprintf(msg, (size_t)cap, "Young's modulus is in Pa but the density (%.3g) looks like t/mm³: in metres it is kg/m³ (steel 7850).", rho);
    else if (u == CCX_UNITS_MM && rho > 100)
        snprintf(msg, (size_t)cap, "Young's modulus is in MPa but the density (%.4g) looks like kg/m³: in millimetres it is t/mm³ (steel 7.85e-9).", rho);
    return u;
}

const char *ccx_dof_name(int dof)
{
    static const char *const N[] = { "?", "x", "y", "z", "rotation about x", "rotation about y", "rotation about z" };
    if (dof >= 1 && dof <= 6) return N[dof];
    if (dof == 11) return "temperature";
    return "?";
}

const char *ccx_analysis_name(int a)
{
    static const char *const N[] = { "Linear static", "Heat transfer", "Natural frequencies", "Dynamic", "Thermal-stress", "Other", "No step" };
    return a >= 0 && a <= CCX_NONE ? N[a] : "?";
}

int ccx_lib_match(const CcxMat *m, int units)
{
    if (!m->hasE) return -1;
    double E = units == CCX_UNITS_MM ? m->E * 1e6 : m->E;
    for (int i = 0; i < CCX_LIB_N; i++) if (fabs(E - CCX_LIB[i].E) < 0.03 * CCX_LIB[i].E) return i;
    return -1;
}

/* ============================================================================ .frd results (C2) */
static int frd_int(const char *s, int a, int w) { char b[24]; int n = 0; for (int i = a; i < a + w && s[i] && s[i] != '\n'; i++) b[n++] = s[i]; b[n] = 0; return atoi(b); }
static float frd_val(const char *s, int a) { char b[16]; int n = 0; for (int i = a; i < a + 12 && s[i] && s[i] != '\n' && s[i] != '\r'; i++) b[n++] = s[i]; b[n] = 0; return (float)atof(b); }

void ccx_frd_free(CcxFrd *r)
{
    free(r->id); free(r->x); free(r->u); free(r->vm); free(r->rf); free(r->T); free(r->edge);
    memset(r, 0, sizeof *r);
}

bool ccx_frd_load(const char *path, CcxFrd *r)
{
    memset(r, 0, sizeof *r);
    char *t = pl_read_file(path, NULL);
    if (!t) return false;
    int cap = 0, maxId = 0, *map = NULL, ecap = 0;
    enum { B_NONE, B_NODES, B_ELEMS, B_DISP, B_STRESS, B_FORC, B_TEMP, B_OTHER } blk = B_NONE;
    int elType = 0, en = 0, enodes[20];
    bool oom = false;                                      /* memory ran out (or an absurd id): the file fails to load */
    for (char *l = t; l && *l && !oom; ) {
        char *e = strchr(l, '\n');
        if (!strncmp(l, "    2C", 6)) blk = B_NODES;
        else if (!strncmp(l, "    3C", 6)) blk = B_ELEMS;
        else if (!strncmp(l, "  100C", 6)) { r->time = atof(l + 12); blk = B_OTHER; }
        else if (!strncmp(l, " -4  ", 5)) {
            blk = !strncmp(l + 5, "DISP", 4) ? B_DISP : !strncmp(l + 5, "STRESS", 6) ? B_STRESS : !strncmp(l + 5, "FORC", 4) ? B_FORC
                : !strncmp(l + 5, "NDTEMP", 6) ? B_TEMP : B_OTHER;
            if (map && r->n) {                       /* a new block of these: this increment's values replace the last's */
                size_t n = (size_t)r->n;
                if (blk == B_DISP && !r->u && !(r->u = calloc(3 * n, sizeof(float)))) oom = true;
                if (blk == B_STRESS && !r->vm && !(r->vm = calloc(n, sizeof(float)))) oom = true;
                if (blk == B_FORC && !r->rf && !(r->rf = calloc(3 * n, sizeof(float)))) oom = true;
                if (blk == B_TEMP && !r->T && !(r->T = calloc(n, sizeof(float)))) oom = true;
            }
        }
        else if (!strncmp(l, " -3", 3)) {
            if (blk == B_NODES && r->n) {               /* the nodes are in: a map from id to index */
                free(map);
                map = malloc(sizeof(int) * (size_t)(maxId + 1));
                if (!map) { oom = true; break; }
                for (int i = 0; i <= maxId; i++) map[i] = -1;
                for (int i = 0; i < r->n; i++) if (r->id[i] >= 0) map[r->id[i]] = i;   /* (a negative id: a damaged file) */
            }
            blk = B_NONE;
        }
        else if (!strncmp(l, " -1", 3)) {
            int id = frd_int(l, 3, 10);
            if (blk == B_NODES) {
                if (r->n == cap) {
                    int nc = cap ? cap * 2 : 4096;
                    int *ni = realloc(r->id, sizeof(int) * (size_t)nc);
                    if (!ni) { oom = true; break; }
                    r->id = ni;
                    float *nx = realloc(r->x, sizeof(float) * 3 * (size_t)nc);
                    if (!nx) { oom = true; break; }
                    r->x = nx; cap = nc;
                }
                r->id[r->n] = id;
                for (int k = 0; k < 3; k++) r->x[3 * r->n + k] = frd_val(l, 13 + 12 * k);
                if (id > maxId) maxId = id;
                r->n++;
            } else if (blk == B_ELEMS) { elType = frd_int(l, 13, 5); en = 0; }
            else if (map && id >= 0 && id <= maxId && map[id] >= 0) {
                int i = map[id];
                if (blk == B_DISP && r->u) for (int k = 0; k < 3; k++) r->u[3 * i + k] = frd_val(l, 13 + 12 * k);
                else if (blk == B_FORC && r->rf) for (int k = 0; k < 3; k++) r->rf[3 * i + k] = frd_val(l, 13 + 12 * k);
                else if (blk == B_TEMP && r->T) r->T[i] = frd_val(l, 13);
                else if (blk == B_STRESS && r->vm) {
                    double sx = frd_val(l, 13), sy = frd_val(l, 25), sz = frd_val(l, 37), txy = frd_val(l, 49), tyz = frd_val(l, 61), tzx = frd_val(l, 73);
                    r->vm[i] = (float)sqrt(0.5 * ((sx - sy) * (sx - sy) + (sy - sz) * (sy - sz) + (sz - sx) * (sz - sx)) + 3 * (txy * txy + tyz * tyz + tzx * tzx));
                }
            }
        }
        else if (!strncmp(l, " -2", 3) && blk == B_ELEMS) {
            for (int k = 0; k < 10 && en < 20; k++) { int nid = frd_int(l, 3 + 10 * k, 10); if (!nid) break; enodes[en++] = nid; }
            if ((elType == 1 && en == 8) && map) {      /* an 8-node brick: its 12 edges */
                static const int E[12][2] = { {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7} };
                for (int k = 0; k < 12; k++) {
                    int a = enodes[E[k][0]], b = enodes[E[k][1]];
                    if (a < 0 || b < 0 || a > maxId || b > maxId || map[a] < 0 || map[b] < 0) continue;
                    if (r->nEdges == ecap) {
                        int nc = ecap ? ecap * 2 : 8192;
                        int *ne = realloc(r->edge, sizeof(int) * 2 * (size_t)nc);
                        if (!ne) { oom = true; break; }
                        r->edge = ne; ecap = nc;
                    }
                    r->edge[2 * r->nEdges] = map[a]; r->edge[2 * r->nEdges + 1] = map[b]; r->nEdges++;
                }
            }
        }
        l = e ? e + 1 : NULL;
    }
    free(t); free(map);
    if (!r->n || oom) { ccx_frd_free(r); return false; }
    r->uMaxAt = r->vmMaxAt = -1; r->tMin = 1e300; r->tMax = -1e300;
    for (int i = 0; i < r->n; i++) {
        if (r->u) { double m = sqrt(r->u[3*i]*r->u[3*i] + r->u[3*i+1]*r->u[3*i+1] + r->u[3*i+2]*r->u[3*i+2]); if (m > r->uMax) { r->uMax = m; r->uMaxAt = i; } }
        if (r->vm && r->vm[i] > r->vmMax) { r->vmMax = r->vm[i]; r->vmMaxAt = i; }
        /* the supports' reactions: RF at the nodes that don't move (CalculiX's RF holds the applied loads too, so over every node it sums to nothing) */
        if (r->rf && (!r->u || (r->u[3 * i] == 0 && r->u[3 * i + 1] == 0 && r->u[3 * i + 2] == 0))) for (int k = 0; k < 3; k++) r->rfSum[k] += r->rf[3 * i + k];
        if (r->T) { if (r->T[i] < r->tMin) r->tMin = r->T[i]; if (r->T[i] > r->tMax) r->tMax = r->T[i]; }
    }
    return true;
}

bool ccx_beam_check(const CcxModel *m, CcxBeam *b)
{
    memset(b, 0, sizeof *b);
    if (m->analysis != CCX_STATIC || m->nonlinear || m->nBc != 1 || m->nLoad != 1 || m->nMat < 1 || !m->mat[0].hasE || !m->nNodes) return false;
    if (!(m->bc[0].d0 == 1 && m->bc[0].d1 >= 3) || m->bc[0].hasValue) return false;
    int la = m->load[0].dof - 1;
    if (la < 0 || la > 2) return false;
    double ext[3]; for (int k = 0; k < 3; k++) ext[k] = m->hi[k] - m->lo[k];
    int ax = 0; for (int k = 1; k < 3; k++) if (ext[k] > ext[ax]) ax = k;
    if (la == ax) return false;
    int other = 3 - ax - la;
    double L = ext[ax], h = ext[la], w = ext[other];
    if (!(h > 0) || !(w > 0) || L < 5 * fmax(h, w)) return false;      /* slender */
    int nn = ccx_set_nodes(m, m->load[0].set);
    if (nn <= 0) return false;
    b->axis = ax; b->loadAxis = la;
    b->L = L; b->h = h; b->b = w; b->F = fabs(m->load[0].mag) * nn; b->E = m->mat[0].E;
    b->I = w * h * h * h / 12;
    b->delta = b->F * L * L * L / (3 * b->E * b->I);
    b->sigma = b->F * L * (h / 2) / b->I;
    return true;
}

/* ---------------------------------------------------------------- the run (C3) */
void ccx_run_load(const char *dir, const char *job, const char *logPath, CcxRun *r)
{
    memset(r, 0, sizeof *r); r->cpu = -1;
    char p[PL_PATH], f[300];
    snprintf(f, sizeof f, "%s.sta", job); pl_join(p, sizeof p, dir, f);
    char *t = pl_read_file(p, NULL);
    if (t) {
        r->hasSta = true;
        int lastStep = 0;
        for (char *l = t; l && *l; ) {
            char *e = strchr(l, '\n'); if (e) *e = 0;
            int st, inc, att, it; double tt, stt, it2;
            if (sscanf(l, "%d %d %d %d %lf %lf %lf", &st, &inc, &att, &it, &tt, &stt, &it2) == 7) {
                if (st != lastStep) { r->steps++; lastStep = st; }
                if (!r->nInc) { r->incFirst = r->incMin = r->incMax = it2; }
                r->nInc++; r->cutbacks += att > 1 ? att - 1 : 0;
                r->itersTotal += it; if (it > r->itersMax) r->itersMax = it;
                r->tot = tt; r->stepTime = stt; r->incLast = it2;
                if (it2 < r->incMin) r->incMin = it2;
                if (it2 > r->incMax) r->incMax = it2;
            }
            l = e ? e + 1 : NULL;
        }
        free(t);
    }
    snprintf(f, sizeof f, "%s.dat", job); pl_join(p, sizeof p, dir, f);
    if ((t = pl_read_file(p, NULL))) {
        char *q = strstr(t, "E I G E N V A L U E   O U T P U T");
        bool any = false;
        for (char *l = q; l && *l && r->nModes < CCX_MAX_MODES; ) {
            char *e = strchr(l, '\n'); if (e) *e = 0;
            int mode; double ev, rad, hz;
            if (sscanf(l, "%d %lf %lf %lf", &mode, &ev, &rad, &hz) == 4) { r->rad[r->nModes] = rad; r->hz[r->nModes++] = hz; if (ev < 0) r->nNeg++; any = true; }
            else if (any || strstr(l, "P A R T I C I P")) break;   /* the table's end */
            l = e ? e + 1 : NULL;
        }
        free(t);
    }
    if (logPath && (t = pl_read_file(logPath, NULL))) {
        char *q = strstr(t, "Total CalculiX Time:");
        if (q) r->cpu = atof(q + 20);
        free(t);
    }
}

double ccx_beam_f1(const CcxModel *m)
{
    if (m->nBc != 1 || !(m->bc[0].d0 == 1 && m->bc[0].d1 >= 3) || m->bc[0].hasValue || m->nMat < 1 || !m->mat[0].hasE || !m->mat[0].hasRho || !m->nNodes)
        return 0;
    double ext[3]; for (int k = 0; k < 3; k++) ext[k] = m->hi[k] - m->lo[k];
    int ax = 0; for (int k = 1; k < 3; k++) if (ext[k] > ext[ax]) ax = k;
    double a = ext[(ax + 1) % 3], b = ext[(ax + 2) % 3], L = ext[ax];
    if (!(a > 0) || !(b > 0) || L < 5 * fmax(a, b)) return 0;      /* slender */
    double h = fmin(a, b), w = fmax(a, b);                          /* the first mode bends the thin way */
    double I = w * h * h * h / 12, A = w * h;
    return 1.875 * 1.875 / (2 * M_PI) * sqrt(m->mat[0].E * I / (m->mat[0].rho * A * L * L * L * L));
}

/* ---------------------------------------------------------------- the mesh (C4) */
typedef struct { char name[64]; int *id; int n, cap; } MSet;
typedef struct {
    int *nid; float *nx; int nn, ncap;
    int *e; int ne, ecap;                    /* edges as node id pairs */
    MSet set[128]; int nSets;
    int nElems;
    bool oom;                                /* memory ran out: the mesh isn't shown */
} MRead;
static void push_int(MRead *r, int **a, int *n, int *cap, int v)
{
    if (r->oom) return;
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 1024;
        int *na = realloc(*a, sizeof(int) * (size_t)nc);
        if (!na) { r->oom = true; return; }
        *a = na; *cap = nc;
    }
    (*a)[(*n)++] = v;
}
static MSet *mset(MRead *r, const char *name)
{
    for (int i = 0; i < r->nSets; i++) if (!strcasecmp(r->set[i].name, name)) return &r->set[i];
    if (r->nSets >= 128) return NULL;
    MSet *s = &r->set[r->nSets++]; memset(s, 0, sizeof *s); pl_strlcpy(s->name, name, sizeof s->name);
    return s;
}
/* nodes in an element of this type, and its corner edges (pairs of positions) */
static int el_shape(const char *type, const int **edges, int *nEdges)
{
    static const int HEX[] = { 0,1, 1,2, 2,3, 3,0, 4,5, 5,6, 6,7, 7,4, 0,4, 1,5, 2,6, 3,7 };
    static const int TET[] = { 0,1, 1,2, 2,0, 0,3, 1,3, 2,3 };
    static const int WED[] = { 0,1, 1,2, 2,0, 3,4, 4,5, 5,3, 0,3, 1,4, 2,5 };
    static const int QUA[] = { 0,1, 1,2, 2,3, 3,0 };
    static const int TRI[] = { 0,1, 1,2, 2,0 };
    static const int LIN[] = { 0,1 };
    static const int LIN3[] = { 0,2 };
    char t[16]; int k = 0; for (; type[k] && k < 15; k++) t[k] = (char)toupper((unsigned char)type[k]); t[k] = 0;
    #define SH(n_, E_) do { *edges = E_; *nEdges = (int)(sizeof E_ / sizeof E_[0] / 2); return n_; } while (0)
    if (!strncmp(t, "C3D20", 5)) SH(20, HEX);
    if (!strncmp(t, "C3D8", 4)) SH(8, HEX);
    if (!strncmp(t, "C3D10", 5)) SH(10, TET);
    if (!strncmp(t, "C3D4", 4)) SH(4, TET);
    if (!strncmp(t, "C3D15", 5)) SH(15, WED);
    if (!strncmp(t, "C3D6", 4)) SH(6, WED);
    if (!strncmp(t, "S8", 2) || !strncmp(t, "CPS8", 4) || !strncmp(t, "CPE8", 4) || !strncmp(t, "CAX8", 4)) SH(8, QUA);
    if (!strncmp(t, "S4", 2) || !strncmp(t, "CPS4", 4) || !strncmp(t, "CPE4", 4) || !strncmp(t, "CAX4", 4)) SH(4, QUA);
    if (!strncmp(t, "S6", 2) || !strncmp(t, "CPS6", 4) || !strncmp(t, "CPE6", 4) || !strncmp(t, "CAX6", 4)) SH(6, TRI);
    if (!strncmp(t, "S3", 2) || !strncmp(t, "CPS3", 4) || !strncmp(t, "CPE3", 4) || !strncmp(t, "CAX3", 4)) SH(3, TRI);
    if (!strcmp(t, "B32") || !strcmp(t, "B32R") || !strcmp(t, "T3D3")) SH(3, LIN3);
    if (!strncmp(t, "B31", 3) || !strcmp(t, "T3D2")) SH(2, LIN);
    #undef SH
    *edges = NULL; *nEdges = 0; return 0;
}
static void mesh_file(const char *dir, const char *rel, MRead *r, int depth)
{
    if (depth > 4) return;
    char p[PL_PATH]; pl_join(p, sizeof p, dir, rel);
    char *t = pl_read_file(p, NULL);
    if (!t) return;
    enum { M_NONE, M_NODE, M_EL, M_NSET } mode = M_NONE;
    MSet *cur = NULL; bool gen = false;
    int need = 0, nEd = 0; const int *ed = NULL;
    int conn[32], nc = 0;
    for (char *l = t; l && *l; ) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        char line[1024]; pl_strlcpy(line, l, sizeof line); trim(line);
        l = e ? e + 1 : NULL;
        if (!line[0] || (line[0] == '*' && line[1] == '*')) continue;
        if (line[0] == '*') {
            mode = M_NONE; cur = NULL; gen = false; nc = 0;
            char kw[64]; int k = 0;
            for (const char *q = line; *q && *q != ',' && k < 63; q++) kw[k++] = (char)toupper((unsigned char)*q);
            kw[k] = 0; trim(kw);
            char v[256];
            if (!strcmp(kw, "*INCLUDE") && kw_param(line, "INPUT", v, sizeof v)) mesh_file(dir, v, r, depth + 1);
            else if (!strcmp(kw, "*NODE")) { mode = M_NODE; if (kw_param(line, "NSET", v, sizeof v)) cur = mset(r, v); }
            else if (!strcmp(kw, "*ELEMENT")) { mode = M_EL; need = kw_param(line, "TYPE", v, sizeof v) ? el_shape(v, &ed, &nEd) : 0; }
            else if (!strcmp(kw, "*NSET")) { mode = M_NSET; gen = kw_flag(line, "GENERATE"); if (kw_param(line, "NSET", v, sizeof v)) cur = mset(r, v); }
            continue;
        }
        char buf[1024]; pl_strlcpy(buf, line, sizeof buf);
        char *f[40]; int n = split(buf, f, 40);
        if (!n) continue;
        if (mode == M_NODE && n >= 3) {
            if (r->oom) break;
            if (r->nn == r->ncap) {
                int nc2 = r->ncap ? r->ncap * 2 : 4096;
                int *ni = realloc(r->nid, sizeof(int) * (size_t)nc2);
                if (!ni) { r->oom = true; break; }
                r->nid = ni;
                float *nx = realloc(r->nx, sizeof(float) * 3 * (size_t)nc2);
                if (!nx) { r->oom = true; break; }
                r->nx = nx; r->ncap = nc2;
            }
            r->nid[r->nn] = atoi(f[0]);
            for (int k = 0; k < 3; k++) r->nx[3 * r->nn + k] = n > 1 + k ? (float)atof(f[1 + k]) : 0;
            if (cur) push_int(r, &cur->id, &cur->n, &cur->cap, r->nid[r->nn]);
            r->nn++;
        } else if (mode == M_EL && need) {
            for (int i = 0; i < n && nc < 32; i++) conn[nc++] = atoi(f[i]);   /* the id, then the nodes; may run onto the next line */
            if (nc >= need + 1) {
                for (int k = 0; k < nEd; k++) {
                    int a = conn[1 + ed[2 * k]], b = conn[1 + ed[2 * k + 1]];
                    push_int(r, &r->e, &r->ne, &r->ecap, a < b ? a : b); push_int(r, &r->e, &r->ne, &r->ecap, a < b ? b : a);
                }
                r->nElems++; nc = 0;
            }
        } else if (mode == M_NSET && cur) {
            if (gen && n >= 2) { int a = atoi(f[0]), b = atoi(f[1]), s = n > 2 ? atoi(f[2]) : 1; if (s <= 0) s = 1;
                          if ((double)b - a > 1e7 * s) continue;     /* a damaged range: ten million ids at most */
                          for (int v = a; v <= b && !r->oom; v += s) push_int(r, &cur->id, &cur->n, &cur->cap, v); }
            else for (int i = 0; i < n; i++) {
                if (isdigit((unsigned char)f[i][0])) push_int(r, &cur->id, &cur->n, &cur->cap, atoi(f[i]));
                else { MSet *o = NULL; for (int k = 0; k < r->nSets; k++) if (!strcasecmp(r->set[k].name, f[i])) o = &r->set[k];
                       if (o && o != cur) for (int k = 0; k < o->n; k++) push_int(r, &cur->id, &cur->n, &cur->cap, o->id[k]); }
            }
        }
    }
    free(t);
}
static int cmp_edge(const void *a, const void *b)
{
    const int *x = a, *y = b;
    return x[0] != y[0] ? (x[0] > y[0]) - (x[0] < y[0]) : (x[1] > y[1]) - (x[1] < y[1]);
}
void ccx_mesh_free(CcxMesh *g) { free(g->id); free(g->x); free(g->role); free(g->edge); memset(g, 0, sizeof *g); }
bool ccx_mesh_load(const char *dir, const char *input, const CcxModel *m, CcxMesh *g)
{
    memset(g, 0, sizeof *g);
    MRead r; memset(&r, 0, sizeof r);
    mesh_file(dir, input, &r, 0);
    bool ok = r.nn > 0 && !r.oom;
    int maxId = 0; for (int i = 0; ok && i < r.nn; i++) if (r.nid[i] > maxId) maxId = r.nid[i];
    int *map = ok ? malloc(sizeof(int) * (size_t)(maxId + 1)) : NULL;
    if (ok && !map) ok = false;
    if (ok) {
        for (int i = 0; i <= maxId; i++) map[i] = -1;
        for (int i = 0; i < r.nn; i++) if (r.nid[i] >= 0) map[r.nid[i]] = i;
        g->n = r.nn; g->id = r.nid; g->x = r.nx; r.nid = NULL; r.nx = NULL;
        g->role = calloc((size_t)g->n, 1);
        g->nElems = r.nElems;
        if (r.ne) g->edge = malloc(sizeof(int) * (size_t)r.ne);
        if (!g->role || (r.ne && !g->edge)) { free(map); map = NULL; ccx_mesh_free(g); ok = false; }
        if (ok && r.ne) {                             /* edges shared by neighbouring elements, once */
            qsort(r.e, (size_t)r.ne / 2, sizeof(int) * 2, cmp_edge);
            for (int i = 0; i < r.ne; i += 2) {
                if (i && r.e[i] == r.e[i - 2] && r.e[i + 1] == r.e[i - 1]) continue;
                int a = r.e[i], b = r.e[i + 1];
                if (a < 0 || b < 0 || a > maxId || b > maxId || map[a] < 0 || map[b] < 0) continue;
                g->edge[2 * g->nEdges] = map[a]; g->edge[2 * g->nEdges + 1] = map[b]; g->nEdges++;
            }
        }
        for (int pass = 0; pass < 2 && m && ok; pass++) {   /* the supports' and the point loads' nodes */
            int nb = pass ? m->nLoad : m->nBc;
            for (int b = 0; b < nb; b++) {
                const char *sn = pass ? m->load[b].set : m->bc[b].set;
                unsigned char bit = 16;
                if (!pass) {
                    bit = 0;
                    for (int dof = m->bc[b].d0; dof <= m->bc[b].d1 && dof <= 11; dof++) bit |= dof >= 1 && dof <= 3 ? (unsigned char)(1 << (dof - 1)) : dof == 11 ? 8 : 0;
                    if (!bit) continue;               /* rotations only */
                }
                if (isdigit((unsigned char)sn[0])) { int id = atoi(sn); if (id >= 0 && id <= maxId && map[id] >= 0) g->role[map[id]] |= bit; continue; }
                for (int k = 0; k < r.nSets; k++)
                    if (!strcasecmp(r.set[k].name, sn))
                        for (int q = 0; q < r.set[k].n; q++) { int id = r.set[k].id[q]; if (id >= 0 && id <= maxId && map[id] >= 0) g->role[map[id]] |= bit; }
            }
        }
        free(map);
    }
    free(r.nid); free(r.nx); free(r.e);
    for (int i = 0; i < r.nSets; i++) free(r.set[i].id);
    return ok;
}

static int cmp_float(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
int ccx_locking_risk(const CcxModel *m, const CcxMesh *g)
{
    if (strcasecmp(m->elType, "C3D8") || !m->nNodes) return 0;
    if (m->analysis != CCX_STATIC && m->analysis != CCX_FREQUENCY && m->analysis != CCX_DYNAMIC) return 0;
    double ext[3]; for (int k = 0; k < 3; k++) ext[k] = m->hi[k] - m->lo[k];
    int thin = 0; for (int k = 1; k < 3; k++) if (ext[k] < ext[thin]) thin = k;
    double mx = fmax(ext[0], fmax(ext[1], ext[2]));
    if (!(ext[thin] > 0) || mx < 4 * ext[thin]) return 0;            /* not slender or thin: it doesn't bend much */
    if (!g || !g->n) return -1;
    float *v = malloc(sizeof(float) * (size_t)g->n);                  /* node levels through the thin side */
    if (!v) return -1;
    for (int i = 0; i < g->n; i++) v[i] = g->x[3 * i + thin];
    qsort(v, (size_t)g->n, sizeof(float), cmp_float);
    int levels = 1; float tol = (float)(1e-6 * ext[thin]);
    for (int i = 1; i < g->n; i++) if (v[i] - v[i - 1] > tol) levels++;
    free(v);
    return levels - 1 <= 3 ? levels - 1 : 0;
}

int ccx_set_element_type(const char *dir, const char *input, const char *from, const char *to)
{
    int changed = 0;
    const char *files[16]; char names[16][256]; int nf = 0;
    pl_strlcpy(names[nf], input, sizeof names[0]); files[nf] = names[nf]; nf++;
    for (int fi = 0; fi < nf; fi++) {
        char p[PL_PATH]; pl_join(p, sizeof p, dir, files[fi]);
        char *t = pl_read_file(p, NULL);
        if (!t) continue;
        size_t cap = strlen(t) + 4096, o = 0; char *out = malloc(cap); int here = 0;
        bool bad = !out;                                   /* memory ran out: this file is left as it was */
        for (char *l = t; l && *l && !bad; ) {
            char *e = strchr(l, '\n'); size_t len = e ? (size_t)(e - l) : strlen(l);
            char line[1024]; snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), l);
            char up[1024]; size_t k = 0; for (; line[k] && k < sizeof up - 1; k++) up[k] = (char)toupper((unsigned char)line[k]); up[k] = 0;
            char v[256];
            if (!strncmp(up, "*INCLUDE", 8) && kw_param(line, "INPUT", v, sizeof v) && nf < 16) { pl_strlcpy(names[nf], v, sizeof names[0]); files[nf] = names[nf]; nf++; }
            if (!strncmp(up, "*ELEMENT", 8) && kw_param(line, "TYPE", v, sizeof v) && !strcasecmp(v, from)) {
                char *at = strstr(up, "TYPE"); size_t pos = (size_t)(at - up);
                char *eq = strchr(line + pos, '='); size_t vs = (size_t)(eq - line) + 1;
                while (line[vs] == ' ') vs++;
                size_t ve = vs; while (line[ve] && line[ve] != ',' && line[ve] != ' ') ve++;
                char nl[1100]; snprintf(nl, sizeof nl, "%.*s%s%s", (int)vs, line, to, line + ve);
                size_t nlen = strlen(nl);
                if (o + nlen + 2 > cap) { size_t nc = (o + nlen) * 2 + 16; char *no = realloc(out, nc); if (!no) { bad = true; break; } out = no; cap = nc; }
                memcpy(out + o, nl, nlen); o += nlen; here++;
            } else {
                if (o + len + 2 > cap) { size_t nc = (o + len) * 2 + 16; char *no = realloc(out, nc); if (!no) { bad = true; break; } out = no; cap = nc; }
                memcpy(out + o, l, len); o += len;
            }
            if (e) out[o++] = '\n';
            l = e ? e + 1 : NULL;
        }
        if (here && !bad && pl_write_file(p, out, o)) changed += here;
        free(out); free(t);
    }
    return changed;
}
