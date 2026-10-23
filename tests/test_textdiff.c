/* test_textdiff: the review-before-saving diff (textdiff.c). A value change is one - and one + line with context, a
 * summary "deltaT: 0.005 -> 0.0025"; added and removed entries; line numbers on both sides; identical texts give
 * nothing; a big file with one change stays small; a change near both ends gives two hunks with a skip between. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "textdiff.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void dump(const Diff *d)
{
    for (int i = 0; i < d->n; i++) printf("  %c %3d %3d  %.*s\n", d->l[i].kind, d->l[i].aLine, d->l[i].bLine, d->l[i].len, d->l[i].s);
}

int main(void)
{
    const char *a = "FoamFile\n{\n    object controlDict;\n}\napplication icoFoam;\nstartTime 0;\nendTime 0.5;\ndeltaT 0.005;\nwriteControl timeStep;\nwriteInterval 20;\n";
    const char *b = "FoamFile\n{\n    object controlDict;\n}\napplication icoFoam;\nstartTime 0;\nendTime 0.5;\ndeltaT 0.0025;\nwriteControl timeStep;\nwriteInterval 20;\nmaxCo 0.5;\n";
    Diff d; CHECK(diff_texts(a, b, 2, &d), "diff");
    dump(&d);
    CHECK(d.added == 2 && d.removed == 1, "added %d removed %d", d.added, d.removed);
    int minus = -1; for (int i = 0; i < d.n; i++) if (d.l[i].kind == '-') minus = i;
    CHECK(minus >= 0 && d.l[minus].aLine == 8 && !strncmp(d.l[minus].s, "deltaT 0.005;", 13), "removed line");
    char sum[8][160]; int ns = diff_summary(&d, sum, 8);
    for (int i = 0; i < ns; i++) printf("  summary: %s\n", sum[i]);
    CHECK(ns == 2 && !strcmp(sum[0], "deltaT: 0.005 \xe2\x86\x92 0.0025") && !strcmp(sum[1], "maxCo: added (0.5)"), "summary");
    diff_free(&d);

    CHECK(diff_texts(a, a, 3, &d) && d.n == 0, "identical: %d lines", d.n); diff_free(&d);

    /* MOOSE key = value */
    CHECK(diff_texts("[Executioner]\n  dt = 5\n  end_time = 600\n[]\n", "[Executioner]\n  dt = 5\n  end_time = 300\n[]\n", 1, &d), "moose");
    ns = diff_summary(&d, sum, 8);
    CHECK(ns == 1 && !strcmp(sum[0], "end_time: 600 \xe2\x86\x92 300"), "moose summary '%s'", ns ? sum[0] : "-");
    diff_free(&d);

    /* a big file, a change at each end: two hunks with a skip */
    char *big = malloc(400000), *big2 = malloc(400000);
    int k = 0; for (int i = 0; i < 20000; i++) k += sprintf(big + k, "line %d\n", i);
    strcpy(big2, big);
    big2[5] = 'X';                                   /* "line X" in the first line */
    char *last = strstr(big2, "line 19999"); last[5] = 'Y';
    CHECK(diff_texts(big, big2, 2, &d), "big");
    int skips = 0; for (int i = 0; i < d.n; i++) if (d.l[i].kind == '@') skips++;
    CHECK(d.added == 2 && d.removed == 2 && skips == 1 && d.n < 20, "big: +%d -%d skips %d n %d", d.added, d.removed, skips, d.n);
    diff_free(&d);
    /* a middle too big for the table still gives a correct (if plain) diff */
    for (int i = 0; i < 400000 && big2[i]; i++) if (big2[i] == 'l') big2[i] = 'L';
    CHECK(diff_texts(big, big2, 0, &d) && d.added == 20000 && d.removed == 20000, "huge: +%d -%d", d.added, d.removed);
    diff_free(&d);
    free(big); free(big2);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
