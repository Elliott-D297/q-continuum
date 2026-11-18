/* test_meshopts.c -- the STL mesher's "Every OpenFOAM setting" core (meshopts.c) without OpenFOAM:
     test_meshopts <scratch>
   the keyword table is consistent (no keyword twice, defaults among the choices, every dictionary named is one the mesher
   writes), the user's values round-trip through mesher/advanced.ini, they are applied to the dictionaries Generate writes
   (a value, a new keyword, a new and a replaced sub-dictionary, a removed entry, per boundary), and they count in whether
   the mesh is out of date (checkMesh's options don't). tests/meshopts_openfoam.sh meshes with them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "foamdict.h"
#include "meshopts.h"
#include "stlmesh.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

/* a 0.1 m cube, one solid a face (so six boundaries) */
static void write_box(const char *path)
{
    static const int F[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 4, 7, 3 }, { 1, 2, 6, 5 } };
    static const char *const N[6] = { "zmin", "zmax", "ymin", "ymax", "xmin", "xmax" };
    double v[8][3];
    for (int i = 0; i < 8; i++) { v[i][0] = (i == 1 || i == 2 || i == 5 || i == 6) ? 0.1 : 0; v[i][1] = (i == 2 || i == 3 || i == 6 || i == 7) ? 0.1 : 0; v[i][2] = i >= 4 ? 0.1 : 0; }
    FILE *f = fopen(path, "w");
    for (int k = 0; k < 6; k++) {
        fprintf(f, "solid %s\n", N[k]);
        int t[2][3] = { { F[k][0], F[k][1], F[k][2] }, { F[k][0], F[k][2], F[k][3] } };
        for (int j = 0; j < 2; j++) {
            fprintf(f, " facet normal 0 0 0\n  outer loop\n");
            for (int q = 0; q < 3; q++) fprintf(f, "   vertex %g %g %g\n", v[t[j][q]][0], v[t[j][q]][1], v[t[j][q]][2]);
            fprintf(f, "  endloop\n endfacet\n");
        }
        fprintf(f, "endsolid %s\n", N[k]);
    }
    fclose(f);
}

static void table_checks(void)
{
    for (int i = 0; i < MESH_OPTS_N; i++) {
        const MeshOpt *a = &MESH_OPTS[i];
        for (int j = i + 1; j < MESH_OPTS_N; j++)
            CHECK(strcmp(a->file, MESH_OPTS[j].file) || strcmp(a->path, MESH_OPTS[j].path), "%s %s twice in the table", a->file, a->path);
        CHECK(a->label && *a->label && a->group && *a->group, "%s %s: no label or group", a->file, a->path);
        CHECK(((a->flags & (MO_PATCH | MO_REGION)) != 0) == (strstr(a->path, "%s") != NULL), "%s %s: %%s and its flag disagree", a->file, a->path);
        CHECK(((a->flags & MO_PART) != 0) == (strchr(a->path, '@') != NULL), "%s %s: @ and its flag disagree", a->file, a->path);
        if (a->type == MO_BOOL) CHECK(a->choices && strchr(a->choices, '|') && !strchr(strchr(a->choices, '|') + 1, '|'), "%s %s: a switch needs two words", a->file, a->path);
        if ((a->type == MO_BOOL || a->type == MO_WORD) && a->choices && a->dflt && a->dflt[0] != '=') {
            char err[160];
            CHECK(mo_check(a, a->dflt, err, sizeof err), "%s %s: default %s isn't one of %s", a->file, a->path, a->dflt, a->choices);
        }
        if ((a->type == MO_INT || a->type == MO_REAL) && a->dflt && a->dflt[0] != '=' && strcmp(a->dflt, "unlimited")) {
            char err[160];
            CHECK(mo_check(a, a->dflt, err, sizeof err), "%s %s: default %s isn't a number", a->file, a->path, a->dflt);
        }
    }
}

/* every dictionary the table names for a mesher is one its preview writes, and each keyword's parent exists or can be
   made (a per-boundary keyword resolves to a dictionary the mesher writes for that boundary) */
static void preview_checks(const char *label, MeshCfg *m, Stl *s)
{
    MeshPreview pv;
    CHECK(mesher_preview(m, s, &pv), "%s: no preview", label);
    unsigned mask = mo_active_mask(m->mesher, m->twoD, mesher_poly_convert(m));
    int nUsed = 0;
    for (int i = 0; i < MESH_OPTS_N; i++) {
        const MeshOpt *op = &MESH_OPTS[i];
        if (!(op->when & mask) || !strncmp(op->file, "cmd:", 4)) continue;
        const char *t = mesher_preview_text(&pv, op->file);
        CHECK(t != NULL, "%s: the table has %s %s but the mesher writes no %s", label, op->file, op->path, op->file);
        if (!t) continue;
        nUsed++;
        if (op->flags & MO_PART) {                        /* the @ is the first sub-dictionary there */
            FDFile *f = fd_from_text(op->file, t);
            char dir[256]; snprintf(dir, sizeof dir, "%.*s", (int)(strchr(op->path, '@') - op->path - 1), op->path);
            FDEntry *d = fd_find(f, dir);
            bool any = false; for (int k = 0; d && k < d->nChild; k++) if (d->child[k].kind == FD_DICT) any = true;
            CHECK(any, "%s: %s has no sub-dictionary for %s", label, dir, op->path);
            fd_free(f);
        }
    }
    printf("%s: %d dictionaries, %d keywords in the table\n", label, pv.n, nUsed);
    CHECK(nUsed > 10, "%s: only %d keywords", label, nUsed);
    mesher_preview_free(&pv);
}

/* test_meshopts defaults <case>: every keyword of the case's mesher with a plain OpenFOAM default set to it in
   mesher/advanced.ini (not the per-boundary / per-region ones: those would retype every patch), so a real run shows that
   OpenFOAM takes each keyword and value as the table writes them */
static int defaults(const char *casePath)
{
    MeshCfg m; memset(&m, 0, sizeof m);
    if (!mesher_load(casePath, &m)) { fprintf(stderr, "no mesher.ini in %s\n", casePath); return 1; }
    unsigned mask = mo_active_mask(m.mesher, m.twoD, m.polyConvert && !m.twoD);
    MeshOverrides o; mo_init(&o);
    for (int i = 0; i < MESH_OPTS_N; i++) {
        const MeshOpt *op = &MESH_OPTS[i];
        if (!(op->when & mask) || !op->dflt || op->dflt[0] == '=' || !strcmp(op->dflt, "unlimited") || (op->flags & (MO_PATCH | MO_REGION | MO_PART))) continue;
        if (op->type == MO_TEXT && !strcmp(op->dflt, "()")) continue;
        mo_set(&o, op->file, op->path, op->dflt);
    }
    bool ok = mo_save(casePath, &o);
    printf("%d keywords set to OpenFOAM's defaults\n", o.n);
    mo_free(&o);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "defaults")) return defaults(argv[2]);
    if (argc != 2) { fprintf(stderr, "usage: test_meshopts <scratch>\n"); return 2; }
    char d[PL_PATH - 128]; snprintf(d, sizeof d, "%s/meshopts", argv[1]);
    pl_remove_tree(d); pl_mkdirs(d);
    char stl[PL_PATH + 16]; snprintf(stl, sizeof stl, "%s/box.stl", d);
    write_box(stl);
    table_checks();
    printf("table: %d keywords\n", MESH_OPTS_N);

    Stl s; CHECK(stl_load(stl, &s), "box: %s", s.err);
    stl_split(&s, 30);
    MeshCfg m; memset(&m, 0, sizeof m);
    pl_strlcpy(m.stl, stl, sizeof m.stl); m.scale = 1;
    mesher_defaults(&m, &s);
    m.splitAngle = 0;                                     /* the file's own solids: six boundaries */
    stl_split(&s, 0);
    mesher_take_names(&m, &s);
    CHECK(s.nRegions == 6, "box: %d boundaries", s.nRegions);
    static const struct { int mesher; bool poly; const char *label; } MS[] = {
        { MESHER_SNAPPY, false, "snappyHexMesh" }, { MESHER_CFCART, true, "cfMesh cartesianMesh + polyDualMesh" }, { MESHER_FOAMY, false, "foamyHexMesh" } };
    for (size_t k = 0; k < sizeof MS / sizeof *MS; k++) {
        MeshCfg mm = m; mm.mesher = MS[k].mesher; mm.polyConvert = MS[k].poly;
        preview_checks(MS[k].label, &mm, &s);
    }
    {   /* external, in a tunnel: the refinement region round the body is a per-region name */
        MeshCfg mm = m; mm.domain = DOMAIN_EXTERNAL; mesher_size_defaults(&mm, &s, 0);
        preview_checks("snappyHexMesh in a tunnel", &mm, &s);
    }

    /* the file round-trips */
    MeshOverrides o; mo_init(&o);
    mo_set(&o, "system/snappyHexMeshDict", "castellatedMeshControls/nCellsBetweenLevels", "3");
    mo_set(&o, "system/snappyHexMeshDict", "castellatedMeshControls/gapLevelIncrement", "2");
    mo_set(&o, "system/snappyHexMeshDict", "addLayersControls/layers/xmax/nSurfaceLayers", "4");
    mo_set(&o, "system/snappyHexMeshDict", "mergeTolerance", MO_REMOVE);
    mo_set(&o, "system/snappyHexMeshDict", "castellatedMeshControls/limitRegions/box", "{ mode inside; levels ((1e15 1)); }");
    mo_set(&o, "system/snappyHexMeshDict", "snapControls", "{ nSmoothPatch 5; tolerance 3.0; nSolveIter 40; nRelaxIter 4; }");
    mo_set(&o, "system/meshQualityDict", "maxNonOrtho", "70");
    mo_set(&o, "cmd:checkMesh", "-allGeometry", "true");
    mo_set(&o, "cmd:polyDualMesh", "extra", "-splitAllFaces; rm -rf /");
    CHECK(o.n == 9, "%d values", o.n);
    mo_set(&o, "cmd:polyDualMesh", "extra", NULL);
    CHECK(o.n == 8 && !mo_get(&o, "cmd:polyDualMesh", "extra"), "unset failed");
    Str t; str_init(&t); mo_text(&o, &t);
    MeshOverrides o2; mo_init(&o2); mo_parse(t.s, &o2);
    CHECK(o2.n == o.n, "round trip: %d of %d", o2.n, o.n);
    for (int i = 0; i < o.n; i++) {
        const char *v = mo_get(&o2, o.s[i].file, o.s[i].path);
        CHECK(v && !strcmp(v, o.s[i].value), "round trip lost %s %s", o.s[i].file, o.s[i].path);
    }
    CHECK(mo_cmd_flag(&o, "checkMesh", "-allGeometry", false) && mo_cmd_flag(&o, "polyDualMesh", "-concaveMultiCells", true), "flags");
    CHECK(!mo_safe_args("-x; rm -rf /") && !mo_safe_args("$(id)") && mo_safe_args("-writeSets vtk"), "argument check");
    {   /* values checked by kind */
        char err[160];
        const MeshOpt *nb = NULL, *vec = NULL, *sw = NULL;
        for (int i = 0; i < MESH_OPTS_N; i++) {
            if (!strcmp(MESH_OPTS[i].path, "castellatedMeshControls/nCellsBetweenLevels")) nb = &MESH_OPTS[i];
            if (!strcmp(MESH_OPTS[i].path, "castellatedMeshControls/locationInMesh")) vec = &MESH_OPTS[i];
            if (!strcmp(MESH_OPTS[i].path, "snapControls/implicitFeatureSnap")) sw = &MESH_OPTS[i];
        }
        CHECK(nb && mo_check(nb, "3", err, sizeof err) && !mo_check(nb, "3.5", err, sizeof err) && !mo_check(nb, "x", err, sizeof err), "int check");
        CHECK(vec && mo_check(vec, "(0.1 0 -2e-3)", err, sizeof err) && !mo_check(vec, "(1 2)", err, sizeof err), "vector check");
        CHECK(sw && mo_check(sw, "true", err, sizeof err) && !mo_check(sw, "yes", err, sizeof err), "switch check");
        CHECK(mo_check_text("((1e15 2))", err, sizeof err) && !mo_check_text("((1e15 2)", err, sizeof err) && !mo_check_text("1; foo 2", err, sizeof err)
              && mo_check_text("{ a 1; b 2; }", err, sizeof err), "text check");
    }

    /* Generate applies them */
    char cp[PL_PATH + 16]; snprintf(cp, sizeof cp, "%s/case", d);
    pl_mkdirs(cp);
    CHECK(mo_save(cp, &o), "save");
    char err[300];
    m.mesher = MESHER_SNAPPY; m.prism = true; m.layers = 3;
    CHECK(mesher_prepare(cp, &m, &s, err, sizeof err), "prepare: %s", err);
    char p[PL_PATH + 64]; snprintf(p, sizeof p, "%s/system/snappyHexMeshDict", cp);
    FDFile *f = fd_load(p);
    CHECK(f && f->ok, "snappyHexMeshDict doesn't parse after the values: %s", f ? f->err : "missing");
    char v[256];
    CHECK(f && fd_get(f, "castellatedMeshControls/nCellsBetweenLevels", v, sizeof v) && !strcmp(v, "3"), "nCellsBetweenLevels = %s", v);
    CHECK(f && fd_get(f, "castellatedMeshControls/gapLevelIncrement", v, sizeof v) && !strcmp(v, "2"), "a new keyword: %s", v);
    CHECK(f && fd_get(f, "addLayersControls/layers/xmax/nSurfaceLayers", v, sizeof v) && !strcmp(v, "4"), "per boundary: %s", v);
    CHECK(f && fd_get(f, "addLayersControls/layers/xmin/nSurfaceLayers", v, sizeof v) && !strcmp(v, "3"), "the others kept: %s", v);
    CHECK(f && !fd_find(f, "mergeTolerance"), "mergeTolerance not removed");
    CHECK(f && fd_get(f, "castellatedMeshControls/limitRegions/box/mode", v, sizeof v) && !strcmp(v, "inside"), "new sub-dictionary: %s", v);
    CHECK(f && fd_get(f, "snapControls/nSmoothPatch", v, sizeof v) && !strcmp(v, "5") && !fd_find(f, "snapControls/nFeatureSnapIter"),
          "replaced sub-dictionary");
    CHECK(f && fd_get(f, "castellatedMeshControls/maxLocalCells", v, sizeof v), "the rest kept");
    fd_free(f);
    snprintf(p, sizeof p, "%s/system/meshQualityDict", cp);
    f = fd_load(p);
    CHECK(f && fd_get(f, "maxNonOrtho", v, sizeof v) && !strcmp(v, "70"), "meshQualityDict maxNonOrtho = %s", v);
    fd_free(f);

    /* the mesh is current; a value that changes the mesh makes it out of date, checkMesh's options don't */
    MeshCfg mm = m; mesher_take_names(&mm, &s);
    CHECK(mesher_mesh_current(cp, &mm) == 1, "not current after prepare");
    mo_set(&o, "cmd:checkMesh", "-allTopology", "true"); mo_save(cp, &o);
    CHECK(mesher_mesh_current(cp, &mm) == 1, "checkMesh's options made the mesh out of date");
    mo_set(&o, "system/snappyHexMeshDict", "castellatedMeshControls/nCellsBetweenLevels", "4"); mo_save(cp, &o);
    CHECK(mesher_mesh_current(cp, &mm) == 0, "a changed value didn't make the mesh out of date");

    /* a value that can't go in stops Generate with a reason */
    mo_set(&o, "system/snappyHexMeshDict", "castellatedMeshControls", "2"); mo_save(cp, &o);
    CHECK(!mesher_prepare(cp, &m, &s, err, sizeof err) && strstr(err, "dictionary"), "a plain value over a dictionary was accepted (%s)", err);

    /* none left: no file, and the signature is as before the panel existed */
    o.n = 0; mo_save(cp, &o);
    snprintf(p, sizeof p, "%s/%s", cp, MO_INI);
    CHECK(!pl_exists(p), "advanced.ini kept with nothing in it");
    Str sg; str_init(&sg); mo_sig_text(cp, &sg);
    CHECK(sg.len == 0, "signature without values: '%s'", sg.s);

    mo_free(&o); mo_free(&o2); str_free(&t); str_free(&sg); stl_free(&s);
    if (fails) { fprintf(stderr, "%d failed\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
