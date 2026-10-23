/* test_props.c -- property tests of the dictionary editor on real tutorial files.
 *
 *   test_props <tutorials dir> <etc/bashrc> <scratch dir> [files] [seed]
 *
 * 1. Edit and revert: any entry set to another value and back gives the original bytes.
 * 2. Edits OpenFOAM accepts: numbers changed, an entry added, an entry removed; foamDictionary must still read every
 *    file it read before the edits, and read back exactly what was changed (one foamDictionary run per file, all in
 *    one shell).
 * Deterministic for a given seed. Default: 300 files, seed 1.
 */
#define _GNU_SOURCE
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
static unsigned long long rng;
static unsigned rnd(unsigned n) { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)((rng >> 33) % (n ? n : 1)); }

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
        if (S_ISDIR(st.st_mode)) { if (strcmp(e->d_name, "polyMesh") && strncmp(e->d_name, "processor", 9)) walk(p, out); continue; }
        if (!S_ISREG(st.st_mode) || st.st_size > (1 << 19)) continue;
        char *h = pl_read_head(p, 1024);
        if (h && strstr(h, "FoamFile")) add(out, p);
        free(h);
    }
    closedir(d);
}

/* leaf value entries an edit can target: plain keys, not in lists, not macros or directives */
typedef struct { char path[600]; char val[200]; } Leaf;
static int leaves(const FDFile *f, const FDEntry *d, const char *prefix, Leaf *out, int n, int max)
{
    for (int i = 0; i < d->nChild && n < max; i++) {
        const FDEntry *e = &d->child[i];
        if (e->kind == FD_DIRECTIVE || !strcmp(e->key, "FoamFile") || e->key[0] == '"' || e->key[0] == '#' || e->key[0] == '$' ||
            strchr(e->key, '/') || strchr(e->key, ' ')) continue;
        char path[600]; snprintf(path, sizeof path, "%s%s%s", prefix, prefix[0] ? "/" : "", e->key);
        if (e->kind == FD_DICT) { n = leaves(f, e, path, out, n, max); continue; }
        int len = e->valEnd - e->valStart;
        if (len <= 0 || len >= 200 || e->nChild) continue;
        Leaf *l = &out[n];
        pl_strlcpy(l->path, path, sizeof l->path);
        memcpy(l->val, f->text + e->valStart, (size_t)len); l->val[len] = 0;
        if (strchr(l->val, '$') || strchr(l->val, '#') || strchr(l->val, '\n')) continue;
        n++;
    }
    return n;
}

static bool unique_path(FDFile *f, const char *path)      /* fd_find takes the first of duplicates: avoid them */
{
    FDEntry *e = fd_find(f, path);
    if (!e) return false;
    char last[300]; const char *s = strrchr(path, '/'); pl_strlcpy(last, s ? s + 1 : path, sizeof last);
    char parent[600]; pl_strlcpy(parent, path, sizeof parent);
    char *sl = strrchr(parent, '/'); if (sl) *sl = 0; else parent[0] = 0;
    FDEntry *pd = parent[0] ? fd_find(f, parent) : &f->root;
    int c = 0;
    for (int i = 0; pd && i < pd->nChild; i++) if (!strcmp(pd->child[i].key, last)) c++;
    return c == 1;
}

int main(int argc, char **argv)
{
    if (argc < 4) { printf("usage: test_props <tutorials> <bashrc> <scratch> [files] [seed]\n"); return 2; }
    int max = argc > 4 ? atoi(argv[4]) : 300;
    rng = argc > 5 ? strtoull(argv[5], NULL, 10) : 1;
    List files = { 0 };
    walk(argv[1], &files);
    qsort(files.v, (size_t)files.n, sizeof(char *), cmp);
    int step = max > 0 && files.n > max ? files.n / max : 1;
    char dir[PL_PATH]; pl_join(dir, sizeof dir, argv[3], "props");
    pl_remove_tree(dir);                                     /* results of an earlier run must not count */
    pl_mkdirs(dir);
    char sh[PL_PATH + 16]; snprintf(sh, sizeof sh, "%s/run.sh", dir);
    FILE *script = fopen(sh, "w");
    char q[PL_PATH + 8]; pl_shell_quote(q, sizeof q, argv[2]);
    fprintf(script, "source %s >/dev/null 2>&1 || exit 3\n", q);
    static Leaf L[4000];
    int nRevert = 0, revertFail = 0, nEdited = 0;
    typedef struct { char file[PL_PATH]; char set[3][600]; int nSet; char removed[600]; bool includes, removes; } Expect;
    Expect *ex = calloc((size_t)(files.n / step + 2), sizeof *ex);
    int nEx = 0;
    for (int i = 0; i < files.n; i += step) {
        FDFile *f = fd_load(files.v[i]);
        if (!f || !f->ok || f->binary) { fd_free(f); continue; }
        int n = leaves(f, &f->root, "", L, 0, 4000);
        if (!n) { fd_free(f); continue; }
        char *orig = strdup(f->text);
        /* 1. edit + revert */
        for (int k = 0; k < 5; k++) {
            Leaf *l = &L[rnd((unsigned)n)];
            if (!unique_path(f, l->path)) continue;
            FDEntry *e = fd_find(f, l->path);
            fd_put_entry(f, e, "12345");
            e = fd_find(f, l->path);
            if (e) fd_put_entry(f, e, l->val);
            nRevert++;
            if (strcmp(f->text, orig)) {
                revertFail++;
                if (revertFail <= 10) printf("REVERT %s : %s did not give the original bytes back\n", files.v[i] + strlen(argv[1]) + 1, l->path);
                fd_set_text(f, orig);
            }
        }
        /* 2. edits OpenFOAM must accept */
        Expect *x = &ex[nEx];
        memset(x, 0, sizeof *x);
        for (int k = 0; k < 6 && x->nSet < 3; k++) {
            Leaf *l = &L[rnd((unsigned)n)];
            char t[200]; pl_strlcpy(t, l->val, sizeof t); pl_trim_inplace(t);
            if (!pl_is_number(t) || !unique_path(f, l->path)) continue;
            bool dup = false;
            for (int s = 0; s < x->nSet; s++) if (!strcmp(x->set[s], l->path)) dup = true;
            if (dup) continue;
            fd_put_entry(f, fd_find(f, l->path), "42");
            pl_strlcpy(x->set[x->nSet++], l->path, sizeof x->set[0]);
        }
        fd_put(f, "qcPropertyTestEntry", "7");
        Leaf *r = &L[rnd((unsigned)n)];
        bool clash = false;
        for (int s = 0; s < x->nSet; s++) if (pl_starts_with(x->set[s], r->path) || pl_starts_with(r->path, x->set[s])) clash = true;
        const char *rk = strrchr(r->path, '/'); rk = rk ? rk + 1 : r->path;
        char ref[sizeof r->path + 2]; snprintf(ref, sizeof ref, "$%s", rk);
        /* a macro elsewhere may use it ($name, $/a/name, or in an #include'd file): removing it would break the file */
        const char *first = strstr(f->text, rk);
        bool used = strstr(f->text, ref) || strstr(f->text, "#include") || (first && strstr(first + 1, rk));
        if (!clash && !used && unique_path(f, r->path) && strcmp(r->path, "qcPropertyTestEntry")) { fd_remove(f, r->path); pl_strlcpy(x->removed, r->path, sizeof x->removed); }
        if (!f->ok) { printf("BROKE  %s : the edits left a file the parser can't read: %s\n", files.v[i], f->err); revertFail++; }
        /* the original goes next to the edited copy, so both see the same relative #includes */
        char out[PL_PATH + 32], org[PL_PATH + 32];
        snprintf(out, sizeof out, "%s/%d.edited", dir, nEx);
        snprintf(org, sizeof org, "%s/%d.orig", dir, nEx);
        fd_save_as(f, out);
        pl_write_file(org, orig, strlen(orig));
        pl_strlcpy(x->file, files.v[i], sizeof x->file);
        x->includes = strstr(orig, "#include") != NULL;
        x->removes = strstr(orig, "#remove") != NULL;
        char qo[PL_PATH + 8], qi[PL_PATH + 8]; pl_shell_quote(qo, sizeof qo, out); pl_shell_quote(qi, sizeof qi, org);
        fprintf(script, "if foamDictionary %s > /dev/null 2>&1; then foamDictionary %s > %s/%d.out 2>/dev/null || echo FAIL > %s/%d.out; fi\n",
                qi, qo, dir, nEx, dir, nEx);
        nEx++; nEdited++;
        free(orig); fd_free(f);
    }
    fclose(script);
    char cmd[PL_PATH + 64]; snprintf(cmd, sizeof cmd, "bash %s/run.sh", dir);
    if (system(cmd) != 0) { printf("could not run foamDictionary\n"); return 2; }
    int checked = 0, unread = 0, wrong = 0;
    for (int i = 0; i < nEx; i++) {
        char out[PL_PATH + 32]; snprintf(out, sizeof out, "%s/%d.out", dir, i);
        char *t = pl_read_file(out, NULL);
        if (!t) continue;                                    /* foamDictionary couldn't read the original either */
        checked++;
        if (!strncmp(t, "FAIL", 4)) { unread++; if (unread <= 10) printf("UNREAD %s: foamDictionary read it before the edits, not after\n", ex[i].file); free(t); continue; }
        FDFile *o = fd_from_text("", t);
        char v[200];
        if (!fd_get(o, "qcPropertyTestEntry", v, sizeof v) || strcmp(v, "7")) { wrong++; printf("ADDED  %s: the new entry reads '%s'\n", ex[i].file, v); }
        for (int s = 0; s < ex[i].nSet && !ex[i].removes; s++)           /* #remove may take the entry out again */
            if (!fd_get(o, ex[i].set[s], v, sizeof v) || fabs(atof(v) - 42) > 1e-9) { wrong++; if (wrong <= 10) printf("SET    %s : %s reads '%s', not 42\n", ex[i].file, ex[i].set[s], v); }
        if (ex[i].removed[0] && !ex[i].includes && fd_find(o, ex[i].removed) && !strchr(t, '$')) {   /* an #include may add it back */ wrong++; if (wrong <= 10) printf("REMOVE %s : %s is still there\n", ex[i].file, ex[i].removed); }
        fd_free(o); free(t);
    }
    printf("edit+revert: %d edits, %d wrong. Edited files: %d, %d checked by foamDictionary: %d unreadable after the edits, %d with "
           "a value not as written\n", nRevert, revertFail, nEdited, checked, unread, wrong);
    return revertFail || unread || wrong ? 1 : 0;
}
