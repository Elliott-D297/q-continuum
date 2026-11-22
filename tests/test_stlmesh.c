/* test_stlmesh.c -- the STL mesher without the GUI:
     test_stlmesh init <stl> <case> [scale]   mesher/mesher.ini with the defaults for that STL, as choosing it does
     test_stlmesh prep <case>                 the mesh dictionaries from mesher/mesher.ini, as Generate does; for a 2-D
                                              mesh it writes the extrudeMesh step's shell lines to <case>/mesher/2d.sh
     test_stlmesh estimate <case>...        the cell estimate for each case's mesher.ini (no files written)
     test_stlmesh regions <scratch>          long pipes split into regions: the small end caps stay their own regions (an
                                              inlet on a long pipe is a tiny share of the area; user's exhaust manifold 2026-11-20)
     test_stlmesh smooth <stl> <out.stl>      a 2-D profile (the NACA airfoil, in mm) made smooth: checks it is closed, keeps
                                              its regions, its corners small, its area and its trailing edge
   tests/stlmesh_mesh.sh then runs the tools */
#define _GNU_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stlmesh.h"
#include "meshexamples.h"

static int init(const char *stl, const char *casePath, double scale)
{
    Stl s;
    if (!stl_load(stl, &s)) { fprintf(stderr, "%s\n", s.err); return 1; }
    MeshCfg m; memset(&m, 0, sizeof m);
    pl_strlcpy(m.stl, stl, sizeof m.stl);
    m.scale = scale;
    stl_set_scale(&s, scale);
    stl_split(&s, 30);                                     /* as the page does: bounds, area and volume first */
    mesher_defaults(&m, &s);
    stl_split_part(&s, m.splitAngle, casePath);
    mesher_take_names(&m, &s);
    pl_mkdirs(casePath);
    bool ok = mesher_save(casePath, &m);
    stl_free(&s);
    return ok ? 0 : 1;
}

#define FAILIF(c, ...) do { if (c) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); return 1; } } while (0)

static int smooth(const char *stl, const char *out)
{
    Stl a, b;
    FAILIF(!stl_load(stl, &a), "%s", a.err);
    stl_set_scale(&a, 0.001); stl_split(&a, 30);
    int n = 0; for (int k = 1; k < 3; k++) if (a.bmax[k] - a.bmin[k] < a.bmax[n] - a.bmin[n]) n = k;
    int np; double kink;
    FAILIF(!stl_profile_facets(&a, n, 30, &np, &kink), "no profile found");
    printf("original: %d points, corners up to %.1f deg, %d regions, area %.6g\n", np, kink, a.nRegions, a.volume / (a.bmax[n] - a.bmin[n]));
    char err[300];
    int no = stl_smooth_profile(&a, n, 30, 0.0025, out, err, sizeof err);
    FAILIF(!no, "%s", err);
    FAILIF(!stl_load(out, &b), "%s", b.err);
    stl_set_scale(&b, 0.001); stl_split(&b, 30);
    int np2; double kink2;
    FAILIF(!stl_profile_facets(&b, n, 30, &np2, &kink2), "the smooth STL has no single profile");
    double A0 = a.volume / (a.bmax[n] - a.bmin[n]), A1 = b.volume / (b.bmax[n] - b.bmin[n]);
    printf("smooth: %d points, corners up to %.2f deg, %d regions, area %.6g, open edges %d\n", np2, kink2, b.nRegions, A1, b.openEdges);
    FAILIF(b.openEdges || b.nonManifold, "not closed: %d open, %d non-manifold edges", b.openEdges, b.nonManifold);
    FAILIF(b.nRegions != a.nRegions, "%d regions, not %d", b.nRegions, a.nRegions);
    for (int r = 0; r < a.nRegions; r++) FAILIF(fabs(a.reg[r].n[n]) > 0.5 && fabs(b.reg[r].n[n]) < 0.5, "region %d isn't a cap any more", r);
    FAILIF(np2 < 400 || kink2 > 2 || kink2 >= kink, "still coarse: %d points, %.2f deg", np2, kink2);
    FAILIF(fabs(A1 - A0) > 0.01 * A0, "the profile's area changed: %.6g, was %.6g", A1, A0);
    for (int k = 0; k < 3; k++) FAILIF(fabs(b.bmax[k] - a.bmax[k]) > 0.002 || fabs(b.bmin[k] - a.bmin[k]) > 0.002, "its box moved along %c", "xyz"[k]);
    stl_free(&a); stl_free(&b);
    printf("ok\n");
    return 0;
}


/* a closed pipe along z, radius R, length L (mm), nseg facets round, as an ASCII STL with one solid (as CAD exports) */
static void write_pipe(const char *path, double R, double L, int nseg, bool square)
{
    FILE *f = fopen(path, "w");
    fprintf(f, "solid part\n");
    #define TRI(a, b, c) fprintf(f, " facet normal 0 0 0\n  outer loop\n   vertex %.6f %.6f %.6f\n   vertex %.6f %.6f %.6f\n   vertex %.6f %.6f %.6f\n  endloop\n endfacet\n", \
                                 a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2])
    if (square) nseg = 4;
    for (int i = 0; i < nseg; i++) {
        double a0 = 2 * M_PI * i / nseg + (square ? M_PI / 4 : 0), a1 = 2 * M_PI * (i + 1) / nseg + (square ? M_PI / 4 : 0);
        double p0[3] = { R * cos(a0), R * sin(a0), 0 }, p1[3] = { R * cos(a1), R * sin(a1), 0 };
        double q0[3] = { p0[0], p0[1], L }, q1[3] = { p1[0], p1[1], L };
        double c0[3] = { 0, 0, 0 }, c1[3] = { 0, 0, L };
        int nz = 40;                                       /* the wall in rings, like a real tessellation */
        for (int k = 0; k < nz; k++) {
            double z0 = L * k / nz, z1 = L * (k + 1) / nz;
            double A[3] = { p0[0], p0[1], z0 }, B[3] = { p1[0], p1[1], z0 }, C[3] = { p1[0], p1[1], z1 }, D[3] = { p0[0], p0[1], z1 };
            TRI(A, B, C); TRI(A, C, D);
        }
        TRI(c0, p1, p0);                                   /* the caps, as fans */
        TRI(c1, q0, q1);
    }
    #undef TRI
    fprintf(f, "endsolid part\n");
    fclose(f);
}

static int regions(const char *dir)
{
    char p[1024];
    pl_mkdirs(dir);
    static const struct { const char *name; double R, L; int nseg; bool square; } P[] = {
        { "duct",  5, 1000, 4, true },    /* 10 mm square duct, 1 m long: each cap 0.25 % of the area */
        { "pipe", 10, 3000, 96, false },  /* round, 20 mm bore, 3 m long: each cap 0.17 %                */
    };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
        snprintf(p, sizeof p, "%s/%s.stl", dir, P[i].name);
        write_pipe(p, P[i].R, P[i].L, P[i].nseg, P[i].square);
        Stl s;
        FAILIF(!stl_load(p, &s), "%s", s.err);
        stl_set_scale(&s, 0.001); stl_split(&s, 30);
        MeshCfg m; memset(&m, 0, sizeof m);
        mesher_defaults(&m, &s);
        char cs[1024]; snprintf(cs, sizeof cs, "%s/%s-case", dir, P[i].name);
        stl_split_part(&s, m.splitAngle, cs);
        int caps = 0, walls = 0;
        for (int r = 0; r < s.nRegions; r++) { if (fabs(s.reg[r].n[2]) > 0.99) caps++; else walls++; }
        printf("%s: split angle %g, %d regions: %d caps, %d wall\n", P[i].name, m.splitAngle, s.nRegions, caps, walls);
        FAILIF(caps != 2, "%s: %d end caps found, not 2 (folded into the wall?)", P[i].name, caps);
        FAILIF(walls != (P[i].square ? 4 : 1), "%s: %d wall regions", P[i].name, walls);
        stl_free(&s);
    }
    printf("ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "estimate")) {
        for (int i = 2; i < argc; i++) {
            MeshCfg m; memset(&m, 0, sizeof m);
            if (!mesher_load(argv[i], &m) || !m.stl[0]) { fprintf(stderr, "no mesher.ini in %s\n", argv[i]); continue; }
            Stl s;
            if (!stl_load(m.stl, &s)) { fprintf(stderr, "%s\n", s.err); continue; }
            stl_set_scale(&s, m.scale > 0 ? m.scale : 1);
            stl_split_part(&s, m.splitAngle, argv[i]);
            mesher_apply_names(&m, &s);
            printf("%.0f %s\n", mesher_estimate_cells(&m, &s), argv[i]);
            stl_free(&s);
        }
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "example")) {     /* the meshing tutorial's example case, as its button makes it */
        char err[300];
        int k = -1; for (int i = 0; i < MESH_EX__N; i++) if (!strcmp(argv[2], MESH_EXAMPLES[i].word)) k = i;
        if (k < 0 || !mesh_example_setup(k, argv[3], err, sizeof err)) { fprintf(stderr, "%s\n", k < 0 ? "no such example" : err); return 1; }
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "location")) {    /* plan step 9 E8: the picked point in the fluid round-trips and is used */
        MeshCfg m; memset(&m, 0, sizeof m);
        FAILIF(!mesher_load(argv[3], &m) || !m.stl[0], "no mesher.ini in %s", argv[3]);
        Stl s; FAILIF(!stl_load(m.stl, &s), "%s", s.err);
        stl_set_scale(&s, m.scale > 0 ? m.scale : 1); stl_split_part(&s, m.splitAngle, argv[3]); mesher_apply_names(&m, &s);
        double c[3] = { (s.bmin[0] + s.bmax[0]) / 2, (s.bmin[1] + s.bmax[1]) / 2, (s.bmin[2] + s.bmax[2]) / 2 }, out[3];
        double far[3] = { s.bmax[0] + (s.bmax[0] - s.bmin[0]), c[1], c[2] };
        m.domain = DOMAIN_INTERNAL;
        FAILIF(!mesher_in_fluid(&m, &s, c), "internal: the centre of a box is in the fluid");
        FAILIF(mesher_in_fluid(&m, &s, far), "internal: a point outside the box isn't");
        mesher_location(&m, &s, out);
        FAILIF(!mesher_in_fluid(&m, &s, out), "internal: the worked-out point is in the fluid");
        double pick[3] = { s.bmin[0] + 0.1 * (s.bmax[0] - s.bmin[0]), c[1], c[2] };
        m.locPicked = true; for (int k = 0; k < 3; k++) m.loc[k] = pick[k];
        FAILIF(!mesher_save(argv[3], &m), "save");
        MeshCfg r; memset(&r, 0, sizeof r);
        FAILIF(!mesher_load(argv[3], &r) || !r.locPicked || fabs(r.loc[0] - pick[0]) > 1e-9 * fabs(pick[0]) + 1e-12, "the picked point round-trips");
        mesher_location(&r, &s, out);
        FAILIF(out[0] != r.loc[0] || out[1] != r.loc[1] || out[2] != r.loc[2], "the picked point is the one used");
        char err[300]; r.mesher = MESHER_SNAPPY;
        FAILIF(!mesher_prepare(argv[3], &r, &s, err, sizeof err), "%s", err);
        char sp[PL_PATH]; pl_join(sp, sizeof sp, argv[3], "system/snappyHexMeshDict");
        char *t = pl_read_file(sp, NULL), want[64]; snprintf(want, sizeof want, "%.6g", pick[0]);
        char *li = t ? strstr(t, "locationInMesh") : NULL;
        FAILIF(!li || !strstr(li, want), "snappyHexMeshDict's locationInMesh is the picked point (%s)", want);
        free(t); stl_free(&s);
        printf("location: all checks passed\n");
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "preset")) {      /* plan step 10 E10: a preset saved, listed and applied (base size scaled to the part) */
        MeshCfg m; memset(&m, 0, sizeof m);
        FAILIF(!mesher_load(argv[3], &m) || !m.stl[0], "no mesher.ini in %s", argv[3]);
        Stl s; FAILIF(!stl_load(m.stl, &s), "%s", s.err);
        stl_set_scale(&s, 1); stl_split_part(&s, m.splitAngle, argv[3]);
        m.layers = 7; m.targetPct = 33; m.mesher = MESHER_SNAPPY; m.base = 0.02; m.prism = true;
        FAILIF(!mesher_preset_save(argv[4], "fine walls", &m, &s), "save");
        FAILIF(mesher_preset_save(argv[4], "../evil", &m, &s), "a name with / refused");
        char names[8][64]; int n = mesher_preset_list(argv[4], names, 8);
        FAILIF(n != 1 || strcmp(names[0], "fine walls"), "listed (%d)", n);
        MeshCfg o; memset(&o, 0, sizeof o); mesher_load(argv[3], &o); o.layers = 2; o.targetPct = 80;
        Stl big = s; stl_set_scale(&big, 2);              /* the same part twice the size: twice the base */
        double L1 = 0, L2 = 0; for (int k = 0; k < 3; k++) { L1 = fmax(L1, s.bmax[k] - s.bmin[k]); }
        FAILIF(!mesher_preset_apply(argv[4], "fine walls", &o, &big), "apply");
        for (int k = 0; k < 3; k++) L2 = fmax(L2, big.bmax[k] - big.bmin[k]);
        FAILIF(o.layers != 7 || o.targetPct != 33 || !o.prism, "the preset's settings taken (%d layers, %g %%)", o.layers, o.targetPct);
        FAILIF(fabs(o.base - 0.02 * L2 / L1) > 1e-9, "the base size scaled with the part (%g, expected %g)", o.base, 0.02 * L2 / L1);
        FAILIF(!o.stl[0] || strcmp(o.stl, m.stl), "the part itself not touched");
        stl_free(&s);
        printf("preset: all checks passed\n");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "regions")) return regions(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "smooth")) return smooth(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "init")) return init(argv[2], argv[3], argc > 4 ? atof(argv[4]) : 1);
    if (argc != 3 || strcmp(argv[1], "prep")) { fprintf(stderr, "usage: test_stlmesh init <stl> <case> [scale] | prep <case>\n"); return 2; }
    MeshCfg m; memset(&m, 0, sizeof m);
    if (!mesher_load(argv[2], &m) || !m.stl[0]) { fprintf(stderr, "no mesher/mesher.ini (or no stl in it) in %s\n", argv[2]); return 1; }
    Stl s;
    if (!stl_load(m.stl, &s)) { fprintf(stderr, "%s\n", s.err); return 1; }
    stl_set_scale(&s, m.scale > 0 ? m.scale : 1);
    stl_split_part(&s, m.splitAngle, argv[2]);
    mesher_apply_names(&m, &s);
    char err[300];
    if (!mesher_prepare(argv[2], &m, &s, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    if (m.twoD) {
        Str sh; str_init(&sh);
        mesher_2d_script(&m, &s, &sh);
        char p[PL_PATH]; pl_join(p, sizeof p, argv[2], "mesher/2d.sh");
        pl_write_file(p, sh.s, sh.len);
        str_free(&sh);
    }
    if (m.mesher == MESHER_FOAMY || (mesher_poly_convert(&m) && m.mesher != MESHER_POLY && !mesher_is_cfmesh(m.mesher))) {
        Str sh; str_init(&sh);                            /* foamy, or a mesh made polyhedral: what Generate runs after the mesher, as mesher/after.sh */
        str_cat(&sh, "set -e\n");
        if (m.mesher == MESHER_FOAMY) {
            str_catf(&sh, "rm -rf constant/polyMesh\n( %s ) > log.foamy 2>&1\n( ", m.twoD ? mesher_foamy2d_cmd() : mesher_foamy_cmd());
            if (m.twoD) mesher_foamy2d_after(&m, &s, &sh);
            mesher_type_script(&m, &s, &sh); str_cat(&sh, ") > log.patchTypes 2>&1\n");
        }
        if (mesher_poly_convert(&m)) str_catf(&sh, "polyDualMesh %g -overwrite -concaveMultiCells > log.polyDualMesh 2>&1\n", m.featureAngle);
        char p[PL_PATH]; pl_join(p, sizeof p, argv[2], "mesher/after.sh");
        pl_write_file(p, sh.s, sh.len);
        str_free(&sh);
    }
    if (mesher_is_cfmesh(m.mesher)) {                     /* what Generate runs (page_stlmesh.c generate), as mesher/cf.sh */
        Str sh; str_init(&sh);
        str_cat(&sh, "set -e\nrm -rf constant/polyMesh\n");
        if (m.twoD) mesher_cf2d_script(&m, true, &sh);
        str_catf(&sh, "surfaceFeatureEdges -angle %g constant/triSurface/cfmesh.stl constant/triSurface/cfmesh.fms > log.surfaceFeatureEdges 2>&1\n", m.featureAngle);
        str_catf(&sh, "%s > log.%s 2>&1\n( ", mesher_program(&m), mesher_program(&m));
        if (m.twoD) mesher_cf2d_script(&m, false, &sh);
        mesher_type_script(&m, &s, &sh);
        str_cat(&sh, ") > log.patchTypes 2>&1\n");
        if (mesher_poly_convert(&m)) str_catf(&sh, "polyDualMesh %g -overwrite -concaveMultiCells > log.polyDualMesh 2>&1\n", m.featureAngle);
        char p[PL_PATH]; pl_join(p, sizeof p, argv[2], "mesher/cf.sh");
        pl_write_file(p, sh.s, sh.len);
        str_free(&sh);
    }
    printf("%d triangles, %d boundaries, about %.0f cells\n", s.nTri, s.nRegions, mesher_estimate_cells(&m, &s));
    stl_free(&s);
    return 0;
}
