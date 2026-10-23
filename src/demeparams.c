/* demeparams.c -- see demeparams.h. params.ini is edited line by line, so comments, blank lines and order survive a
 * save; the progress parser reads only what the log gained since its last look. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "demeparams.h"
#include "pl_util.h"

/* ============================================================================ params.ini */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

/* "# Label | unit | help" into the three fields (missing ones left empty) */
static void describe(DemeParam *p, const char *comment)
{
    char b[512]; pl_strlcpy(b, comment, sizeof b);
    char *f[3] = { b, NULL, NULL };
    for (int i = 1; i < 3; i++) { char *bar = f[i - 1] ? strchr(f[i - 1], '|') : NULL; if (bar) { *bar = 0; f[i] = bar + 1; } }
    pl_strlcpy(p->label, f[0] ? trim(f[0]) : "", sizeof p->label);
    pl_strlcpy(p->unit, f[1] ? trim(f[1]) : "", sizeof p->unit);
    pl_strlcpy(p->help, f[2] ? trim(f[2]) : "", sizeof p->help);
}

void deme_params_free(DemeParams *d)
{
    for (int i = 0; i < d->nLines; i++) free(d->lines[i]);
    free(d->lines);
    free(d->p);
    memset(d, 0, sizeof *d);
}

bool deme_params_load(DemeParams *d, const char *path)
{
    deme_params_free(d);
    pl_strlcpy(d->path, path, sizeof d->path);
    size_t len = 0;
    char *t = pl_read_file(path, &len);
    if (!t) return false;
    d->stamp = deme_params_stamp(path);
    int cap = 64; d->lines = calloc((size_t)cap, sizeof(char *));
    for (const char *q = t; *q; ) {
        const char *nl = strchr(q, '\n');
        size_t n = nl ? (size_t)(nl - q) : strlen(q);
        if (d->nLines == cap) { cap *= 2; d->lines = realloc(d->lines, sizeof(char *) * (size_t)cap); }
        d->lines[d->nLines++] = pl_strndup(q, n);
        q = nl ? nl + 1 : q + n;
    }
    free(t);
    d->p = calloc((size_t)(d->nLines ? d->nLines : 1), sizeof *d->p);
    char section[48] = "", comment[512] = "";
    for (int i = 0; i < d->nLines; i++) {
        char b[1024]; pl_strlcpy(b, d->lines[i], sizeof b);
        char *s = trim(b);
        if (!*s) { comment[0] = 0; continue; }
        if (*s == '#') { pl_strlcpy(comment, trim(s + 1), sizeof comment); continue; }
        if (*s == '[') { char *e = strchr(s, ']'); if (e) *e = 0; pl_strlcpy(section, trim(s + 1), sizeof section); comment[0] = 0; continue; }
        char *eq = strchr(s, '=');
        if (!eq) { comment[0] = 0; continue; }
        *eq = 0;
        char *v = eq + 1, *h = strchr(v, '#');
        if (h) *h = 0;
        DemeParam *p = &d->p[d->n++];
        pl_strlcpy(p->section, section, sizeof p->section);
        pl_strlcpy(p->key, trim(s), sizeof p->key);
        pl_strlcpy(p->value, trim(v), sizeof p->value);
        p->line = i;
        if (comment[0]) describe(p, comment); else pl_strlcpy(p->label, p->key, sizeof p->label);
        comment[0] = 0;
    }
    d->dirty = false;
    return true;
}

bool deme_params_save(DemeParams *d)
{
    size_t len = 0;
    for (int i = 0; i < d->nLines; i++) len += strlen(d->lines[i]) + 1;
    char *b = malloc(len + 1), *w = b;
    for (int i = 0; i < d->nLines; i++) { size_t n = strlen(d->lines[i]); memcpy(w, d->lines[i], n); w += n; *w++ = '\n'; }
    bool ok = pl_write_file(d->path, b, (size_t)(w - b));
    free(b);
    if (ok) { d->dirty = false; d->stamp = deme_params_stamp(d->path); }
    return ok;
}

long long deme_params_stamp(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec + (long long)st.st_size * 7919;
}

DemeParam *deme_param(DemeParams *d, const char *key)
{
    for (int i = 0; i < d->n; i++) if (!strcmp(d->p[i].key, key)) return &d->p[i];
    return NULL;
}

double deme_param_num(const DemeParams *d, const char *key, double def)
{
    for (int i = 0; i < d->n; i++) if (!strcmp(d->p[i].key, key)) {
        char *end = NULL;
        double v = strtod(d->p[i].value, &end);
        return end && end != d->p[i].value ? v : def;
    }
    return def;
}

bool deme_param_set(DemeParams *d, const char *key, const char *value)
{
    DemeParam *p = deme_param(d, key);
    if (!p) return false;
    if (!strcmp(p->value, value)) return true;
    pl_strlcpy(p->value, value, sizeof p->value);
    /* rewrite the line: its indent and a trailing comment stay */
    const char *old = d->lines[p->line], *eq = strchr(old, '='), *h = eq ? strchr(eq, '#') : NULL;
    size_t cap = strlen(old) + strlen(key) + strlen(value) + 8;
    char *nl = malloc(cap);
    if (!eq) snprintf(nl, cap, "%s = %s", key, value);
    else snprintf(nl, cap, "%.*s %s%s%s", (int)(eq - old) + 1, old, value, h ? "  " : "", h ? h : "");
    free(d->lines[p->line]);
    d->lines[p->line] = nl;
    d->dirty = true;
    return true;
}

bool deme_param_set_num(DemeParams *d, const char *key, double v)
{
    char b[48]; snprintf(b, sizeof b, "%.6g", v);
    return deme_param_set(d, key, b);
}

double deme_rayleigh_dt(double r, double rho, double E, double nu)
{
    if (r <= 0 || rho <= 0 || E <= 0) return 0;
    double G = E / (2 * (1 + nu));
    return M_PI * r * sqrt(rho / G) / (0.1631 * nu + 0.8766);
}

/* starting values, not calibrated ones: tune friction and rolling friction against an angle of repose */
const DemePreset DEME_GRAVITY[] = {
    { "Earth", "9.81 m/s²", { { "gravity", "9.81" } } },
    { "Moon", "1.62 m/s²", { { "gravity", "1.62" } } },
    { "Mars", "3.71 m/s²", { { "gravity", "3.71" } } },
};
const int DEME_N_GRAVITY = 3;
const DemePreset DEME_MATERIALS[] = {
    { "Glass beads", "smooth, bouncy", { { "density", "2500" }, { "youngs_modulus", "1e8" }, { "poisson_ratio", "0.25" },
                                         { "restitution", "0.9" }, { "friction", "0.3" }, { "rolling_friction", "0.01" } } },
    { "Quartz sand", "angular, dry", { { "density", "2650" }, { "youngs_modulus", "1e8" }, { "poisson_ratio", "0.3" },
                                       { "restitution", "0.5" }, { "friction", "0.6" }, { "rolling_friction", "0.2" } } },
    { "Lunar regolith", "very angular, grain density ~3.1 g/cm³", { { "density", "3100" }, { "youngs_modulus", "1e8" },
                                       { "poisson_ratio", "0.3" }, { "restitution", "0.3" }, { "friction", "0.7" }, { "rolling_friction", "0.3" } } },
    { "Water ice", "grains near 100 K", { { "density", "917" }, { "youngs_modulus", "1e8" }, { "poisson_ratio", "0.33" },
                                          { "restitution", "0.4" }, { "friction", "0.25" }, { "rolling_friction", "0.05" } } },
    { "Steel shot", "round, heavy", { { "density", "7800" }, { "youngs_modulus", "1e9" }, { "poisson_ratio", "0.3" },
                                      { "restitution", "0.8" }, { "friction", "0.3" }, { "rolling_friction", "0.01" } } },
};
const int DEME_N_MATERIALS = 5;

int deme_apply_preset(DemeParams *d, const DemePreset *p)
{
    int n = 0;
    for (int i = 0; i < 8 && p->kv[i][0]; i++) n += deme_param_set(d, p->kv[i][0], p->kv[i][1]);
    return n;
}

/* ============================================================================ progress */
void deme_progress_reset(DemeProgress *p) { memset(p, 0, sizeof *p); }

static bool field(const char *line, const char *key, double *out)
{
    const char *k = strstr(line, key);
    if (!k) return false;
    char *end = NULL;
    double v = strtod(k + strlen(key), &end);
    if (end == k + strlen(key)) return false;
    *out = v;
    return true;
}

const char *deme_crash_text(int crash)
{
    switch (crash) {
    case DEME_CRASH_FORCES: return "The contact forces blew up and DEME stopped. Usually the time step is too big for the stiffest "
                                   "contact (see the Rayleigh check in Setup: Use 20 %), or a particle moved faster than Largest particle "
                                   "speed. Lower the time step, raise that speed, or soften Young's modulus.";
    case DEME_CRASH_MEMORY: return "The GPU ran out of memory: fewer particles (Most particles), larger grains or a smaller domain.";
    case DEME_CRASH_NO_PARAMS: return "The driver found no params.ini and ran on its built-in defaults.";
    default: return "";
    }
}

void deme_progress_line(DemeProgress *p, const char *line, double now)
{
    if (strstr(line, "invalid force") || strstr(line, "unspecified launch failure") || strstr(line, "illegal memory access")) {
        if (!p->crash) p->crash = DEME_CRASH_FORCES;
        return;
    }
    if (strstr(line, "out of memory") || strstr(line, "cudaErrorMemoryAllocation")) { p->crash = DEME_CRASH_MEMORY; return; }
    if (!strncmp(line, "Parameters: none found", 22)) { p->crash = DEME_CRASH_NO_PARAMS; return; }
    if (strncmp(line, "QC ", 3)) {
        /* the old starter's "Frame i of n, t = x s" */
        int a, b; double t;
        if (sscanf(line, "Frame %d of %d, t = %lf", &a, &b, &t) == 3) { p->frame = a; p->frames = b; p->t = t; p->any = true; }
        return;
    }
    const char *s = line + 3;
    if (!strncmp(s, "result ", 7)) {
        char name[48] = "", val[48] = "";
        const char *eq = strchr(s + 7, '=');
        if (!eq) return;
        snprintf(name, sizeof name, "%.*s", (int)(eq - (s + 7)) < 47 ? (int)(eq - (s + 7)) : 47, s + 7);
        pl_strlcpy(val, eq + 1, sizeof val);
        pl_trim_inplace(val);
        for (int i = 0; i < p->nRes; i++) if (!strcmp(p->resName[i], name)) { pl_strlcpy(p->resVal[i], val, sizeof p->resVal[i]); return; }
        if (p->nRes < DEME_RESULTS) { pl_strlcpy(p->resName[p->nRes], name, 48); pl_strlcpy(p->resVal[p->nRes], val, 48); p->nRes++; }
        return;
    }
    double t;
    if (strncmp(s, "t=", 2) || !field(s, "t=", &t)) { pl_strlcpy(p->extra, s, sizeof p->extra); pl_trim_inplace(p->extra); return; }
    double v;
    p->t = t; p->any = true;
    if (field(s, "end=", &v)) p->end = v;
    if (field(s, "ke=", &v)) p->ke = v;
    if (field(s, "vmax=", &v)) p->vmax = v;
    if (field(s, " n=", &v)) p->n = (long)v;
    const char *fr = strstr(s, "frame=");
    if (fr) sscanf(fr + 6, "%d/%d", &p->frame, &p->frames);
    if (p->wall0 == 0) { p->wall0 = now; p->t0 = t; }
    else if (now - p->wall0 > 0.5 && t > p->t0) p->rate = (t - p->t0) / (now - p->wall0);
    if (p->nS == DEME_SERIES) {                       /* full: keep every other sample */
        for (int i = 0; i < DEME_SERIES / 2; i++) { p->ts[i] = p->ts[2 * i]; p->kes[i] = p->kes[2 * i]; }
        p->nS = DEME_SERIES / 2;
    }
    p->ts[p->nS] = (float)t; p->kes[p->nS] = (float)p->ke; p->nS++;
}

void deme_progress_poll(DemeProgress *p, const char *logPath, double now)
{
    struct stat st;
    if (stat(logPath, &st) != 0) { if (p->offset) deme_progress_reset(p); return; }
    if (st.st_size < p->offset) deme_progress_reset(p);          /* a new run started the log again */
    if (st.st_size == p->offset) return;
    FILE *f = fopen(logPath, "r");
    if (!f) return;
    fseek(f, p->offset, SEEK_SET);
    char line[1024];
    long at = p->offset;
    /* a run's first look reads everything: those samples are history, not a rate */
    bool history = p->offset == 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        if (n && line[n - 1] != '\n' && !feof(f)) { at += (long)n; continue; }   /* an over-long line: skip it */
        if (n && line[n - 1] != '\n') break;                                  /* half-written: read it next time */
        at += (long)n;
        line[strcspn(line, "\r\n")] = 0;
        deme_progress_line(p, line, history ? 0 : now);
    }
    if (history) { p->wall0 = now; p->t0 = p->t; p->rate = 0; }
    p->offset = at;
    fclose(f);
}

/* ============================================================================ frames */
void deme_frame_free(DemeFrame *f) { free(f->x); free(f->y); free(f->z); free(f->r); free(f->v); memset(f, 0, sizeof *f); }

static void grow(DemeFrame *f, int need)
{
    if (need <= f->cap) return;
    int c = f->cap ? f->cap * 2 : 4096;
    while (c < need) c *= 2;
    f->x = realloc(f->x, sizeof(float) * (size_t)c); f->y = realloc(f->y, sizeof(float) * (size_t)c);
    f->z = realloc(f->z, sizeof(float) * (size_t)c); f->r = realloc(f->r, sizeof(float) * (size_t)c);
    f->v = realloc(f->v, sizeof(float) * (size_t)c);
    f->cap = c;
}

/* a SPARTA (or LAMMPS) dump: "ITEM: ATOMS id type x y z vx vy vz", one particle a line; no radius (drawn as points) */
static bool dump_load(DemeFrame *f, char *t)
{
    char *at = strstr(t, "ITEM: ATOMS");
    if (!at) return false;
    char *nl = strchr(at, '\n'); if (!nl) return false;
    *nl = 0;
    int col[6] = { -1, -1, -1, -1, -1, -1 }, nc = 0;
    static const char *const NAMES[6] = { "x", "y", "z", "vx", "vy", "vz" };
    for (char *c = strtok(at + 11, " \t\r"); c; c = strtok(NULL, " \t\r"), nc++)
        for (int k = 0; k < 6; k++) if (!strcmp(c, NAMES[k])) col[k] = nc;
    if (col[0] < 0 || col[1] < 0) return false;
    for (int k = 0; k < 3; k++) { f->lo[k] = 1e30f; f->hi[k] = -1e30f; }
    f->vmax = 0; f->rmin = 0; f->rmax = 0;
    for (char *s = nl + 1; *s; ) {
        char *e = strchr(s, '\n');
        if (e) *e = 0;
        double v[16] = { 0 }; int k = 0;
        for (char *q = s; k < 16; k++) { char *end; v[k] = strtod(q, &end); if (end == q) break; q = end; }
        if (k > col[1]) {
            grow(f, f->n + 1);
            int i = f->n++;
            f->x[i] = (float)v[col[0]]; f->y[i] = (float)v[col[1]]; f->z[i] = col[2] >= 0 && col[2] < k ? (float)v[col[2]] : 0;
            f->r[i] = 0;
            double sp = 0; for (int a = 3; a < 6; a++) if (col[a] >= 0 && col[a] < k) sp += v[col[a]] * v[col[a]];
            f->v[i] = (float)sqrt(sp);
            float p[3] = { f->x[i], f->y[i], f->z[i] };
            for (int a = 0; a < 3; a++) { if (p[a] < f->lo[a]) f->lo[a] = p[a]; if (p[a] > f->hi[a]) f->hi[a] = p[a]; }
            if (f->v[i] > f->vmax) f->vmax = f->v[i];
        }
        if (!e) break;
        s = e + 1;
    }
    if (!f->n) for (int k = 0; k < 3; k++) f->lo[k] = f->hi[k] = 0;
    return f->n > 0;
}

bool deme_frame_load(DemeFrame *f, const char *csv)
{
    f->n = 0;
    size_t len = 0;
    char *t = pl_read_file(csv, &len);
    if (!t) return false;
    if (!strncmp(t, "ITEM: TIMESTEP", 14)) { bool ok = dump_load(f, t); free(t); return ok; }
    char *s = t, *nl = strchr(s, '\n');
    if (!nl) { free(t); return false; }
    *nl = 0;
    /* the header names the columns */
    int col[5] = { -1, -1, -1, -1, -1 }, nc = 0;
    static const char *const NAMES[5] = { "X", "Y", "Z", "r", "absv" };
    for (char *c = strtok(s, ",\r"); c; c = strtok(NULL, ",\r"), nc++)
        for (int k = 0; k < 5; k++) if (!strcmp(trim(c), NAMES[k])) col[k] = nc;
    if (col[0] < 0 || col[1] < 0 || col[2] < 0) { free(t); return false; }
    for (int k = 0; k < 3; k++) { f->lo[k] = 1e30f; f->hi[k] = -1e30f; }
    f->vmax = 0; f->rmin = 1e30f; f->rmax = 0;
    for (s = nl + 1; *s; ) {
        char *e = strchr(s, '\n');
        if (e) *e = 0;
        double v[16] = { 0 }; int k = 0;
        for (char *q = s; k < 16; k++) {
            char *end = NULL;
            v[k] = strtod(q, &end);
            if (end == q) break;
            q = end;
            if (*q != ',') { k++; break; }
            q++;
        }
        if (k > col[2]) {
            grow(f, f->n + 1);
            int i = f->n++;
            f->x[i] = (float)v[col[0]]; f->y[i] = (float)v[col[1]]; f->z[i] = (float)v[col[2]];
            f->r[i] = col[3] >= 0 && col[3] < k ? (float)v[col[3]] : 0;
            f->v[i] = col[4] >= 0 && col[4] < k ? (float)v[col[4]] : 0;
            float p[3] = { f->x[i], f->y[i], f->z[i] };
            for (int a = 0; a < 3; a++) { if (p[a] < f->lo[a]) f->lo[a] = p[a]; if (p[a] > f->hi[a]) f->hi[a] = p[a]; }
            if (f->v[i] > f->vmax) f->vmax = f->v[i];
            if (f->r[i] > f->rmax) f->rmax = f->r[i];
            if (f->r[i] < f->rmin) f->rmin = f->r[i];
        }
        if (!e) break;
        s = e + 1;
    }
    free(t);
    if (!f->n) { f->rmin = 0; for (int k = 0; k < 3; k++) f->lo[k] = f->hi[k] = 0; }
    return f->n > 0;
}
