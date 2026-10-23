/* test_oracle.c -- the dictionary parser against OpenFOAM's own reading of the same files.
 *
 *   test_oracle <tutorials dir> <etc/bashrc> <scratch dir> [max files]
 *
 * foamDictionary prints each file as OpenFOAM reads it. Every value the parser finds must be in that output under the
 * same keyword path, with the same text once whitespace is normalised. Values holding $macros or #directives are
 * skipped (OpenFOAM expands them while reading), and entries only OpenFOAM has (pulled in by #include) are ignored:
 * the check is that what the app shows and edits is what OpenFOAM reads. A deterministic sample of max files (default
 * 400; 0 = all) keeps the run to a minute; all 9000 take about 15.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "foamdict.h"

typedef struct { char **v; int n, cap; } List;
static void add(List *l, const char *s) { if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 256; l->v = realloc(l->v, sizeof(char *) * (size_t)l->cap); } l->v[l->n++] = strdup(s); }
static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static bool has_header(const char *path)
{
    char *h = pl_read_head(path, 1024);
    bool ok = h && strstr(h, "FoamFile");
    free(h);
    return ok;
}

static void walk(const char *dir, List *out)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[PL_PATH]; pl_join(p, sizeof p, dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!strcmp(e->d_name, "polyMesh") || !strncmp(e->d_name, "processor", 9)) continue;
            walk(p, out);
        } else if (S_ISREG(st.st_mode) && st.st_size < (1 << 20) && has_header(p)) add(out, p);
    }
    closedir(d);
}

/* the same value? Token by token: OpenFOAM writes ( 1 2 ) where the file had (1 2), and 1e-07 or 0.1 for 1e-7 or 1e-1,
   so punctuation splits tokens and numbers compare as numbers */
static int tokens(const char *s, char tok[][96], int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        if (s[0] == '/' && s[1] == '/') { while (*s && *s != '\n') s++; continue; }       /* comments are not values */
        if (s[0] == '/' && s[1] == '*') { const char *e = strstr(s + 2, "*/"); s = e ? e + 2 : s + strlen(s); continue; }
        if (!*s) break;
        int k = 0;
        if (strchr("()[]{};/", *s)) tok[n][k++] = *s++;
        else while (*s && !strchr(" \t\n\r()[]{};/", *s) && k < 95) tok[n][k++] = *s++;
        tok[n][k] = 0;
        if (strcmp(tok[n], "\"\"")) n++;                    /* OpenFOAM writes an empty string as nothing */
    }
    return n;
}
static bool same_value(const char *a, const char *b)
{
    static char ta[4096][96], tb[4096][96];
    int na = tokens(a, ta, 4096), nb = tokens(b, tb, 4096);
    if (na != nb) return false;
    for (int i = 0; i < na; i++) {
        if (!strcmp(ta[i], tb[i])) continue;
        char *ea, *eb;
        double x = strtod(ta[i], &ea), y = strtod(tb[i], &eb);
        if (*ea || *eb || ea == ta[i] || eb == tb[i]) return false;
        if (x != y && fabs(x - y) > 5e-6 * fmax(fabs(x), fabs(y))) return false;   /* foamDictionary writes 6 significant digits */
    }
    return true;
}

static void norm(const char *s, char *out, int cap)          /* for the report only */
{
    int k = 0; bool sp = false;
    for (; *s && k < cap - 1; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') { if (k) sp = true; continue; }
        if (sp && k < cap - 1) { out[k++] = ' '; sp = false; }
        out[k++] = *s;
    }
    out[k] = 0;
}

static int nVals, nSkip, nMiss, nDiff, nFiles, nBad, nCode, nCond;

/* an entry a #remove in the same dictionary takes out: OpenFOAM no longer has it */
static bool removed(const FDFile *f, const FDEntry *d, const char *key)
{
    for (int i = 0; i < d->nChild; i++) {
        const FDEntry *e = &d->child[i];
        if (e->kind != FD_DIRECTIVE || strncmp(f->text + e->start, "#remove", 7)) continue;
        int n = e->end - e->start;
        char t[512]; snprintf(t, sizeof t, "%.*s", n < 511 ? n : 511, f->text + e->start);
        for (char *p = strstr(t, key); p; p = strstr(p + 1, key))
            if (!isalnum((unsigned char)p[strlen(key)]) && !isalnum((unsigned char)p[-1])) return true;
        for (char *q = strchr(t, '"'); q; ) {                 /* #remove "__.*", #remove ( "r(Inner|Outer).*" ... ) */
            char *e2 = strchr(q + 1, '"');
            if (!e2) break;
            char pat[256]; snprintf(pat, sizeof pat, "%.*s", (int)(e2 - q + 1), q);
            if (pl_foam_key_matches(pat, key)) return true;
            q = strchr(e2 + 1, '"');
        }
    }
    return false;
}

static void compare(const FDFile *mine, const FDEntry *d, FDFile *ref, const char *prefix, const char *file)
{
    for (int i = 0; i < d->nChild; i++) {
        const FDEntry *e = &d->child[i];
        if (e->kind == FD_DIRECTIVE || !strcmp(e->key, "FoamFile")) continue;
        if (removed(mine, d, e->key)) { nSkip++; continue; }
        char path[1024]; snprintf(path, sizeof path, "%s%s%s", prefix, prefix[0] ? "/" : "", e->key);
        if (e->key[0] == '#' || e->key[0] == '$' || strchr(e->key, '/')) { nSkip++; continue; }
        if (e->kind == FD_DICT) { compare(mine, e, ref, path, file); continue; }
        if (fd_find((FDFile *)mine, path) != e) { nSkip++; continue; }   /* overridden by a later definition */
        int n = e->valEnd - e->valStart;
        char *v = malloc((size_t)(n > 0 ? n : 0) + 1);
        memcpy(v, mine->text + e->valStart, (size_t)(n > 0 ? n : 0)); v[n > 0 ? n : 0] = 0;
        if (strchr(v, '$') || strchr(v, '#') || n > 60000 || e->nChild) { nSkip++; free(v); continue; }
        nVals++;
        FDEntry *r = fd_find(ref, path);
        if (!r) {                                                /* a later duplicate key, or under a regex key: skipped */
            nMiss++;
            if (nMiss <= 15) printf("MISS  %s : %s\n", file, path);
            free(v); continue;
        }
        char *rv = malloc((size_t)(r->valEnd - r->valStart) + 1);
        memcpy(rv, ref->text + r->valStart, (size_t)(r->valEnd - r->valStart)); rv[r->valEnd - r->valStart] = 0;
        static char a[70000], b[70000];
        norm(v, a, sizeof a); norm(rv, b, sizeof b);
        if (!same_value(v, rv)) {
            nDiff++;
            if (nDiff <= 25) printf("DIFF  %s : %s\n      ours   [%.120s]\n      OpenFOAM [%.120s]\n", file, path, a, b);
        }
        free(v); free(rv);
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) { printf("usage: test_oracle <tutorials> <bashrc> <scratch> [max files]\n"); return 2; }
    int max = argc > 4 ? atoi(argv[4]) : 400;
    const char *all = getenv("QC_ORACLE_ALL");
    if (all && *all == '1') max = 0;
    List files = { 0 };
    walk(argv[1], &files);
    qsort(files.v, (size_t)files.n, sizeof(char *), cmp);
    List pick = { 0 };
    int step = max > 0 && files.n > max ? files.n / max : 1;
    for (int i = 0; i < files.n; i += step) add(&pick, files.v[i]);
    /* one shell runs every foamDictionary: sourcing the environment once */
    char dir[PL_PATH]; pl_join(dir, sizeof dir, argv[3], "oracle");
    pl_remove_tree(dir);                                     /* results of an earlier run must not count */
    pl_mkdirs(dir);
    char sh[PL_PATH]; pl_join(sh, sizeof sh, dir, "run.sh");
    FILE *f = fopen(sh, "w");
    if (!f) { printf("cannot write %s\n", sh); return 2; }
    char q[PL_PATH + 8]; pl_shell_quote(q, sizeof q, argv[2]);
    fprintf(f, "source %s >/dev/null 2>&1 || exit 3\n", q);
    for (int i = 0; i < pick.n; i++) {
        char qf[PL_PATH + 8]; pl_shell_quote(qf, sizeof qf, pick.v[i]);
        fprintf(f, "foamDictionary %s > %s/%d.txt 2>/dev/null || rm -f %s/%d.txt\n", qf, dir, i, dir, i);
    }
    fclose(f);
    char cmd[PL_PATH + 64]; snprintf(cmd, sizeof cmd, "bash %s", sh);
    if (system(cmd) != 0) { printf("could not run foamDictionary (is %s an OpenFOAM etc/bashrc?)\n", argv[2]); return 2; }
    for (int i = 0; i < pick.n; i++) {
        char out[PL_PATH + 32]; snprintf(out, sizeof out, "%s/%d.txt", dir, i);
        FDFile *ref = fd_load(out);
        if (!ref) { nBad++; continue; }                          /* OpenFOAM couldn't read it on its own (needs the case) */
        FDFile *mine = fd_load(pick.v[i]);
        if (mine && strstr(mine->text, "#codeStream")) nCode++;   /* its code can print into foamDictionary's output */
        else if (mine && (strstr(mine->text, "#if") || strstr(mine->text, "#ifeq"))) nCond++;   /* branches: see README */
        else if (mine && mine->ok && ref->ok) { nFiles++; compare(mine, &mine->root, ref, "", pick.v[i] + strlen(argv[1]) + 1); }
        fd_free(mine); fd_free(ref);
    }
    printf("%d files compared (%d not readable by foamDictionary alone, %d with #codeStream, %d with #if), %d values: %d different, %d not found in OpenFOAM's "
           "output, %d skipped ($macros, #directives)\n", nFiles, nBad, nCode, nCond, nVals, nDiff, nMiss, nSkip);
    return nDiff || nMiss ? 1 : 0;
}
