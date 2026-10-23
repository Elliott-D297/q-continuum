/* page_xsection.c -- the cross section of a 2-D case: its cells, its patches in colour, and a field over it. Drawn
 * once into a texture and redrawn only when the mesh, the field or the view changes. Wheel zooms, drag pans,
 * double-click fits. A 3-D mesh is cut by a plane (x, y or z, moved with a slider) and shown the same way. Shown on
 * the Mesh page (View tab) and, small, on the Overview. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "rlgl.h"
#include "xsection.h"

static struct {
    XSection X;
    char     path[PL_PATH];
    char     field[64];                 /* "" = the mesh alone */
    char     time[32];
    time_t   fieldMtime;
    char     fields[96][64]; int nFields;
    double   checkedAt;
    RenderTexture2D rt; int rtW, rtH; bool rtOk;
    bool     dirty;
    float    cu, cv, scale;             /* view centre and logical pixels per metre */
    bool     fitted;
    float    lastW, lastH;
    bool     dragging; Vector2 dragFrom; float dragCu, dragCv;
    bool     edges;
    int      hoverPoly;
    int      fitMode;                   /* the next fit: 0 automatic, 1 whole domain, 2 walls */
    /* a 3-D mesh: the cut */
    int      axis; float frac; bool reslice, sliding; char region[64];
} V = { .edges = true, .hoverPoly = -1, .axis = 2, .frac = 0.5f };

/* a perceptual rainbow (Turbo, polynomial fit) */
static Color turbo(float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float r = 0.13572138f + t * (4.61539260f + t * (-42.66032258f + t * (132.13108234f + t * (-152.94239396f + t * 59.28637943f))));
    float g = 0.09140261f + t * (2.19418839f + t * (4.84296658f + t * (-14.18503333f + t * (4.27729857f + t * 2.82956604f))));
    float b = 0.10667330f + t * (12.64194608f + t * (-60.58204836f + t * (110.36276771f + t * (-89.90310912f + t * 27.34824973f))));
    return (Color){ (unsigned char)(fminf(fmaxf(r, 0), 1) * 255), (unsigned char)(fminf(fmaxf(g, 0), 1) * 255), (unsigned char)(fminf(fmaxf(b, 0), 1) * 255), 255 };
}

/* the fields at the latest written time (or the initial ones) */
static void list_fields(const Case *c)
{
    V.nFields = 0;
    const char *lt = c->s.latest;
    if (c->s.nTimes > 0 && lt[0]) pl_strlcpy(V.time, lt, sizeof V.time);
    else pl_strlcpy(V.time, c->s.hasZero ? "0" : "", sizeof V.time);
    if (!V.time[0]) return;
    char rel[200]; if (c->region[0]) snprintf(rel, sizeof rel, "%s/%s", V.time, c->region); else pl_strlcpy(rel, V.time, sizeof rel);
    char d[PL_PATH]; pl_join(d, sizeof d, c->s.path, rel);
    NameList l = { 0 };
    pl_list_dir(d, &l, false, true);
    names_sort(&l);
    for (int i = 0; i < l.n && V.nFields < 96; i++) {
        const char *n = l.names[i];
        if (n[0] == '.' || pl_ends_with(n, ".gz") || pl_ends_with(n, "~") || pl_ends_with(n, ".orig")) continue;
        if (pl_starts_with(n, "ILambda_")) continue;                  /* fvDOM's one field per ray: G is their sum */
        char fp[PL_PATH]; pl_join(fp, sizeof fp, d, n);
        if (pl_is_dir(fp)) continue;                                  /* uniform/, a region's folder */
        pl_strlcpy(V.fields[V.nFields++], n, sizeof V.fields[0]);
    }
    names_free(&l);
    /* the fields people look at first go to the top (T sorted after qr, below the list's fold) */
    static const char *first[] = { "U", "p", "p_rgh", "T", "alpha.water", "k", "epsilon", "omega", "nut" };
    int at = 0;
    for (size_t m = 0; m < sizeof first / sizeof first[0]; m++)
        for (int i = at; i < V.nFields; i++)
            if (!strcmp(V.fields[i], first[m])) {
                char keep[sizeof V.fields[0]]; pl_strlcpy(keep, V.fields[i], sizeof keep);
                memmove(V.fields[at + 1], V.fields[at], (size_t)(i - at) * sizeof V.fields[0]);
                pl_strlcpy(V.fields[at++], keep, sizeof V.fields[0]);
                break;
            }
}

/* a 2-D case: its own plane; a 3-D one (or a region): the cut at V.axis, V.frac */
static void load_mesh(const Case *c)
{
    char dir[PL_PATH]; pl_join(dir, sizeof dir, c->s.path, case_rpath((Case *)c, "constant/polyMesh"));
    if (!c->region[0] && xs_is_2d(c->s.path)) xs_load(&V.X, c->s.path);
    else xs_load_slice(&V.X, dir, V.axis, V.frac);
}

static void refresh(const Case *c)
{
    char owner[PL_PATH]; pl_join(owner, sizeof owner, c->s.path, case_rpath((Case *)c, "constant/polyMesh/owner"));
    bool newCase = strcmp(V.path, c->s.path) != 0 || strcmp(V.region, c->region) != 0;
    bool otherCase = strcmp(V.path, c->s.path) != 0;          /* another region keeps the field when it has one too */
    if (V.reslice) {                                          /* the plane moved: cut again, keep the view and the field */
        V.reslice = false;
        load_mesh(c);
        V.dirty = true; V.hoverPoly = -1; V.fieldMtime = 0;
        if (V.field[0]) { char fn[160]; pl_fmt_ui(fn, sizeof fn, "%s%s%s", c->region, c->region[0] ? "/" : "", V.field);
                          if (!xs_load_field(&V.X, c->s.path, V.time, fn)) V.field[0] = 0; }
        return;
    }
    if (!newCase && GetTime() - V.checkedAt < 2.0) return;
    V.checkedAt = GetTime();
    if (newCase || pl_mtime(owner) != V.X.meshTime) {
        if (newCase) { V.frac = 0.5f; V.axis = 2; }
        pl_strlcpy(V.path, c->s.path, sizeof V.path);
        pl_strlcpy(V.region, c->region, sizeof V.region);
        load_mesh(c);
        /* a 3-D cut: across the longest two directions, through the middle */
        if (V.X.slice && newCase) {
            float e[3]; for (int k = 0; k < 3; k++) e[k] = 0;
            e[V.X.axU] = V.X.hi[0] - V.X.lo[0]; e[V.X.axV] = V.X.hi[1] - V.X.lo[1]; e[V.X.sliceAxis] = V.X.sliceHi - V.X.sliceLo;
            int thin = 0; for (int k = 1; k < 3; k++) if (e[k] < e[thin]) thin = k;
            if (thin != V.axis) { V.axis = thin; load_mesh(c); }
        }
        V.fitted = false; V.dirty = true; V.hoverPoly = -1; V.fitMode = 0;
        if (otherCase) V.field[0] = 0;
        V.fieldMtime = 0;
    }
    char was[32]; pl_strlcpy(was, V.time, sizeof was);
    list_fields(c);
    /* a field: (re)load when chosen, when a newer time is written, or when its file changes */
    if (V.field[0]) {
        char fp[PL_PATH], rel[PL_PATH]; pl_fmt_ui(rel, sizeof rel, "%s/%s%s%s", V.time, c->region, c->region[0] ? "/" : "", V.field);
        pl_join(fp, sizeof fp, c->s.path, rel);
        time_t m = pl_mtime(fp);
        if (!m) { V.field[0] = 0; free(V.X.val); V.X.val = NULL; V.dirty = true; }
        else if (strcmp(was, V.time) || m != V.fieldMtime || !V.X.val) {
            V.fieldMtime = m;
            char fn[160]; pl_fmt_ui(fn, sizeof fn, "%s%s%s", c->region, c->region[0] ? "/" : "", V.field);
            if (!xs_load_field(&V.X, c->s.path, V.time, fn)) V.field[0] = 0;
            V.dirty = true;
        }
    }
}

/* the extent of the wall patches, when there are any */
static bool walls_box(float lo[2], float hi[2])
{
    XSection *x = &V.X;
    lo[0] = lo[1] = 1e30f; hi[0] = hi[1] = -1e30f;
    bool any = false;
    for (int i = 0; i < x->nSeg; i++) {
        if (strcmp(x->ptype[x->segPatch[i]], "wall")) continue;
        float *g = &x->seg[i * 4];
        for (int k = 0; k < 2; k++) {
            lo[0] = fminf(lo[0], g[k * 2]); hi[0] = fmaxf(hi[0], g[k * 2]);
            lo[1] = fminf(lo[1], g[k * 2 + 1]); hi[1] = fmaxf(hi[1], g[k * 2 + 1]);
        }
        any = true;
    }
    return any;
}

/* mode 0: the walls when they are small in a big domain (an airfoil in its far field), else everything; 1 everything;
   2 the walls */
static void fit(float w, float h, int mode)
{
    XSection *x = &V.X;
    float lo[2] = { x->lo[0], x->lo[1] }, hi[2] = { x->hi[0], x->hi[1] }, wl[2], wh[2];
    if (mode != 1 && walls_box(wl, wh)) {
        float ws = fmaxf(wh[0] - wl[0], wh[1] - wl[1]), ds = fmaxf(hi[0] - lo[0], hi[1] - lo[1]);
        if (mode == 2 || ws < 0.2f * ds) {
            float m = ws * 0.35f;                          /* some flow around them */
            lo[0] = wl[0] - m; lo[1] = wl[1] - m; hi[0] = wh[0] + m; hi[1] = wh[1] + m;
        }
    }
    float du = hi[0] - lo[0], dv = hi[1] - lo[1];
    V.cu = (lo[0] + hi[0]) / 2; V.cv = (lo[1] + hi[1]) / 2;
    V.scale = fminf(w / fmaxf(du, 1e-12f), h / fmaxf(dv, 1e-12f)) * 0.92f;
    V.fitted = true; V.dirty = true;
}

static void redraw(Rectangle r)
{
    float s = ui_scale();
    int w = (int)(r.width * s), h = (int)(r.height * s);
    if (w < 8 || h < 8) return;
    if (!V.rtOk || w != V.rtW || h != V.rtH) {
        if (V.rtOk) UnloadRenderTexture(V.rt);
        V.rt = LoadRenderTexture(w, h);
        V.rtOk = V.rt.id != 0; V.rtW = w; V.rtH = h;
    }
    if (!V.rtOk) return;
    XSection *x = &V.X;
    EndScissorMode();
    BeginTextureMode(V.rt);
    ClearBackground(TH.chartBg);
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    float k = V.scale * s;
#define PX(U) ((float)(((U) - V.cu) * k + w * 0.5f))
#define PY(Q) ((float)(h * 0.5f - ((Q) - V.cv) * k))
    Color base = ui_mix(TH.surface2, TH.text3, 0.25f), edge = ui_alpha(TH.text, 0.28f);
    bool showEdges = V.edges;
    /* cells too small to see edges between: no edges, they'd only make a grey smear */
    if (showEdges && x->nPoly > 0) {
        float area = (x->hi[0] - x->lo[0]) * (x->hi[1] - x->lo[1]) * k * k / x->nPoly;
        if (area < 30) showEdges = false;
    }
    float range = x->val ? fmaxf(x->vmax - x->vmin, 1e-30f) : 1;
    for (int p = 0; p < x->nPoly; p++) {
        int a = x->start[p], e = x->start[p + 1];
        if (e - a < 3) continue;
        /* off screen: skip */
        float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
        for (int i = a; i < e; i++) { float sx = PX(x->u[i]), sy = PY(x->v[i]); minx = fminf(minx, sx); maxx = fmaxf(maxx, sx); miny = fminf(miny, sy); maxy = fmaxf(maxy, sy); }
        if (maxx < 0 || minx > w || maxy < 0 || miny > h) continue;
        Color col = base;
        if (x->val && x->cell[p] < x->nCells) col = turbo((x->val[x->cell[p]] - x->vmin) / range);
        Vector2 o = { PX(x->u[a]), PY(x->v[a]) };
        for (int i = a + 1; i + 1 < e; i++)
            DrawTriangle(o, (Vector2){ PX(x->u[i]), PY(x->v[i]) }, (Vector2){ PX(x->u[i + 1]), PY(x->v[i + 1]) }, col);
        if (showEdges)
            for (int i = a; i < e; i++) {
                int j = i + 1 < e ? i + 1 : a;
                DrawLineV((Vector2){ PX(x->u[i]), PY(x->v[i]) }, (Vector2){ PX(x->u[j]), PY(x->v[j]) }, edge);
            }
    }
    /* the patches, thick and coloured */
    for (int i = 0; i < x->nSeg; i++) {
        float *g = &x->seg[i * 4];
        DrawLineEx((Vector2){ PX(g[0]), PY(g[1]) }, (Vector2){ PX(g[2]), PY(g[3]) }, 2.5f * s, TH.series[x->segPatch[i] % 10]);
    }
#undef PX
#undef PY
    rlDrawRenderBatchActive();                 /* draw the batch while culling is still off: polygons come either way round */
    rlEnableBackfaceCulling();
    EndTextureMode();
    ui_restore_2d();
    V.dirty = false;
}

static bool point_in_poly(const XSection *x, int p, float u, float v)
{
    int a = x->start[p], e = x->start[p + 1];
    bool in = false;
    for (int i = a, j = e - 1; i < e; j = i++)
        if (((x->v[i] > v) != (x->v[j] > v)) && (u < (x->u[j] - x->u[i]) * (v - x->v[i]) / (x->v[j] - x->v[i]) + x->u[i])) in = !in;
    return in;
}

/* the panel. compact: the Overview's preview (no controls but the field); returns the height used */
float xsection_panel(Rectangle r, Case *c, bool compact)
{
    refresh(c);
    XSection *x = &V.X;
    float y0 = r.y;
    if (!x->ok) {
        ui_rect(r, 12, TH.chartBg);
        ui_rect_lines(r, 12, 1, TH.lineSoft);
        ui_text_center(F_REG, 13, x->err[0] ? x->err : "No mesh yet.", r, TH.text3);
        return r.height;
    }
    /* controls */
    float cy = r.y;
    const char *opts[97]; opts[0] = "Mesh only";
    for (int i = 0; i < V.nFields; i++) opts[i + 1] = V.fields[i];
    char pick[64] = "";
    float dw = compact ? 170 : 220;
    if (ui_dropdown(ui_id(compact ? "xs-field-c" : "xs-field"), (Rectangle){ r.x, cy, dw, 32 }, V.field[0] ? V.field : "Mesh only", opts,
                    V.nFields + 1, DD_MONO | DD_FILTER, pick, sizeof pick, "colour by")) {
        if (!strcmp(pick, "Mesh only")) { V.field[0] = 0; free(x->val); x->val = NULL; }
        else { pl_strlcpy(V.field, pick, sizeof V.field); V.fieldMtime = 0; V.checkedAt = -1e9; refresh(c); }
        V.dirty = true;
    }
    float bx = r.x + dw + 10;
    if (!compact) {
        if (ui_checkbox(ui_id("xs-edges"), (Rectangle){ bx, cy + 4, 110, 24 }, &V.edges, "Cell edges")) V.dirty = true;
        bx += 124;
        float lo[2], hi[2];
        if (walls_box(lo, hi)) {
            float bw = ui_button_width(NULL, "Walls");
            if (ui_button(ui_id("xs-fit-w"), (Rectangle){ bx, cy, bw, 32 }, NULL, "Walls", BTN_GHOST)) { V.fitMode = 2; V.fitted = false; }
            bx += bw + 8;
        }
        float bw = ui_button_width(NULL, "Whole domain");
        if (ui_button(ui_id("xs-fit"), (Rectangle){ bx, cy, bw, 32 }, NULL, "Whole domain", BTN_GHOST)) { V.fitMode = 1; V.fitted = false; }
        bx += bw + 12;
    }
    char info[160];
    if (x->val) snprintf(info, sizeof info, "%s at t = %s:  %.4g to %.4g", x->field, x->time, x->vmin, x->vmax);
    else snprintf(info, sizeof info, x->slice ? "%d cells cut" : "%d cells", x->nPoly);
    ui_text_clip(F_REG, 12.5f, info, bx, cy + 8, r.x + r.width - bx, TH.text3);
    cy += 42;
    if (x->slice && !compact) {                               /* a 3-D mesh: which plane, and where along its normal */
        static const char *ax[] = { "Cut across x", "Cut across y", "Cut across z" };
        int a = V.axis;
        if (ui_segmented(ui_id("xs-axis"), (Rectangle){ r.x, cy, 360, 32 }, ax, 3, &a) && a != V.axis) {
            V.axis = a; V.reslice = true; V.fitted = false;
        }
        Rectangle tr = { r.x + 380, cy + 4, r.width - 380 - 190, 24 };
        UiId sid = ui_id("xs-pos");
        ui_register(sid, tr, "slice position");
        ui_rect((Rectangle){ tr.x, tr.y + 9, tr.width, 6 }, 3, TH.lineSoft);
        float kx = tr.x + tr.width * V.frac;
        ui_rect((Rectangle){ tr.x, tr.y + 9, kx - tr.x, 6 }, 3, TH.accent);
        bool sh = ui_hovered(tr);
        if (sh) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (ui_pressed(tr)) V.sliding = true;
        if (V.sliding) {
            if (!ui_mouse_down()) V.sliding = false;
            else {
                float f = (ui_mouse().x - tr.x) / tr.width; f = f < 0 ? 0 : f > 1 ? 1 : f;
                if (fabsf(f - V.frac) > 0.002f) { V.frac = f; V.reslice = true; }
                ui_request_frames(2);
            }
        }
        if (sh) { float wh = ui_take_wheel(); if (wh != 0) { V.frac = fminf(1, fmaxf(0, V.frac + (wh > 0 ? 0.01f : -0.01f))); V.reslice = true; } }
        DrawCircleV((Vector2){ kx, tr.y + 12 }, sh || V.sliding ? 9 : 7, TH.accent);
        char pos[64]; static const char *an[] = { "x", "y", "z" };
        pl_fmt_ui(pos, sizeof pos, "%s = %.4g m", an[V.axis], x->slicePos);
        ui_text(F_MONO, 12.5f, pos, r.x + r.width - 176, cy + 8, TH.text2);
        ui_tooltip(tr, "Drag (or the wheel) to move the cut through the mesh");
        cy += 44;
    }
    Rectangle view = { r.x, cy, r.width, r.y + r.height - cy - (compact ? 30 : 34) };
    if (view.height < 60) return r.height;
    if (!V.fitted || fabsf(view.width - V.lastW) > 0.5f || fabsf(view.height - V.lastH) > 0.5f) {
        if (!V.fitted) fit(view.width, view.height, V.fitMode);
        V.lastW = view.width; V.lastH = view.height; V.dirty = true;
    }
    /* zoom, pan, fit (the large view only) */
    if (!compact) {
        bool hov = ui_hovered(view);
        Vector2 m = ui_mouse();
        float wheel = hov && A.modal == MD_NONE ? ui_take_wheel() : 0;
        if (wheel != 0) {
            float mu = V.cu + (m.x - view.x - view.width / 2) / V.scale, mv = V.cv - (m.y - view.y - view.height / 2) / V.scale;
            float f = wheel > 0 ? 1.25f : 0.8f;
            V.scale *= f;
            V.cu = mu - (m.x - view.x - view.width / 2) / V.scale;   /* keep the point under the mouse where it is */
            V.cv = mv + (m.y - view.y - view.height / 2) / V.scale;
            V.dirty = true;
        }
        if (hov && ui_double_clicked()) { V.fitMode = 0; V.fitted = false; }
        else if (hov && ui_pressed(view)) { V.dragging = true; V.dragFrom = m; V.dragCu = V.cu; V.dragCv = V.cv; }
        if (V.dragging) {
            if (ui_mouse_down()) {
                V.cu = V.dragCu - (m.x - V.dragFrom.x) / V.scale;
                V.cv = V.dragCv + (m.y - V.dragFrom.y) / V.scale;
                V.dirty = true;
                ui_cursor(MOUSE_CURSOR_RESIZE_ALL);
            } else V.dragging = false;
        }
        /* the cell under the mouse */
        V.hoverPoly = -1;
        if (hov && !V.dragging) {
            float mu = V.cu + (m.x - view.x - view.width / 2) / V.scale, mv = V.cv - (m.y - view.y - view.height / 2) / V.scale;
            for (int p = 0; p < x->nPoly; p++) if (point_in_poly(x, p, mu, mv)) { V.hoverPoly = p; break; }
        }
    } else if (ui_hovered(view)) {
        ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (ui_clicked(view)) { A.meshTab = 2; app_go(PG_MESH); }
    }
    if (V.dirty) redraw(view);
    if (V.rtOk) {
        DrawTexturePro(V.rt.texture, (Rectangle){ 0, 0, (float)V.rtW, -(float)V.rtH }, view, (Vector2){ 0, 0 }, 0, WHITE);
        ui_rect_lines(view, 12, 1, TH.lineSoft);
    }
    if (V.hoverPoly >= 0) {
        int cell = x->cell[V.hoverPoly];
        char t[120];
        if (x->val && cell < x->nCells) snprintf(t, sizeof t, "cell %d   %s = %.5g", cell, x->field, x->val[cell]);
        else snprintf(t, sizeof t, "cell %d", cell);
        Vector2 m = ui_mouse();
        float tw = ui_text_w(F_MONO, 12, t) + 20;
        Rectangle b = { fminf(m.x + 14, view.x + view.width - tw - 4), m.y + 16, tw, 26 };
        ui_rect(b, 6, ui_alpha(TH.raised, 0.95f));
        ui_rect_lines(b, 6, 1, TH.line);
        ui_text(F_MONO, 12, t, b.x + 10, b.y + 6, TH.text);
    }
    /* legend: the patches, and the colour scale */
    float ly = view.y + view.height + 8, lx = r.x;
    for (int i = 0; i < x->nPatch; i++) {
        if (!strcmp(x->ptype[i], "empty")) continue;
        Color col = TH.series[i % 10];
        float tw = ui_text_w(F_MONO, 12, x->patch[i]);
        if (lx + tw + 24 > r.x + r.width - (x->val ? 230 : 0)) break;
        DrawRectangleRec((Rectangle){ lx, ly + 7, 14, 4 }, col);
        ui_text(F_MONO, 12, x->patch[i], lx + 19, ly, TH.text2);
        lx += tw + 34;
    }
    if (x->val) {
        float gw = 200, gx = r.x + r.width - gw;
        for (int i = 0; i < 50; i++) DrawRectangleRec((Rectangle){ gx + i * gw / 50, ly + 2, gw / 50 + 1, 10 }, turbo(i / 49.0f));
        char a[32], b[32]; snprintf(a, sizeof a, "%.3g", x->vmin); snprintf(b, sizeof b, "%.3g", x->vmax);
        ui_text(F_MONO, 11, a, gx, ly + 13, TH.text3);
        ui_text(F_MONO, 11, b, gx + gw - ui_text_w(F_MONO, 11, b), ly + 13, TH.text3);
    }
    if (!compact) ui_tooltip(view, "Wheel: zoom   Drag: pan   Double-click: fit");
    else ui_tooltip(view, "Open the cross section on the Mesh page");
    return r.y + r.height - y0;
}
