/* monitor.c -- function-object output as time series (see monitor.h) */
#define _GNU_SOURCE
#include "monitor.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ============================================================================ lines */
/* the groups of a line: tab-separated when there are tabs (OpenFOAM's writeFile), else whitespace-separated with
   "(a b c)" kept together (probes) */
static int split_groups(const char *line, char g[][96], int max)
{
    int n = 0;
    bool tabs = strchr(line, '\t') != NULL;
    const char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int k = 0;
        if (tabs) {
            while (*p && *p != '\t' && k < 95) g[n][k++] = *p++;
            while (k > 0 && g[n][k - 1] == ' ') k--;
        } else {
            int depth = 0;
            while (*p && k < 95 && (depth > 0 || (*p != ' ' && *p != '\t'))) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                g[n][k++] = *p++;
            }
        }
        g[n][k] = 0;
        if (k) n++;
        if (tabs) while (*p && *p != '\t') p++;
    }
    return n;
}

static bool is_number(const char *s, double *v)
{
    char *e;
    *v = strtod(s, &e);
    return e != s && (*e == 0 || isspace((unsigned char)*e));
}

static int col_index(MonFile *f, const char *name)
{
    for (int i = 0; i < f->nCols; i++) if (!strcmp(f->col[i], name)) return i;
    if (f->nCols >= MON_MAX_COLS) return -1;
    int c = f->nCols++;
    pl_strlcpy(f->col[c], name, sizeof f->col[c]);
    f->v[c] = malloc(sizeof(float) * (size_t)(f->cap ? f->cap : 1));
    for (int i = 0; i < f->n; i++) f->v[c][i] = NAN;           /* rows before the column appeared */
    return c;
}

static void push_row(MonFile *f, double t)
{
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 1024;
        f->t = realloc(f->t, sizeof(float) * (size_t)f->cap);
        for (int c = 0; c < f->nCols; c++) f->v[c] = realloc(f->v[c], sizeof(float) * (size_t)f->cap);
    }
    f->t[f->n] = (float)t;
    for (int c = 0; c < f->nCols; c++) f->v[c][f->n] = NAN;
    f->n++;
}

static void set_cell(MonFile *f, const char *name, double v)
{
    int c = col_index(f, name);
    if (c < 0 || f->n == 0) return;
    f->v[c][f->n - 1] = (float)v;
}

static void parse_line(MonFile *f, char *line)
{
    while (*line == ' ') line++;
    if (!*line) return;
    if (line[0] == '#') {
        const char *h = line + 1;
        while (*h == ' ' || *h == '\t') h++;
        if (pl_starts_with(h, "Probe ") && f->nProbes < MON_MAX_COLS) { f->nProbes++; return; }
        if (pl_starts_with(h, "Time")) pl_strlcpy(f->lastHeader, h, sizeof f->lastHeader);
        return;
    }
    static char g[MON_MAX_COLS + 8][96];
    int n = split_groups(line, g, MON_MAX_COLS + 8);
    double t;
    if (n < 2 || !is_number(g[0], &t)) return;
    static char h[MON_MAX_COLS + 8][96];
    int nh = f->lastHeader[0] ? split_groups(f->lastHeader, h, MON_MAX_COLS + 8) : 0;   /* h[0] = "Time" */
    push_row(f, t);
    char prefix[64] = "";
    for (int i = 1; i < n; i++) {
        const char *hn = i < nh ? h[i] : NULL;
        char base[200];                                      /* a column label: clipped when it doesn't fit */
        if (f->nProbes > 0) pl_fmt_ui(base, sizeof base, "probe %s", hn ? hn : "?");
        else if (hn) pl_fmt_ui(base, sizeof base, "%s%s%s", prefix, prefix[0] ? " " : "", hn);
        else pl_fmt_ui(base, sizeof base, "%s%sc%d", prefix, prefix[0] ? " " : "", i);
        double v;
        if (g[i][0] == '(') {                                   /* a vector (or tensor): one column per component */
            const char *q = g[i] + 1;
            static const char *cn[] = { "x", "y", "z", "4", "5", "6", "7", "8", "9" };
            for (int k = 0; k < 9; k++) {
                char *e; v = strtod(q, &e);
                if (e == q) break;
                char nm[112]; snprintf(nm, sizeof nm, "%.90s_%s", base, cn[k]);
                set_cell(f, nm, v);
                q = e;
            }
        } else if (is_number(g[i], &v)) set_cell(f, base, v);
        else if (hn && !strcmp(hn, "field")) pl_strlcpy(prefix, g[i], sizeof prefix);   /* fieldMinMax: rows per field */
        /* other words (solver names, true/false) are not plotted */
    }
}

static void consume(MonFile *f, const char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\r') continue;
        if (c == '\n' || f->partialLen >= (int)sizeof f->partial - 1) {
            f->partial[f->partialLen] = 0;
            parse_line(f, f->partial);
            f->partialLen = 0;
            if (c == '\n') continue;
        }
        f->partial[f->partialLen++] = c;
    }
}

static void monfile_clear(MonFile *f)
{
    free(f->t);
    for (int c = 0; c < f->nCols; c++) free(f->v[c]);
    f->t = NULL; memset(f->v, 0, sizeof f->v);
    f->n = f->cap = f->nCols = f->nProbes = 0;
    f->partialLen = 0; f->partPos = 0; f->lastHeader[0] = 0;
    memset(f->off, 0, sizeof f->off); memset(f->ino, 0, sizeof f->ino);
}

static void monfile_read(MonFile *f)
{
    /* a part rewritten from the start (a run restarted at the same time): read everything again */
    for (int p = 0; p < f->nParts; p++) {
        struct stat st;
        if (stat(f->parts[p], &st) != 0) continue;
        if ((f->ino[p] && f->ino[p] != (unsigned long long)st.st_ino) || (long long)st.st_size < f->off[p]) { monfile_clear(f); break; }
    }
    int budget = 64;                                              /* <= 4 MB per poll */
    for (; f->partPos < f->nParts && budget > 0; ) {
        int p = f->partPos;
        struct stat st;
        if (stat(f->parts[p], &st) != 0) { f->partPos++; continue; }
        f->ino[p] = (unsigned long long)st.st_ino;
        if ((long long)st.st_size > f->off[p]) {
            FILE *fp = fopen(f->parts[p], "rb");
            if (!fp) break;
            if (fseek(fp, (long)f->off[p], SEEK_SET)) { fclose(fp); break; }
            char buf[65536];
            size_t r;
            while (budget-- > 0 && (r = fread(buf, 1, sizeof buf, fp)) > 0) { consume(f, buf, (int)r); f->off[p] += (long long)r; if (r < sizeof buf) break; }
            fclose(fp);
        }
        if (p < f->nParts - 1 && f->off[p] >= (long long)st.st_size) {
            if (f->partialLen) { f->partial[f->partialLen] = 0; parse_line(f, f->partial); f->partialLen = 0; }
            f->partPos++;                                         /* an older part is complete: on to the next */
        } else break;
    }
}

/* ============================================================================ discovery */
static int cmp_time(const void *a, const void *b)
{
    double x = atof(*(char *const *)a), y = atof(*(char *const *)b);
    return (x > y) - (x < y);
}

/* is this a time series written by a function object? It has a "# Time ..." header near the top */
static bool time_series(const char *path)
{
    long long sz = pl_file_size(path);
    if (sz <= 0 || sz > (512LL << 20)) return false;
    char *h = pl_read_head(path, 4096);
    if (!h) return false;
    bool ok = false;
    for (char *l = h; l && *l && !ok; ) {
        char *e = strchr(l, '\n');
        if (l[0] != '#') break;
        const char *q = l + 1;
        while (*q == ' ' || *q == '\t') q++;
        if (pl_starts_with(q, "Time")) ok = true;
        l = e ? e + 1 : NULL;
    }
    free(h);
    return ok;
}

/* the first time folder decides: a time series has its "# Time" file there from the start. Sampled sets and surfaces
   write a folder per output time (100 000 of them for damBreak's sampleSets), and opening every file of every folder
   each scan froze the window for seconds */
static bool not_series(MonSet *s, const char *td, const char *keyPrefix)
{
    for (int i = 0; i < s->n; i++) if (pl_starts_with(s->f[i].key, keyPrefix) && s->f[i].key[strlen(keyPrefix)] == '/') return false;
    NameList files = { 0 };
    pl_list_dir(td, &files, false, true);
    bool written = false, series = false;
    for (int k = 0; k < files.n && !series; k++) {
        char fp[PL_PATH]; pl_join(fp, sizeof fp, td, files.names[k]);
        if (pl_file_size(fp) > 0) { written = true; series = time_series(fp); }
    }
    names_free(&files);
    return written && !series;                                 /* nothing written yet: ask again next scan */
}

static bool skipped(const MonSet *s, const char *keyPrefix)
{
    for (int i = 0; i < s->nSkip; i++) if (!strcmp(s->skip[i], keyPrefix)) return true;
    return false;
}

static void add_series_dir(MonSet *s, const char *foDir, const char *keyPrefix)
{
    if (skipped(s, keyPrefix)) return;
    NameList times = { 0 };
    pl_list_dir(foDir, &times, true, false);
    int nt = 0;
    for (int i = 0; i < times.n; i++) if (pl_is_number(times.names[i])) times.names[nt++] = times.names[i]; else free(times.names[i]);
    times.n = nt;
    if (times.n > 1) qsort(times.names, (size_t)times.n, sizeof(char *), cmp_time);
    if (times.n > 0) {
        char td[PL_PATH]; pl_join(td, sizeof td, foDir, times.names[0]);
        if (not_series(s, td, keyPrefix)) {
            if (s->nSkip < MON_MAX_SKIP) pl_strlcpy(s->skip[s->nSkip++], keyPrefix, sizeof s->skip[0]);
            names_free(&times);
            return;
        }
    }
    for (int ti = 0; ti < times.n; ti++) {
        char td[PL_PATH]; pl_join(td, sizeof td, foDir, times.names[ti]);
        NameList files = { 0 };
        pl_list_dir(td, &files, false, true);
        for (int k = 0; k < files.n; k++) {
            char fp[PL_PATH]; pl_join(fp, sizeof fp, td, files.names[k]);
            char key[256]; snprintf(key, sizeof key, "%s/%s", keyPrefix, files.names[k]);
            int idx = monset_find(s, key);
            if (idx < 0) {
                if (s->n >= MON_MAX_FILES || !time_series(fp)) continue;
                MonFile *f = &s->f[s->n++];
                memset(f, 0, sizeof *f);
                pl_strlcpy(f->key, key, sizeof f->key);
                snprintf(f->label, sizeof f->label, "%s · %s", keyPrefix, files.names[k]);
                idx = s->n - 1;
            }
            MonFile *f = &s->f[idx];
            bool have = false;
            for (int p = 0; p < f->nParts; p++) if (!strcmp(f->parts[p], fp)) have = true;
            if (!have && f->nParts < MON_MAX_PARTS) pl_strlcpy(f->parts[f->nParts++], fp, sizeof f->parts[0]);
        }
        names_free(&files);
    }
    names_free(&times);
}

static void scan(MonSet *s)
{
    char pp[PL_PATH]; pl_join(pp, sizeof pp, s->casePath, "postProcessing");
    NameList l1 = { 0 };
    pl_list_dir(pp, &l1, true, false);
    for (int i = 0; i < l1.n; i++) {
        if (skipped(s, l1.names[i])) continue;                 /* before listing it: that alone took 0.3 s for 100 000 folders */
        char d1[PL_PATH]; pl_join(d1, sizeof d1, pp, l1.names[i]);
        NameList l2 = { 0 };
        pl_list_dir(d1, &l2, true, false);
        bool hasTimes = false;
        for (int k = 0; k < l2.n; k++) if (pl_is_number(l2.names[k])) hasTimes = true;
        if (hasTimes) add_series_dir(s, d1, l1.names[i]);
        else for (int k = 0; k < l2.n; k++) {                 /* multi-region: postProcessing/<region>/<name>/<time> */
            char d2[PL_PATH], kp[200]; pl_join(d2, sizeof d2, d1, l2.names[k]);
            snprintf(kp, sizeof kp, "%s/%s", l1.names[i], l2.names[k]);
            add_series_dir(s, d2, kp);
        }
        names_free(&l2);
    }
    names_free(&l1);
}

/* ============================================================================ set */
void monset_close(MonSet *s)
{
    for (int i = 0; i < s->n; i++) monfile_clear(&s->f[i]);
    memset(s, 0, sizeof *s);
}

void monset_open(MonSet *s, const char *casePath)
{
    monset_close(s);
    pl_strlcpy(s->casePath, casePath, sizeof s->casePath);
    monset_poll(s, 0);
}

int monset_find(const MonSet *s, const char *key)
{
    for (int i = 0; i < s->n; i++) if (!strcmp(s->f[i].key, key)) return i;
    return -1;
}

void monset_poll(MonSet *s, double now)
{
    if (!s->casePath[0]) return;
    if (now == 0 || now - s->lastScan > 3.0) {
        s->lastScan = now;
        /* a cleaned case: postProcessing gone, start over */
        char pp[PL_PATH]; pl_join(pp, sizeof pp, s->casePath, "postProcessing");
        if (!pl_is_dir(pp) && (s->n || s->nSkip)) { char c[PL_PATH]; pl_strlcpy(c, s->casePath, sizeof c); monset_close(s); pl_strlcpy(s->casePath, c, sizeof s->casePath); }
        scan(s);
    }
    for (int i = 0; i < s->n; i++) monfile_read(&s->f[i]);
}

bool mon_converged(const MonFile *f, int c, int window, double tol, double *change)
{
    if (change) *change = -1;
    if (c < 0 || c >= f->nCols || window < 2) return false;
    double lo = 1e300, hi = -1e300, sum = 0; int k = 0;
    for (int i = f->n - 1; i >= 0 && k < window; i--) {
        float v = f->v[c][i];
        if (isnan(v)) continue;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        sum += v; k++;
    }
    if (k < window) return false;
    double mean = sum / k;
    double rel = fabs(mean) > 1e-300 ? (hi - lo) / fabs(mean) : (hi - lo);
    if (change) *change = rel;
    return rel < tol;
}
