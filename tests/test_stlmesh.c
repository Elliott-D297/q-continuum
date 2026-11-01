/* test_stlmesh.c -- the STL mesher without the GUI:
     test_stlmesh init <stl> <case> [scale]   mesher/mesher.ini with the defaults for that STL, as choosing it does
     test_stlmesh prep <case>                 the mesh dictionaries from mesher/mesher.ini, as Generate does; for a 2-D
                                              mesh it writes the extrudeMesh step's shell lines to <case>/mesher/2d.sh
     test_stlmesh smooth <stl> <out.stl>      a 2-D profile (the NACA airfoil, in mm) made smooth: checks it is closed, keeps
                                              its regions, its corners small, its area and its trailing edge
   tests/stlmesh_mesh.sh then runs the tools */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stlmesh.h"

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
    stl_split(&s, m.splitAngle);
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

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "smooth")) return smooth(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "init")) return init(argv[2], argv[3], argc > 4 ? atof(argv[4]) : 1);
    if (argc != 3 || strcmp(argv[1], "prep")) { fprintf(stderr, "usage: test_stlmesh init <stl> <case> [scale] | prep <case>\n"); return 2; }
    MeshCfg m; memset(&m, 0, sizeof m);
    if (!mesher_load(argv[2], &m) || !m.stl[0]) { fprintf(stderr, "no mesher/mesher.ini (or no stl in it) in %s\n", argv[2]); return 1; }
    Stl s;
    if (!stl_load(m.stl, &s)) { fprintf(stderr, "%s\n", s.err); return 1; }
    stl_set_scale(&s, m.scale > 0 ? m.scale : 1);
    stl_split(&s, m.splitAngle);
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
    printf("%d triangles, %d boundaries, about %.0f cells\n", s.nTri, s.nRegions, mesher_estimate_cells(&m, &s));
    stl_free(&s);
    return 0;
}
