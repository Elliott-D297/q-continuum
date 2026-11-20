/* test_glossary.c -- the teaching pieces' core (mesh overhaul plan step 1): glossary.c and working.c.
     test_glossary <src dir>
   The glossary is consistent (keys and aliases unique, every entry complete, every related key and every [[term]] in its
   text an entry), the markup strips as the UI draws it, every [[term]] in the app's sources names an entry, and the
   working panels' numbers are the hand calculations of docs/TEACHING.md and agree with the mesher's own (stlmesh.c). */
#define _GNU_SOURCE
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "glossary.h"
#include "pl_util.h"
#include "stlmesh.h"
#include "working.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

/* every [[key]] / [[key|...]] in s names an entry; loose: only what looks like markup (a key of letters, digits, + - _ and
   spaces, then | or ]]), so shell tests ([[ -f x ]]) and character classes ([[:space:]]) in the sources don't count */
static int check_markup(const char *s, const char *where)
{
    int n = 0;
    for (const char *p = s; (p = strstr(p, "[[")); p += 2) {
        const char *q = p + 2;
        if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9'))) continue;
        while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || strchr("+-_ ", *q)) q++;
        if (*q != '|' && strncmp(q, "]]", 2)) continue;
        const char *e = strstr(p + 2, "]]");
        if (!e) { CHECK(0, "%s: unclosed [[ in \"%.40s\"", where, p); break; }
        const char *bar = memchr(p + 2, '|', (size_t)(e - p - 2));
        char k[64]; snprintf(k, sizeof k, "%.*s", (int)((bar ? bar : e) - p - 2), p + 2);
        CHECK(gloss_find(k), "%s: [[%s]] is no glossary entry", where, k);
        n++;
    }
    return n;
}

static void list_words(const char *list, void (*fn)(const char *, int), int i)
{
    for (const char *p = list; p && *p;) {
        const char *e = strchr(p, '|');
        char w[64]; snprintf(w, sizeof w, "%.*s", (int)(e ? e - p : (long)strlen(p)), p);
        fn(w, i);
        p = e ? e + 1 : NULL;
    }
}
static void alias_unique(const char *w, int i)
{
    const GlossTerm *t = gloss_find(w);
    CHECK(t == gloss_at(i), "alias \"%s\" of %s finds %s", w, gloss_at(i)->key, t ? t->key : "nothing");
}
static void see_exists(const char *w, int i) { CHECK(gloss_find(w), "%s: see also \"%s\" is no entry", gloss_at(i)->key, w); }

static void table_checks(void)
{
    int n = gloss_count();
    CHECK(n >= 40, "only %d entries", n);
    for (int i = 0; i < n; i++) {
        const GlossTerm *t = gloss_at(i);
        CHECK(t->key && *t->key && t->name && *t->name, "entry %d has no key or name", i);
        CHECK(t->what && strlen(t->what) > 40, "%s: no explanation", t->key);
        CHECK(t->picture && *t->picture, "%s: no picture", t->key);
        CHECK(t->number && *t->number, "%s: no number", t->key);
        CHECK(t->aliases && t->foam && t->see, "%s: NULL field (use \"\")", t->key);
        CHECK(!strpbrk(t->key, " []|"), "%s: key with a space or markup character", t->key);
        for (int j = 0; j < i; j++) CHECK(strcasecmp(t->key, gloss_at(j)->key), "key %s twice", t->key);
        CHECK(gloss_find(t->key) == t, "%s doesn't find itself", t->key);
        list_words(t->aliases, alias_unique, i);
        list_words(t->see, see_exists, i);
        char where[80]; snprintf(where, sizeof where, "glossary %s", t->key);
        check_markup(t->what, where); check_markup(t->picture, where); check_markup(t->number, where); check_markup(t->foam, where);
        char k[48];
        CHECK(gloss_see(t, 0, k, sizeof k) || !*t->see, "%s: see list unreadable", t->key);
    }
    CHECK(gloss_find("Y+") == gloss_find("y+") && gloss_find("yplus") == gloss_find("y+"), "y+ by case / alias");
    CHECK(!gloss_find("") && !gloss_find(NULL) && !gloss_find("nonsense-term"), "unknown words find nothing");
}

static void markup_checks(void)
{
    char out[256]; GlossSpan sp[4];
    int n = gloss_strip("the [[y+]] of the [[first-layer|first cell]].", out, sizeof out, sp, 4);
    CHECK(n == 2 && !strcmp(out, "the y+ of the first cell."), "strip: %d \"%s\"", n, out);
    CHECK(n == 2 && sp[0].start == 4 && sp[0].end == 6 && !strcmp(sp[0].key, "y+"), "span 0 %d..%d %s", sp[0].start, sp[0].end, sp[0].key);
    CHECK(n == 2 && sp[1].start == 14 && sp[1].end == 24 && !strcmp(sp[1].key, "first-layer"), "span 1 %d..%d %s", sp[1].start, sp[1].end, sp[1].key);
    n = gloss_strip("open [[ bracket and a ]] later [[x", out, sizeof out, sp, 4);
    CHECK(n == 1 && !strcmp(out, "open  bracket and a  later [[x"), "unclosed: %d \"%s\"", n, out);   /* "[[ bracket and a ]]" is a span */
    n = gloss_strip("[[prism-layers]] and [[non-orthogonality|non-orthogonality]]", out, sizeof out, sp, 4);
    CHECK(n == 2 && !strcmp(out, "prism layers and non-orthogonality") && !strcmp(sp[0].key, "prism-layers"), "hyphens: \"%s\"", out);
    n = gloss_strip("no markup", out, sizeof out, NULL, 0);
    CHECK(n == 0 && !strcmp(out, "no markup"), "plain");
    n = gloss_strip("[[a|bb]] [[c]]", out, 4, sp, 4);                   /* truncated: spans stay inside the text */
    CHECK(!strcmp(out, "bb ") && sp[n - 1].end <= 3, "truncated \"%s\"", out);
    int idx[128];
    n = gloss_search("skew", idx, 128);
    CHECK(n >= 1 && !strcmp(gloss_at(idx[0])->key, "skewness"), "search skew");
    n = gloss_search("inflation", idx, 128);
    CHECK(n >= 1 && !strcmp(gloss_at(idx[0])->key, "prism-layers"), "search by alias");
    CHECK(gloss_search("", idx, 128) == gloss_count(), "empty search lists all");
    CHECK(gloss_search("zzzqqq", idx, 128) == 0, "no match");
}

/* every [[term]] in the app's sources (string literals: the shown text) is an entry */
static void sources_checks(const char *dir)
{
    DIR *d = opendir(dir);
    CHECK(d, "can't open %s", dir);
    if (!d) return;
    int total = 0, files = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 3 || strcmp(e->d_name + l - 2, ".c") || !strcmp(e->d_name, "glossary.c")
            || !strcmp(e->d_name, "ui.c")) continue;     /* glossary.c: checked entry by entry; ui.c: the markup's own comments */
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        char *t = pl_read_file(p, NULL);
        if (!t) continue;
        int k = check_markup(t, e->d_name);
        if (k) files++;
        total += k;
        free(t);
    }
    closedir(d);
    CHECK(total >= 20 && files >= 3, "only %d [[terms]] in %d source files", total, files);
    printf("glossary: %d entries; %d [[terms]] in %d source files\n", gloss_count(), total, files);
}

static const WkLine *line(const Working *w, int kind, const char *sym)
{
    for (int i = 0; i < w->n; i++) if (w->l[i].kind == kind && !strcmp(w->l[i].sym, sym)) return &w->l[i];
    return NULL;
}

static void working_checks(void)
{
    char b[64];
    wk_num(b, sizeof b, 0.0003852); CHECK(!strcmp(b, "3.852e-04"), "wk_num small: %s", b);
    wk_num(b, sizeof b, 385.24); CHECK(!strcmp(b, "385.2"), "wk_num: %s", b);
    char v[40], u[16];
    wk_len(3.4e-5, v, sizeof v, u, sizeof u); CHECK(!strcmp(v, "34") && !strcmp(u, "µm"), "wk_len µm: %s %s", v, u);
    wk_len(0.0025, v, sizeof v, u, sizeof u); CHECK(!strcmp(v, "2.5") && !strcmp(u, "mm"), "wk_len mm: %s %s", v, u);
    wk_len(1.5, v, sizeof v, u, sizeof u); CHECK(!strcmp(v, "1.5") && !strcmp(u, "m"), "wk_len m: %s %s", v, u);

    /* docs/TEACHING.md example 1: T = 10 mm, N = 10, r = 1.2, h = 4 mm */
    Working w;
    wk_layer_stack(&w, 0.010, 10, 1.2, 0.004);
    const WkLine *t1 = line(&w, WK_RESULT, "t1"), *tN = line(&w, WK_RESULT, "tN");
    CHECK(t1 && !strcmp(t1->val, "385.2") && !strcmp(t1->unit, "µm"), "t1 = %s %s", t1 ? t1->val : "?", t1 ? t1->unit : "");
    CHECK(tN && !strcmp(tN->val, "1.988") && !strcmp(tN->unit, "mm"), "tN = %s %s", tN ? tN->val : "?", tN ? tN->unit : "");
    double first = 0.010 * 0.2 / (pow(1.2, 10) - 1), sum = 0;
    for (int i = 0; i < 10; i++) sum += first * pow(1.2, i);
    CHECK(fabs(sum - 0.010) < 1e-12, "the layers add up to T (%g)", sum);
    char txt[4096]; wk_text(&w, txt, sizeof txt);
    CHECK(strstr(txt, "Given") && strstr(txt, "Result") && strstr(txt, "t1 = 385.2 µm") && !strstr(txt, "[["), "wk_text:\n%s", txt);
    CHECK(strstr(txt, "tN / h = 1.988 mm / 4 mm = 49.69 %"), "outer share line:\n%s", txt);
    wk_layer_stack(&w, 0.003, 3, 1.0, 0);
    t1 = line(&w, WK_RESULT, "t1");
    CHECK(t1 && !strcmp(t1->val, "1") && !strcmp(t1->unit, "mm"), "equal layers t1 = %s %s", t1 ? t1->val : "?", t1 ? t1->unit : "");

    /* example 2: B = 40 mm, s = 30 %, L = 1 m, snappyHexMesh: level 2, h = 10 mm, n = 100 */
    wk_surface_cell(&w, 0.040, 30, true, 1.0);
    const WkLine *h = line(&w, WK_RESULT, "h"), *n = line(&w, WK_RESULT, "n");
    CHECK(h && !strcmp(h->val, "10") && !strcmp(h->unit, "mm"), "octree h = %s %s", h ? h->val : "?", h ? h->unit : "");
    CHECK(n && !strcmp(n->val, "100"), "octree n = %s", n ? n->val : "?");
    wk_text(&w, txt, sizeof txt);
    CHECK(strstr(txt, "level = round(log2(100 / 30)) = round(1.737) = 2") && strstr(txt, "Asked for 12 mm"), "octree working:\n%s", txt);
    wk_surface_cell(&w, 0.040, 30, false, 1.0);
    h = line(&w, WK_RESULT, "h");
    CHECK(h && !strcmp(h->val, "12") && !strcmp(h->unit, "mm"), "plain h = %s %s", h ? h->val : "?", h ? h->unit : "");

    /* the same numbers as the mesher's own (stlmesh.c), over a range of targets */
    MeshCfg m; memset(&m, 0, sizeof m);
    m.base = 0.04;
    for (int oct = 0; oct < 2; oct++) {
        m.mesher = oct ? MESHER_SNAPPY : MESHER_TET;
        for (double pct = 3; pct <= 150; pct *= 1.37) {
            m.targetPct = pct;
            wk_surface_cell(&w, m.base, pct, oct, 0);
            h = line(&w, WK_RESULT, "h");
            double want = mesher_surface_cell(&m);
            char wv[40], wu[16]; wk_len(want, wv, sizeof wv, wu, sizeof wu);
            CHECK(h && !strcmp(h->val, wv) && !strcmp(h->unit, wu), "%s %g %%: working %s %s, mesher %s %s",
                  oct ? "snappy" : "tet", pct, h ? h->val : "?", h ? h->unit : "", wv, wu);
        }
    }
}

int main(int argc, char **argv)
{
    table_checks();
    markup_checks();
    working_checks();
    if (argc > 1) sources_checks(argv[1]);
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("glossary and working: all passed\n");
    return 0;
}
