/* test_cplbuild.c -- the coupled-case builder: test_cplbuild <fluidCase> <solidCase> <outDir> builds a CHT coupling from two plain
   OpenFOAM cases (k 100 for the laplacianFoam side) and checks the nuances it reports; with --bad it checks the refusals */
#include <stdio.h>
#include <string.h>
#include "cplbuild.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool any(CplCheck *c, int n, int sev, const char *needle)
{
    for (int i = 0; i < n; i++) if (c[i].sev == sev && strstr(c[i].text, needle)) return true;
    return false;
}

int main(int argc, char **argv)
{
    if (argc < 4) { printf("usage: test_cplbuild <fluid> <solid> <out>\n"); return 2; }
    CplBuild b; memset(&b, 0, sizeof b);
    snprintf(b.p[0].name, sizeof b.p[0].name, "Fluid"); snprintf(b.p[0].src, sizeof b.p[0].src, "%s", argv[1]); snprintf(b.p[0].patch, sizeof b.p[0].patch, "interface");
    snprintf(b.p[1].name, sizeof b.p[1].name, "Solid"); snprintf(b.p[1].src, sizeof b.p[1].src, "%s", argv[2]); snprintf(b.p[1].patch, sizeof b.p[1].patch, "interface");
    cplb_inspect(&b.p[0]); cplb_inspect(&b.p[1]);
    CplCheck ck[24];
    /* refusals first: no k for laplacianFoam, a patch that isn't there, the same name twice */
    int n = cplb_check(&b, ck, 24);
    CHECK(any(ck, n, 2, "needs the conductivity k"), "no k refused");
    b.p[1].k = 100;
    snprintf(b.p[1].patch, sizeof b.p[1].patch, "nope");
    n = cplb_check(&b, ck, 24);
    CHECK(any(ck, n, 2, "has no patch nope"), "missing patch refused");
    snprintf(b.p[1].patch, sizeof b.p[1].patch, "interface");
    double dt = b.p[1].deltaT; b.p[1].deltaT = 0.003; b.window = 0.01;
    n = cplb_check(&b, ck, 24);
    CHECK(any(ck, n, 2, "doesn't divide the time window"), "deltaT not dividing the window refused");
    b.p[1].deltaT = dt; b.window = 0;
    n = cplb_check(&b, ck, 24);
    for (int i = 0; i < n; i++) printf("[%d] %s\n", ck[i].sev, ck[i].text);
    CHECK(!any(ck, n, 2, ""), "no refusals for the tutorial pair");
    CHECK(b.p[0].dims == 2 && !strcmp(b.p[0].solverType, "compressible") && !strcmp(b.p[1].solverType, "basic"), "2-D, compressible + basic");
    char err[400];
    CHECK(cplb_write(&b, argv[3], err, sizeof err), "write: %s", err);
    printf(fails ? "FAILED: %d failures\n" : "passed\n", fails);
    return fails != 0;
}
