/* fuzz_foamdict.c -- libFuzzer harness for the dictionary parser and its span edits.
 * Every input must parse without a memory error, give spans inside the text, survive a self-edit (each value
 * replaced by its own text) byte for byte, and survive a few real edits (set, add, remove) without crashing. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "foamdict.h"

static void check(const FDFile *f, const FDEntry *e, int lo, int hi)
{
    for (int i = 0; i < e->nChild; i++) {
        const FDEntry *c = &e->child[i];
        if (c->start < lo || c->end > hi || c->keyStart > c->keyEnd || c->valStart > c->valEnd || c->valEnd > f->len) {
            fprintf(stderr, "bad span: key '%.40s' start %d end %d key %d-%d val %d-%d parent %d-%d len %d\n", c->key, c->start, c->end,
                    c->keyStart, c->keyEnd, c->valStart, c->valEnd, lo, hi, f->len);
            abort();
        }
        check(f, c, c->valStart, c->valEnd);
    }
}

static const FDEntry *first_value(const FDEntry *e)
{
    for (int i = 0; i < e->nChild; i++) {
        if (e->child[i].kind == FD_VALUE) return &e->child[i];
        const FDEntry *r = first_value(&e->child[i]);
        if (r) return r;
    }
    return NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *t = malloc(size + 1);
    memcpy(t, data, size); t[size] = 0;
    FDFile *f = fd_from_text("", t);
    if (f) {
        check(f, &f->root, 0, f->len);
        char *orig = strdup(f->text);
        for (int i = 0; i < f->root.nChild && i < 8; i++) {     /* each top-level value replaced by itself: no change */
            FDEntry *e = &f->root.child[i];
            if (e->kind != FD_VALUE) continue;
            int n = e->valEnd - e->valStart;
            char *v = malloc((size_t)n + 1); memcpy(v, f->text + e->valStart, (size_t)n); v[n] = 0;
            char key[64]; snprintf(key, sizeof key, "%.60s", e->key);          /* e is gone once the edit re-parses */
            fd_put_entry(f, e, v);
            free(v);
            if (strcmp(f->text, orig)) { fprintf(stderr, "self-edit of '%s' changed the text\n", key); abort(); }
        }
        const FDEntry *e = first_value(&f->root);
        if (e) {
            char key[128]; snprintf(key, sizeof key, "%.100s", e->key);
            fd_put(f, key, "1");
            fd_put(f, "fuzzAdded/inner", "(1 2 3)");
            fd_remove(f, key);
            check(f, &f->root, 0, f->len);
        }
        free(orig);
        fd_free(f);
    }
    free(t);
    return 0;
}
