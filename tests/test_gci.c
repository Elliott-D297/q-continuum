/* test_gci.c -- Richardson extrapolation and the GCI (mesh overhaul plan step 8, gci.c) against the worked example of
   Celik et al. (2008), J. Fluids Eng. 130, 078001, Table 1 (first column): N = 18000, 8000, 4500 cells in 2-D,
   f = 6.063, 5.972, 5.863 -> r21 1.5, r32 1.333, p 1.53, f_ext 6.1685, e_a 1.5 %, e_ext 1.7 %, GCI 2.2 %. Then the
   other kinds (two meshes, oscillating, diverging, unchanged, a field of exact second-order data) and bad input.
     test_gci */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gci.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)
#define NEAR(a, b, tol, what) CHECK(fabs((double)(a) - (double)(b)) <= (tol), "%s: %.6g, expected %.6g", what, (double)(a), (double)(b))

int main(void)
{
    GciOut o; Working wk;
    /* Celik et al., in the order given (coarse first, to check the sorting) */
    GciIn in = { .n = 3, .h = { gci_h(4500, 2), gci_h(18000, 2), gci_h(8000, 2) }, .f = { 5.863, 6.063, 5.972 } };
    CHECK(gci_compute(&in, &o, &wk) && o.ok, "Celik: computes");
    CHECK(o.idx[0] == 1 && o.idx[1] == 2 && o.idx[2] == 0, "sorted fine to coarse");
    NEAR(o.r21, 1.5, 1e-9, "r21"); NEAR(o.r32, 4.0 / 3, 1e-9, "r32");
    NEAR(o.p, 1.53, 0.005, "apparent order p");
    NEAR(o.fExt, 6.1685, 0.0005, "f_ext");
    NEAR(o.ea * 100, 1.5, 0.05, "e_a %");
    NEAR(o.eext * 100, 1.7, 0.05, "e_ext %");
    NEAR(o.gci * 100, 2.2, 0.05, "GCI fine %");
    CHECK(o.kind == GCI_MONOTONIC && !o.pAssumed, "monotonic, p found");
    CHECK(o.verdict <= 1, "a 2.2 %% GCI is fine or close (verdict %d: %s)", o.verdict, o.verdictText);
    char t[8192]; wk_text(&wk, t, sizeof t);
    CHECK(strstr(t, "Richardson") && strstr(t, "GCI = 1.25"), "working text");

    /* exact second-order data, f = 1 + h^2, ratio 2: p = 2, f_ext = 1, asymptotic ratio 1 */
    GciIn ex = { .n = 3, .h = { 0.1, 0.2, 0.4 }, .f = { 1.01, 1.04, 1.16 } };
    gci_compute(&ex, &o, NULL);
    NEAR(o.p, 2, 1e-6, "exact p"); NEAR(o.fExt, 1, 1e-9, "exact f_ext"); NEAR(o.asym, 1.01 / 1.04, 1e-6, "exact asymptotic ratio (f1 / f2: the GCIs are relative)");
    CHECK(o.verdict == 0, "1 %% change at p = 2: fine enough (%s)", o.verdictText);

    /* two meshes: p assumed 2, factor 3 */
    GciIn two = { .n = 2, .h = { 1, 2 }, .f = { 1.01, 1.04 } };
    gci_compute(&two, &o, NULL);
    CHECK(o.pAssumed && o.used == 2, "two meshes: p assumed");
    NEAR(o.gci, 3 * (0.03 / 1.01) / 3, 1e-9, "two-mesh GCI = 3 e_a / (r^2 - 1)");

    /* oscillating: up, down */
    GciIn osc = { .n = 3, .h = { 1, 1.5, 2.25 }, .f = { 1.00, 1.02, 0.97 } };
    gci_compute(&osc, &o, NULL);
    CHECK(o.kind == GCI_OSCILLATING && o.R < 0, "oscillating (R %g)", o.R);

    /* diverging: changes growing as the mesh gets finer */
    GciIn div = { .n = 3, .h = { 1, 1.5, 2.25 }, .f = { 1.3, 1.1, 1.0 } };
    gci_compute(&div, &o, NULL);
    CHECK(o.kind == GCI_DIVERGING && o.verdict == 2 && strstr(o.verdictText, "Diverging"), "diverging");

    /* unchanged */
    GciIn same = { .n = 3, .h = { 1, 2, 4 }, .f = { 3, 3, 3 } };
    gci_compute(&same, &o, NULL);
    CHECK(o.ok && o.kind == GCI_SAME && o.fExt == 3, "unchanged");

    /* more than three: the finest three; NaN and repeated sizes left out; too few */
    GciIn many = { .n = 6, .h = { 0.4, 0.1, NAN, 0.2, 0.2, 0.8 }, .f = { 1.16, 1.01, 5, 1.04, 9, 1.64 } };
    gci_compute(&many, &o, NULL);
    CHECK(o.used == 3 && o.idx[0] == 1 && o.idx[1] == 3 && o.idx[2] == 0, "the finest three (%d %d %d)", o.idx[0], o.idx[1], o.idx[2]);
    NEAR(o.p, 2, 1e-6, "finest three: p");
    GciIn one = { .n = 2, .h = { 1, 1 }, .f = { 1, 2 } };
    CHECK(!gci_compute(&one, &o, NULL) && strstr(o.why, "two meshes"), "one size twice: refused");
    GciIn bad = { .n = 3, .h = { 1, 2, 4 }, .f = { NAN, INFINITY, 1 } };
    CHECK(!gci_compute(&bad, &o, NULL), "no finite values: refused");

    NEAR(gci_h(1000, 3), 0.1, 1e-12, "h of 1000 cells in 3-D"); NEAR(gci_h(100, 2), 0.1, 1e-12, "h of 100 cells in 2-D");
    if (fails) { fprintf(stderr, "%d failures\n", fails); return 1; }
    printf("gci: all checks passed\n");
    return 0;
}
