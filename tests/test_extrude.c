/* test_extrude.c -- extrudeMesh dictionaries (mesh overhaul plan step 6, extrude.c).
     test_extrude axi <case> <axisDir 0-2> <axisAt> <angle>    the 2-D case's three dictionaries into <case>/system
     test_extrude patch <case> <patch> <model> <layers> <thickness>
     test_extrude unit                                          what is refused
   tests/extrude_openfoam.sh runs the tools on the results. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extrude.h"
#include "pl_util.h"
#include "precheck.h"

static int put(const char *dir, const char *name, char *text)
{
    char p[1024]; snprintf(p, sizeof p, "%s/system/%s", dir, name);
    int ok = text && pl_write_file(p, text, strlen(text));
    free(text);
    return ok;
}

int main(int argc, char **argv)
{
    char why[400];
    if (argc > 1 && !strcmp(argv[1], "unit")) {
        int fails = 0;
        AxiCfg a = { .thin = 2, .axisDir = 0, .axisAt = 0, .angle = 5, .lo = { 0, -0.05, 0 }, .hi = { 1, 0.05, 0.01 } };
        snprintf(a.empties, sizeof a.empties, "frontAndBack");
        char *t, *c, *e;
        if (ex_axi_dicts(&a, &t, &c, &e, why, sizeof why) || !strstr(why, "crosses the axis")) { fprintf(stderr, "FAIL: crossing the axis not refused (%s)\n", why); fails++; }
        a.lo[1] = 0;
        if (!ex_axi_dicts(&a, &t, &c, &e, why, sizeof why) || !strstr(e, "axis            (-1 0 0)")) { fprintf(stderr, "FAIL: above the x axis: axis -x\n"); fails++; }
        free(t); free(c); free(e);
        a.lo[1] = -0.05; a.hi[1] = 0;
        if (!ex_axi_dicts(&a, &t, &c, &e, why, sizeof why) || !strstr(e, "axis            (1 0 0)")) { fprintf(stderr, "FAIL: below the x axis: axis +x\n"); fails++; }
        free(t); free(c); free(e);
        a.axisDir = 2;
        if (ex_axi_dicts(&a, &t, &c, &e, why, sizeof why)) { fprintf(stderr, "FAIL: an axis across the plane accepted\n"); fails++; }
        a.axisDir = 0; a.angle = 30;
        if (ex_axi_dicts(&a, &t, &c, &e, why, sizeof why)) { fprintf(stderr, "FAIL: 30 degrees accepted\n"); fails++; }
        ExtrudeCfg x = { .model = EX_LINEAR_NORMAL, .nLayers = 10, .expansion = 1, .thickness = 0 };
        snprintf(x.patch, sizeof x.patch, "outlet");
        if (ex_patch_dict(&x, why, sizeof why)) { fprintf(stderr, "FAIL: zero thickness accepted\n"); fails++; }
        x.thickness = 0.5;
        char *d = ex_patch_dict(&x, why, sizeof why);
        if (!d || !strstr(d, "constructFrom   mesh;") || !strstr(d, "sourcePatches   (outlet);") || !strstr(d, "thickness       0.5;")) { fprintf(stderr, "FAIL: linearNormal dict\n"); fails++; }
        free(d);
        if (fails) return 1;
        printf("extrude unit: all passed\n");
        return 0;
    }
    if (argc > 5 && !strcmp(argv[1], "axi")) {
        AxiCfg a = { .thin = 2, .axisDir = atoi(argv[3]), .axisAt = atof(argv[4]), .angle = atof(argv[5]) };
        if (!mesh_bounds(argv[2], a.lo, a.hi)) { fprintf(stderr, "no mesh in %s\n", argv[2]); return 1; }
        snprintf(a.empties, sizeof a.empties, "frontAndBack");
        char *t, *c, *e;
        if (!ex_axi_dicts(&a, &t, &c, &e, why, sizeof why)) { printf("refused: %s\n", why); return 3; }
        return put(argv[2], "topoSetDict.axi", t) && put(argv[2], "createPatchDict.axi", c) && put(argv[2], "extrudeMeshDict", e) ? 0 : 1;
    }
    if (argc > 6 && !strcmp(argv[1], "patch")) {
        ExtrudeCfg x = { .model = atoi(argv[4]), .nLayers = atoi(argv[5]), .expansion = 1.1, .thickness = atof(argv[6]) };
        snprintf(x.patch, sizeof x.patch, "%s", argv[3]);
        char *d = ex_patch_dict(&x, why, sizeof why);
        if (!d) { fprintf(stderr, "%s\n", why); return 1; }
        return put(argv[2], "extrudeMeshDict", d) ? 0 : 1;
    }
    return 2;
}
