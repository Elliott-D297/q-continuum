/* test_viz.c -- Results > Visualization's engine (viz.c) on a real blockMesh case with fields whose answers are known.
     test_viz write <case>      writes 0/T (2x + 3y - z + 1), 0/U (solid rotation, omega 2, about the z line through
                                (0.5 0.5)) and 0/p (kinematic) onto the case's mesh, cell centres and boundary faces exact
     test_viz check <case> [time]   checks the engine against them (time defaults to 0)
     test_viz same <caseA> <caseB>  the fields of two cases (ASCII and the same converted to binary) load the same
   tests/viz_openfoam.sh makes the cube with blockMesh, runs these, and converts to binary with foamFormatConvert. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pl_util.h"
#include "viz.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static double T_at(const double *x) { return 2 * x[0] + 3 * x[1] - x[2] + 1; }
static const double W = 2;                       /* omega */
static void U_at(const double *x, double *u) { u[0] = -W * (x[1] - 0.5); u[1] = W * (x[0] - 0.5); u[2] = 0; }

static void write_field(const VzMesh *m, const char *path, const char *cls, const char *dims, int nc, bool isU)
{
    Str o; str_init(&o);
    str_catf(&o, "FoamFile { version 2.0; format ascii; class %s; object x; }\ndimensions %s;\ninternalField nonuniform List<%s> %ld\n(\n",
             cls, dims, nc == 3 ? "vector" : "scalar", m->nCells);
    for (long c = 0; c < m->nCells; c++) {
        const double *x = &m->cCtr[3 * c];
        if (isU) { double u[3]; U_at(x, u); str_catf(&o, "(%.17g %.17g %.17g)\n", u[0], u[1], u[2]); }
        else str_catf(&o, "%.17g\n", nc == 1 && dims[1] == '0' && dims[3] == '2' ? T_at(x) * 10 : T_at(x));
    }
    str_cat(&o, ");\nboundaryField\n{\n");
    for (int p = 0; p < m->nPatch; p++) {
        str_catf(&o, "    %s\n    {\n        type fixedValue;\n        value nonuniform List<%s> %ld\n(\n", m->patch[p].name, nc == 3 ? "vector" : "scalar", m->patch[p].nFaces);
        for (long f = m->patch[p].start; f < m->patch[p].start + m->patch[p].nFaces; f++) {
            const double *x = &m->fCtr[3 * f];
            if (isU) { double u[3]; U_at(x, u); str_catf(&o, "(%.17g %.17g %.17g)\n", u[0], u[1], u[2]); }
            else str_catf(&o, "%.17g\n", nc == 1 && dims[1] == '0' && dims[3] == '2' ? T_at(x) * 10 : T_at(x));
        }
        str_cat(&o, ");\n    }\n");
    }
    str_cat(&o, "}\n");
    pl_write_file(path, o.s, (size_t)o.len);
    str_free(&o);
}

static void load_mesh(VzMesh *m, const char *cs)
{
    char d[1024]; snprintf(d, sizeof d, "%s/constant/polyMesh", cs);
    if (!vz_mesh_load(m, d)) { fprintf(stderr, "FAIL: mesh: %s\n", m->err); exit(1); }
}

static double tri_area(const VzTris *t)
{
    double a = 0;
    for (long i = 0; i < t->n; i++) {
        const float *v = &t->xyz[9 * i];
        double e0[3] = { v[3] - v[0], v[4] - v[1], v[5] - v[2] }, e1[3] = { v[6] - v[0], v[7] - v[1], v[8] - v[2] };
        double c[3] = { e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2], e0[0] * e1[1] - e0[1] * e1[0] };
        a += 0.5 * sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    }
    return a;
}

static void check(const char *cs, const char *time)
{
    VzMesh m; load_mesh(&m, cs);
    CHECK(m.nCells == 1000, "cells %ld", m.nCells);
    double vol = 0; for (long c = 0; c < m.nCells; c++) vol += m.cVol[c];
    CHECK(fabs(vol - 1) < 1e-9, "volume %g", vol);
    CHECK(fabs(m.h - 0.1) < 1e-9, "cell size %g", m.h);
    VzField T, U, p;
    CHECK(vz_field_load(&T, &m, cs, time, "T") && T.nc == 1, "T loads");
    CHECK(vz_field_load(&U, &m, cs, time, "U") && U.nc == 3, "U loads");
    CHECK(vz_field_load(&p, &m, cs, time, "p") && p.nc == 1 && !strcmp(p.dims, "[0 2 -2 0 0 0 0]"), "p loads, dims '%s'", p.dims);
    if (fails) exit(1);
    /* point values: the linear field exactly, inside and on the boundary */
    double worst = 0;
    for (long i = 0; i < m.nPoints; i++) worst = fmax(worst, fabs(vz_point(&T, VQ_VALUE, i) - T_at(&m.pt[3 * i])));
    CHECK(worst < 1e-9, "point values off by %g", worst);
    /* gradients: (2 3 -1) everywhere; vorticity 2 omega; Q omega^2 for pure rotation */
    double *g = vz_gradient(&m, &T, 0);
    worst = 0;
    for (long c = 0; c < m.nCells; c++) worst = fmax(worst, fabs(g[3 * c] - 2) + fabs(g[3 * c + 1] - 3) + fabs(g[3 * c + 2] + 1));
    CHECK(worst < 1e-9, "grad T off by %g", worst);
    free(g);
    VzField d;
    CHECK(vz_derived(&d, &m, VD_VORTMAG, NULL, &U, 0), "vorticity");
    worst = 0; for (long c = 0; c < m.nCells; c++) worst = fmax(worst, fabs(d.c[c] - 2 * W));
    CHECK(worst < 1e-9, "vorticity off by %g", worst);
    vz_field_free(&d);
    CHECK(vz_derived(&d, &m, VD_Q, NULL, &U, 0), "Q");
    worst = 0; for (long c = 0; c < m.nCells; c++) worst = fmax(worst, fabs(d.c[c] - W * W));
    CHECK(worst < 1e-9, "Q off by %g", worst);
    vz_field_free(&d);
    CHECK(vz_derived(&d, &m, VD_PPA, &p, NULL, 1.2), "p in Pa");
    CHECK(fabs(d.c[0] - p.c[0] * 1.2) < 1e-12 && !strcmp(d.dims, "[1 -1 -2 0 0 0 0]"), "p x rho");
    vz_field_free(&d);
    CHECK(vz_derived(&d, &m, VD_PTOTAL, &p, &U, 1.2), "total pressure");
    { const double *u = &U.c[0]; CHECK(fabs(d.c[0] - 1.2 * (p.c[0] + 0.5 * (u[0] * u[0] + u[1] * u[1]))) < 1e-9, "total pressure value"); }
    vz_field_free(&d);
    /* statistics: the mean of T over the cube is T at its centre, 3 */
    VzStats st; vz_stats_cells(&m, &T, VQ_VALUE, &st);
    CHECK(fabs(st.mean - 3) < 1e-9, "mean T %g", st.mean);
    double bins[10]; vz_histogram(&m, &T, VQ_VALUE, st.min, st.max, 10, bins);
    double sb = 0; for (int i = 0; i < 10; i++) sb += bins[i];
    CHECK(fabs(sb - 1) < 1e-9, "histogram sums to %g", sb);
    /* location and sampling */
    int found = 0; double sworst = 0;
    long hint = -1;
    for (int i = 0; i < 200; i++) {
        double x[3] = { 0.013 + 0.97 * fmod(i * 0.61803, 1), 0.013 + 0.97 * fmod(i * 0.41421, 1), 0.013 + 0.97 * fmod(i * 0.73205, 1) };
        long c = vz_find_cell(&m, x, i % 2 ? hint : -1);
        if (c >= 0) {
            found++;
            bool in = true;
            for (int k = 0; k < 3; k++) in = in && fabs(x[k] - m.cCtr[3 * c + k]) <= 0.05 + 1e-12;
            CHECK(in, "point (%g %g %g) put in cell %ld centred (%g %g %g)", x[0], x[1], x[2], c, m.cCtr[3 * c], m.cCtr[3 * c + 1], m.cCtr[3 * c + 2]);
            hint = c;
            double v; vz_sample(&m, &T, VQ_VALUE, x, &c, &v);
            sworst = fmax(sworst, fabs(v - T_at(x)));
        }
    }
    CHECK(found == 200, "found %d of 200 points", found);
    CHECK(sworst < 0.25, "sampling off by %g (T spans 0..6)", sworst);
    double out[3] = { 1.5, 0.5, 0.5 };
    CHECK(vz_find_cell(&m, out, -1) == -1, "a point outside: no cell");
    /* the boundary: area 6, the face values exact */
    VzTris t; vz_boundary(&m, &T, VQ_VALUE, NULL, false, &t);
    CHECK(fabs(tri_area(&t) - 6) < 1e-9, "boundary area %g", tri_area(&t));
    vz_tris_free(&t);
    vz_boundary(&m, &T, VQ_VALUE, NULL, true, &t);
    worst = 0;
    for (long i = 0; i < t.n; i++) for (int k = 1; k < 3; k++) { double x[3] = { t.xyz[9 * i + 3 * k], t.xyz[9 * i + 3 * k + 1], t.xyz[9 * i + 3 * k + 2] }; worst = fmax(worst, fabs(t.val[3 * i + k] - T_at(x))); }
    CHECK(worst < 1e-5, "smooth boundary values off by %g", worst);
    /* iso-lines on it: every point at its level */
    VzLines l; vz_isolines(&t, 0.5, 5.5, 5, false, &l);
    worst = 0;
    for (long i = 0; i < l.nv; i++) { double x[3] = { l.xyz[3 * i], l.xyz[3 * i + 1], l.xyz[3 * i + 2] }; worst = fmax(worst, fabs(T_at(x) - l.val[i])); }
    CHECK(l.nl > 20 && worst < 1e-5, "iso-lines: %ld segments, off by %g", l.nl, worst);
    vz_lines_free(&l);
    /* picking: straight down onto the top face (z = 1) */
    double o[3] = { 0.31, 0.47, 5 }, dir[3] = { 0, 0, -1 }, dist;
    long hit = vz_pick(&t, o, dir, &dist);
    CHECK(hit >= 0 && fabs(dist - 4) < 1e-6, "pick: triangle %ld at %g", hit, dist);
    vz_tris_free(&t);
    /* a section across the cells (z = 0.53, between layers of points): area 1, values exact */
    double org[3] = { 0.5, 0.5, 0.53 }, nz[3] = { 0, 0, 1 };
    vz_section(&m, org, nz, &T, VQ_VALUE, true, &t);
    worst = 0;
    for (long i = 0; i < t.n; i++) for (int k = 0; k < 3; k++) {
        double x[3] = { t.xyz[9 * i + 3 * k], t.xyz[9 * i + 3 * k + 1], t.xyz[9 * i + 3 * k + 2] };
        worst = fmax(worst, fabs(t.val[3 * i + k] - T_at(x)) + fabs(x[2] - 0.53));
    }
    CHECK(fabs(tri_area(&t) - 1) < 1e-9 && worst < 1e-5, "section: area %g, off by %g", tri_area(&t), worst);
    {   /* its report: area 1, the mean of T over z = 0.53 is T at (0.5, 0.5, 0.53); a uniform U = (1, 2, 3) puts 3 through it,
           and -3 seen the other way */
        VzField Uu; memset(&Uu, 0, sizeof Uu); Uu.nc = 3;
        Uu.c = malloc(sizeof(double) * 3 * (size_t)m.nCells); Uu.b = malloc(sizeof(double) * 3 * (size_t)(m.nFaces - m.nInternal)); Uu.p = malloc(sizeof(double) * 3 * (size_t)m.nPoints);
        for (long i = 0; i < m.nCells; i++) { Uu.c[3 * i] = 1; Uu.c[3 * i + 1] = 2; Uu.c[3 * i + 2] = 3; }
        for (long i = 0; i < m.nFaces - m.nInternal; i++) { Uu.b[3 * i] = 1; Uu.b[3 * i + 1] = 2; Uu.b[3 * i + 2] = 3; }
        for (long i = 0; i < m.nPoints; i++) { Uu.p[3 * i] = 1; Uu.p[3 * i + 1] = 2; Uu.p[3 * i + 2] = 3; }
        VzSurfReport r; double x0[3] = { 0.5, 0.5, 0.53 }, down[3] = { 0, 0, -1 };
        vz_surface_report(&m, &t, &Uu, nz, &r);
        CHECK(fabs(r.area - 1) < 1e-9 && fabs(r.mean - T_at(x0)) < 1e-6 && fabs(r.flow - 3) < 1e-9, "report: area %g mean %g (%g) flow %g", r.area, r.mean, T_at(x0), r.flow);
        vz_surface_report(&m, &t, &Uu, down, &r);
        CHECK(fabs(r.flow + 3) < 1e-9 && fabs(r.flowAbs - 3) < 1e-9, "report: flow seen downward %g", r.flow);
        vz_field_free(&Uu);
    }
    vz_tris_free(&t);
    /* a tilted section */
    double nt[3] = { 1, 1, 1 };
    vz_section(&m, org, nt, &T, VQ_VALUE, true, &t);
    worst = 0;
    for (long i = 0; i < t.n; i++) for (int k = 0; k < 3; k++) {
        double x[3] = { t.xyz[9 * i + 3 * k], t.xyz[9 * i + 3 * k + 1], t.xyz[9 * i + 3 * k + 2] };
        worst = fmax(worst, fabs((x[0] - org[0]) + (x[1] - org[1]) + (x[2] - org[2])) / sqrt(3));
    }
    CHECK(t.n > 100 && worst < 1e-6, "tilted section: %ld triangles, %g off the plane (vertices kept as floats)", t.n, worst);
    vz_tris_free(&t);
    /* an iso-surface of T = 3: on the plane 2x + 3y - z = 2, normals along (2 3 -1) */
    vz_isosurface(&m, &T, VQ_VALUE, 3, NULL, 0, &t);
    worst = 0; double nworst = 0;
    for (long i = 0; i < t.n; i++) {
        for (int k = 0; k < 3; k++) { double x[3] = { t.xyz[9 * i + 3 * k], t.xyz[9 * i + 3 * k + 1], t.xyz[9 * i + 3 * k + 2] }; worst = fmax(worst, fabs(T_at(x) - 3)); }
        double nn = fabs(t.nrm[3 * i] * 2 + t.nrm[3 * i + 1] * 3 - t.nrm[3 * i + 2]) / sqrt(14);
        if (tri_area(&(VzTris){ .n = 1, .xyz = &t.xyz[9 * i] }) > 1e-12) nworst = fmax(nworst, 1 - nn);
    }
    CHECK(t.n > 100 && worst < 1e-5 && nworst < 1e-4, "iso-surface: %ld triangles, off by %g, normals off by %g", t.n, worst, nworst);
    vz_tris_free(&t);
    /* a threshold: the cells with T between 0 and 2: a closed set of faces, all coloured within range */
    vz_threshold(&m, &T, VQ_VALUE, 0, 2, NULL, 0, &t);
    CHECK(t.n > 0, "threshold: %ld triangles", t.n);
    vz_clip(&t, org, nz);
    for (long i = 0; i < t.n; i++) {
        bool any = false;
        for (int k = 0; k < 3; k++) any = any || t.xyz[9 * i + 3 * k + 2] <= 0.53 + 1e-9;
        if (!any) { CHECK(0, "clip kept a triangle wholly above the plane"); break; }
    }
    vz_tris_free(&t);
    /* streamlines in solid rotation: circles about the axis */
    VzSeeds s = { .kind = VS_LINE, .a = { 0.6, 0.5, 0.5 }, .b = { 0.8, 0.5, 0.5 }, .n = 3, .dir = 2, .maxSteps = 400, .maxLen = 1.5 };
    vz_streamlines(&m, &U, &s, NULL, 0, &l);
    CHECK(l.nl == 3, "streamlines: %ld", l.nl);
    for (long k = 0; k < l.nl; k++) {
        double r0 = 0.1 * (k + 1), rw = 0;
        for (long i = l.off[k]; i < l.off[k + 1]; i++) rw = fmax(rw, fabs(hypot(l.xyz[3 * i] - 0.5, l.xyz[3 * i + 1] - 0.5) - r0) / r0);
        CHECK(rw < 0.03, "streamline %ld: radius off by %.3g %%", k, rw * 100);
        CHECK(l.off[k + 1] - l.off[k] > 20, "streamline %ld has %ld points", k, l.off[k + 1] - l.off[k]);
    }
    vz_lines_free(&l);
    VzSeeds sp = { .kind = VS_PLANE, .a = { 0.5, 0.5, 0.5 }, .b = { 0, 0, 1 }, .size = 0.5, .n = 4 };
    double *pts; long nsp = vz_seed_points(&m, &sp, &pts);
    CHECK(nsp == 16 && fabs(pts[2] - 0.5) < 1e-12, "plane seeds %ld", nsp);
    free(pts);
    VzSeeds spt = { .kind = VS_PATCH, .patch = 0, .n = 10 };
    nsp = vz_seed_points(&m, &spt, &pts);
    CHECK(nsp == 10, "patch seeds %ld", nsp);
    for (long i = 0; i < nsp; i++) CHECK(vz_find_cell(&m, &pts[3 * i], -1) >= 0, "patch seed %ld inside", i);
    free(pts);
    /* glyphs over a section */
    vz_section(&m, org, nz, &T, VQ_VALUE, true, &t);
    float *gp, *gv; long ng = vz_glyph_points(&m, &U, &t, 0.2, &gp, &gv);
    CHECK(ng >= 16 && ng <= 36, "glyphs: %ld at spacing 0.2 over a unit square", ng);
    worst = 0;
    for (long i = 0; i < ng; i++) { double x[3] = { gp[3 * i], gp[3 * i + 1], gp[3 * i + 2] }, u[3]; U_at(x, u); worst = fmax(worst, fabs(gv[3 * i] - u[0]) + fabs(gv[3 * i + 1] - u[1])); }
    CHECK(worst < 0.3, "glyph vectors off by %g", worst);
    free(gp); free(gv);
    vz_tris_free(&t);
    /* LIC on the mid plane */
    int w = 64, h = 64; float *lum = malloc(sizeof(float) * w * h), *val = malloc(sizeof(float) * w * h); double eu[3], ev[3];
    vz_lic(&m, &U, NULL, 0, org, nz, 1.2, w, h, 10, lum, val, eu, ev);
    int inside = 0; bool range = true;
    for (int i = 0; i < w * h; i++) if (!isnan(lum[i])) { inside++; range = range && lum[i] >= 0 && lum[i] <= 1; }
    CHECK(inside > w * h / 2 && inside < w * h && range, "LIC: %d of %d pixels inside, lum in 0..1: %d", inside, w * h, range);
    free(lum); free(val);
    /* resampling and a line probe */
    int n3[3] = { 8, 8, 8 };
    float *vox = vz_resample(&m, &T, VQ_VALUE, n3);
    worst = 0;
    for (int k = 0; k < 8; k++) for (int j = 0; j < 8; j++) for (int i = 0; i < 8; i++) {
        double x[3] = { (i + 0.5) / 8, (j + 0.5) / 8, (k + 0.5) / 8 };
        worst = fmax(worst, fabs(vox[(k * 8 + j) * 8 + i] - T_at(x)));
    }
    CHECK(worst < 0.25, "resampled off by %g", worst);
    free(vox);
    double pa[3] = { 0.05, 0.5, 0.5 }, pb[3] = { 0.95, 0.5, 0.5 }, pos[3 * 20], pv[20];
    int nin = vz_line_probe(&m, &T, VQ_VALUE, pa, pb, 20, pos, pv);
    worst = 0; for (int i = 0; i < 20; i++) worst = fmax(worst, fabs(pv[i] - T_at(&pos[3 * i])));
    CHECK(nin == 20 && worst < 0.25, "line probe: %d inside, off by %g", nin, worst);
    vz_field_free(&T); vz_field_free(&U); vz_field_free(&p);
    vz_mesh_free(&m);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "write")) {
        VzMesh m; load_mesh(&m, argv[2]);
        char d[1024], f[1100];
        snprintf(d, sizeof d, "%s/0", argv[2]); pl_mkdirs(d);
        snprintf(f, sizeof f, "%s/T", d); write_field(&m, f, "volScalarField", "[0 0 0 1 0 0 0]", 1, false);
        snprintf(f, sizeof f, "%s/U", d); write_field(&m, f, "volVectorField", "[0 1 -1 0 0 0 0]", 3, true);
        snprintf(f, sizeof f, "%s/p", d); write_field(&m, f, "volScalarField", "[0 2 -2 0 0 0 0]", 1, false);
        vz_mesh_free(&m);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "check")) check(argv[2], argc > 3 ? argv[3] : "0");
    else if (argc >= 4 && !strcmp(argv[1], "same")) {
        VzMesh a, b; load_mesh(&a, argv[2]); load_mesh(&b, argv[3]);
        CHECK(a.nCells == b.nCells && a.nFaces == b.nFaces, "the two meshes differ");
        const char *names[3] = { "T", "U", "p" };
        for (int k = 0; k < 3; k++) {
            VzField fa, fb;
            bool la = vz_field_load(&fa, &a, argv[2], "0", names[k]), lb = vz_field_load(&fb, &b, argv[3], "0", names[k]);
            CHECK(la && lb, "%s loads in both", names[k]);
            if (!la || !lb) continue;
            double worst = 0;
            for (long i = 0; i < a.nCells * fa.nc; i++) worst = fmax(worst, fabs(fa.c[i] - fb.c[i]));
            for (long i = 0; i < (a.nFaces - a.nInternal) * fa.nc; i++) worst = fmax(worst, fabs(fa.b[i] - fb.b[i]));
            CHECK(worst < 1e-12, "%s: ASCII and binary differ by %g", names[k], worst);
            vz_field_free(&fa); vz_field_free(&fb);
        }
        vz_mesh_free(&a); vz_mesh_free(&b);
    } else if (argc >= 3 && !strcmp(argv[1], "procs")) {
        /* a decomposed copy: the processors' fields on the whole mesh, the same as the case's own */
        VzMesh m; load_mesh(&m, argv[2]);
        int np = vz_mesh_procs(&m, argv[2]);
        CHECK(np == 4, "processors: %d (%s)", np, m.err);
        const char *names[3] = { "T", "U", "p" };
        for (int k = 0; k < 3 && np; k++) {
            VzField a, b;
            bool la = vz_field_load(&a, &m, argv[2], "0", names[k]), lb = vz_field_load_procs(&b, &m, argv[2], "0", names[k]);
            CHECK(la && lb && a.nc == b.nc, "%s loads both ways", names[k]);
            if (!la || !lb) continue;
            double worst = 0;
            for (long i = 0; i < m.nCells * a.nc; i++) worst = fmax(worst, fabs(a.c[i] - b.c[i]));
            for (long i = 0; i < (m.nFaces - m.nInternal) * a.nc; i++) worst = fmax(worst, fabs(a.b[i] - b.b[i]));
            for (long i = 0; i < m.nPoints * a.nc; i++) worst = fmax(worst, fabs(a.p[i] - b.p[i]));
            CHECK(worst < 1e-12, "%s: decomposed differs by %g", names[k], worst);
            CHECK(!strcmp(a.dims, b.dims), "%s dims '%s' vs '%s'", names[k], a.dims, b.dims);
            vz_field_free(&a); vz_field_free(&b);
        }
        vz_mesh_free(&m);
    } else return 2;
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("viz %s: all passed\n", argv[1]);
    return 0;
}
