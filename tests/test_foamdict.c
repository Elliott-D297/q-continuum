/* test_foamdict.c -- round-trip and span checks for the dictionary parser over a directory tree.
 *
 *   test_foamdict <dir> [<dir> ...]
 *
 * Every regular file whose head holds a FoamFile header (polyMesh and processor* skipped) is
 *   1. loaded and saved to a scratch copy, which must be byte-identical to the original;
 *   2. checked structurally: every entry's key/value spans lie inside the text and inside the
 *      parent's value span, and the key text matches the span;
 *   3. edited in place with each value's own text (fd_put_entry), which must leave the bytes unchanged;
 *   4. re-parsed from its own text, which must give the same tree.
 * Files with NUL bytes (writeFormat binary) must instead load read-only and refuse to save.
 * Parse errors (f->ok == false) are reported separately: they are not round-trip failures, but the
 * app cannot edit entries after the error position reliably.
 * Exit status: 0 when no file failed checks 1-4.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "foamdict.h"

static int nFiles, nFail, nParseErr, nBinary;
static char scratch[PL_PATH];

static bool spans_ok(const FDFile *f, const FDEntry *e, int lo, int hi, char *why, int cap)
{
    for (int i = 0; i < e->nChild; i++) {
        const FDEntry *c = &e->child[i];
        if (!(lo <= c->start && c->start <= c->keyStart && c->keyStart <= c->keyEnd && c->valStart <= c->valEnd &&
              c->end <= hi && c->valEnd <= c->end + 1)) {
            snprintf(why, cap, "bad span for '%.60s' (start %d key %d-%d val %d-%d end %d, parent %d-%d)", c->key,
                     c->start, c->keyStart, c->keyEnd, c->valStart, c->valEnd, c->end, lo, hi);
            return false;
        }
        if ((int)strlen(c->key) != c->keyEnd - c->keyStart || strncmp(c->key, f->text + c->keyStart, strlen(c->key))) {
            snprintf(why, cap, "key text mismatch for '%.60s'", c->key);
            return false;
        }
        if (!spans_ok(f, c, c->valStart, c->valEnd, why, cap)) return false;
    }
    return true;
}

static bool same_tree(const FDEntry *a, const FDEntry *b)
{
    if (a->nChild != b->nChild || a->kind != b->kind || a->start != b->start || a->end != b->end ||
        a->valStart != b->valStart || a->valEnd != b->valEnd) return false;
    if ((a->key == NULL) != (b->key == NULL) || (a->key && strcmp(a->key, b->key))) return false;
    for (int i = 0; i < a->nChild; i++) if (!same_tree(&a->child[i], &b->child[i])) return false;
    return true;
}

/* collect value spans first: every edit re-parses, which frees the tree */
typedef struct { int a, b; } Span;
static void collect(const FDEntry *e, Span **v, int *n, int *cap)
{
    for (int i = 0; i < e->nChild; i++) {
        const FDEntry *c = &e->child[i];
        if (c->kind == FD_VALUE && c->valEnd > c->valStart) {
            if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *v = realloc(*v, sizeof(Span) * (size_t)*cap); }
            (*v)[(*n)++] = (Span){ c->valStart, c->valEnd };
        }
        collect(c, v, n, cap);
    }
}

static void fail(const char *path, const char *why) { printf("FAIL  %s\n      %s\n", path, why); nFail++; }

static void check_file(const char *path)
{
    size_t n;
    char *orig = pl_read_file(path, &n);
    if (!orig) return;
    nFiles++;
    FDFile *f = fd_load(path);
    if (!f) { fail(path, "fd_load returned NULL"); free(orig); return; }
    if ((size_t)f->len != n) {
        /* binary fields: must be flagged read-only, and saving must be refused */
        unlink(scratch);
        bool refused = !fd_save_as(f, scratch) && !fd_save(f) && !pl_exists(scratch) && !fd_replace_span(f, 0, 0, "x");
        if (!f->binary || !refused) fail(path, "file with NUL bytes was not protected against saving");
        else nBinary++;
        fd_free(f); free(orig); return;
    }
    if (!f->ok) { printf("PARSE %s\n      %s\n", path, f->err); nParseErr++; }

    /* 1. save to a scratch copy */
    if (!fd_save_as(f, scratch)) { fail(path, "save failed"); goto done; }
    size_t m; char *back = pl_read_file(scratch, &m);
    bool same = back && m == n && !memcmp(back, orig, n);
    free(back);
    if (!same) { fail(path, "saved bytes differ"); goto done; }

    /* 2. spans */
    char why[300];
    if (!spans_ok(f, &f->root, 0, f->len, why, sizeof why)) { fail(path, why); goto done; }

    /* 4. re-parse stability */
    FDFile *g = fd_from_text(path, f->text);
    bool st = g && same_tree(&f->root, &g->root);
    fd_free(g);
    if (!st) { fail(path, "re-parsing the same text gave a different tree"); goto done; }

    /* 3. put every value back as itself (capped: the cost is quadratic in the entry count) */
    Span *sp = NULL; int ns = 0, cs = 0;
    collect(&f->root, &sp, &ns, &cs);
    for (int i = 0; i < ns && i < 400; i++) {
        int a = sp[i].a, b = sp[i].b;
        char *val = pl_strndup(f->text + a, (size_t)(b - a));
        fd_replace_span(f, a, b, val);
        free(val);
        if ((size_t)f->len != n || memcmp(f->text, orig, n)) { fail(path, "putting a value back as itself changed the file"); break; }
    }
    free(sp);
done:
    fd_free(f);
    free(orig);
}

static bool has_header(const char *path)
{
    char *h = pl_read_head(path, 4096);
    bool y = h && strstr(h, "FoamFile") && !memchr(h, 0, strlen(h) < 4096 ? strlen(h) : 4096);
    free(h);
    return y;
}

static void walk(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        if (nm[0] == '.') continue;
        char p[PL_PATH]; pl_join(p, sizeof p, dir, nm);
        struct stat st;
        if (lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!strcmp(nm, "polyMesh") || !strncmp(nm, "processor", 9)) continue;
            walk(p);
        } else if (S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (8 << 20) && !pl_ends_with(nm, ".gz") && has_header(p)) {
            check_file(p);
        }
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <dir> [<dir> ...]\n", argv[0]); return 2; }
    snprintf(scratch, sizeof scratch, "/tmp/qc-roundtrip-%d", (int)getpid());
    for (int i = 1; i < argc; i++) walk(argv[i]);
    unlink(scratch);
    printf("\n%d dictionaries: %d failed round-trip/span checks, %d with parse errors, %d binary (read-only)\n", nFiles, nFail, nParseErr, nBinary);
    return nFail ? 1 : 0;
}
