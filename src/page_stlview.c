#define _GNU_SOURCE
/* page_stlview.c -- the STL mesher's 3-D view (split from page_stlmesh.c, improvement plan A5, 2026-11-26): the part
 * drawn, a face clicked to pick its boundary, what each step adds to the view (the tunnel, the location in the fluid,
 * the refinement boxes, the mesh). page_stlmesh_int.h is what it shares with the page. */
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "app.h"
#include "calc.h"
#include "precheck.h"
#include "teach.h"
#include "assets.h"
#include "orbit.h"
#include "rlgl.h"
#include "simsetup.h"
#include "stlmesh.h"
#include "meshopts.h"
#include "page_meshadv.h"
#include "meshexamples.h"
#include "salometool.h"
#include "page_stlmesh_int.h"

/* ============================================================================ the part in 3-D: click a face to pick its boundary */
StlView STLV = { .hover = -1, .drawnSel = -2, .drawnHover = -2, .up = -1, .O = { .upAxis = 1, .spin = -1, .back = { 0, 0, 1 }, .up = { 0, 1, 0 } } };   /* the 3-D view (page_stlmesh_int.h) */

Color sm_region_color(int i) { return TH.series[i % 10]; }

/* another case: the view is rebuilt and fitted to its part */
void sm_view_forget(void)
{
    STLV.hover = -1; STLV.engaged = STLV.dragging = STLV.panning = false;
    STLV.sig = -1; STLV.dirty = true; STLV.up = -1;
}

/* the theme's colours: the models carry them, so a theme change builds them again (the view kept another theme's blue
   while the list beside it showed this one's orange, user 2026-11-10) */
static double theme_sig(void)
{
    double h = TH.dark ? 0.5 : 0;
    for (int i = 0; i < 10; i++) h += (TH.series[i].r * 3.0 + TH.series[i].g * 5.0 + TH.series[i].b * 7.0) * (i + 1);
    return h;
}

double sm_geometry_sig(void)
{
    const Stl *s = &STLP.stl;
    double h = s->nTri * 1.0 + STLP.gen * 1e3 + s->nRegions * 1e-3 + STLP.m.scale * 7.1 + STLP.m.splitAngle * 0.013 + (double)(uintptr_t)s->tri * 1e-9;
    for (int k = 0; k < 3; k++) h += s->bmin[k] * 3.3 + s->bmax[k] * 5.9;
    for (int r = 0; r < s->nRegions; r++) h += s->reg[r].nTri * (r + 1) * 1e-6;
    return h + theme_sig() * 1e-7;
}

static void view_unload(void)
{
    for (int i = 0; i < STL_MAX_REGIONS; i++) if (STLV.has[i]) { UnloadModel(STLV.model[i]); STLV.has[i] = false; }
}

/* one model a boundary, flat shaded by a fixed light (two-sided), as on Mesh > View */
static void view_build(void)
{
    view_unload();
    const Stl *s = &STLP.stl;
    float L[3] = { 0.45f, 0.75f, 0.5f };
    float ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    for (int k = 0; k < 3; k++) L[k] /= ll;
    for (int r = 0; r < s->nRegions; r++) {
        int n = s->reg[r].nTri;
        if (n <= 0 || n > 0x7fffffff / 12) continue;
        Mesh mesh = { 0 };
        mesh.vertexCount = n * 3; mesh.triangleCount = n;
        mesh.vertices = MemAlloc((unsigned)(sizeof(float) * 9 * (size_t)n));
        mesh.colors = MemAlloc((unsigned)(12 * (size_t)n));
        if (!mesh.vertices || !mesh.colors) { MemFree(mesh.vertices); MemFree(mesh.colors); continue; }
        Color base = sm_region_color(r);
        int v = 0;
        for (int t = 0; t < s->nTri && v < n * 3; t++) {
            if (s->region[t] != r) continue;
            const float *p = s->tri + 9 * (size_t)t;
            float e1[3] = { p[3] - p[0], p[4] - p[1], p[5] - p[2] }, e2[3] = { p[6] - p[0], p[7] - p[1], p[8] - p[2] };
            float nr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            float nl = sqrtf(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]);
            float d = nl > 0 ? fabsf(nr[0] * L[0] + nr[1] * L[1] + nr[2] * L[2]) / nl : 1;
            float k = 0.42f + 0.58f * d;
            Color c = { (unsigned char)(base.r * k), (unsigned char)(base.g * k), (unsigned char)(base.b * k), 255 };
            memcpy(&mesh.vertices[v * 3], p, sizeof(float) * 9);
            for (int q = 0; q < 3; q++) memcpy(&mesh.colors[(v + q) * 4], &c, 4);
            v += 3;
        }
        UploadMesh(&mesh, false);
        STLV.model[r] = LoadModelFromMesh(mesh);
        STLV.has[r] = true;
    }
    STLV.sig = sm_geometry_sig();
}

/* the whole part in the view, whatever the view's shape: its bounding sphere inside the narrower of the two fields of view.
   A part much thinner one way (a profile, a plate) is looked at face on, a little from the side, not edge on. */
static void view_fit(float aspect)
{
    const Stl *s = &STLP.stl;
    STLV.O.target = (Vector3){ (s->bmin[0] + s->bmax[0]) / 2, (s->bmin[1] + s->bmax[1]) / 2, (s->bmin[2] + s->bmax[2]) / 2 };
    float d[3] = { s->bmax[0] - s->bmin[0], s->bmax[1] - s->bmin[1], s->bmax[2] - s->bmin[2] };
    int thin = 0; for (int k = 1; k < 3; k++) if (d[k] < d[thin]) thin = k;
    float big = fmaxf(d[0], fmaxf(d[1], d[2]));
    int u = STLV.up < 0 ? 1 : STLV.up;
    STLV.O.upAxis = u;                                      /* the camera's frame: y is the up axis (orbit.h) */
    if (d[thin] < 0.25f * big) {                          /* the camera along the thin axis, turned a little */
        if (thin == u) orbit_set(&STLV.O, 0.5f, 1.2f);                     /* from above */
        else if (thin == (u + 2) % 3) orbit_set(&STLV.O, 1.2f, 0.25f);     /* the frame's x */
        else orbit_set(&STLV.O, 0.35f, 0.25f);                             /* the frame's z */
    } else orbit_set(&STLV.O, 0.7f, 0.45f);
    /* as close as the box's corners allow, seen from there: each within the vertical and the horizontal field of view */
    float tv = tanf(35.f / 2 * DEG2RAD), th = tv * fmaxf(aspect, 0.1f);   /* view_camera's fovy */
    Vector3 back = STLV.O.back, right = orbit_right(&STLV.O), up = STLV.O.up;
    float need = 1e-9f;
    for (int c = 0; c < 8; c++) {
        float p[3] = { (c & 1 ? d[0] : -d[0]) / 2, (c & 2 ? d[1] : -d[1]) / 2, (c & 4 ? d[2] : -d[2]) / 2 };
        float x = p[0] * right.x + p[1] * right.y + p[2] * right.z, y = p[0] * up.x + p[1] * up.y + p[2] * up.z;
        float z = p[0] * back.x + p[1] * back.y + p[2] * back.z;
        need = fmaxf(need, fmaxf(z + fabsf(x) / th, z + fabsf(y) / tv));
    }
    STLV.O.dist = need * 1.15f;
    STLV.dirty = true;
}

static Camera3D view_camera(void)
{
    return orbit_camera(&STLV.O);
}

/* the boundary under a point of the view (-1: none): the nearest triangle the ray through it hits */
static int view_pick_tri(Rectangle view, Vector2 mp)
{
    float s = ui_scale();
    Vector2 pos = { (mp.x - view.x) * s, (mp.y - view.y) * s };
    Ray ray = GetScreenToWorldRayEx(pos, view_camera(), STLV.rtW, STLV.rtH);
    float best = 1e30f; int hit = -1;
    const Stl *st = &STLP.stl;
    for (int t = 0; t < st->nTri; t++) {
        const float *p = st->tri + 9 * (size_t)t;
        RayCollision h = GetRayCollisionTriangle(ray, (Vector3){ p[0], p[1], p[2] }, (Vector3){ p[3], p[4], p[5] }, (Vector3){ p[6], p[7], p[8] });
        if (h.hit && h.distance < best) { best = h.distance; hit = t; }
    }
    return hit;
}

/* the point on the part under the mouse and its triangle's normal; the triangle, -1: none */
static int view_pick_point(Rectangle view, Vector2 mp, double P[3], double N[3])
{
    float s = ui_scale();
    Vector2 pos = { (mp.x - view.x) * s, (mp.y - view.y) * s };
    Ray ray = GetScreenToWorldRayEx(pos, view_camera(), STLV.rtW, STLV.rtH);
    float best = 1e30f; int hit = -1; Vector3 hp = { 0 }, hn = { 0 };
    const Stl *st = &STLP.stl;
    for (int t = 0; t < st->nTri; t++) {
        const float *p = st->tri + 9 * (size_t)t;
        RayCollision h = GetRayCollisionTriangle(ray, (Vector3){ p[0], p[1], p[2] }, (Vector3){ p[3], p[4], p[5] }, (Vector3){ p[6], p[7], p[8] });
        if (h.hit && h.distance < best) { best = h.distance; hit = t; hp = h.point; hn = h.normal; }
    }
    if (hit >= 0) { P[0] = hp.x; P[1] = hp.y; P[2] = hp.z; N[0] = hn.x; N[1] = hn.y; N[2] = hn.z; }
    return hit;
}

/* the point in the fluid (picked or worked out) and whether it is in the fluid: worked out again only when the settings or
   the part change (the automatic point samples the whole part) */
void sm_loc_now(double out[3], bool *inFluid)
{
    static MeshCfg was; static unsigned wasGen = ~0u; static long wasTri = -1; static double L[3]; static bool ok;
    if (memcmp(&was, &STLP.m, sizeof was) || wasGen != STLP.gen || wasTri != STLP.stl.nTri) {
        was = STLP.m; wasGen = STLP.gen; wasTri = STLP.stl.nTri;
        mesher_location(&STLP.m, &STLP.stl, L);
        ok = mesher_in_fluid(&STLP.m, &STLP.stl, L);
    }
    for (int k = 0; k < 3; k++) out[k] = L[k];
    if (inFluid) *inFluid = ok;
}

/* a click on the part (Pick on the part): a point just off the surface on the fluid's side, checked to be in the fluid */
static void pick_location(Rectangle view, Vector2 mp)
{
    double P[3], N[3];
    if (view_pick_point(view, mp, P, N) < 0) { ui_toast(TOAST_INFO, "Click on the part: the point goes just off its surface, on the fluid's side"); return; }
    double size = 0; for (int k = 0; k < 3; k++) size = fmax(size, STLP.stl.bmax[k] - STLP.stl.bmin[k]);
    double d = fmax(0.5 * mesher_surface_cell(&STLP.m), 1e-3 * size);
    for (int tries = 0; tries < 6; tries++, d *= 2)
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            double q[3]; for (int k = 0; k < 3; k++) q[k] = P[k] + sgn * d * N[k];
            if (mesher_in_fluid(&STLP.m, &STLP.stl, q)) {
                for (int k = 0; k < 3; k++) STLP.m.loc[k] = q[k];
                STLP.m.locPicked = true; STLP.pickLoc = false; sm_save_cfg(); STLV.dirty = true;
                ui_toast(TOAST_OK, "Point in the fluid: (%.4g, %.4g, %.4g), %.3g mm off the surface", q[0], q[1], q[2], d * 1e3);
                return;
            }
        }
    ui_toast(TOAST_WARN, "No point in the fluid next to that spot (a thin wall, or an open part): try another");
}

static int view_pick(Rectangle view, Vector2 mp)
{
    float s = ui_scale();
    Vector2 pos = { (mp.x - view.x) * s, (mp.y - view.y) * s };
    Ray ray = GetScreenToWorldRayEx(pos, view_camera(), STLV.rtW, STLV.rtH);
    float best = 1e30f; int hit = -1;
    const Stl *st = &STLP.stl;
    for (int t = 0; t < st->nTri; t++) {
        const float *p = st->tri + 9 * (size_t)t;
        RayCollision h = GetRayCollisionTriangle(ray, (Vector3){ p[0], p[1], p[2] }, (Vector3){ p[3], p[4], p[5] }, (Vector3){ p[6], p[7], p[8] });
        if (h.hit && h.distance < best) { best = h.distance; hit = st->region[t]; }
    }
    return hit;
}

/* ---------------------------------------------------------------------------- what each step adds to the view
   Geometry: the part's box with its sizes (a wrong unit shows at once). Mesh settings: a base cell and a surface cell
   drawn to scale beside the part, the box kept at the base size round a body in a tunnel, the volumetric controls. */
static void wire_box(const double lo[3], const double hi[3], Color c)
{
    for (int a = 0; a < 3; a++)
        for (int e = 0; e < 4; e++) {
            double p0[3], p1[3];
            int b1 = (a + 1) % 3, b2 = (a + 2) % 3;
            p0[a] = lo[a]; p1[a] = hi[a];
            p0[b1] = p1[b1] = e & 1 ? hi[b1] : lo[b1];
            p0[b2] = p1[b2] = e & 2 ? hi[b2] : lo[b2];
            DrawLine3D((Vector3){ (float)p0[0], (float)p0[1], (float)p0[2] }, (Vector3){ (float)p1[0], (float)p1[1], (float)p1[2] }, c);
        }
}

/* the two cells drawn to scale: under the part (the view's up), side by side along its longest side, so they read
   from wherever it is seen */
static void size_glyphs(const Stl *st, double cell[2], double at[2][3])
{
    int u = STLV.up < 0 ? 2 : STLV.up, a = (u + 1) % 3, o;
    if (st->bmax[(u + 2) % 3] - st->bmin[(u + 2) % 3] > st->bmax[a] - st->bmin[a]) a = (u + 2) % 3;
    o = 3 - u - a;
    double L = 0; for (int k = 0; k < 3; k++) L = fmax(L, st->bmax[k] - st->bmin[k]);
    cell[0] = STLP.m.base; cell[1] = mesher_surface_cell(&STLP.m);
    double along = st->bmin[a];
    for (int g = 0; g < 2; g++) {
        at[g][a] = along;
        at[g][u] = st->bmin[u] - 0.12 * L - cell[g];
        at[g][o] = (st->bmin[o] + st->bmax[o]) / 2 - cell[g] / 2;
        along += cell[g] + 0.25 * L;
    }
}

static void view_overlay_3d(const Stl *st)
{
    Color dim = TH.dark ? (Color){ 230, 200, 120, 220 } : (Color){ 150, 100, 20, 220 };
    if (STLP.open == ST_FLOW) {                               /* the point in the fluid: green in the fluid, red not */
        double L[3]; bool ok; sm_loc_now(L, &ok);
        double size = 0; for (int k = 0; k < 3; k++) size = fmax(size, st->bmax[k] - st->bmin[k]);
        rlDrawRenderBatchActive(); rlDisableDepthTest();     /* seen through the part: for internal flow it is inside it */
        DrawSphere((Vector3){ (float)L[0], (float)L[1], (float)L[2] }, (float)(0.025 * size), ok ? TH.ok : TH.err);
        rlDrawRenderBatchActive(); rlEnableDepthTest();
    }
    if (STLP.open == ST_GEOM) {
        double lo[3] = { st->bmin[0], st->bmin[1], st->bmin[2] }, hi[3] = { st->bmax[0], st->bmax[1], st->bmax[2] };
        wire_box(lo, hi, dim);
    }
    if (STLP.open == ST_MESH) {
        double cell[2], at[2][3];
        size_glyphs(st, cell, at);
        double L = 0; for (int k = 0; k < 3; k++) L = fmax(L, st->bmax[k] - st->bmin[k]);
        for (int g = 0; g < 2; g++) {
            if (cell[g] > 3 * L || cell[g] < L * 1e-4) continue;   /* off the scale of the view */
            Vector3 c = { (float)(at[g][0] + cell[g] / 2), (float)(at[g][1] + cell[g] / 2), (float)(at[g][2] + cell[g] / 2) };
            Color k = g ? TH.accent : dim;
            DrawCube(c, (float)cell[g], (float)cell[g], (float)cell[g], ui_alpha(k, 0.35f));
            DrawCubeWires(c, (float)cell[g], (float)cell[g], (float)cell[g], k);
        }
        if (STLP.m.domain == DOMAIN_EXTERNAL && !STLP.m.twoD) {
            double lo[3], hi[3];
            mesher_near_box(&STLP.m, st, lo, hi);
            wire_box(lo, hi, ui_alpha(TH.accent, 0.5f));
        }
        for (int i = 0; i < STLP.m.nZones; i++) {
            const MeshZone *z = &STLP.m.zone[i];
            Color zc = TH.series[(i + 3) % 10];
            if (z->kind == ZONE_BOX) wire_box(z->a, z->b, zc);
            else if (z->kind == ZONE_SPHERE) DrawSphereWires((Vector3){ (float)z->a[0], (float)z->a[1], (float)z->a[2] }, (float)z->r, 8, 12, zc);
            else DrawCylinderWiresEx((Vector3){ (float)z->a[0], (float)z->a[1], (float)z->a[2] }, (Vector3){ (float)z->b[0], (float)z->b[1], (float)z->b[2] },
                                     (float)z->r, (float)z->r, 16, zc);
        }
    }
    rlDrawRenderBatchActive();
}

static Rectangle LBL[8]; static int nLbl;              /* the labels drawn in this redraw: the next ones keep clear of them */

static void label_at(Camera3D cam, float s, Vector3 p, const char *t, Color c)
{
    Vector2 q = GetWorldToScreenEx(p, cam, STLV.rtW, STLV.rtH);
    if (q.x < -50 || q.y < -50 || q.x > STLV.rtW + 50 || q.y > STLV.rtH + 50) return;
    Font f = ui_font(F_MED, 14 * s);
    Vector2 m = MeasureTextEx(f, t, 14 * s, 0);
    for (int tries = 0; tries < 8; tries++) {             /* overlapping one already there: moved down under it */
        Rectangle me = { q.x - m.x / 2 - 6 * s, q.y - m.y / 2 - 3 * s, m.x + 12 * s, m.y + 6 * s };
        bool hit = false;
        for (int i = 0; i < nLbl && !hit; i++) if (CheckCollisionRecs(me, LBL[i])) { q.y = LBL[i].y + LBL[i].height + m.y / 2 + 5 * s; hit = true; }
        if (!hit) break;
    }
    if (nLbl < 8) LBL[nLbl++] = (Rectangle){ q.x - m.x / 2 - 6 * s, q.y - m.y / 2 - 3 * s, m.x + 12 * s, m.y + 6 * s };
    DrawRectangleRounded((Rectangle){ q.x - m.x / 2 - 6 * s, q.y - m.y / 2 - 3 * s, m.x + 12 * s, m.y + 6 * s }, 0.4f, 6,
                         TH.dark ? (Color){ 10, 14, 22, 200 } : (Color){ 255, 255, 255, 210 });
    DrawTextEx(f, t, (Vector2){ q.x - m.x / 2, q.y - m.y / 2 }, 14 * s, 0, c);
}

static void view_overlay_labels(Camera3D cam, float s)
{
    nLbl = 0;
    const Stl *st = &STLP.stl;
    Color dim = TH.dark ? (Color){ 240, 210, 130, 255 } : (Color){ 130, 85, 10, 255 };
    char b[48], l[32];
    if (STLP.open == ST_GEOM) {                              /* each size along the box edge it measures */
        for (int a = 0; a < 3; a++) {
            Vector3 p = { st->bmin[0], st->bmin[1], st->bmin[2] };
            (&p.x)[a] = (st->bmin[a] + st->bmax[a]) / 2;
            sm_fmt_len(l, sizeof l, st->bmax[a] - st->bmin[a]);
            snprintf(b, sizeof b, "%c  %s", "XYZ"[a], l);
            label_at(cam, s, p, b, dim);
        }
    }
    if (STLP.open == ST_MESH) {
        double cell[2], at[2][3], L = 0;
        size_glyphs(st, cell, at);
        for (int k = 0; k < 3; k++) L = fmax(L, st->bmax[k] - st->bmin[k]);
        for (int g = 0; g < 2; g++) {
            if (cell[g] > 3 * L || cell[g] < L * 1e-4) continue;
            sm_fmt_len(l, sizeof l, cell[g]);
            snprintf(b, sizeof b, "%s %s", g ? "surface cell" : "base cell", l);
            double lp[3] = { at[g][0] + cell[g] / 2, at[g][1] + cell[g] / 2, at[g][2] + cell[g] / 2 };
            lp[STLV.up < 0 ? 2 : STLV.up] -= cell[g] / 2 + 0.05 * L;       /* under the cube */
            label_at(cam, s, (Vector3){ (float)lp[0], (float)lp[1], (float)lp[2] }, b, g ? TH.accent : dim);
        }
    }
}

/* the picked boundary's border: its triangles' edges used once within it (shared with another boundary, or open). Built
   again when the pick or the geometry changes. Returns the count, *out the segments (6 floats each). */
/* the border of a set of triangles: their edges used once within the set (shared with a triangle outside it, or open);
   6 floats a segment into *seg (grown as needed) */
static int border_build(bool (*in)(const Stl *, int), float **seg, int *cap)
{
    const Stl *st = &STLP.stl;
    int n = 0;
    int ne = 0; for (int t = 0; t < st->nTri; t++) if (in(st, t)) ne += 3;
    size_t hc = 1;
    while (hc < (size_t)ne * 2 + 2) hc <<= 1;
    struct E { float a[3], b[3]; int cnt; bool used; } *h = calloc(hc, sizeof *h);
    if (!h) return n;
    float L = 0; for (int k = 0; k < 3; k++) L = fmaxf(L, st->bmax[k] - st->bmin[k]);
    float q = fmaxf(L * 1e-6f, 1e-12f);
    for (int t = 0; t < st->nTri; t++) {
        if (!in(st, t)) continue;
        const float *p = st->tri + 9 * (size_t)t;
        for (int e = 0; e < 3; e++) {
            const float *a = p + 3 * e, *b = p + 3 * ((e + 1) % 3);
            long ka[3], kb[3]; for (int k = 0; k < 3; k++) { ka[k] = lroundf(a[k] / q); kb[k] = lroundf(b[k] / q); }
            if (memcmp(ka, kb, sizeof ka) > 0) { const float *x = a; a = b; b = x; long y[3]; memcpy(y, ka, sizeof y); memcpy(ka, kb, sizeof ka); memcpy(kb, y, sizeof kb); }
            uint64_t hh = 1469598103934665603ULL;
            for (int k = 0; k < 3; k++) { hh = (hh ^ (uint64_t)ka[k]) * 1099511628211ULL; hh = (hh ^ (uint64_t)kb[k]) * 1099511628211ULL; }
            size_t j = hh & (hc - 1);
            while (h[j].used) {
                long ja[3], jb[3]; for (int k = 0; k < 3; k++) { ja[k] = lroundf(h[j].a[k] / q); jb[k] = lroundf(h[j].b[k] / q); }
                if (!memcmp(ja, ka, sizeof ja) && !memcmp(jb, kb, sizeof jb)) break;
                j = (j + 1) & (hc - 1);
            }
            if (!h[j].used) { h[j].used = true; memcpy(h[j].a, a, sizeof h[j].a); memcpy(h[j].b, b, sizeof h[j].b); }
            h[j].cnt++;
        }
    }
    for (size_t j = 0; j < hc; j++) {
        if (!h[j].used || h[j].cnt != 1) continue;
        if (n == *cap) { *cap = *cap ? *cap * 2 : 1024; float *ns = realloc(*seg, sizeof(float) * 6 * (size_t)*cap); if (!ns) break; *seg = ns; }
        memcpy(*seg + 6 * (size_t)n, h[j].a, sizeof(float) * 3); memcpy(*seg + 6 * (size_t)n + 3, h[j].b, sizeof(float) * 3);
        n++;
    }
    free(h);
    return n;
}

static bool in_sel(const Stl *st, int t) { return st->region[t] == STLP.sel; }

/* the picked boundary's border, built again when the pick or the geometry changes */
static int sel_border(const float **out)
{
    static float *seg; static int n, cap, forSel = -2; static double forSig = -1;
    double sig = sm_geometry_sig();
    if (forSel != STLP.sel || forSig != sig) {
        forSel = STLP.sel; forSig = sig;
        n = STLP.sel >= 0 && STLP.sel < STLP.stl.nRegions ? border_build(in_sel, &seg, &cap) : 0;
    }
    *out = seg;
    return n;
}

/* CAD faces picked (Boundaries, a part from STEP or IGES): by face id, shift-click adds or takes away */
StlPick STLPK;                                          /* faces picked in the view (page_stlmesh_int.h) */
void sm_pick_reset(void) { STLPK.n = 0; if (STLPK.on) memset(STLPK.on, 0, (size_t)STLPK.cap); STLPK.gen++; }
void sm_pick_clear(void) { if (STLPK.on) memset(STLPK.on, 0, (size_t)STLPK.cap); STLPK.n = 0; STLPK.gen++; STLV.dirty = true; }
static void pick_face(int f, bool add)
{
    if (f < 1) { if (!add) sm_pick_clear(); return; }
    if (f >= STLPK.cap) { int nc = f + 64; bool *no = realloc(STLPK.on, (size_t)nc); if (!no) return; memset(no + STLPK.cap, 0, (size_t)(nc - STLPK.cap)); STLPK.on = no; STLPK.cap = nc; }
    if (!add) sm_pick_clear();
    STLPK.on[f] = !STLPK.on[f];
    STLPK.n += STLPK.on[f] ? 1 : -1;
    STLPK.gen++; STLV.dirty = true;
}
bool sm_in_pick(const Stl *st, int t) { int f = st->face ? st->face[t] : 0; return f > 0 && f < STLPK.cap && STLPK.on[f]; }
static int pick_border(const float **out)
{
    static float *seg; static int n, cap; static unsigned forGen = ~0u; static double forSig = -1;
    double sig = sm_geometry_sig();
    if (forGen != STLPK.gen || forSig != sig) { forGen = STLPK.gen; forSig = sig; n = STLPK.n ? border_build(sm_in_pick, &seg, &cap) : 0; }
    *out = seg;
    return n;
}

static void view_redraw(Rectangle r)
{
    float s = ui_scale();
    int w = (int)(r.width * s), h = (int)(r.height * s);
    if (w < 8 || h < 8) return;
    if (!STLV.rtOk || w != STLV.rtW || h != STLV.rtH) {
        if (STLV.rtOk) UnloadRenderTexture(STLV.rt);
        STLV.rt = LoadRenderTexture(w, h);
        STLV.rtOk = STLV.rt.id != 0; STLV.rtW = w; STLV.rtH = h;
    }
    if (!STLV.rtOk) return;
    EndScissorMode();
    BeginTextureMode(STLV.rt);
    ClearBackground(TH.dark ? (Color){ 10, 14, 22, 255 } : (Color){ 236, 240, 247, 255 });
    rlDrawRenderBatchActive();
    Camera3D cam = view_camera();
    BeginMode3D(cam);
    rlSetClipPlanes(STLV.O.dist * 0.01, STLV.O.dist * 20.0);
    rlDisableBackfaceCulling();
    const Stl *st = &STLP.stl;
    for (int r2 = 0; r2 < st->nRegions; r2++) {
        if (!STLV.has[r2]) continue;
        /* the picked boundary stands out: the others dim; the one under the mouse brightens */
        unsigned char g = STLP.sel >= 0 ? (r2 == STLP.sel ? 255 : r2 == STLV.hover ? 200 : 110) : (r2 == STLV.hover ? 255 : 215);
        DrawModel(STLV.model[r2], (Vector3){ 0, 0, 0 }, 1, (Color){ g, g, g, 255 });
    }
    /* outlines a hair toward the camera: every triangle's when there are few, the picked boundary's in the accent */
    Vector3 to = { cam.position.x - cam.target.x, cam.position.y - cam.target.y, cam.position.z - cam.target.z };
    float tl = sqrtf(to.x * to.x + to.y * to.y + to.z * to.z), k = STLV.O.dist * 0.0015f / fmaxf(tl, 1e-12f);
    to.x *= k; to.y *= k; to.z *= k;
    if (st->nTri <= VIEW_EDGES) {                         /* every triangle's outline, faint */
        Color ec = TH.dark ? (Color){ 0, 0, 0, 110 } : (Color){ 20, 30, 50, 80 };
        rlBegin(RL_LINES);
        rlColor4ub(ec.r, ec.g, ec.b, ec.a);
        for (int t = 0; t < st->nTri; t++) {
            const float *p = st->tri + 9 * (size_t)t;
            for (int e = 0; e < 3; e++) {
                const float *a = p + 3 * e, *b = p + 3 * ((e + 1) % 3);
                rlVertex3f(a[0] + to.x, a[1] + to.y, a[2] + to.z);
                rlVertex3f(b[0] + to.x, b[1] + to.y, b[2] + to.z);
            }
        }
        rlEnd();
        rlDrawRenderBatchActive();
    }
    if (STLPK.n) {                                           /* CAD faces picked: their border, white over everything */
        const float *se; int ns = pick_border(&se);
        rlBegin(RL_LINES);
        rlColor4ub(255, 255, 255, 255);
        for (int i = 0; i < ns; i++) {
            const float *g = se + 6 * (size_t)i;
            rlVertex3f(g[0] + to.x * 2, g[1] + to.y * 2, g[2] + to.z * 2);
            rlVertex3f(g[3] + to.x * 2, g[4] + to.y * 2, g[5] + to.z * 2);
        }
        rlEnd();
        rlDrawRenderBatchActive();
    }
    if (STLP.sel >= 0) {                                     /* the picked boundary: its border in the accent, its own colour inside
                                                             (every triangle outlined painted a fine face all accent) */
        const float *se; int ns = sel_border(&se);
        rlBegin(RL_LINES);
        rlColor4ub(TH.accent.r, TH.accent.g, TH.accent.b, 255);
        for (int i = 0; i < ns; i++) {
            const float *g = se + 6 * (size_t)i;
            rlVertex3f(g[0] + to.x, g[1] + to.y, g[2] + to.z);
            rlVertex3f(g[3] + to.x, g[4] + to.y, g[5] + to.z);
        }
        rlEnd();
        rlDrawRenderBatchActive();
    }
    /* external: the wind tunnel's box, and an arrow ahead of the body the way the flow goes */
    if (STLP.m.domain == DOMAIN_EXTERNAL) {
        double lo[3], hi[3];
        mesher_domain(&STLP.m, st, lo, hi);
        Color bc = TH.dark ? (Color){ 150, 165, 190, 150 } : (Color){ 60, 75, 100, 150 };
        for (int a = 0; a < 3; a++)                       /* the 12 edges: 4 along each axis */
            for (int e = 0; e < 4; e++) {
                double p0[3], p1[3];
                int b1 = (a + 1) % 3, b2 = (a + 2) % 3;
                p0[a] = lo[a]; p1[a] = hi[a];
                p0[b1] = p1[b1] = e & 1 ? hi[b1] : lo[b1];
                p0[b2] = p1[b2] = e & 2 ? hi[b2] : lo[b2];
                DrawLine3D((Vector3){ (float)p0[0], (float)p0[1], (float)p0[2] }, (Vector3){ (float)p1[0], (float)p1[1], (float)p1[2] }, bc);
            }
        int f = STLP.m.flowDir / 2; float sg = STLP.m.flowDir % 2 ? -1.f : 1.f;
        float c[3], ext = 0;
        for (int k = 0; k < 3; k++) { c[k] = (st->bmin[k] + st->bmax[k]) / 2; ext = fmaxf(ext, st->bmax[k] - st->bmin[k]); }
        float half = (st->bmax[f] - st->bmin[f]) / 2, len = ext * 0.45f, r = ext * 0.018f;
        Vector3 a0 = { c[0], c[1], c[2] }, a1 = a0, a2 = a0;
        float *p0 = &a0.x, *p1 = &a1.x, *p2 = &a2.x;
        p0[f] -= sg * (half + len * 1.25f); p1[f] -= sg * (half + len * 0.45f); p2[f] -= sg * (half + len * 0.2f);
        DrawCylinderEx(a0, a1, r, r, 10, TH.accent);
        DrawCylinderEx(a1, a2, r * 2.6f, 0, 14, TH.accent);
        rlDrawRenderBatchActive();
    }
    view_overlay_3d(st);
    rlEnableBackfaceCulling();
    EndMode3D();
    view_overlay_labels(cam, s);
    rlSetClipPlanes(RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR);
    /* the axes, bottom left */
    float ax = 46 * s, ay = h - 46 * s, al = 28 * s;
    orbit_draw_axes(&STLV.O, ax, ay, al, s);
    EndTextureMode();
    ui_restore_2d();
    STLV.dirty = false;
    STLV.drawnSel = STLP.sel; STLV.drawnHover = STLV.hover;
}

void sm_select_region(int r);

void sm_view_panel(Rectangle view)
{
    if (STLV.sig != sm_geometry_sig() || (!STLV.has[0] && STLP.stl.nRegions > 0)) { view_build(); STLV.touched = false; STLV.fitW = 0; }
    {   /* fitted to the view's real shape, again when it changes (a step with the view beside it, or above), until turned by hand */
        int vw = (int)(view.width * ui_scale()), vh = (int)(view.height * ui_scale());
        if (!STLV.touched && vh > 0 && (vw != STLV.fitW || vh != STLV.fitH)) { view_fit((float)vw / vh); STLV.fitW = vw; STLV.fitH = vh; }
    }
    ui_rect(view, 12, TH.dark ? (Color){ 10, 14, 22, 255 } : (Color){ 236, 240, 247, 255 });
    if (STLV.up < 0) { STLV.up = mesher_up(&STLP.m) / 2; STLV.touched = false; STLV.fitW = 0; }   /* the case's up (CAD's z, or the tunnel's Up) */
    Rectangle upR = { view.x + view.width - 160, view.y + 10, 150, 30 };
    Rectangle turnR = { view.x + 10, view.y + 6, orbit_tools_w(true) + 16, 40 };       /* top left, or under Up when narrow */
    if (turnR.x + turnR.width > upR.x - 36) turnR.y = upR.y + upR.height + 8;
    bool hov = ui_hovered(view) && !ui_hovered(upR) && !ui_hovered(turnR) && A.modal == MD_NONE;
    Vector2 mp = ui_mouse();
    if (!hov) STLV.engaged = false;
    if (hov && ui_pressed(view)) {
        STLV.engaged = true;
        if (ui_time() - STLV.lastClick < 0.35) { view_fit(view.height > 0 ? view.width / view.height : 1.5f); STLV.touched = false; STLV.lastClick = 0; }
        else STLV.lastClick = ui_time();
        bool shift = inp_key_down(KEY_LEFT_SHIFT) || inp_key_down(KEY_RIGHT_SHIFT);
        STLV.dragging = !shift; STLV.panning = shift; STLV.last = STLV.down = mp; STLV.moved = false;
    }
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) { STLV.panning = true; STLV.dragging = false; STLV.last = STLV.down = mp; STLV.moved = true; }
    if (STLV.dragging || STLV.panning) {
        bool down = STLV.dragging ? ui_mouse_down() : (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || ui_mouse_down());
        if (fabsf(mp.x - STLV.down.x) + fabsf(mp.y - STLV.down.y) > 4) STLV.moved = true;
        if (!down) {
            if (!STLV.moved && hov && STLP.open == ST_FLOW && STLP.pickLoc) { pick_location(view, mp); STLV.engaged = false; }   /* E8: a pick, not a wish to zoom: the wheel scrolls the page */
            else if (!STLV.moved && hov && STLP.open == ST_BOUND) {    /* a click, not a drag: pick the boundary there (a CAD part: the face too) */
                int t = view_pick_tri(view, mp);
                bool add = inp_key_down(KEY_LEFT_SHIFT) || inp_key_down(KEY_RIGHT_SHIFT);
                if (STLP.stl.face) pick_face(t >= 0 ? STLP.stl.face[t] : 0, add);
                if (!add) sm_select_region(t >= 0 ? STLP.stl.region[t] : -1);
            }
            STLV.dragging = STLV.panning = false;
        } else {
            float dx = mp.x - STLV.last.x, dy = mp.y - STLV.last.y;
            if (STLV.moved && (dx != 0 || dy != 0)) {
                STLV.touched = true;
                if (STLV.dragging) orbit_drag(&STLV.O, STLV.last, mp, (Vector2){ view.x + view.width / 2, view.y + view.height / 2 });
                else orbit_pan(&STLV.O, dx, dy);
                STLV.dirty = true;
            }
            STLV.last = mp;
            ui_request_frames(2);
        }
    }
    /* the boundary under the mouse, named beside it (not while turning, nor on very large parts) */
    int hv = -1;
    if (hov && !STLV.dragging && !STLV.panning && STLP.stl.nTri <= 300000 && STLV.rtOk && STLP.open == ST_BOUND) {
        static Vector2 at = { -1, -1 }; static int was = -1;
        if (mp.x != at.x || mp.y != at.y) { at = mp; was = view_pick(view, mp); }
        hv = was;
    }
    if (hv != STLV.hover) { STLV.hover = hv; STLV.dirty = true; }
    if (hov) {
        if (orbit_keys(&STLV.O)) { STLV.dirty = true; STLV.touched = true; }
        /* the view sits in a scrolling page: the wheel scrolls past it, and zooms once it is clicked (or with Ctrl) */
        if (STLV.engaged || inp_key_down(KEY_LEFT_CONTROL) || inp_key_down(KEY_RIGHT_CONTROL)) {
            float wh = ui_take_wheel();
            if (wh != 0) { STLV.O.dist *= powf(0.88f, wh); STLV.dirty = true; STLV.touched = true; }
        }
        ui_cursor(STLV.panning ? MOUSE_CURSOR_RESIZE_ALL : MOUSE_CURSOR_POINTING_HAND);
    }
    if (STLP.sel != STLV.drawnSel || STLV.hover != STLV.drawnHover) STLV.dirty = true;
    {   /* the tunnel and the arrow follow the domain's settings */
        double ds = STLP.open * 37.0 + STLV.up * 53.0 + STLP.m.base * 41.0 + STLP.m.targetPct * 0.43 + STLP.m.nZones * 3.1 + STLP.m.scale * 0.17 + STLP.m.domain + STLP.m.flowDir * 7.0 + mesher_up(&STLP.m) * 13.0 + STLP.m.up * 1.1 + STLP.m.down * 3.3 + STLP.m.side * 5.7 + STLP.m.top * 11.9 + STLP.m.below * 2.9 + STLP.m.ground * 0.7 + STLP.m.refLen * 17.3 + STLP.m.twoD * 23.0 + STLP.m.normal2D * 29.0;
        static double was = -1;
        if (ds != was) { was = ds; STLV.dirty = true; }
    }
    if (STLV.dirty) view_redraw(view);
    if (STLV.rtOk) DrawTexturePro(STLV.rt.texture, (Rectangle){ 0, 0, (float)STLV.rtW, -(float)STLV.rtH }, view, (Vector2){ 0, 0 }, 0, WHITE);
    ui_rect_lines(view, 12, 1, TH.lineSoft);
    ui_register(ui_id("stl-view"), view, "STL view");
    {   /* which axis is up: the view turns about it (STL files come with x, y or z up, whatever the CAD program likes) */
        static const char *ax[] = { "X", "Y", "Z" };
        int u = STLV.up;
        ui_text(F_MED, 12, ui_cap("Up"), upR.x - 26, upR.y + 8, TH.dark ? (Color){ 200, 210, 230, 200 } : (Color){ 60, 70, 90, 200 });
        if (ui_segmented(ui_id("stl-view-up"), upR, ax, 3, &u) && u != STLV.up) { STLV.up = u; STLV.touched = false; STLV.fitW = 0; STLV.dirty = true; }
        ui_tooltip(upR, "The axis that points up in the view");
    }
    ui_rect(turnR, 9, ui_alpha(TH.surface, 0.82f));
    switch (orbit_tools(&STLV.O, "stl-view", turnR.x + 8, turnR.y + 5, true)) {
    case 1: STLV.touched = true; STLV.dirty = true; break;
    case 2: STLV.touched = false; STLV.fitW = 0; STLV.dirty = true; break;      /* fitted afresh next frame */
    }
    if (STLV.hover >= 0 && STLV.hover < STLP.stl.nRegions) {
        const char *nm = STLP.stl.reg[STLV.hover].name;
        float tw = ui_text_w(F_MONO_MED, 13, nm) + 18;
        Rectangle tag = { fminf(mp.x + 14, view.x + view.width - tw - 6), mp.y - 32, tw, 26 };
        ui_rect(tag, 6, ui_alpha(TH.surface, 0.92f));
        ui_rect_lines(tag, 6, 1, sm_region_color(STLV.hover));
        ui_text(F_MONO_MED, 13, nm, tag.x + 9, tag.y + 5, TH.text);
    }
    char how[240];
    if (STLP.open == ST_BOUND) snprintf(how, sizeof how, "Click a face to pick its boundary  ·  %s  ·  right-drag to move  ·  click, then wheel to zoom", orbit_hint(&STLV.O));
    else {
        snprintf(how, sizeof how, "%s  ·  right-drag to move  ·  click, then wheel to zoom  ·  double-click to fit", orbit_hint(&STLV.O));
        how[0] = (char)(how[0] - 'a' + 'A');
    }
    ui_text_clip(F_REG, 12, how, view.x + 96, view.y + view.height - 22, view.width - 110,             /* right of the axes */ TH.dark ? (Color){ 200, 210, 230, 160 } : (Color){ 60, 70, 90, 170 });
}

