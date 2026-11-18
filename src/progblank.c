/* progblank.c -- blank projects of the other programs: see progblank.h */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "progblank.h"
#include "assets.h"
#include "pl_util.h"
#include "stlmesh.h"

/* ---------------------------------------------------------------- a surface as triangles */
typedef struct { double *v; int nv, capv; int *t; int nt, capt; } Soup;   /* v: 3 a vertex; t: 3 vertex indices a triangle */

static int soup_v(Soup *s, double x, double y, double z)
{
    if (s->nv == s->capv) { s->capv = s->capv ? s->capv * 2 : 1024; s->v = realloc(s->v, sizeof(double) * 3 * (size_t)s->capv); }
    s->v[3 * s->nv] = x; s->v[3 * s->nv + 1] = y; s->v[3 * s->nv + 2] = z;
    return s->nv++;
}
static void soup_t(Soup *s, int a, int b, int c)
{
    if (a == b || b == c || a == c) return;
    if (s->nt == s->capt) { s->capt = s->capt ? s->capt * 2 : 1024; s->t = realloc(s->t, sizeof(int) * 3 * (size_t)s->capt); }
    s->t[3 * s->nt] = a; s->t[3 * s->nt + 1] = b; s->t[3 * s->nt + 2] = c;
    s->nt++;
}
static void soup_free(Soup *s) { free(s->v); free(s->t); memset(s, 0, sizeof *s); }

static bool read_obj(const char *path, Soup *s, char *err, int cap)
{
    char *txt = pl_read_file(path, NULL);
    if (!txt) { snprintf(err, (size_t)cap, "Could not read %.200s", path); return false; }
    int base = s->nv;
    for (char *l = txt, *next; l && *l; l = next) {
        next = strchr(l, '\n'); if (next) *next++ = 0;
        while (*l == ' ' || *l == '\t') l++;
        if (l[0] == 'v' && (l[1] == ' ' || l[1] == '\t')) {
            double x = 0, y = 0, z = 0;
            if (sscanf(l + 2, "%lf %lf %lf", &x, &y, &z) == 3) soup_v(s, x, y, z);
        } else if (l[0] == 'f' && (l[1] == ' ' || l[1] == '\t')) {
            int idx[64], n = 0;
            char *q = l + 2;
            while (*q && n < 64) {
                while (*q == ' ' || *q == '\t') q++;
                if (!*q || *q == '\r') break;
                long k = strtol(q, &q, 10);
                while (*q && *q != ' ' && *q != '\t') q++;          /* past /uv/normal */
                int vi = k > 0 ? base + (int)k - 1 : k < 0 ? s->nv + (int)k : -1;
                if (vi >= 0 && vi < s->nv) idx[n++] = vi;
            }
            for (int i = 1; i + 1 < n; i++) soup_t(s, idx[0], idx[i], idx[i + 1]);   /* a fan */
        }
    }
    free(txt);
    if (!s->nt) { snprintf(err, (size_t)cap, "%.200s has no faces", pl_basename(path)); return false; }
    return true;
}

/* corners closer than tol become one (sort by quantised position) */
static const double *g_wv; static double g_wtol;
static int cmp_corner(const void *a, const void *b)
{
    int i = *(const int *)a, j = *(const int *)b;
    for (int k = 0; k < 3; k++) {
        double qi = floor(g_wv[3 * i + k] / g_wtol), qj = floor(g_wv[3 * j + k] / g_wtol);
        if (qi != qj) return qi < qj ? -1 : 1;
    }
    return 0;
}
static void weld(Soup *s, double tol)
{
    if (!s->nv) return;
    int *ord = malloc(sizeof(int) * (size_t)s->nv), *map = malloc(sizeof(int) * (size_t)s->nv);
    for (int i = 0; i < s->nv; i++) ord[i] = i;
    g_wv = s->v; g_wtol = tol;
    qsort(ord, (size_t)s->nv, sizeof(int), cmp_corner);
    double *nv = malloc(sizeof(double) * 3 * (size_t)s->nv);
    int n = 0;
    for (int i = 0; i < s->nv; i++) {
        if (i == 0 || cmp_corner(&ord[i - 1], &ord[i])) { memcpy(nv + 3 * n, s->v + 3 * ord[i], sizeof(double) * 3); n++; }
        map[ord[i]] = n - 1;
    }
    free(s->v); s->v = nv; s->nv = s->capv = n;
    int nt = 0;
    for (int i = 0; i < s->nt; i++) {
        int a = map[s->t[3 * i]], b = map[s->t[3 * i + 1]], c = map[s->t[3 * i + 2]];
        if (a == b || b == c || a == c) continue;
        s->t[3 * nt] = a; s->t[3 * nt + 1] = b; s->t[3 * nt + 2] = c; nt++;
    }
    s->nt = nt;
    free(ord); free(map);
}

bool part_to_obj(const char *src, double scale, const char *dstObj, double lo[3], double hi[3], char *err, int cap)
{
    Soup s = { 0 };
    const char *e = strrchr(src, '.');
    if (e && !strcasecmp(e, ".obj")) { if (!read_obj(src, &s, err, cap)) { soup_free(&s); return false; } }
    else if (e && !strcasecmp(e, ".stl")) {
        Stl st; memset(&st, 0, sizeof st);
        if (!stl_load(src, &st)) { snprintf(err, (size_t)cap, "%s", st.err[0] ? st.err : "Could not read the STL"); stl_free(&st); return false; }
        for (int i = 0; i < st.nTri; i++) {
            const float *t = st.tri + 9 * i;
            int a = soup_v(&s, t[0], t[1], t[2]), b = soup_v(&s, t[3], t[4], t[5]), c = soup_v(&s, t[6], t[7], t[8]);
            soup_t(&s, a, b, c);
        }
        stl_free(&st);
    } else { snprintf(err, (size_t)cap, "Use an STL or OBJ file (save STEP or IGES as STL from your CAD program)"); return false; }
    if (!s.nt) { snprintf(err, (size_t)cap, "%.200s has no triangles", pl_basename(src)); soup_free(&s); return false; }
    if (!(scale > 0)) scale = 1;
    double l[3] = { 1e300, 1e300, 1e300 }, h[3] = { -1e300, -1e300, -1e300 };
    for (int i = 0; i < s.nv; i++) for (int k = 0; k < 3; k++) {
        double v = s.v[3 * i + k] *= scale;
        if (v < l[k]) l[k] = v;
        if (v > h[k]) h[k] = v;
    }
    double size = sqrt((h[0] - l[0]) * (h[0] - l[0]) + (h[1] - l[1]) * (h[1] - l[1]) + (h[2] - l[2]) * (h[2] - l[2]));
    if (!(size > 0)) { snprintf(err, (size_t)cap, "The part has no size"); soup_free(&s); return false; }
    double sh[3] = { -(l[0] + h[0]) / 2, -(l[1] + h[1]) / 2, -l[2] };
    for (int i = 0; i < s.nv; i++) for (int k = 0; k < 3; k++) s.v[3 * i + k] += sh[k];
    for (int k = 0; k < 3; k++) { lo[k] = l[k] + sh[k]; hi[k] = h[k] + sh[k]; }
    weld(&s, size * 1e-7);
    Str o; str_init(&o);
    str_catf(&o, "# %s, scaled by %g to metres, centred on x and y, standing on z = 0 (Q-Continuum)\n", pl_basename(src), scale);
    for (int i = 0; i < s.nv; i++) str_catf(&o, "v %.9g %.9g %.9g\n", s.v[3 * i], s.v[3 * i + 1], s.v[3 * i + 2]);
    for (int i = 0; i < s.nt; i++) str_catf(&o, "f %d %d %d\n", s.t[3 * i] + 1, s.t[3 * i + 1] + 1, s.t[3 * i + 2] + 1);
    bool ok = pl_write_file(dstObj, o.s, o.len);
    if (!ok) snprintf(err, (size_t)cap, "Could not write %.200s", dstObj);
    str_free(&o); soup_free(&s);
    return ok;
}

bool block_stl(const char *path, const double d[3], char *err, int cap)
{
    static const int F[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
    static const int N[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { 0, -1, 0 }, { 0, 1, 0 }, { -1, 0, 0 }, { 1, 0, 0 } };
    double c[8][3];
    for (int i = 0; i < 8; i++) { c[i][0] = i & 1 ? d[0] : 0; c[i][1] = i & 2 ? d[1] : 0; c[i][2] = i & 4 ? d[2] : 0; }
    Str o; str_init(&o);
    str_cat(&o, "solid block\n");
    for (int f = 0; f < 6; f++) for (int t = 0; t < 2; t++) {
        int a = F[f][0], b = F[f][t ? 2 : 1], cc = F[f][t ? 3 : 2];
        str_catf(&o, " facet normal %d %d %d\n  outer loop\n", N[f][0], N[f][1], N[f][2]);
        const int v[3] = { a, b, cc };
        for (int k = 0; k < 3; k++) str_catf(&o, "   vertex %.9g %.9g %.9g\n", c[v[k]][0], c[v[k]][1], c[v[k]][2]);
        str_cat(&o, "  endloop\n endfacet\n");
    }
    str_cat(&o, "endsolid block\n");
    bool ok = pl_write_file(path, o.s, o.len);
    if (!ok) snprintf(err, (size_t)cap, "Could not write %.200s", path);
    str_free(&o);
    return ok;
}

/* ---------------------------------------------------------------- the projects */
static bool put_asset(const char *dir, const char *rel, const unsigned char *a, unsigned int n, const char *driver)
{
    char p[PL_PATH], d[PL_PATH]; pl_join(p, sizeof p, dir, rel);
    pl_dirname(d, sizeof d, p);
    if (!pl_mkdirs(d)) return false;
    if (!driver) return pl_write_file(p, (const char *)a, n);
    Str s; str_init(&s);
    const char *t = (const char *)a, *e = t + n;
    for (const char *q = t; q < e; ) {
        const char *at = memmem(q, (size_t)(e - q), "@DRIVER@", 8);   /* the asset has no NUL at its end */
        if (!at) { str_catn(&s, q, (size_t)(e - q)); break; }
        str_catn(&s, q, (size_t)(at - q)); str_cat(&s, driver); q = at + 8;
    }
    bool ok = pl_write_file(p, s.s, s.len);
    str_free(&s);
    return ok;
}

static bool the_part(const char *dir, const char *part, double scale, double lo[3], double hi[3], char *err, int cap)
{
    char g[PL_PATH], o[PL_PATH];
    pl_join(g, sizeof g, dir, "geometry");
    pl_join(o, sizeof o, g, "part.obj");
    if (!pl_mkdirs(g)) { snprintf(err, (size_t)cap, "Could not make %.200s", g); return false; }
    if (!part_to_obj(part, scale, o, lo, hi, err, cap)) return false;
    char cp[PL_PATH]; pl_join(cp, sizeof cp, g, pl_basename(part));   /* the original kept beside it */
    if (strcmp(pl_basename(part), "part.obj")) { size_t n; char *b = pl_read_file(part, &n); if (b) { pl_write_file(cp, b, n); free(b); } }
    return true;
}

static double sig2(double v) { if (!(v > 0)) return v; double p = pow(10, floor(log10(v)) - 1); return round(v / p) * p; }

bool chrono_write_blank(const char *dir, const char *part, double scale, const double block[3], char *err, int cap)
{
    err[0] = 0;
    if (!pl_mkdirs(dir)) { snprintf(err, (size_t)cap, "Could not make %.200s", dir); return false; }
    double lo[3], hi[3];
    if (part && !the_part(dir, part, scale, lo, hi, err, cap)) return false;
    if (!put_asset(dir, "CMakeLists.txt", asset_chrono_CMakeLists_txt, asset_chrono_CMakeLists_txt_len, "body") ||
        !put_asset(dir, "src/body.cpp", asset_body_cpp, asset_body_cpp_len, NULL)) { snprintf(err, (size_t)cap, "Could not write into %.200s", dir); return false; }
    double b[3] = { block ? block[0] : 0.1, block ? block[1] : 0.05, block ? block[2] : 0.02 };
    Str s; str_init(&s);
    str_cat(&s, "# A Chrono project of your own: the part dropped onto the ground (smooth contact). src/body.cpp reads this\n"
                "# file at start: a change needs no rebuild. Edit src/body.cpp to add bodies, joints or motors.\n");
    if (part) str_catf(&s, "mesh = geometry/part.obj   # your part in metres (%s, x%g): %.4g x %.4g x %.4g m. Empty: the block below\n",
                       pl_basename(part), scale, hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
    else str_cat(&s, "mesh =               # a closed surface as OBJ in metres (geometry/part.obj); empty: the block below\n");
    str_catf(&s, "sim_time = 2         # s\ndt = 1e-4            # s\ngravity = 9.81       # m/s^2 (Moon 1.62, Mars 3.71)\n"
                 "density = 7800       # kg/m^3 (steel; aluminium 2700, ABS 1050)\n"
                 "drop_height = 0.1    # m, from its lowest point to the ground\ntilt = 15            # deg, turned about x before the drop\n"
                 "youngs_modulus = 1e7 # Pa, contact stiffness: softer than the real material so the time step can be larger\n"
                 "poisson_ratio = 0.3\nrestitution = 0.4    # 0 dead, 1 perfectly elastic\nfriction = 0.5\n"
                 "box_x = %g          # m, the block when there is no mesh\nbox_y = %g\nbox_z = %g\n", b[0], b[1], b[2]);
    char p[PL_PATH]; pl_join(p, sizeof p, dir, "params.ini");
    bool ok = pl_write_file(p, s.s, s.len);
    str_free(&s);
    if (!ok) snprintf(err, (size_t)cap, "Could not write %.200s", p);
    return ok;
}

bool deme_write_blank(const char *dir, const char *part, double scale, const double box[3], char *err, int cap)
{
    err[0] = 0;
    if (!pl_mkdirs(dir)) { snprintf(err, (size_t)cap, "Could not make %.200s", dir); return false; }
    double lo[3] = { 0 }, hi[3] = { 0 };
    if (part && !the_part(dir, part, scale, lo, hi, err, cap)) return false;
    if (!put_asset(dir, "CMakeLists.txt", asset_CMakeLists_template_txt, asset_CMakeLists_template_txt_len, "pour") ||
        !put_asset(dir, "src/pour.cpp", asset_pour_template_cpp, asset_pour_template_cpp_len, NULL) ||
        !put_asset(dir, "src/qc_deme.hpp", asset_qc_deme_hpp, asset_qc_deme_hpp_len, NULL)) { snprintf(err, (size_t)cap, "Could not write into %.200s", dir); return false; }
    /* the sizes follow the part (or the box): grains about a fortieth of its width, a layer ten grains thick above it */
    double X, Y, H, rmax, fz;
    if (part) {
        double dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2], wmax = fmax(dx, dy);
        rmax = sig2(wmax / 40);
        X = sig2(fmax(1.5 * dx, dx + 20 * rmax)); Y = sig2(fmax(1.5 * dy, dy + 20 * rmax));
        fz = sig2(dz + 4 * rmax);
        H = sig2(fz + 20 * rmax);
    } else {
        X = box ? box[0] : 0.1; Y = box ? box[1] : 0.1; H = box ? box[2] : 0.2;
        rmax = sig2(fmin(X, Y) / 40);
        fz = sig2(2 * rmax);
    }
    double rmin = sig2(0.6 * rmax), fh = sig2(fmin(10 * rmax, H - fz > 4 * rmax ? H - fz - 2 * rmax : 10 * rmax));
    double dt = sig2(0.005 * rmin), g = 9.81, tFall = sqrt(2 * (fz + fh) / g);
    double vmax = ceil(1.5 * sqrt(2 * g * (fz + fh)) + 1);
    Str s; str_init(&s);
    str_cat(&s, "# Q-Continuum DEME parameters for pour: grains poured onto your part (or into the box) settle under gravity.\n"
                "# The driver reads this file each time it starts: change a value and run again, no rebuild.\n"
                "# A comment \"# Label | unit | help\" describes the key on the line below it.\n\n");
    str_catf(&s, "[Time]\n# Simulated time | s\nsim_time = %g\n# Time step | s | Keep it below about a fifth of the Rayleigh time step (shown on the DEME page)\n"
                 "dt = %g\n# Frames per simulated second | 1/s\nfps = 20\n# Largest particle speed | m/s | DEME sizes its contact search by it: set it above the fastest particle\n"
                 "max_velocity = %g\n\n", sig2(tFall + 0.4), dt, vmax);
    str_cat(&s, "[Gravity]\n# Gravity | m/s² | 9.81 Earth, 1.62 Moon, 3.71 Mars\ngravity = 9.81\n\n");
    str_catf(&s, "[Particles]\n# Smallest radius | m\nradius_min = %g\n# Largest radius | m | The same as the smallest for one size\nradius_max = %g\n"
                 "# Size bins | | Sizes from the smallest to the largest, equal numbers of each\nsize_bins = 5\n# Grain density | kg/m³\ndensity = 2500\n"
                 "# Most particles | | The fill region is sampled, then cut to this many\nmax_particles = 20000\n\n", rmin, rmax);
    str_cat(&s, "[Material]\n# Young's modulus | Pa | DEM softens it to allow a larger time step: 1e7 to 1e9 is usual\nyoungs_modulus = 1e8\n"
                "# Poisson's ratio | |\npoisson_ratio = 0.3\n# Restitution | | 0 dead, 1 perfectly elastic\nrestitution = 0.5\n"
                "# Sliding friction | |\nfriction = 0.5\n# Rolling friction | | Stands in for grain shape: angular grains 0.1 to 0.3\nrolling_friction = 0.0\n\n");
    str_cat(&s, "[Domain]\n");
    if (part) str_catf(&s, "# The part | | An OBJ in metres, centred on x and y, on the floor (%s, x%g: %.4g x %.4g x %.4g m). Empty: no part\nmesh = geometry/part.obj\n",
                       pl_basename(part), scale, hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
    else str_cat(&s, "# The part | | An OBJ in metres, centred on x and y, on the floor (geometry/part.obj). Empty: no part\nmesh =\n");
    str_catf(&s, "# Box length | m | Walls all round, open at the top\nbox_x = %g\n# Box width | m\nbox_y = %g\n# Box height | m\nbox_height = %g\n"
                 "# Fill bottom | m | Grains start in a layer from this height\nfill_bottom = %g\n# Fill height | m | The layer's thickness\nfill_height = %g\n",
             X, Y, H, fz, fh);
    char p[PL_PATH]; pl_join(p, sizeof p, dir, "params.ini");
    bool ok = pl_write_file(p, s.s, s.len);
    str_free(&s);
    if (!ok) snprintf(err, (size_t)cap, "Could not write %.200s", p);
    return ok;
}
