/* test_meshtools.c -- Mesh > Tools (mesh overhaul plan step 10, meshtools.c).
     test_meshtools unit <scratch>        the patch rename on a written mesh and fields, the scripts' checks
     test_meshtools write <case> <tool>   writes the tool's dictionary into the case and prints its command (meshtools_openfoam.sh) */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "meshtools.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static const char *BND =
    "FoamFile\n{\n    version 2.0;\n    format ascii;\n    class polyBoundaryMesh;\n    location \"constant/polyMesh\";\n    object boundary;\n}\n"
    "// a comment with movingWall { in it\n\n3\n(\n    movingWall\n    {\n        type            wall;\n        inGroups        1(wall);\n        nFaces          20;\n        startFace       760;\n    }\n"
    "    fixedWalls\n    {\n        type            wall;\n        nFaces          60;\n        startFace       780;\n    }\n"
    "    frontAndBack\n    {\n        type            empty;\n        nFaces          800;\n        startFace       840;\n    }\n)\n";
static const char *FIELD =
    "FoamFile { version 2.0; format ascii; class volVectorField; object U; }\ndimensions [0 1 -1 0 0 0 0];\ninternalField uniform (0 0 0);\n"
    "boundaryField\n{\n    movingWall { type fixedValue; value uniform (1 0 0); }\n    fixedWalls { type noSlip; }\n    frontAndBack { type empty; }\n}\n";

static MtIn base(int tool)
{
    MtIn in; memset(&in, 0, sizeof in);
    in.tool = tool; in.scale = 1; in.rotAxis[2] = 1; in.normal[1] = 1;
    in.lo[0] = in.lo[1] = in.lo[2] = 0; in.hi[0] = in.hi[1] = 0.05; in.hi[2] = 0.01;
    snprintf(in.name, sizeof in.name, "porous"); snprintf(in.patches, sizeof in.patches, "movingWall fixedWalls"); snprintf(in.type, sizeof in.type, "wall");
    snprintf(in.zone, sizeof in.zone, "plate");
    return in;
}

static int unit(const char *scratch)
{
    char c[PL_PATH]; pl_join(c, sizeof c, scratch, "mt_case");
    pl_remove_tree(c);
    char d[PL_PATH], p[PL_PATH];
    pl_join(d, sizeof d, c, "constant/polyMesh"); pl_mkdirs(d);
    pl_join(p, sizeof p, d, "boundary"); pl_write_file(p, BND, strlen(BND));
    pl_join(d, sizeof d, c, "0"); pl_mkdirs(d); pl_join(p, sizeof p, d, "U"); pl_write_file(p, FIELD, strlen(FIELD));
    pl_join(d, sizeof d, c, "0.orig"); pl_mkdirs(d); pl_join(p, sizeof p, d, "U"); pl_write_file(p, FIELD, strlen(FIELD));
    char names[16][64], types[16][32];
    int n = mt_patches(c, names, types, 16);
    CHECK(n == 3 && !strcmp(names[0], "movingWall") && !strcmp(types[0], "wall") && !strcmp(names[2], "frontAndBack") && !strcmp(types[2], "empty"),
          "patches read (%d: %s %s)", n, n ? names[0] : "", n ? types[0] : "");
    char err[300]; int files = 0;
    CHECK(mt_rename_patch(c, "movingWall", "lid", &files, err, sizeof err), "rename: %s", err);
    CHECK(files == 3, "rename touched the mesh and two fields (%d)", files);
    n = mt_patches(c, names, types, 16);
    CHECK(n == 3 && !strcmp(names[0], "lid") && !strcmp(types[0], "wall"), "renamed in the mesh");
    pl_join(p, sizeof p, c, "constant/polyMesh/boundary");
    char *t = pl_read_file(p, NULL);
    CHECK(t && strstr(t, "with movingWall { in it") && strstr(t, "    lid\n    {"), "only the patch's name changed (the comment kept)");
    free(t);
    pl_join(p, sizeof p, c, "0/U"); t = pl_read_file(p, NULL);
    CHECK(t && strstr(t, "    lid { type fixedValue; value uniform (1 0 0); }") && !strstr(t, "movingWall"), "renamed in 0/U");
    free(t);
    pl_join(p, sizeof p, c, "0.orig/U"); t = pl_read_file(p, NULL);
    CHECK(t && strstr(t, "lid {"), "renamed in 0.orig/U"); free(t);
    CHECK(!mt_rename_patch(c, "lid", "fixedWalls", NULL, err, sizeof err) && strstr(err, "already"), "a taken name refused");
    CHECK(!mt_rename_patch(c, "nope", "x", NULL, err, sizeof err) && strstr(err, "No patch"), "a missing patch refused");
    CHECK(!mt_rename_patch(c, "lid", "two words", NULL, err, sizeof err) && strstr(err, "valid"), "a bad name refused");
    CHECK(!mt_rename_patch(c, "lid", "9lid", NULL, err, sizeof err), "a name starting with a digit refused");
    /* the scripts */
    char cmd[2000], rel[200], dict[4000];
    MtIn in = base(MT_TRANSFORM); in.scale = 0.001; in.rotDeg = 90; in.move[0] = 1;
    CHECK(mt_script(&in, cmd, sizeof cmd, rel, sizeof rel, dict, sizeof dict, err, sizeof err), "transform: %s", err);
    const char *a = strstr(cmd, "-scale 0.001"), *b = strstr(cmd, "-rotate-angle '((0 0 1) 90)'"), *e = strstr(cmd, "-translate '(1 0 0)'");
    CHECK(a && b && e && a < b && b < e, "transform: scale, turn, move in that order (%s)", cmd);
    in = base(MT_TRANSFORM); CHECK(!mt_script(&in, cmd, sizeof cmd, rel, sizeof rel, dict, sizeof dict, err, sizeof err) && strstr(err, "Nothing"), "transform: nothing to do refused");
    in = base(MT_TRANSFORM); in.scale = NAN; CHECK(!mt_script(&in, cmd, sizeof cmd, rel, sizeof rel, dict, sizeof dict, err, sizeof err), "transform: NaN refused");
    in = base(MT_TRANSFORM); in.rotDeg = 10; in.rotAxis[2] = 0; CHECK(!mt_script(&in, cmd, sizeof cmd, rel, sizeof rel, dict, sizeof dict, err, sizeof err), "transform: no axis refused");
    in = base(MT_MIRROR); CHECK(mt_script(&in, cmd, sizeof cmd, rel, sizeof rel, dict, sizeof dict, err, sizeof err) && strstr(dict, "pointAndNormal") && !strcmp(rel, "system/mirrorMeshDict.qc-tool"), "mirror");
    in = base(MT_MIRROR); in.normal[1] = 0; CHECK(!mt_script(&in, cmd, sizeof cmd, rel, sizeof rel, dict, sizeof dict, err, sizeof err), "mirror: no normal refused");
    in = base(MT_ZONE); CHECK(mt_script(&in, cmd, sizeof cmd, rel, sizeof rel, dict, sizeof dict, err, sizeof err) && strstr(dict, "setToCellZone") && strstr(dict, "boxToCell"), "zone");
    in = base(MT_ZONE); in.hi[0] = -1; CHECK(!mt_script(&in, cmd, sizeof cmd, rel, sizeof rel, dict, sizeof dict, err, sizeof err), "zone: an empty box refused");
    in = base(MT_MERGE); CHECK(mt_script(&in, cmd, sizeof cmd, rel, sizeof rel, dict, sizeof dict, err, sizeof err) && strstr(dict, "patches         (movingWall fixedWalls)"), "merge");
    in = base(MT_MERGE); snprintf(in.type, sizeof in.type, "cyclic"); CHECK(!mt_script(&in, cmd, sizeof cmd, rel, sizeof rel, dict, sizeof dict, err, sizeof err), "merge: an unsupported type refused");
    in = base(MT_BAFFLES); CHECK(mt_script(&in, cmd, sizeof cmd, rel, sizeof rel, dict, sizeof dict, err, sizeof err) && strstr(dict, "plate_master"), "baffles");
    /* pl_shell_quote (sweep 2026-11-21): whole or nothing, never a cut-off path */
    { char q[18]; pl_shell_quote(q, sizeof q, "a'b c"); CHECK(!strcmp(q, "'a'\\''b c'"), "quote with an apostrophe: %s", q);
      pl_shell_quote(q, sizeof q, "/home/user/case"); CHECK(!strcmp(q, "'/home/user/case'"), "exactly fits: %s", q);
      pl_shell_quote(q, sizeof q, "/home/user/cases"); CHECK(!strcmp(q, "''"), "too long: empty, not cut: %s", q); }
    if (fails) { fprintf(stderr, "%d failures\n", fails); return 1; }
    printf("meshtools: all checks passed\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "unit")) return unit(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "write")) {
        static const char *const W[MT__N] = { "transform", "mirror", "refine", "renumber", "zone", "merge", "baffles", "surface" };
        int t = -1; for (int i = 0; i < MT__N; i++) if (!strcmp(argv[3], W[i])) t = i;
        if (t < 0) { fprintf(stderr, "no such tool\n"); return 2; }
        MtIn in = base(t);
        if (t == MT_TRANSFORM) { in.scale = 2; in.rotDeg = 90; in.move[2] = 1; }
        if (t == MT_MIRROR) { in.normal[1] = 0; in.normal[0] = 1; }   /* about x = 0: the cavity grows to -0.1 */
        char cmd[2000], rel[200], dict[4000], err[300];
        if (!mt_script(&in, cmd, sizeof cmd, rel, sizeof rel, dict, sizeof dict, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
        if (rel[0]) { char p[PL_PATH]; pl_join(p, sizeof p, argv[2], rel); pl_write_file(p, dict, strlen(dict)); }
        printf("%s\n", cmd);
        return 0;
    }
    fprintf(stderr, "usage: test_meshtools unit <scratch> | write <case> <tool>\n");
    return 2;
}
