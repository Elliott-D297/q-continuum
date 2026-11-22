/* page_blocks.c -- Mesh > Block builder (mesh overhaul plan step 5, gaps L3 / E2): start from a template in sizes a learner
 * knows (with the wall's first cell from the y+ calculator), or edit system/blockMeshDict's vertices, blocks, edges and
 * patches as tables, checked as blockMesh would and shown in 3-D. The model, the templates and the checks are bmesh.c.
 *
 * Saving: a template writes the whole blockMeshDict (the one there before kept as blockMeshDict.orig) and its settings to
 * mesher/blocks.ini; the tables put vertices / blocks / edges / boundary / scale back into the file and leave the rest
 * (comments, mergePatchPairs, other keys). A file the model can't hold without loss ($macros, #eval, projections) is shown
 * read-only with the reason. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "bmesh.h"
#include "extrude.h"
#include "precheck.h"
#include "orbit.h"
#include "rlgl.h"

static struct {
    char      path[PL_PATH];          /* the case this state is for                                   */
    BmModel  *m;                      /* the working model                                            */
    bool      have;                   /* m holds something                                            */
    bool      fromFile;               /* m was read from system/blockMeshDict (not yet changed)       */
    bool      readOnly;               /* the file can't be edited here                                */
    char      why[420];
    bool      dirty;                  /* changed since read or written                                */
    int       tmpl;                   /* the template m comes from, -1: hand edits / the file          */
    double    par[BT__N][BT_MAXPAR];
    char      err[300];
    long long sig;                    /* blockMeshDict's identity when read                           */
    int       table;                  /* 0 vertices 1 blocks 2 edges 3 patches                        */
    int       selKind, sel;           /* the selected row (kind as table), -1 none                    */
    BmIssue   is[64]; int nIs;
    BmDraw    draw; bool drawDirty;
    Orbit     O; bool fitted, touched; float aspect;
    RenderTexture2D rt; bool rtOk; int rtW, rtH; bool viewDirty;
    bool      dragging, panning; Vector2 last; double lastClick;
    bool      active;                 /* clicked: the wheel zooms it (else it scrolls the page past it) */
    bool      labels;
    bool      overwriteOk;            /* the file changed outside since read, and Save was pressed once: the next Save writes */
    int       tmplArmed;              /* unsaved hand edits: the template clicked once (a second click replaces them) */
} B = { .tmplArmed = -1, .tmpl = -1, .selKind = -1, .sel = -1, .labels = true };

static void defaults(void)
{
    for (int t = 0; t < BT__N; t++) for (int i = 0; i < BM_TEMPLATES[t].nPar; i++) B.par[t][i] = BM_TEMPLATES[t].par[i].def;
}

static void changed(void)
{
    B.drawDirty = true; B.viewDirty = true;
    B.nIs = B.have ? bm_check(B.m, B.is, 64) : 0;
}

static long long file_sig(const char *p) { FDSig s; return fd_sig(p, &s) ? s.sec * 1000003LL + s.nsec + s.size * 7 : 0; }

/* mesher/blocks.ini: "template = pipe" and its settings, one "key = value" a line */
static int ini_load(const char *casePath)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, "mesher/blocks.ini");
    char *t = pl_read_file(p, NULL);
    if (!t) return -1;
    int tm = -1;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        char k[64], v[128];
        if (sscanf(l, " %63[^= ] = %127s", k, v) != 2) continue;
        if (!strcmp(k, "template")) { for (int i = 0; i < BT__N; i++) if (!strcmp(v, BM_TEMPLATES[i].key)) tm = i; continue; }
        if (tm >= 0) { int i = bm_template_par(tm, k); if (i >= 0) B.par[tm][i] = atof(v); }
    }
    free(t);
    return tm;
}

static void ini_save(const char *casePath)
{
    char d[PL_PATH], p[PL_PATH]; pl_join(d, sizeof d, casePath, "mesher"); pl_mkdirs(d); pl_join(p, sizeof p, d, "blocks.ini");
    Str o; str_init(&o);
    str_cat(&o, "# Q-Continuum, Mesh > Block builder: the template system/blockMeshDict was written from (custom: edited by hand)\n");
    if (B.tmpl >= 0) {
        str_catf(&o, "template = %s\n", BM_TEMPLATES[B.tmpl].key);
        for (int i = 0; i < BM_TEMPLATES[B.tmpl].nPar; i++) str_catf(&o, "%s = %.17g\n", BM_TEMPLATES[B.tmpl].par[i].key, B.par[B.tmpl][i]);   /* exact: reloading compares the files */
    } else str_cat(&o, "template = custom\n");
    pl_write_file(p, o.s, (size_t)o.len);
    str_free(&o);
}

static void load(Case *c)
{
    if (!B.m) { B.m = calloc(1, sizeof *B.m); defaults(); }
    pl_strlcpy(B.path, c->s.path, sizeof B.path);
    B.have = B.fromFile = B.readOnly = B.dirty = false; B.why[0] = B.err[0] = 0; B.tmpl = -1; B.sel = B.selKind = -1;
    B.fitted = B.touched = false;
    defaults();
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, "system/blockMeshDict");
    B.sig = file_sig(p);
    int tm = ini_load(c->s.path);
    FDFile *f = pl_is_file(p) ? fd_load(p) : NULL;
    if (f) {
        if (bm_read(f, B.m, B.why, sizeof B.why)) {
            B.have = B.fromFile = true;
            /* still the template's: the same text as it would write now */
            if (tm >= 0) {
                BmModel *t = malloc(sizeof *t); char e[200];
                if (bm_template(tm, B.par[tm], t, e, sizeof e)) {
                    char *a = bm_write(t, NULL), *b = bm_write(B.m, NULL);
                    if (!strcmp(a, b)) B.tmpl = tm;
                    free(a); free(b);
                }
                free(t);
            }
        } else B.readOnly = true;
        fd_free(f);
    }
    changed();
}

static void use_template(int t)
{
    char e[300];
    BmModel *m = malloc(sizeof *m);
    if (bm_template(t, B.par[t], m, e, sizeof e)) { memcpy(B.m, m, sizeof *m); B.have = true; B.err[0] = 0; }
    else pl_strlcpy(B.err, e, sizeof B.err);
    free(m);
    B.tmpl = t; B.dirty = true; B.fromFile = false;
    changed();
}

/* a hand edit: the template's settings no longer describe the model */
static void hand_edit(void)
{
    if (B.tmpl >= 0) ui_toast(TOAST_INFO, "Edited by hand: the %s template's settings no longer apply", BM_TEMPLATES[B.tmpl].name);
    B.tmpl = -1; B.dirty = true; B.fromFile = false;
    changed();
}

static bool save(Case *c, bool mesh)
{
    if (!B.have) return false;
    int errs = 0;
    for (int i = 0; i < B.nIs; i++) if (B.is[i].level >= 2) errs++;
    if (errs) { ui_toast(TOAST_ERR, "%d problem%s to fix first (the list under the tables)", errs, errs == 1 ? "" : "s"); return false; }
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, "system/blockMeshDict");
    /* changed on disk since it was read (an editor, a script): say so once; the second Save writes over it */
    if (B.sig && pl_is_file(p) && file_sig(p) != B.sig && !B.overwriteOk) {
        B.overwriteOk = true;
        ui_toast(TOAST_WARN, "system/blockMeshDict changed outside Q-Continuum since it was read: Save again to write over it, or Revert to see it");
        return false;
    }
    B.overwriteOk = false;
    bool ok;
    FDFile *f = pl_is_file(p) ? fd_load(p) : NULL;
    if (f && B.tmpl < 0 && !B.readOnly) {
        /* the tables: put the model back, the rest of the file as it was */
        BmModel *chk = malloc(sizeof *chk); char w[420];
        bool readable = bm_read(f, chk, w, sizeof w);
        free(chk);
        if (!readable) {
            ui_toast(TOAST_ERR, "Not saved: the file on disk can't be edited here any more (%s)", w);
            fd_free(f);
            return false;
        }
        ok = bm_apply(f, B.m) && fd_save(f);
    } else {
        if (f) {                                       /* a template replaces the file: the first one kept as .orig, the last as .bak */
            char o[PL_PATH + 8]; snprintf(o, sizeof o, "%s.orig", p);
            if (!pl_is_file(o)) pl_write_file(o, f->text, (size_t)f->len);
            snprintf(o, sizeof o, "%s.bak", p);
            pl_write_file(o, f->text, (size_t)f->len);
        }
        char note[600] = "";
        if (B.tmpl >= 0) {
            const BmTemplate *T = &BM_TEMPLATES[B.tmpl];
            int k = snprintf(note, sizeof note, "%s template:", T->name);
            for (int i = 0; i < T->nPar && k < (int)sizeof note - 60; i++)
                k += snprintf(note + k, sizeof note - (size_t)k, " %s %.6g%s%s;", T->par[i].key, B.par[B.tmpl][i], T->par[i].unit[0] ? " " : "", T->par[i].unit);
        }
        char *txt = bm_write(B.m, note);
        char sd[PL_PATH]; pl_join(sd, sizeof sd, c->s.path, "system"); pl_mkdirs(sd);
        ok = pl_write_file(p, txt, strlen(txt));
        free(txt);
    }
    if (f) fd_free(f);
    if (!ok) { ui_toast(TOAST_ERR, "Could not write system/blockMeshDict"); return false; }
    ini_save(c->s.path);
    B.dirty = false; B.fromFile = true; B.readOnly = false;
    B.sig = file_sig(p);
    case_rescan(c);
    if (mesh) app_generate_mesh();
    else ui_toast(TOAST_OK, "Wrote system/blockMeshDict: %.0f cells. Generate mesh runs blockMesh", bm_cells(B.m));
    return true;
}

/* ---- the y+ calculator's first cell, sent here (page_calc.c) */
bool blocks_wants_first(void) { return A.meshTab == 5 && A.cs && !strcmp(B.path, A.cs->s.path) && B.tmpl >= 0 && bm_template_par(B.tmpl, "first") >= 0; }
/* "What mesh do I need?" sends a template (and a first cell, 0: none): applied when the page next draws for the case */
static struct { char path[PL_PATH]; int tmpl; double first; } SUG = { "", -1, 0 };
void blocks_suggest(int tmpl, double first)
{
    if (!A.cs || tmpl < 0 || tmpl >= BT__N) return;
    pl_strlcpy(SUG.path, A.cs->s.path, sizeof SUG.path); SUG.tmpl = tmpl; SUG.first = first;
}

void blocks_take_first(double h1)
{
    if (!blocks_wants_first()) return;
    B.par[B.tmpl][bm_template_par(B.tmpl, "first")] = h1;
    use_template(B.tmpl);
    ui_toast(TOAST_OK, "Block builder: first cell at the walls %.3g mm", h1 * 1e3);
}

/* ================================================================ the preview */
static Color patch_col(const char *type, int i)
{
    if (!strcmp(type, "wall")) return (Color){ 140, 150, 170, 255 };
    if (!strcmp(type, "empty")) return (Color){ 0, 0, 0, 0 };
    if (!strcmp(type, "wedge")) return (Color){ 120, 120, 200, 255 };
    if (!strncmp(type, "symmetry", 8)) return (Color){ 130, 200, 160, 255 };
    static const Color P[] = { { 80, 160, 255, 255 }, { 255, 150, 70, 255 }, { 230, 90, 160, 255 }, { 120, 220, 220, 255 } };
    return P[i % 4];
}

static void fit(void)
{
    const BmDraw *d = &B.draw;
    B.O.target = (Vector3){ (d->lo[0] + d->hi[0]) / 2, (d->lo[1] + d->hi[1]) / 2, (d->lo[2] + d->hi[2]) / 2 };
    float dx = d->hi[0] - d->lo[0], dy = d->hi[1] - d->lo[1], dz = d->hi[2] - d->lo[2];
    float mx = fmaxf(dx, fmaxf(dy, dz)), mn = fminf(dx, fminf(dy, dz));
    bool flat = false;                                       /* 2-D: empty or wedge patches, or very thin */
    for (int p = 0; p < B.m->np; p++) if (!strcmp(B.m->p[p].type, "empty") || !strcmp(B.m->p[p].type, "wedge")) flat = true;
    B.O.upAxis = 1; B.O.spin = -1;
    if (mx > 0 && (flat || mn < mx * 0.05f) && dz == mn) orbit_set(&B.O, 0, 0);   /* 2-D in x-y: straight at it */
    else orbit_set(&B.O, 0.6f, 0.4f);
    float tv = tanf(35.f / 2 * DEG2RAD), th = tv * fmaxf(B.aspect > 0 ? B.aspect : 1.6f, 0.1f);
    Vector3 back = B.O.back, right = orbit_right(&B.O), up = B.O.up;
    float need = 1e-9f;
    for (int c = 0; c < 8; c++) {
        float p[3] = { (c & 1 ? dx : -dx) / 2, (c & 2 ? dy : -dy) / 2, (c & 4 ? dz : -dz) / 2 };
        float x = p[0] * right.x + p[1] * right.y + p[2] * right.z, y = p[0] * up.x + p[1] * up.y + p[2] * up.z;
        float z = p[0] * back.x + p[1] * back.y + p[2] * back.z;
        need = fmaxf(need, fmaxf(z + fabsf(x) / th, z + fabsf(y) / tv));
    }
    B.O.dist = need * 1.12f;
    B.fitted = true; B.viewDirty = true;
}

static Vector3 V3(const BmModel *m, int i) { double s = m->scale > 0 ? m->scale : 1; return (Vector3){ (float)(m->v[i][0] * s), (float)(m->v[i][1] * s), (float)(m->v[i][2] * s) }; }

static void redraw(Rectangle r)
{
    float s = ui_scale();
    int w = (int)(r.width * s), h = (int)(r.height * s);
    if (w < 8 || h < 8) return;
    if (!B.rtOk || w != B.rtW || h != B.rtH) {
        if (B.rtOk) UnloadRenderTexture(B.rt);
        B.rt = LoadRenderTexture(w, h);
        B.rtOk = B.rt.id != 0; B.rtW = w; B.rtH = h;
    }
    if (!B.rtOk) return;
    EndScissorMode();
    BeginTextureMode(B.rt);
    ClearBackground(TH.dark ? (Color){ 10, 14, 22, 255 } : (Color){ 236, 240, 247, 255 });
    rlDrawRenderBatchActive();
    Camera3D cam = orbit_camera(&B.O);
    BeginMode3D(cam);
    rlSetClipPlanes(B.O.dist * 0.01, B.O.dist * 20.0);
    rlDisableBackfaceCulling();
    const BmModel *m = B.m;
    /* the patches' faces, faintly (their corners: curved edges are drawn as lines over them) */
    rlBegin(RL_TRIANGLES);
    for (int p = 0; p < m->np; p++) {
        Color c = patch_col(m->p[p].type, p);
        bool sel = B.selKind == 3 && B.sel == p;
        if (c.a == 0 && !sel) continue;
        if (sel) c = TH.lcars ? TH.lc[1] : TH.accent;
        rlColor4ub(c.r, c.g, c.b, sel ? 150 : 60);
        for (int f = 0; f < m->p[p].nf; f++) {
            const int *q = m->p[p].f[f];
            bool okf = true;
            for (int k = 0; k < 4; k++) if (q[k] < 0 || q[k] >= m->nv) okf = false;
            if (!okf) continue;
            Vector3 a = V3(m, q[0]), b = V3(m, q[1]), cc = V3(m, q[2]), d = V3(m, q[3]);
            rlVertex3f(a.x, a.y, a.z); rlVertex3f(b.x, b.y, b.z); rlVertex3f(cc.x, cc.y, cc.z);
            rlVertex3f(a.x, a.y, a.z); rlVertex3f(cc.x, cc.y, cc.z); rlVertex3f(d.x, d.y, d.z);
        }
    }
    rlEnd();
    rlDrawRenderBatchActive();
    /* the block edges and the cell divisions */
    Color ec = TH.lcars ? TH.lc[1] : TH.accent, tc = TH.dark ? (Color){ 200, 210, 230, 140 } : (Color){ 40, 50, 70, 140 };
    rlBegin(RL_LINES);
    for (int i = 0; i < B.draw.nSeg; i++) {
        Color c = B.draw.segBlockEdge[i] ? ec : tc;
        rlColor4ub(c.r, c.g, c.b, c.a);
        const float *g = &B.draw.seg[i * 6];
        rlVertex3f(g[0], g[1], g[2]); rlVertex3f(g[3], g[4], g[5]);
    }
    /* the selected block, bright */
    if (B.selKind == 1 && B.sel >= 0 && B.sel < m->nb) {
        Color c = TH.warn;
        rlColor4ub(c.r, c.g, c.b, 255);
        for (int k = 0; k < 12; k++)
            for (int sgm = 0; sgm < 16; sgm++) {
                double p0[3], p1[3];
                bm_edge_point(m, B.sel, k, sgm / 16.0, p0); bm_edge_point(m, B.sel, k, (sgm + 1) / 16.0, p1);
                double sc = m->scale > 0 ? m->scale : 1;
                rlVertex3f((float)(p0[0] * sc), (float)(p0[1] * sc), (float)(p0[2] * sc));
                rlVertex3f((float)(p1[0] * sc), (float)(p1[1] * sc), (float)(p1[2] * sc));
            }
    }
    /* the selected curved edge, bright */
    if (B.selKind == 2 && B.sel >= 0 && B.sel < m->ne) {
        const BmEdge *e = &m->e[B.sel];
        if (e->a >= 0 && e->a < m->nv && e->b >= 0 && e->b < m->nv) {
            /* drawn through any block that uses it, so the curve shows as blockMesh will make it */
            bool drawn = false;
            for (int b = 0; b < m->nb && !drawn; b++)
                for (int k = 0; k < 12 && !drawn; k++) {
                    int va, vb;
                    if (!bm_block_edge(m, b, k, &va, &vb) || !((va == e->a && vb == e->b) || (va == e->b && vb == e->a))) continue;
                    drawn = true;
                    Color c = TH.warn;
                    rlColor4ub(c.r, c.g, c.b, 255);
                    double sc = m->scale > 0 ? m->scale : 1;
                    for (int sgm = 0; sgm < 32; sgm++) {
                        double p0[3], p1[3];
                        bm_edge_point(m, b, k, sgm / 32.0, p0); bm_edge_point(m, b, k, (sgm + 1) / 32.0, p1);
                        rlVertex3f((float)(p0[0] * sc), (float)(p0[1] * sc), (float)(p0[2] * sc));
                        rlVertex3f((float)(p1[0] * sc), (float)(p1[1] * sc), (float)(p1[2] * sc));
                    }
                }
        }
    }
    rlEnd();
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    EndMode3D();
    rlSetClipPlanes(RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR);
    orbit_draw_axes(&B.O, 46 * s, h - 46 * s, 28 * s, s);
    EndTextureMode();
    ui_restore_2d();
    B.viewDirty = false;
}

static float preview(Rectangle view)
{
    if (B.drawDirty) { bm_draw_free(&B.draw); bm_draw_build(B.m, &B.draw); B.drawDirty = false; B.viewDirty = true; if (!B.touched) B.fitted = false; }
    ui_rect(view, 12, TH.dark ? (Color){ 10, 14, 22, 255 } : (Color){ 236, 240, 247, 255 });
    float asp = view.height > 0 ? view.width / view.height : 1.6f;
    if (fabsf(asp - B.aspect) > 0.01f) { B.aspect = asp; if (!B.touched) B.fitted = false; }
    if (!B.fitted) fit();
    Rectangle turnR = { view.x + 12, view.y + 10, orbit_tools_w(false) + 16, 40 };
    bool hov = ui_hovered(view) && !ui_hovered(turnR) && A.modal == MD_NONE;
    Vector2 mp = ui_mouse();
    if (!ui_hovered(view) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) B.active = false;   /* a click elsewhere lets the wheel go */
    if (hov && ui_pressed(view)) {
        B.active = true;
        if (ui_time() - B.lastClick < 0.35) { B.touched = false; fit(); B.lastClick = 0; }
        else B.lastClick = ui_time();
        bool shift = inp_key_down(KEY_LEFT_SHIFT) || inp_key_down(KEY_RIGHT_SHIFT);
        B.dragging = !shift; B.panning = shift; B.last = mp;
    }
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) { B.panning = true; B.dragging = false; B.last = mp; }
    if (B.dragging || B.panning) {
        bool down = B.dragging ? ui_mouse_down() : (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || ui_mouse_down());
        if (!down) B.dragging = B.panning = false;
        else {
            float dx = mp.x - B.last.x, dy = mp.y - B.last.y;
            if (dx != 0 || dy != 0) {
                if (B.dragging) orbit_drag(&B.O, B.last, mp, (Vector2){ view.x + view.width / 2, view.y + view.height / 2 });
                else orbit_pan(&B.O, dx, dy);
                B.viewDirty = true; B.touched = true;
            }
            B.last = mp;
            ui_request_frames(2);
        }
    }
    if (hov) {
        if (orbit_keys(&B.O)) B.viewDirty = true;
        float wh = B.active ? ui_take_wheel() : 0;            /* the page scrolls past an untouched preview */
        if (wh != 0) { B.O.dist *= powf(0.88f, wh); B.viewDirty = true; B.touched = true; }
        ui_cursor(B.panning ? MOUSE_CURSOR_RESIZE_ALL : MOUSE_CURSOR_POINTING_HAND);
    }
    if (B.viewDirty) redraw(view);
    if (B.rtOk) DrawTexturePro(B.rt.texture, (Rectangle){ 0, 0, (float)B.rtW, -(float)B.rtH }, view, (Vector2){ 0, 0 }, 0, WHITE);
    /* the vertex numbers, projected */
    const BmModel *m = B.m;
    if (B.labels && m->nv <= 120) {
        Camera3D cam = orbit_camera(&B.O);
        float s = ui_scale();
        for (int i = 0; i < m->nv; i++) {
            Vector2 p = GetWorldToScreenEx(V3(m, i), cam, B.rtW, B.rtH);
            float px = view.x + p.x / s, py = view.y + p.y / s;
            if (px < view.x + 4 || px > view.x + view.width - 20 || py < view.y + 4 || py > view.y + view.height - 14) continue;
            bool sel = B.selKind == 0 && B.sel == i;
            DrawCircleV((Vector2){ px, py }, sel ? 4.5f : 2.5f, sel ? TH.warn : TH.text2);
            char t[12]; snprintf(t, sizeof t, "%d", i);
            ui_text(F_MONO, 11, t, px + 4, py - 13, sel ? TH.warn : TH.text3);
        }
    }
    ui_rect_lines(view, 12, 1, TH.lineSoft);
    ui_register(ui_id("bk-view"), view, "Block preview");
    ui_rect(turnR, 9, ui_alpha(TH.surface, 0.82f));
    if (orbit_tools(&B.O, "bk", turnR.x + 8, turnR.y + 5, false)) B.viewDirty = true;
    const char *how = B.active ? "Drag to turn  ·  right-drag or Shift+drag to move  ·  wheel to zoom  ·  double-click to fit"
                               : "Drag to turn  ·  right-drag or Shift+drag to move  ·  click, then wheel to zoom  ·  double-click to fit";
    ui_text_clip(F_REG, 12, how, view.x + 96, view.y + view.height - 22, view.width - 110, TH.dark ? (Color){ 200, 210, 230, 160 } : (Color){ 60, 70, 90, 170 });
    return view.height;
}

/* ================================================================ the template form */
static bool num_row(const char *id, float x, float y, float lw, float fw, const char *label, const char *unit, double *v, double lo, double hi)
{
    ui_text_clip(F_REG, 13, label, x, y + 9, lw - 8, TH.text2);
    char b[48];
    if (*v != 0 && (fabs(*v) < 1e-3 || fabs(*v) >= 1e6)) snprintf(b, sizeof b, "%.4g", *v); else snprintf(b, sizeof b, "%.6g", *v);
    float uw = unit && *unit ? ui_text_w(F_MONO, 12, unit) + 10 : 0;
    Rectangle r = { x + lw, y, fw - uw, 32 };
    if (uw) ui_text(F_MONO, 12, unit, r.x + r.width + 6, y + 9, TH.text3);
    if (!ui_input(ui_id(id), r, b, sizeof b, NULL, INPUT_MONO | INPUT_NUMERIC)) return false;
    char *e; double d = strtod(b, &e);
    while (e && *e == ' ') e++;
    if (e == b || (e && *e) || !isfinite(d) || d < lo || d > hi) { ui_toast(TOAST_ERR, "Give a number from %g to %g", lo, hi); return false; }   /* NaN passes < and > */
    *v = d;
    return true;
}

static float template_form(Case *c, float x, float y, float w)
{
    float y0 = y;
    const BmTemplate *T = &BM_TEMPLATES[B.tmpl];
    bool wide = w >= 900;
    float fw = wide ? 420 : w, lw = 190, rowH = 40;
    float fy = y;
    ui_text(F_SEMI, 16, T->name, x, fy, TH.text); fy += 26;
    fy += ui_text_wrap(F_REG, 13, T->what, (Rectangle){ x, fy, fw, 80 }, 19, TH.text3, true) + 10;
    for (int i = 0; i < T->nPar; i++) {
        const BmParam *P = &T->par[i];
        char id[48]; snprintf(id, sizeof id, "bk-p-%s", P->key);
        double v = B.par[B.tmpl][i];
        bool first = !strcmp(P->key, "first");
        float fieldW = fminf(170, fw - lw - (first ? 100 : 0));
        if (num_row(id, x, fy, lw, fieldW, P->label, P->unit, &v, P->lo, P->hi)) {
            B.par[B.tmpl][i] = v; use_template(B.tmpl);
        }
        if (P->hint[0]) ui_tooltip((Rectangle){ x, fy, lw - 8, 32 }, P->hint);
        if (first) {                                     /* inside the form's column, after the unit */
            float bw = ui_button_width(ICON_SIGMA, "y+"), bx = fminf(x + lw + fieldW + 26, x + fw - bw);
            if (ui_button(ui_id("bk-yplus"), (Rectangle){ bx, fy, bw, 32 }, ICON_SIGMA, "y+", BTN_GHOST)) calc_open(CALC_WALL);
            ui_tooltip((Rectangle){ bx, fy, bw, 32 }, "Work out the first cell from y+ (Calculators): its Use in the block builder button puts it here");
        }
        fy += rowH;
    }
    char t[300];
    pl_fmt_ui(t, sizeof t, "%d blocks, %.0f cells. Makes: %s.", B.m->nb, bm_cells(B.m), T->patches);
    fy += ui_text_wrap(F_REG, 13, t, (Rectangle){ x, fy + 4, fw, 80 }, 19, TH.text2, true) + 14;
    if (B.err[0]) fy += ui_text_wrap(F_REG, 13, B.err, (Rectangle){ x, fy, fw, 80 }, 19, TH.err, true) + 10;
    float bw = ui_button_width(ICON_PLAY, "Write and mesh"), bw2 = ui_button_width(ICON_FILE_TEXT, "Write blockMeshDict");
    ui_disable(A.cj->active || B.err[0]);
    if (ui_button(ui_id("bk-mesh"), (Rectangle){ x, fy, bw, 38 }, ICON_PLAY, "Write and mesh", BTN_PRIMARY)) save(c, true);
    if (ui_button(ui_id("bk-write"), (Rectangle){ x + bw + 10, fy, bw2, 38 }, ICON_FILE_TEXT, "Write blockMeshDict", BTN_SECONDARY)) save(c, false);
    ui_disable(false);
    fy += 50;
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, "system/blockMeshDict");
    if (pl_is_file(p) && !B.fromFile)
        fy += ui_text_wrap(F_REG, 12.5f, "Writing replaces system/blockMeshDict; the one there now is kept as blockMeshDict.bak (the very first as .orig).", (Rectangle){ x, fy, fw, 60 }, 18, TH.text3, true) + 8;
    float py = wide ? y : fy + 6, ph = wide ? fmaxf(380, fy - y) : 380;
    preview((Rectangle){ wide ? x + fw + 24 : x, py, wide ? w - fw - 24 : w, ph });
    return fmaxf(fy, py + ph) - y0 + 10;
}

/* ================================================================ the tables */
static const char *const PTYPES[] = { "patch", "wall", "symmetryPlane", "symmetry", "empty", "wedge", "cyclic" };
#define NPTYPES 7

/* an editable cell; need: the length the whole text needs (from the bm_*_text functions). Text too long for the cell's
   buffer shows read-only: editing a cut-off copy would write the cut-off copy back */
#define CELL_CAP 8192
static bool text_cell(UiId id, Rectangle r, const char *text, int need, char *out, int cap)
{
    char b[CELL_CAP]; pl_strlcpy(b, text, sizeof b);
    if (need >= CELL_CAP - 1) {
        ui_input(id, r, b, sizeof b, NULL, INPUT_MONO | INPUT_READONLY);
        ui_tooltip(r, "Too long to edit here: edit system/blockMeshDict in Files");
        return false;
    }
    if (!ui_input(id, r, b, sizeof b, NULL, INPUT_MONO)) return false;
    pl_strlcpy(out, b, (size_t)cap);
    return true;
}

static void sel_cell(int kind, int i, Rectangle r)
{
    bool sel = B.selKind == kind && B.sel == i;
    char t[16]; snprintf(t, sizeof t, "%d", i);
    if (ui_row(ui_idn(ui_id(kind == 0 ? "bk-sv" : kind == 1 ? "bk-sb" : kind == 2 ? "bk-se" : "bk-sp"), i), r, sel)) {
        if (sel) B.selKind = B.sel = -1; else { B.selKind = kind; B.sel = i; }
        B.viewDirty = true;
    }
    ui_text(F_MONO_MED, 12.5f, t, r.x + 8, r.y + 9, sel ? TH.warn : TH.text3);
}

static float tables(float x, float y, float w)
{
    float y0 = y;
    BmModel *m = B.m;
    static const char *TAB[4] = { "Vertices", "Blocks", "Edges", "Patches" };
    float th;
    ui_tabs(ui_id("bk-tab"), x, y, TAB, 4, &B.table, &th);
    y += th + 10;
    const float rh = 40;
    char nb[CELL_CAP];
    int del = -1;
    if (B.table == 0) {
        ui_text(F_REG, 12, "#     x                y                z   (before the scale)", x + 8, y, TH.text3);
        double sc = m->scale;
        if (num_row("bk-scale", x + w - 260, y - 8, 70, 150, "scale", NULL, &sc, 1e-12, 1e12) && sc != m->scale) { m->scale = sc; hand_edit(); }
        y += 30;
        float cw = fminf(180, (w - 110) / 3);
        for (int i = 0; i < m->nv; i++) {
            Rectangle row = { x, y, w, rh - 4 };
            if (!ui_culled(row.y, row.y + row.height)) {
                sel_cell(0, i, (Rectangle){ x, y, 44, 32 });
                for (int k = 0; k < 3; k++) {
                    double v = m->v[i][k];
                    char id[32]; snprintf(id, sizeof id, "bk-v%d-%d", i, k);
                    if (num_row(id, x + 50 + k * (cw + 8) - 0, y, 0, cw, "", NULL, &v, -1e12, 1e12) && v != m->v[i][k]) { m->v[i][k] = v; hand_edit(); }
                }
                if (ui_button(ui_idn(ui_id("bk-vdel"), i), (Rectangle){ x + w - 36, y, 32, 32 }, ICON_TRASH_2, NULL, BTN_ICON)) del = i;
            }
            y += rh;
        }
        if (del >= 0) {                                      /* later vertices move down one: renumber what uses them */
            for (int i = del; i < m->nv - 1; i++) memcpy(m->v[i], m->v[i + 1], sizeof m->v[i]);
            m->nv--;
#define RENUM(v) do { if ((v) > del) (v)--; else if ((v) == del) (v) = -1; } while (0)
            for (int b = 0; b < m->nb; b++) for (int k = 0; k < 8; k++) RENUM(m->b[b].v[k]);
            for (int e = 0; e < m->ne; e++) { RENUM(m->e[e].a); RENUM(m->e[e].b); }
            for (int p = 0; p < m->np; p++) for (int f = 0; f < m->p[p].nf; f++) for (int k = 0; k < 4; k++) RENUM(m->p[p].f[f][k]);
#undef RENUM
            hand_edit();
        }
        if (m->nv < BM_MAXV && ui_button(ui_id("bk-vadd"), (Rectangle){ x, y, ui_button_width(ICON_PLUS, "Add a vertex"), 32 }, ICON_PLUS, "Add a vertex", BTN_SECONDARY)) {
            if (m->nv) memcpy(m->v[m->nv], m->v[m->nv - 1], sizeof m->v[0]); else memset(m->v[0], 0, sizeof m->v[0]);
            m->nv++; hand_edit();
        }
        y += 44;
    } else if (B.table == 1) {
        float vw = fminf(230, w * 0.26f), nw = 120, gw = w - 50 - vw - nw - 140 - 50;
        ui_text(F_REG, 12, "#     vertices (8)", x + 8, y, TH.text3);
        ui_text(F_REG, 12, "cells", x + 58 + vw, y, TH.text3);
        ui_text(F_REG, 12, "grading: 3 values (simpleGrading) or 12 (edgeGrading); ((len cells ratio) ...) for multi-grading", x + 66 + vw + nw, y, TH.text3);
        y += 22;
        for (int i = 0; i < m->nb; i++) {
            BmBlock *b = &m->b[i];
            if (!ui_culled(y, y + rh)) {
                sel_cell(1, i, (Rectangle){ x, y, 44, 32 });
                char t[CELL_CAP];
                int need = bm_ints_text(b->v, 8, t, sizeof t);
                if (text_cell(ui_idn(ui_id("bk-bv"), i), (Rectangle){ x + 50, y, vw, 32 }, t, need, nb, sizeof nb)) {
                    if (bm_ints_parse(nb, b->v, 8)) hand_edit(); else ui_toast(TOAST_ERR, "Eight vertex numbers");
                }
                need = bm_ints_text(b->n, 3, t, sizeof t);
                if (text_cell(ui_idn(ui_id("bk-bn"), i), (Rectangle){ x + 58 + vw, y, nw, 32 }, t, need, nb, sizeof nb)) {
                    int n[3];
                    if (bm_ints_parse(nb, n, 3) && n[0] > 0 && n[1] > 0 && n[2] > 0) { memcpy(b->n, n, sizeof n); hand_edit(); }
                    else ui_toast(TOAST_ERR, "Three cell counts, each at least 1");
                }
                need = bm_grading_text(b, t, sizeof t);
                if (text_cell(ui_idn(ui_id("bk-bg"), i), (Rectangle){ x + 66 + vw + nw, y, fmaxf(120, gw), 32 }, t, need, nb, sizeof nb)) {
                    if (bm_grading_parse(nb, b)) hand_edit(); else ui_toast(TOAST_ERR, "Three or twelve gradings: numbers above 0, or ((length cells ratio) ...)");
                }
                if (text_cell(ui_idn(ui_id("bk-bz"), i), (Rectangle){ x + w - 186, y, 140, 32 }, b->zone, (int)strlen(b->zone), nb, sizeof nb)) {
                    pl_trim_inplace(nb);
                    if (strpbrk(nb, " \t(){};\"$#")) ui_toast(TOAST_ERR, "A zone name is one word (or empty: no zone)");
                    else { pl_strlcpy(b->zone, nb, sizeof b->zone); hand_edit(); }
                }
                if (ui_button(ui_idn(ui_id("bk-bdel"), i), (Rectangle){ x + w - 36, y, 32, 32 }, ICON_TRASH_2, NULL, BTN_ICON)) del = i;
            }
            y += rh;
        }
        if (del >= 0) { for (int i = del; i < m->nb - 1; i++) m->b[i] = m->b[i + 1]; m->nb--; hand_edit(); }
        if (m->nb < BM_MAXB && ui_button(ui_id("bk-badd"), (Rectangle){ x, y, ui_button_width(ICON_PLUS, "Add a block"), 32 }, ICON_PLUS, "Add a block", BTN_SECONDARY)) {
            BmBlock *b = &m->b[m->nb];
            memset(b, 0, sizeof *b);
            for (int k = 0; k < 8; k++) b->v[k] = k < m->nv ? k : 0;
            b->n[0] = b->n[1] = b->n[2] = 10;
            for (int k = 0; k < 3; k++) { b->g[k].n = 1; b->g[k].len[0] = b->g[k].cells[0] = b->g[k].ratio[0] = 1; }
            m->nb++; hand_edit();
        }
        y += 44;
    } else if (B.table == 2) {
        ui_text(F_REG, 12, "#     kind                from to    points: arc one (x y z) on it; spline / polyLine / BSpline the ones between", x + 8, y, TH.text3);
        y += 22;
        for (int i = 0; i < m->ne; i++) {
            BmEdge *e = &m->e[i];
            if (!ui_culled(y, y + rh)) {
                sel_cell(2, i, (Rectangle){ x, y, 44, 32 });
                int k = e->kind;
                if (ui_select(ui_idn(ui_id("bk-ek"), i), (Rectangle){ x + 50, y, 130, 32 }, BE_NAME, BE__N, &k) && k != e->kind) {
                    e->kind = k;
                    if (k == BE_ARC && e->np > 1) e->np = 1;
                    hand_edit();
                }
                int ab[2] = { e->a, e->b }; char t[CELL_CAP];
                int need = bm_ints_text(ab, 2, t, sizeof t);
                if (text_cell(ui_idn(ui_id("bk-eab"), i), (Rectangle){ x + 188, y, 90, 32 }, t, need, nb, sizeof nb)) {
                    if (bm_ints_parse(nb, ab, 2)) { e->a = ab[0]; e->b = ab[1]; hand_edit(); } else ui_toast(TOAST_ERR, "Two vertex numbers");
                }
                need = bm_points_text(e, t, sizeof t);
                if (text_cell(ui_idn(ui_id("bk-ep"), i), (Rectangle){ x + 286, y, w - 286 - 44, 32 }, t, need, nb, sizeof nb)) {
                    if (bm_points_parse(nb, e)) hand_edit(); else ui_toast(TOAST_ERR, e->kind == BE_ARC ? "One point (x y z) the arc passes through" : "Points as (x y z) (x y z) ...");
                }
                if (ui_button(ui_idn(ui_id("bk-edel"), i), (Rectangle){ x + w - 36, y, 32, 32 }, ICON_TRASH_2, NULL, BTN_ICON)) del = i;
            }
            y += rh;
        }
        if (del >= 0) { for (int i = del; i < m->ne - 1; i++) m->e[i] = m->e[i + 1]; m->ne--; hand_edit(); }
        if (m->ne < BM_MAXE && m->nv >= 2 && ui_button(ui_id("bk-eadd"), (Rectangle){ x, y, ui_button_width(ICON_PLUS, "Add a curved edge"), 32 }, ICON_PLUS, "Add a curved edge", BTN_SECONDARY)) {
            BmEdge *e = &m->e[m->ne];
            memset(e, 0, sizeof *e);
            e->kind = BE_ARC; e->a = 0; e->b = 1; e->np = 1;
            for (int k = 0; k < 3; k++) e->p[0][k] = (m->v[0][k] + m->v[1][k]) / 2;
            e->p[0][1] += 0.1 * (fabs(m->v[1][0] - m->v[0][0]) + 1e-3);
            m->ne++; hand_edit();
        }
        y += 44;
    } else {
        ui_text(F_REG, 12, "#     name                       type                  faces: (a b c d) ... each a block's face, listed outward", x + 8, y, TH.text3);
        y += 22;
        for (int i = 0; i < m->np; i++) {
            BmPatch *p = &m->p[i];
            if (!ui_culled(y, y + rh)) {
                sel_cell(3, i, (Rectangle){ x, y, 44, 32 });
                if (text_cell(ui_idn(ui_id("bk-pn"), i), (Rectangle){ x + 50, y, 170, 32 }, p->name, (int)strlen(p->name), nb, sizeof nb)) {
                    pl_trim_inplace(nb);
                    if (!nb[0] || strpbrk(nb, " \t(){};\"$#")) ui_toast(TOAST_ERR, "A patch name is one word"); else { pl_strlcpy(p->name, nb, sizeof p->name); hand_edit(); }
                }
                int k = -1;
                for (int j = 0; j < NPTYPES; j++) if (!strcmp(p->type, PTYPES[j])) k = j;
                int was = k;
                if (ui_select_ph(ui_idn(ui_id("bk-pt"), i), (Rectangle){ x + 228, y, 150, 32 }, PTYPES, NPTYPES, &k, p->type) && k != was && k >= 0) {
                    snprintf(p->type, sizeof p->type, "%s", PTYPES[k]); hand_edit();
                }
                char t[CELL_CAP];
                int need = bm_faces_text(p, t, sizeof t);
                if (text_cell(ui_idn(ui_id("bk-pf"), i), (Rectangle){ x + 386, y, w - 386 - 44, 32 }, t, need, nb, sizeof nb)) {
                    if (bm_faces_parse(nb, p)) hand_edit(); else ui_toast(TOAST_ERR, "Faces as (a b c d) (a b c d) ...");
                }
                if (p->extra[0]) ui_tooltip((Rectangle){ x + 50, y, 170, 32 }, p->extra);
                if (ui_button(ui_idn(ui_id("bk-pdel"), i), (Rectangle){ x + w - 36, y, 32, 32 }, ICON_TRASH_2, NULL, BTN_ICON)) del = i;
            }
            y += rh;
        }
        if (del >= 0) { for (int i = del; i < m->np - 1; i++) m->p[i] = m->p[i + 1]; m->np--; hand_edit(); }
        if (m->np < BM_MAXP && ui_button(ui_id("bk-padd"), (Rectangle){ x, y, ui_button_width(ICON_PLUS, "Add a patch"), 32 }, ICON_PLUS, "Add a patch", BTN_SECONDARY)) {
            BmPatch *p = &m->p[m->np];
            memset(p, 0, sizeof *p);
            snprintf(p->name, sizeof p->name, "patch%d", m->np);
            snprintf(p->type, sizeof p->type, "patch");
            m->np++; hand_edit();
        }
        y += 44;
    }
    return y - y0;
}

static float issues(float x, float y, float w)
{
    float y0 = y;
    if (!B.nIs) {
        ui_icon(ICON_CIRCLE_CHECK, x + 8, y + 9, 14, TH.ok);
        ui_text(F_REG, 13, "Nothing blockMesh would refuse.", x + 24, y + 1, TH.text2);
        return 26;
    }
    for (int i = 0; i < B.nIs; i++) {
        Color c = B.is[i].level >= 2 ? TH.err : TH.warn;
        ui_icon(B.is[i].level >= 2 ? ICON_CIRCLE_X : ICON_TRIANGLE_ALERT, x + 8, y + 9, 14, c);
        y += ui_text_wrap(F_REG, 13, B.is[i].text, (Rectangle){ x + 24, y, w - 24, 80 }, 19, B.is[i].level >= 2 ? c : TH.text2, true) + 4;
    }
    return y - y0 + 4;
}

/* ================================================================ extrusion (mesh overhaul plan step 6) */
/* the steps that extrude the mesh, from the dictionaries in system/: also every Generate mesh's, after blockMesh, while
   mesher/extrude.ini says so. The precision goes up while extrudeMesh writes (else the wedge faces load as "not planar") */
#define EX_PREC "cp system/controlDict system/controlDict.qc-precision && foamDictionary -entry writePrecision -set 15 system/controlDict > /dev/null && "
#define EX_UNPREC "; rc=$?; mv -f system/controlDict.qc-precision system/controlDict; exit $rc"
static int extrude_kind(const char *casePath)        /* 0 none, 1 axisymmetric, 2 a patch */
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, "mesher/extrude.ini");
    char *t = pl_read_file(p, NULL);
    int k = !t ? 0 : strstr(t, "kind = axisymmetric") ? 1 : strstr(t, "kind = patch") ? 2 : 0;
    free(t);
    return k;
}

static void extrude_steps(Job *j, int k);
bool extrude_add_steps(Case *c, Job *j)
{
    int k = extrude_kind(c->s.path);
    if (!k) return false;
    extrude_steps(j, k);
    return true;
}

static struct { int axisDir; double axisAt, angle; ExtrudeCfg x; char err[300]; bool init; char path[PL_PATH]; } X;

/* the extrusion steps of a kind (1 axisymmetric, 2 a patch) */
static void extrude_steps(Job *j, int k)
{
    if (k == 1) {
        job_add(j, "axi-sets", "topoSet: the 2-D mesh's front and back faces", "topoSet -dict system/topoSetDict.axi", "log.topoSet.axi", false);
        job_add(j, "axi-patches", "createPatch: them as the front and back patches", "createPatch -dict system/createPatchDict.axi -overwrite", "log.createPatch.axi", false);
    }
    job_add(j, "extrudeMesh", k == 1 ? "extrudeMesh: the wedge" : "extrudeMesh", EX_PREC "extrudeMesh" EX_UNPREC, "log.extrudeMesh", false);
}

static void extrude_run(Case *c, int kind)
{
    if (!app_require_foam() || !app_can_start()) return;
    app_save_all(true);
    job_reset(A.cj, "Mesh");
    job_add(A.cj, "backup", "Keep the mesh as it was (constant/polyMesh.before-extrude)",
            "rm -rf constant/polyMesh.before-extrude && cp -r constant/polyMesh constant/polyMesh.before-extrude && echo kept", "log.extrudeBackup", false);
    extrude_steps(A.cj, kind);
    /* part of the mesh's recipe once it has worked (a failed extrusion isn't repeated by every Generate mesh) */
    job_add(A.cj, "extrude-record", "Generate mesh repeats it from now on (mesher/extrude.ini)",
            kind == 1 ? "mkdir -p mesher && printf '# Q-Continuum: Generate mesh extrudes after meshing (Mesh > Block builder, Extrude)\\nkind = axisymmetric\\n' > mesher/extrude.ini && echo recorded"
                      : "mkdir -p mesher && printf '# Q-Continuum: Generate mesh extrudes after meshing (Mesh > Block builder, Extrude)\\nkind = patch\\n' > mesher/extrude.ini && echo recorded",
            "log.extrudeRecord", false);
    job_add(A.cj, "checkMesh", "checkMesh", "checkMesh", "log.checkMesh", false);
    if (job_start(A.cj, c->s.path, app_bashrc(), GetTime())) ui_toast(TOAST_INFO, "Extruding: every Generate mesh repeats it from now on");
    else ui_toast(TOAST_ERR, "Could not start extrudeMesh");
}

static float extrude_section(Case *c, float x, float y, float w)
{
    float y0 = y;
    if (!c->s.meshed || c->s.nRegions) return 0;
    if (!X.init || strcmp(X.path, c->s.path)) {
        memset(&X, 0, sizeof X); X.init = true; pl_strlcpy(X.path, c->s.path, sizeof X.path);
        X.angle = 5; X.x.model = EX_LINEAR_NORMAL; X.x.nLayers = 10; X.x.expansion = 1; X.x.thickness = 0.1;
        X.x.axis[0] = 1; X.x.angle = 90; X.x.dir[0] = 1;
        for (int i = 0; i < c->nPatches; i++) if (strcmp(c->patches[i].type, "empty") && strcmp(c->patches[i].type, "processor")) { pl_strlcpy(X.x.patch, c->patches[i].name, sizeof X.x.patch); break; }
    }
    y += section_head(x, y + 10, w, "Extrude", "Grow the mesh from one of its patches (more cells beyond an outlet, say), or turn a 2-D mesh "
                                              "into an axisymmetric [[wedge]]: one thin slice of a round flow, far cheaper than 3-D.") + 10;
    int k = extrude_kind(c->s.path);
    if (k) {
        const char *t = k == 1 ? "Every Generate mesh makes this mesh axisymmetric (system/extrudeMeshDict)" : "Every Generate mesh extrudes a patch (system/extrudeMeshDict)";
        float bw = ui_button_width(ICON_TRASH_2, "Stop extruding");
        ui_text_wrap(F_REG, 13, t, (Rectangle){ x, y + 8, w - bw - 16, 40 }, 19, TH.text2, true);
        if (ui_button(ui_id("ex-stop"), (Rectangle){ x + w - bw, y, bw, 34 }, ICON_TRASH_2, "Stop extruding", BTN_GHOST)) {
            char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, "mesher/extrude.ini"); remove(p);
            ui_toast(TOAST_OK, "Generate mesh won't extrude any more: generate again for the mesh without it");
        }
        y += 46;
        y += ui_text_wrap(F_REG, 12.5f, "Generate mesh repeats one extrusion: starting another below replaces this one in the recipe.",
                          (Rectangle){ x, y - 6, w, 40 }, 18, TH.text3, true) + 4;
    }
    /* which way is thin, and which patches are empty: a 2-D mesh */
    double lo[3], hi[3];
    bool bounds = mesh_bounds(c->s.path, lo, hi);
    char empties[256] = "";
    for (int i = 0; i < c->nPatches; i++)
        if (!strcmp(c->patches[i].type, "empty") && c->patches[i].nFaces > 0) {
            size_t n = strlen(empties); snprintf(empties + n, sizeof empties - n, "%s%s", n ? " " : "", c->patches[i].name);
        }
    int thin = -1;
    if (bounds) { double L[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] }; thin = L[0] <= L[1] && L[0] <= L[2] ? 0 : L[1] <= L[2] ? 1 : 2; }
    bool twoD = empties[0] && thin >= 0;
    float half = w >= 900 ? (w - 20) / 2 : w, lw = 170;
    float ax = x, ay = y, bx = w >= 900 ? x + half + 20 : x, by = y;
    /* ---- axisymmetric */
    ui_text(F_SEMI, 15, "Make it axisymmetric", ax, ay, TH.text); ay += 26;
    int nWedge = 0;
    for (int i = 0; i < c->nPatches; i++) if (!strcmp(c->patches[i].type, "wedge") && c->patches[i].nFaces > 0) nWedge++;
    if (!twoD && nWedge >= 2) {
        ay += ui_text_wrap(F_REG, 13, "This mesh is axisymmetric already: it has wedge patches. Mesh > Quality has the wedge angle checkMesh measured.",
                           (Rectangle){ ax, ay, half, 80 }, 19, TH.text2, true) + 10;
    } else if (!twoD) {
        ay += ui_text_wrap(F_REG, 13, "For a 2-D mesh (one cell thick, front and back on empty patches): the radius one way in its plane, the axis along the other.",
                           (Rectangle){ ax, ay, half, 80 }, 19, TH.text3, true) + 10;
    } else {
        int in[2] = { (thin + 1) % 3, (thin + 2) % 3 };
        if (X.axisDir == thin) X.axisDir = in[0];
        static const char *AXN[3] = { "Along x", "Along y", "Along z" };
        const char *items[2] = { AXN[in[0]], AXN[in[1]] };
        int sel = X.axisDir == in[1];
        ui_text(F_REG, 13, "The axis", ax, ay + 9, TH.text2);
        if (ui_segmented(ui_id("ex-axdir"), (Rectangle){ ax + lw, ay, fminf(240, half - lw), 34 }, items, 2, &sel)) X.axisDir = in[sel];
        ay += 42;
        int j = 3 - thin - X.axisDir;
        char lab[48]; snprintf(lab, sizeof lab, "The axis at %c =", "xyz"[j]);
        num_row("ex-axat", ax, ay, lw, fminf(170, half - lw), lab, "m", &X.axisAt, -1e9, 1e9);
        ay += 40;
        num_row("ex-angle", ax, ay, lw, fminf(170, half - lw), "Wedge angle", "°", &X.angle, 0.1, 10);
        ay += 42;
        char t[300];
        pl_fmt_ui(t, sizeof t, "The mesh runs from %c = %.4g to %.4g: %s", "xyz"[j], lo[j], hi[j],
                  lo[j] < X.axisAt - 1e-12 && hi[j] > X.axisAt + 1e-12 ? "it crosses this axis; move the axis to its edge." :
                  "the radius runs from the axis out to its far side. The faces on the axis close up into thin prisms.");
        ay += ui_text_wrap(F_REG, 12.5f, t, (Rectangle){ ax, ay, half, 80 }, 18, TH.text3, true) + 8;
        float bw = ui_button_width(ICON_ROTATE_CCW, "Make it axisymmetric");
        ui_disable(A.cj->active);
        if (ui_button(ui_id("ex-axi"), (Rectangle){ ax, ay, bw, 36 }, ICON_ROTATE_CCW, "Make it axisymmetric", BTN_PRIMARY)) {
            AxiCfg a = { .thin = thin, .axisDir = X.axisDir, .axisAt = X.axisAt, .angle = X.angle };
            memcpy(a.lo, lo, sizeof lo); memcpy(a.hi, hi, sizeof hi);
            bool taken = false;                              /* createPatch would add the faces to a patch already called front / back */
            for (int i = 0; i < c->nPatches; i++) if (!strcmp(c->patches[i].name, "front") || !strcmp(c->patches[i].name, "back")) taken = true;
            if (taken) { pl_strlcpy(a.front, "wedgeFront", sizeof a.front); pl_strlcpy(a.back, "wedgeBack", sizeof a.back); }
            pl_strlcpy(a.empties, empties, sizeof a.empties);
            char *ts, *cp, *ex, why[300];
            if (!ex_axi_dicts(&a, &ts, &cp, &ex, why, sizeof why)) ui_toast(TOAST_ERR, "%s", why);
            else {
                char p[PL_PATH];
                pl_join(p, sizeof p, c->s.path, "system/topoSetDict.axi"); pl_write_file(p, ts, strlen(ts));
                pl_join(p, sizeof p, c->s.path, "system/createPatchDict.axi"); pl_write_file(p, cp, strlen(cp));
                pl_join(p, sizeof p, c->s.path, "system/extrudeMeshDict"); pl_write_file(p, ex, strlen(ex));
                free(ts); free(cp); free(ex);
                extrude_run(c, 1);
            }
        }
        ui_disable(false);
        ay += 48;
    }
    /* ---- a patch */
    if (w < 900) by = ay + 10;
    ui_text(F_SEMI, 15, "Extrude a patch", bx, by, TH.text); by += 26;
    const char *pn[64]; int np = 0, psel = -1;
    for (int i = 0; i < c->nPatches && np < 64; i++) {
        if (!strcmp(c->patches[i].type, "empty") || !strcmp(c->patches[i].type, "processor")) continue;
        if (!strcmp(c->patches[i].name, X.x.patch)) psel = np;
        pn[np++] = c->patches[i].name;
    }
    ui_text(F_REG, 13, "Patch", bx, by + 9, TH.text2);
    if (ui_select_ph(ui_id("ex-patch"), (Rectangle){ bx + lw, by, fminf(220, half - lw), 34 }, pn, np, &psel, "a patch") && psel >= 0) pl_strlcpy(X.x.patch, pn[psel], sizeof X.x.patch);
    by += 42;
    static const char *MOD[3] = { "Straight out", "One direction", "Round an axis" };
    static const int MODK[3] = { EX_LINEAR_NORMAL, EX_LINEAR_DIRECTION, EX_SECTOR };
    int ms = X.x.model == EX_LINEAR_DIRECTION ? 1 : X.x.model == EX_SECTOR ? 2 : 0;
    if (ui_segmented(ui_id("ex-model"), (Rectangle){ bx, by, fminf(420, half), 34 }, MOD, 3, &ms)) X.x.model = MODK[ms];
    by += 42;
    double nl = X.x.nLayers;
    if (num_row("ex-layers", bx, by, lw, fminf(170, half - lw), "Layers", NULL, &nl, 1, 10000)) X.x.nLayers = (int)nl;
    by += 40;
    num_row("ex-exp", bx, by, lw, fminf(170, half - lw), "Growth per layer", NULL, &X.x.expansion, 0.01, 100);
    by += 40;
    if (X.x.model != EX_SECTOR) { num_row("ex-thick", bx, by, lw, fminf(170, half - lw), "Thickness", "m", &X.x.thickness, 1e-12, 1e9); by += 40; }
    if (X.x.model == EX_LINEAR_DIRECTION) {
        ui_text(F_REG, 13, "Direction", bx, by + 9, TH.text2);
        for (int q = 0; q < 3; q++) { char id[24]; snprintf(id, sizeof id, "ex-dir%d", q); num_row(id, bx + lw + q * 72, by, 0, 64, "", NULL, &X.x.dir[q], -1e9, 1e9); }
        by += 40;
    }
    if (X.x.model == EX_SECTOR) {
        ui_text(F_REG, 13, "Point on the axis", bx, by + 9, TH.text2);
        for (int q = 0; q < 3; q++) { char id[24]; snprintf(id, sizeof id, "ex-pt%d", q); num_row(id, bx + lw + q * 72, by, 0, 64, "", NULL, &X.x.point[q], -1e9, 1e9); }
        by += 40;
        ui_text(F_REG, 13, "Axis direction", bx, by + 9, TH.text2);
        for (int q = 0; q < 3; q++) { char id[24]; snprintf(id, sizeof id, "ex-ax%d", q); num_row(id, bx + lw + q * 72, by, 0, 64, "", NULL, &X.x.axis[q], -1e9, 1e9); }
        by += 40;
        num_row("ex-sang", bx, by, lw, fminf(170, half - lw), "Angle", "°", &X.x.angle, 0.1, 360);
        by += 40;
    }
    ui_checkbox(ui_id("ex-flip"), (Rectangle){ bx, by + 4, 260, 22 }, &X.x.flip, "The other way (into the mesh's side)");
    by += 34;
    float bw = ui_button_width(ICON_PLAY, "Extrude");
    ui_disable(A.cj->active || psel < 0);
    if (ui_button(ui_id("ex-go"), (Rectangle){ bx, by, bw, 36 }, ICON_PLAY, "Extrude", BTN_SECONDARY)) {
        char why[300];
        char *d = ex_patch_dict(&X.x, why, sizeof why);
        if (!d) ui_toast(TOAST_ERR, "%s", why);
        else {
            char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, "system/extrudeMeshDict"); pl_write_file(p, d, strlen(d)); free(d);
            extrude_run(c, 2);
        }
    }
    ui_disable(false);
    by += 48;
    return fmaxf(ay, by) - y0;
}

/* ================================================================ the page */
static float blocks_body(Case *c, float x, float y, float w);
float blocks_page(Case *c, float x, float y, float w)
{
    float h = blocks_body(c, x, y, w);
    return h + extrude_section(c, x, y + h, w);
}

static float blocks_body(Case *c, float x, float y, float w)
{
    float y0 = y;
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, "system/blockMeshDict");
    if (!B.m || strcmp(B.path, c->s.path) || (!B.dirty && file_sig(p) != B.sig)) load(c);   /* another case, or the file changed */
    if (SUG.tmpl >= 0 && !strcmp(SUG.path, c->s.path)) {        /* sent by "What mesh do I need?" */
        int t = SUG.tmpl; SUG.tmpl = -1;
        if (B.dirty && B.tmpl < 0) ui_toast(TOAST_WARN, "The tables have changes not saved yet: click %s to replace them with the template", BM_TEMPLATES[t].name);
        else {
            int fi = bm_template_par(t, "first");
            if (fi >= 0 && SUG.first > 0) B.par[t][fi] = SUG.first;
            B.touched = false;
            use_template(t);
            ui_toast(TOAST_OK, "The %s template, from What mesh do I need?: Save writes it to system/blockMeshDict", BM_TEMPLATES[t].name);
        }
    }
    /* the templates */
    y += section_head(x, y, w, "Start from a template", "A mesh from sizes you know: pick a shape, type its dimensions and how fine, look at it, "
                                                       "write it. The [[first-layer|first cell]] at the walls can come from the y+ calculator.");
    int cols = w >= 1000 ? 4 : w >= 640 ? 3 : 2;
    float gap = 12, cw = (w - gap * (cols - 1)) / cols, ch = 84;
    for (int t = 0; t < BT__N; t++) {
        Rectangle r = { x + (t % cols) * (cw + gap), y + (t / cols) * (ch + gap), cw, ch };
        bool sel = B.tmpl == t;
        if (ui_row(ui_idn(ui_id("bk-t"), t), r, sel)) {
            if (B.dirty && B.tmpl < 0 && B.tmplArmed != t) {   /* unsaved hand edits: ask (a second click) before replacing them */
                B.tmplArmed = t;
                ui_toast(TOAST_WARN, "The tables have changes not saved yet: click %s again to replace them with the template", BM_TEMPLATES[t].name);
            } else {
                B.tmplArmed = -1;
                B.touched = false;
                use_template(t);
            }
        }
        ui_register(ui_idn(ui_id("bk-t"), t), r, BM_TEMPLATES[t].name);
        if (!sel) ui_rect_lines(r, TH.lcars ? 14 : 8, 1, TH.lineSoft);
        ui_text_clip(F_MED, 14, BM_TEMPLATES[t].name, r.x + 12, r.y + 10, r.width - 24, sel ? TH.text : TH.text2);
        ui_text_wrap(F_REG, 12, BM_TEMPLATES[t].what, (Rectangle){ r.x + 12, r.y + 32, r.width - 24, 44 }, 16, TH.text3, true);
    }
    y += ((BT__N + cols - 1) / cols) * (ch + gap) + 8;
    if (B.tmpl >= 0) y += template_form(c, x, y + 6, w) + 6;
    /* the blocks themselves */
    y += section_head(x, y + 6, w, "Blocks", "system/blockMeshDict as tables: vertices, hex blocks with their cells and [[grading]], curved edges, "
                                             "patches. Changes are checked as you go; Save writes them, the rest of the file kept.") + 6;
    if (B.readOnly) {
        const char *t = "The block builder can't edit this blockMeshDict";
        float chh = callout_height(w, t, B.why) + 46;
        callout((Rectangle){ x, y, w, chh }, TOAST_INFO, ICON_INFO, t, B.why);
        float bw = ui_button_width(ICON_FILE_TEXT, "Open it in Files");
        if (ui_button(ui_id("bk-open"), (Rectangle){ x + 42, y + chh - 44, bw, 34 }, ICON_FILE_TEXT, "Open it in Files", BTN_SECONDARY)) {
            pl_strlcpy(A.filesSel, "system/blockMeshDict", sizeof A.filesSel); A.filesRaw = true; app_go(PG_FILES);
        }
        return y + chh + 14 - y0;
    }
    if (!B.have) {
        y += ui_text_wrap(F_REG, 13, "No blockMeshDict yet: start from a template above, or from one block.", (Rectangle){ x, y, w, 40 }, 19, TH.text3, true) + 10;
        float bw = ui_button_width(ICON_PLUS, "Start with one block");
        if (ui_button(ui_id("bk-one"), (Rectangle){ x, y, bw, 34 }, ICON_PLUS, "Start with one block", BTN_SECONDARY)) {
            double v[BT_MAXPAR];
            for (int i = 0; i < BM_TEMPLATES[BT_CHANNEL].nPar; i++) v[i] = BM_TEMPLATES[BT_CHANNEL].par[i].def;
            v[bm_template_par(BT_CHANNEL, "W")] = 0.1; v[bm_template_par(BT_CHANNEL, "H")] = 0.1; v[bm_template_par(BT_CHANNEL, "L")] = 0.1;
            v[bm_template_par(BT_CHANNEL, "Nx")] = 10; v[bm_template_par(BT_CHANNEL, "Ny")] = 10; v[bm_template_par(BT_CHANNEL, "Nz")] = 10;
            char e[200];
            if (bm_template(BT_CHANNEL, v, B.m, e, sizeof e)) { B.have = true; hand_edit(); }
        }
        return y + 48 - y0;
    }
    if (B.tmpl < 0) {                                       /* the preview here when no template form shows it */
        float vh = fminf(460, fmaxf(320, w * 0.42f));
        y += preview((Rectangle){ x, y, w, vh }) + 10;
        bool lb = B.labels;
        ui_checkbox(ui_id("bk-labels"), (Rectangle){ x, y, 200, 22 }, &lb, "Vertex numbers");
        if (lb != B.labels) { B.labels = lb; B.viewDirty = true; }
        char t[200]; pl_fmt_ui(t, sizeof t, "%d vertices, %d blocks, %d curved edges, %d patches: %.0f cells", B.m->nv, B.m->nb, B.m->ne, B.m->np, bm_cells(B.m));
        ui_text(F_REG, 13, t, x + 220, y + 3, TH.text2);
        y += 34;
    }
    /* save near the top: the tables can run long, and the wheel over the preview turns the view, not the page */
    float sw = ui_button_width(ICON_SAVE, "Save"), mw = ui_button_width(ICON_PLAY, "Save and mesh"), rw = ui_button_width(ICON_UNDO_2, "Revert");
    ui_disable(!B.dirty || A.cj->active);
    if (ui_button(ui_id("bk-savemesh"), (Rectangle){ x, y, mw, 38 }, ICON_PLAY, "Save and mesh", BTN_PRIMARY)) save(c, true);
    if (ui_button(ui_id("bk-save"), (Rectangle){ x + mw + 10, y, sw, 38 }, ICON_SAVE, "Save", BTN_SECONDARY)) save(c, false);
    if (ui_button(ui_id("bk-revert"), (Rectangle){ x + mw + sw + 20, y, rw, 38 }, ICON_UNDO_2, "Revert", BTN_GHOST)) load(c);
    ui_disable(false);
    if (B.dirty) ui_text(F_REG, 12.5f, "Not saved yet", x + mw + sw + rw + 34, y + 12, TH.warn);
    y += 52;
    y += issues(x, y, w) + 8;
    y += tables(x, y, w);
    return y - y0;
}
