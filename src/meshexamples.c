/* meshexamples.c -- the parts the meshing tutorial meshes (tour.c, user request 2026-11-13: "a tutorial of how to mesh
 * parts, what makes a good mesh, and a couple of examples to go through"). Each is written as an ASCII STL in metres with
 * one solid per boundary, so the STL mesher names the boundaries from the file. */
#include "meshexamples.h"
#include "stlmesh.h"
#include "pl_util.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

const MeshExample MESH_EXAMPLES[MESH_EX__N] = {
    { "duct", "example-duct", "duct.stl", "A square duct", "Water through a 0.5 m duct, 0.1 m square: internal flow" },
    { "sphere", "example-sphere", "sphere.stl", "A sphere in a wind tunnel", "Air past a 0.1 m sphere: external flow, a wake" },
    { "airfoil", "example-airfoil", "naca0012.stl", "A NACA 0012 airfoil (2-D)", "Air past a 1 m chord wing section: a 2-D mesh, one cell thick" },
};

static void tri(FILE *f, const double a[3], const double b[3], const double c[3])
{
    double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    double n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 0) for (int k = 0; k < 3; k++) n[k] /= l;
    fprintf(f, " facet normal %g %g %g\n  outer loop\n", n[0], n[1], n[2]);
    fprintf(f, "   vertex %.9g %.9g %.9g\n   vertex %.9g %.9g %.9g\n   vertex %.9g %.9g %.9g\n", a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]);
    fprintf(f, "  endloop\n endfacet\n");
}
static void quad(FILE *f, const double a[3], const double b[3], const double c[3], const double d[3]) { tri(f, a, b, c); tri(f, a, c, d); }

/* 0.5 m along x, 0.1 m square: inlet at x = 0, outlet at x = 0.5, the four sides walls (normals outward) */
static void duct(FILE *f)
{
    const double L = 0.5, H = 0.1;
    double v[8][3];
    for (int i = 0; i < 8; i++) { v[i][0] = (i & 1) ? L : 0; v[i][1] = (i & 2) ? H : 0; v[i][2] = (i & 4) ? H : 0; }
    fprintf(f, "solid inlet\n");  quad(f, v[0], v[4], v[6], v[2]); fprintf(f, "endsolid inlet\n");
    fprintf(f, "solid outlet\n"); quad(f, v[1], v[3], v[7], v[5]); fprintf(f, "endsolid outlet\n");
    fprintf(f, "solid walls\n");
    quad(f, v[0], v[1], v[5], v[4]);       /* y = 0 */
    quad(f, v[2], v[6], v[7], v[3]);       /* y = H */
    quad(f, v[0], v[2], v[3], v[1]);       /* z = 0 */
    quad(f, v[4], v[5], v[7], v[6]);       /* z = H */
    fprintf(f, "endsolid walls\n");
}

/* diameter 0.1 m at the origin, 48 x 24 facets */
static void sphere(FILE *f)
{
    const double R = 0.05; const int NU = 48, NV = 24;
    fprintf(f, "solid sphere\n");
    for (int j = 0; j < NV; j++)
        for (int i = 0; i < NU; i++) {
            double t0 = M_PI * j / NV, t1 = M_PI * (j + 1) / NV, p0 = 2 * M_PI * i / NU, p1 = 2 * M_PI * (i + 1) / NU;
            double a[3] = { R * sin(t0) * cos(p0), R * sin(t0) * sin(p0), R * cos(t0) };
            double b[3] = { R * sin(t1) * cos(p0), R * sin(t1) * sin(p0), R * cos(t1) };
            double c[3] = { R * sin(t1) * cos(p1), R * sin(t1) * sin(p1), R * cos(t1) };
            double d[3] = { R * sin(t0) * cos(p1), R * sin(t0) * sin(p1), R * cos(t0) };
            if (j > 0) tri(f, a, b, c);
            if (j < NV - 1) tri(f, a, c, d);
        }
    fprintf(f, "endsolid sphere\n");
}

/* NACA 0012, chord 1 m along x, thickness along y, extruded 0.1 m along z; the closed trailing edge form
   (-0.1036 x^4); 120 points a side, closer at the leading and trailing edges. The end caps are solids front / back, which
   the 2-D mesher leaves out (they become the empty front2D / back2D). */
double mesh_example_naca_y(double x)
{
    return 0.6 * (0.2969 * sqrt(x) - 0.1260 * x - 0.3516 * x * x + 0.2843 * x * x * x - 0.1036 * x * x * x * x);
}

static void airfoil(FILE *f)
{
    enum { N = 120 };
    const double Z = 0.1;
    double px[2 * N], py[2 * N];                     /* round the profile: upper from the trailing edge, lower back */
    int n = 0;
    for (int i = 0; i < N; i++) { double x = 0.5 * (1 + cos(M_PI * i / N)); px[n] = x; py[n++] = mesh_example_naca_y(x); }
    for (int i = 0; i < N; i++) { double x = 0.5 * (1 - cos(M_PI * i / N)); px[n] = x; py[n++] = -mesh_example_naca_y(x); }
    fprintf(f, "solid airfoil\n");
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        double a[3] = { px[i], py[i], 0 }, b[3] = { px[j], py[j], 0 }, c[3] = { px[j], py[j], Z }, d[3] = { px[i], py[i], Z };
        quad(f, a, d, c, b);                          /* counter-clockwise round +z seen from outside: normals outward */
    }
    fprintf(f, "endsolid airfoil\n");
    double cx = 0.4, cy = 0;                          /* the profile is convex: a fan from inside it */
    fprintf(f, "solid back\n");
    for (int i = 0; i < n; i++) { int j = (i + 1) % n; double a[3] = { cx, cy, 0 }, b[3] = { px[i], py[i], 0 }, c[3] = { px[j], py[j], 0 }; tri(f, a, c, b); }
    fprintf(f, "endsolid back\nsolid front\n");
    for (int i = 0; i < n; i++) { int j = (i + 1) % n; double a[3] = { cx, cy, Z }, b[3] = { px[i], py[i], Z }, c[3] = { px[j], py[j], Z }; tri(f, a, b, c); }
    fprintf(f, "endsolid front\n");
}

bool mesh_example_write(int k, const char *path)
{
    if (k < 0 || k >= MESH_EX__N) return false;
    FILE *f = fopen(path, "w");
    if (!f) return false;
    if (k == MESH_EX_DUCT) duct(f);
    else if (k == MESH_EX_SPHERE) sphere(f);
    else airfoil(f);
    return fclose(f) == 0;
}

/* the example's case in dir (made already): the STL, its boundaries named and typed from its solids, the flow the
   tutorial describes, mesher/mesher.ini and the dictionaries (mesher_prepare) */
bool mesh_example_setup(int k, const char *dir, char *err, int cap)
{
    if (k < 0 || k >= MESH_EX__N) { snprintf(err, (size_t)cap, "No example %d", k); return false; }
    const MeshExample *ex = &MESH_EXAMPLES[k];
    char stl[PL_PATH]; pl_join(stl, sizeof stl, dir, ex->file);
    pl_mkdirs(dir);
    if (!mesh_example_write(k, stl)) { snprintf(err, (size_t)cap, "Could not write %s", stl); return false; }
    Stl s;
    if (!stl_load(stl, &s)) { snprintf(err, (size_t)cap, "%s", s.err); return false; }
    MeshCfg m; memset(&m, 0, sizeof m); m.scale = 1;
    pl_strlcpy(m.stl, stl, sizeof m.stl);
    stl_split_part(&s, 0, dir);                                /* the file's own solids are the boundaries */
    mesher_defaults(&m, &s);
    m.splitAngle = 0;
    if (k == MESH_EX_SPHERE) {
        m.domain = DOMAIN_EXTERNAL; m.flowDir = DIR_PX; m.upDir = DIR_PZ;
        m.up = 2; m.down = 5; m.side = 2; m.top = 2; m.below = 2;   /* a short tunnel: a minute or two to mesh */
    } else if (k == MESH_EX_AIRFOIL) {
        m.domain = DOMAIN_EXTERNAL; m.twoD = true; m.normal2D = 2; m.flowDir = DIR_PX; m.upDir = DIR_PY;
        m.up = 3; m.down = 6; m.side = 3; m.top = 3; m.below = 3;
    }
    mesher_size_defaults(&m, &s, stl_thickness(&s));
    if (k == MESH_EX_SPHERE) { m.base = 0.01; m.targetPct = 50; m.minPct = 25; }   /* 10 mm, 5 to 2.5 mm on the sphere: 85 000 cells in seconds (the defaults made 360 000) */
    for (int r = 0; r < s.nRegions; r++)
        s.reg[r].type = !strcmp(s.reg[r].name, "inlet") ? BT_INLET : !strcmp(s.reg[r].name, "outlet") ? BT_OUTLET : BT_WALL;
    mesher_take_names(&m, &s);
    bool ok = mesher_prepare(dir, &m, &s, err, cap);
    stl_free(&s);
    return ok;
}
