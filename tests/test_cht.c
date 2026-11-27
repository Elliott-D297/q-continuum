/* test_cht.c -- conjugate heat transfer from choices (improvement plan A2):
 *   test_cht unit <scratch>          a mesh's zones and patches read (made-up polyMesh files), the checks, the files written,
 *                                    a hand edit kept on a second write
 *   test_cht channel <case>          sets up tests/cht_openfoam.sh's channel: a 5 W plate under air at 0.5 m/s
 *   test_cht wall <case>             ... its composite wall: aluminium and steel between 400 K and 300 K
 * The two real cases are run by tests/cht_openfoam.sh, which checks the answers against the hand-worked ones. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chtsetup.h"
#include "pl_util.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("ok    %s\n", msg); else { printf("FAIL  %s\n", msg); fails++; } } while (0)

static void write(const char *dir, const char *rel, const char *text)
{
    char p[PL_PATH]; pl_join(p, sizeof p, dir, rel);
    char d[PL_PATH]; pl_strlcpy(d, p, sizeof d); *strrchr(d, '/') = 0; pl_mkdirs(d);
    pl_write_file(p, text, strlen(text));
}
static bool has(const char *dir, const char *rel, const char *needle)
{
    char p[PL_PATH]; pl_join(p, sizeof p, dir, rel);
    char *t = pl_read_file(p, NULL);
    bool ok = t && strstr(t, needle);
    free(t);
    return ok;
}

static int unit(const char *scratch)
{
    char c[PL_PATH]; pl_join(c, sizeof c, scratch, "cht_unit");
    char cmd[PL_PATH + 16]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", c); if (system(cmd)) return 1;
    write(c, "constant/polyMesh/owner", "FoamFile { note \"nPoints:10 nCells:300 nFaces:20 nInternalFaces:5\"; }\n");
    write(c, "constant/polyMesh/cellZones", "FoamFile { class regIOobject; }\n2\n(\nplate\n{\n    type cellZone;\ncellLabels      List<label> 100\n(\n0 1 2\n)\n;\n}\n"
                                            "chip\n{\n    type cellZone;\ncellLabels      List<label> 50(3 4)\n;\n}\n)\n");
    write(c, "constant/polyMesh/boundary", "FoamFile { class polyBoundaryMesh; }\n4\n(\ninlet { type patch; nFaces 2; startFace 5; }\n"
                                           "outlet { type patch; nFaces 2; startFace 7; }\nwalls { type wall; nFaces 2; startFace 9; }\n"
                                           "frontAndBack { type empty; nFaces 9; startFace 11; }\n)\n");
    CHECK(cht_rest_cells(c) == 150, "cells outside the zones: 300 - 100 - 50");
    ChtSetup s; char err[400];
    CHECK(cht_from_mesh(c, &s, err, sizeof err), "regions from the mesh");
    CHECK(s.nR == 3 && !strcmp(s.r[0].name, "plate") && s.r[2].rest && !strcmp(s.r[2].name, CHT_REST) && s.r[2].kind == CR_FLUID, "two zones and the rest (a fluid)");
    CHECK(s.nP == 3 && s.p[0].role == CP_INLET && s.p[1].role == CP_OUTLET && s.p[2].role == CP_WALL, "patches by name; empty left out");
    char pr[1200], wn[800];
    s.r[1].Q = 3;                                            /* the chip makes 3 W */
    cht_check(&s, pr, sizeof pr, wn, sizeof wn);
    CHECK(!pr[0], "inlet + outlet + a source: runs");
    s.p[1].role = CP_WALL;
    cht_check(&s, pr, sizeof pr, wn, sizeof wn);
    CHECK(strstr(pr, "no outlet") != NULL, "an inlet but no outlet is refused");
    s.p[1].role = CP_OUTLET;
    CHECK(cht_write(c, &s, NULL, 0, err, sizeof err), "written");
    CHECK(has(c, "constant/regionProperties", "fluid       (fluid)") && has(c, "constant/regionProperties", "solid       (plate chip)"), "regionProperties");
    CHECK(has(c, "constant/plate/thermophysicalProperties", "heSolidThermo") && has(c, "constant/plate/thermophysicalProperties", "kappa   167"), "a solid's thermo");
    CHECK(has(c, "constant/fluid/thermophysicalProperties", "perfectGas") && has(c, "constant/fluid/thermophysicalProperties", "Pr              0.7"), "air's thermo (Pr 0.707)");
    CHECK(has(c, "system/chip/fvOptions", "h           (3 0)") && !has(c, "system/plate/fvOptions", "h"), "the chip's 3 W, none in the plate");
    CHECK(has(c, "0.orig/fluid/T", "\"fluid_to_.*\"") && has(c, "0.orig/fluid/T", "kappaMethod     fluidThermo"), "fluid side of the interfaces");
    CHECK(has(c, "0.orig/plate/T", "kappaMethod     solidThermo") && !has(c, "0.orig/plate/T", "inlet"), "a solid has no inlet entry");
    CHECK(has(c, "0.orig/fluid/U", "surfaceNormalFixedValue") && has(c, "0.orig/fluid/p_rgh", "prghPressure"), "inlet speed, outlet pressure");
    CHECK(has(c, "system/topoSetDict", "zones   (plate chip)") && has(c, "system/topoSetDict", "action  invert"), "the rest made a zone");
    CHECK(has(c, "system/controlDict", "chtMultiRegionSimpleFoam") && has(c, "system/controlDict", "outletT_fluid"), "controlDict and reports");
    /* a hand edit kept */
    char p[PL_PATH]; pl_join(p, sizeof p, c, "system/plate/fvSolution");
    char *t = pl_read_file(p, NULL); size_t n = strlen(t);
    char *t2 = malloc(n + 40); snprintf(t2, n + 40, "%s// mine\n", t); pl_write_file(p, t2, strlen(t2)); free(t); free(t2);
    char kept[400];
    s.r[0].T0 = 310;
    CHECK(cht_write(c, &s, kept, sizeof kept, err, sizeof err) && strstr(kept, "system/plate/fvSolution") && has(c, "system/plate/fvSolution", "// mine"),
          "a region file changed by hand is kept on the next write");
    CHECK(has(c, "0.orig/plate/T", "uniform 310"), "... the rest written again");
    ChtSetup b;
    CHECK(cht_load(c, &b) && b.nR == 3 && b.r[1].Q == 3 && b.r[0].T0 == 310 && b.p[0].role == CP_INLET && b.r[2].rest, "cht.ini round trip");
    return fails;
}

static int setup(const char *mode, const char *c)
{
    ChtSetup s; char err[600];
    if (!cht_from_mesh(c, &s, err, sizeof err)) { printf("FAIL  %s\n", err); return 1; }
    s.g[0] = s.g[1] = s.g[2] = 0;
    if (!strcmp(mode, "channel")) {
        int pl = cht_find_region(&s, "plate"), fl = cht_find_region(&s, "air");
        if (pl < 0 || fl < 0) { printf("FAIL  zones plate and air expected\n"); return 1; }
        s.r[pl].kind = CR_SOLID; s.r[pl].mat = 0; s.r[pl].Q = 5;
        s.r[fl].kind = CR_FLUID; s.r[fl].mat = 0; s.r[fl].U0[0] = 0.5;
        int in = cht_find_patch(&s, "inlet");
        s.p[in].U = 0.5; s.p[in].Tin = 300;
        s.endTime = 1500; s.writeEvery = 500;
    } else {
        int a = cht_find_region(&s, "alu"), b = cht_find_region(&s, "steel");
        if (a < 0 || b < 0) { printf("FAIL  zones alu and steel expected\n"); return 1; }
        s.r[a].kind = s.r[b].kind = CR_SOLID; s.r[a].mat = 0; s.r[b].mat = 2;
        int hot = cht_find_patch(&s, "hot"), cold = cht_find_patch(&s, "cold");
        s.p[hot].heat = CT_FIXED; s.p[hot].T = 400; s.p[cold].heat = CT_FIXED; s.p[cold].T = 300;
        s.endTime = 400; s.writeEvery = 400;
    }
    char kept[200];
    if (!cht_write(c, &s, kept, sizeof kept, err, sizeof err)) { printf("FAIL  %s\n", err); return 1; }
    printf("ok    written\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: test_cht unit <scratch> | channel <case> | wall <case>\n"); return 2; }
    if (!strcmp(argv[1], "unit")) { int f = unit(argv[2]); printf(f ? "FAILED (%d)\n" : "all ok\n", f); return f ? 1 : 0; }
    return setup(argv[1], argv[2]);
}
