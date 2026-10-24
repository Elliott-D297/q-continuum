/* page_meshview.c -- Mesh > View: the mesh of any case in 3-D. Its boundary surface (xsection.c ms_load), a colour
 * per patch with flat shading, the face outlines when there are few enough to see, a legend to hide patches (the
 * outer walls hide what is inside), and the counts. Drag to turn it, right-drag (or Shift+drag) to move it, the
 * wheel to zoom, double-click to fit. Drawn into a texture only when something changed, so it costs nothing idle. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "rlgl.h"
#include "xsection.h"

#define MAX_EDGES 600000                           /* face outlines drawn up to this many segments */

static struct {
    MeshSurf  M;
    char      path[PL_PATH];                       /* the polyMesh folder loaded */
    time_t    mt;
    Model     model[MS_MAX_PATCHES]; bool hasModel[MS_MAX_PATCHES];
    bool      hidden[MS_MAX_PATCHES];
    RenderTexture2D rt; int rtW, rtH; bool rtOk;
    bool      dirty, edges, dragging, panning;
    float     yaw, pitch, dist;                    /* the camera around the target */
    Vector3   target;
    Vector2   last;
    double    lastClick;
} V = { .edges = true, .yaw = 0.7f, .pitch = 0.45f };

static Color patch_color(int i, const char *type)
{
    if (!strcmp(type, "empty")) return ui_mix(TH.surface2, TH.text3, 0.35f);
    if (!strcmp(type, "symmetry") || !strcmp(type, "symmetryPlane") || !strcmp(type, "wedge")) return ui_mix(TH.text3, TH.info, 0.4f);
    return TH.series[i % 10];
}

static void unload_models(void)
{
    for (int i = 0; i < MS_MAX_PATCHES; i++) if (V.hasModel[i]) { UnloadModel(V.model[i]); V.hasModel[i] = false; }
}

/* one model per patch: three vertices per triangle, the colour shaded by a fixed light from above and the side */
static void build_models(void)
{
    unload_models();
    const MeshSurf *m = &V.M;
    float L[3] = { 0.45f, 0.75f, 0.5f };
    float ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    for (int k = 0; k < 3; k++) L[k] /= ll;
    for (int p = 0; p < m->nPatch; p++) {
        long n = 0;
        for (long t = 0; t < m->nTri; t++) if (m->triPatch[t] == p) n++;
        if (!n || n * 3 > 0x7fffffff / 4) continue;
        Mesh mesh = { 0 };
        mesh.vertexCount = (int)(n * 3);
        mesh.triangleCount = (int)n;
        mesh.vertices = MemAlloc((unsigned)(sizeof(float) * 3 * (size_t)mesh.vertexCount));
        mesh.colors = MemAlloc((unsigned)(4 * (size_t)mesh.vertexCount));
        if (!mesh.vertices || !mesh.colors) { MemFree(mesh.vertices); MemFree(mesh.colors); continue; }
        Color base = patch_color(p, m->ptype[p]);
        long v = 0;
        for (long t = 0; t < m->nTri; t++) {
            if (m->triPatch[t] != p) continue;
            const float *nr = &m->nrm[t * 3];
            float d = fabsf(nr[0] * L[0] + nr[1] * L[1] + nr[2] * L[2]);       /* two-sided: faces point either way */
            float k = 0.42f + 0.58f * d;
            Color c = { (unsigned char)(base.r * k), (unsigned char)(base.g * k), (unsigned char)(base.b * k), 255 };
            memcpy(&mesh.vertices[v * 3], &m->tri[t * 9], sizeof(float) * 9);
            for (int q = 0; q < 3; q++) memcpy(&mesh.colors[(v + q) * 4], &c, 4);
            v += 3;
        }
        UploadMesh(&mesh, false);
        V.model[p] = LoadModelFromMesh(mesh);
        V.hasModel[p] = true;
    }
}

static void fit(void)
{
    const MeshSurf *m = &V.M;
    V.target = (Vector3){ (m->lo[0] + m->hi[0]) / 2, (m->lo[1] + m->hi[1]) / 2, (m->lo[2] + m->hi[2]) / 2 };
    float dx = m->hi[0] - m->lo[0], dy = m->hi[1] - m->lo[1], dz = m->hi[2] - m->lo[2];
    float diag = sqrtf(dx * dx + dy * dy + dz * dz);
    V.dist = fmaxf(diag, 1e-9f) * 1.9f;
    /* a 2-D case (one thin direction): look straight at its plane */
    float mn = fminf(dx, fminf(dy, dz)), mx = fmaxf(dx, fmaxf(dy, dz));
    if (mx > 0 && mn < mx * 0.02f) {
        if (dz == mn) { V.yaw = 0; V.pitch = 0; }
        else if (dy == mn) { V.yaw = 0; V.pitch = 1.5607f; }
        else { V.yaw = 1.5708f; V.pitch = 0; }
        V.dist = fmaxf(dx, fmaxf(dy, dz)) * 1.8f;
    }
    V.dirty = true;
}

static Camera3D camera(void)
{
    Vector3 off = { V.dist * cosf(V.pitch) * sinf(V.yaw), V.dist * sinf(V.pitch), V.dist * cosf(V.pitch) * cosf(V.yaw) };
    Camera3D c = { 0 };
    c.position = (Vector3){ V.target.x + off.x, V.target.y + off.y, V.target.z + off.z };
    c.target = V.target;
    c.up = (Vector3){ 0, 1, 0 };
    c.fovy = 35;
    c.projection = CAMERA_PERSPECTIVE;
    return c;
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
    EndScissorMode();
    BeginTextureMode(V.rt);
    ClearBackground(TH.dark ? (Color){ 10, 14, 22, 255 } : (Color){ 236, 240, 247, 255 });
    rlDrawRenderBatchActive();
    Camera3D cam = camera();
    BeginMode3D(cam);
    rlSetClipPlanes(V.dist * 0.01, V.dist * 20.0);
    rlDisableBackfaceCulling();
    for (int p = 0; p < V.M.nPatch; p++) if (V.hasModel[p] && !V.hidden[p]) DrawModel(V.model[p], (Vector3){ 0, 0, 0 }, 1, WHITE);
    if (V.edges && V.M.edge) {
        /* outlines a hair in front of the faces: drawn after them with a little depth offset toward the camera */
        Color ec = TH.dark ? (Color){ 0, 0, 0, 120 } : (Color){ 20, 30, 50, 90 };
        Vector3 to = { cam.position.x - cam.target.x, cam.position.y - cam.target.y, cam.position.z - cam.target.z };
        float tl = sqrtf(to.x * to.x + to.y * to.y + to.z * to.z); float k = V.dist * 0.0015f / fmaxf(tl, 1e-12f);
        to.x *= k; to.y *= k; to.z *= k;
        rlBegin(RL_LINES);
        rlColor4ub(ec.r, ec.g, ec.b, ec.a);
        for (long e = 0; e < V.M.nEdge; e++) {
            if (V.hidden[V.M.edgePatch[e]]) continue;
            const float *g = &V.M.edge[e * 6];
            rlVertex3f(g[0] + to.x, g[1] + to.y, g[2] + to.z);
            rlVertex3f(g[3] + to.x, g[4] + to.y, g[5] + to.z);
        }
        rlEnd();
    }
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    EndMode3D();
    rlSetClipPlanes(RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR);
    /* the axes, bottom left */
    float ax = 46 * s, ay = h - 46 * s, al = 28 * s;
    float cy = cosf(V.yaw), sy = sinf(V.yaw), cp = cosf(V.pitch), sp = sinf(V.pitch);
    /* screen projection of the world axes for this orbit: right = (cos yaw, 0, -sin yaw), up = (-sin p sin y, cos p, -sin p cos y) */
    struct { float x, y, z; Color c; const char *n; } AX[3] = { { 1, 0, 0, { 230, 80, 70, 255 }, "x" }, { 0, 1, 0, { 90, 190, 90, 255 }, "y" },
                                                                 { 0, 0, 1, { 80, 140, 240, 255 }, "z" } };
    for (int i = 0; i < 3; i++) {
        float sxv = AX[i].x * cy - AX[i].z * sy;
        float syv = -AX[i].x * sp * sy + AX[i].y * cp - AX[i].z * sp * cy;
        DrawLineEx((Vector2){ ax, ay }, (Vector2){ ax + sxv * al, ay - syv * al }, 2.5f * s, AX[i].c);
        DrawTextEx(GetFontDefault(), AX[i].n, (Vector2){ ax + sxv * (al + 8 * s) - 3 * s, ay - syv * (al + 8 * s) - 6 * s }, 12 * s, 1, AX[i].c);
    }
    EndTextureMode();
    ui_restore_2d();
    V.dirty = false;
}

void meshview_release(void)
{
    unload_models();
    if (V.rtOk) { UnloadRenderTexture(V.rt); V.rtOk = false; }
    V.path[0] = 0;                                         /* switched on again: read the mesh afresh */
}

float meshview_panel(Rectangle r, Case *c)
{
    char dir[PL_PATH]; pl_join(dir, sizeof dir, c->s.path, case_rpath(c, "constant/polyMesh"));
    char own[PL_PATH]; pl_join(own, sizeof own, dir, "owner");
    time_t mt = pl_mtime(own);
    if (strcmp(V.path, dir) || mt != V.mt) {               /* another case or region, or a new mesh */
        pl_strlcpy(V.path, dir, sizeof V.path); V.mt = mt;
        ms_load(&V.M, dir, MAX_EDGES);
        memset(V.hidden, 0, sizeof V.hidden);
        if (V.M.ok) { build_models(); fit(); }
        else unload_models();
        V.dirty = true;
    }
    float y = r.y;
    if (!V.M.ok) {
        const char *t = mt ? "The mesh could not be read" : "No mesh to show yet";
        const char *d = V.M.err[0] ? V.M.err : "Generate the mesh first.";
        float ch = callout_height(r.width, t, d);
        callout((Rectangle){ r.x, y, r.width, ch }, mt ? TOAST_WARN : TOAST_INFO, ICON_GRID_3X3, t, d);
        y += ch + 12;
        if (!mt) {
            ui_disable(A.cj->active);
            if (ui_button(ui_id("mv-gen"), (Rectangle){ r.x, y, 160, 36 }, ICON_PLAY, "Generate mesh", BTN_PRIMARY)) app_generate_mesh();
            ui_disable(false);
            y += 48;
        }
        return y - r.y;
    }
    const MeshSurf *m = &V.M;
    /* counts */
    char t[300], cells[32], faces[32], pts[32];
    snprintf(cells, sizeof cells, "%ld", m->nCells); snprintf(faces, sizeof faces, "%ld", m->nFaces); snprintf(pts, sizeof pts, "%ld", m->nPoints);
    pl_fmt_ui(t, sizeof t, "%s cells  ·  %s faces  ·  %s points  ·  %d patches  ·  %.4g x %.4g x %.4g m", cells, faces, pts, m->nPatch,
              m->hi[0] - m->lo[0], m->hi[1] - m->lo[1], m->hi[2] - m->lo[2]);
    ui_text_clip(F_MED, 13, t, r.x, y + 9, r.width - 300, TH.text2);
    /* tools */
    float bx = r.x + r.width;
    float fw = ui_button_width(ICON_REFRESH_CW, "Fit");
    bx -= fw;
    if (ui_button(ui_id("mv-fit"), (Rectangle){ bx, y, fw, 32 }, ICON_REFRESH_CW, "Fit", BTN_GHOST)) fit();
    static const char *views[] = { "x", "y", "z" };
    for (int k = 2; k >= 0; k--) {
        bx -= 38;
        char lab[8]; snprintf(lab, sizeof lab, "%s", views[k]);
        if (ui_button(ui_idn(ui_id("mv-axis"), k), (Rectangle){ bx, y, 34, 32 }, NULL, lab, BTN_SECONDARY)) {
            V.yaw = k == 0 ? 1.5708f : 0; V.pitch = k == 1 ? 1.5607f : 0; V.dirty = true;
        }
        ui_tooltip((Rectangle){ bx, y, 34, 32 }, k == 0 ? "Look along -x" : k == 1 ? "Look down -y" : "Look along -z");
    }
    bx -= 120;
    bool ed = V.edges;
    ui_disable(!m->edge);
    ui_checkbox(ui_id("mv-edges"), (Rectangle){ bx, y + 6, 110, 20 }, &ed, "Edges");
    ui_disable(false);
    if (!m->edge) ui_tooltip((Rectangle){ bx, y, 110, 32 }, "Too many faces to outline");
    if (ed != V.edges) { V.edges = ed; V.dirty = true; }
    y += 44;
    /* the view and the legend beside it */
    float lw = fminf(260, r.width * 0.28f);
    Rectangle view = { r.x, y, r.width - lw - 14, fmaxf(360, r.height - (y - r.y)) };
    ui_rect(view, 12, TH.dark ? (Color){ 10, 14, 22, 255 } : (Color){ 236, 240, 247, 255 });
    bool hov = ui_hovered(view) && A.modal == MD_NONE;
    Vector2 mp = ui_mouse();
    if (hov && ui_pressed(view)) {
        if (ui_time() - V.lastClick < 0.35) { fit(); V.lastClick = 0; }
        else V.lastClick = ui_time();
        bool shift = inp_key_down(KEY_LEFT_SHIFT) || inp_key_down(KEY_RIGHT_SHIFT);
        V.dragging = !shift; V.panning = shift; V.last = mp;
    }
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) { V.panning = true; V.dragging = false; V.last = mp; }
    if (V.dragging || V.panning) {
        bool down = V.dragging ? ui_mouse_down() : (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || ui_mouse_down());
        if (!down) V.dragging = V.panning = false;
        else {
            float dx = mp.x - V.last.x, dy = mp.y - V.last.y;
            if (dx != 0 || dy != 0) {
                if (V.dragging) {
                    V.yaw -= dx * 0.008f;
                    V.pitch += dy * 0.008f;
                    if (V.pitch > 1.5607f) V.pitch = 1.5607f;
                    if (V.pitch < -1.5607f) V.pitch = -1.5607f;
                } else {
                    float k = V.dist * 0.0016f;
                    float cy = cosf(V.yaw), sy = sinf(V.yaw), cp = cosf(V.pitch), sp = sinf(V.pitch);
                    Vector3 right = { cy, 0, -sy }, up = { -sp * sy, cp, -sp * cy };
                    V.target.x += (-dx * right.x + dy * up.x) * k;
                    V.target.y += (-dx * right.y + dy * up.y) * k;
                    V.target.z += (-dx * right.z + dy * up.z) * k;
                }
                V.dirty = true;
            }
            V.last = mp;
            ui_request_frames(2);
        }
    }
    if (hov) {
        float wh = ui_take_wheel();
        if (wh != 0) { V.dist *= powf(0.88f, wh); V.dirty = true; }
        ui_cursor(V.panning ? MOUSE_CURSOR_RESIZE_ALL : MOUSE_CURSOR_POINTING_HAND);
    }
    if (V.dirty) redraw(view);
    if (V.rtOk) {
        DrawTexturePro(V.rt.texture, (Rectangle){ 0, 0, (float)V.rtW, -(float)V.rtH }, view, (Vector2){ 0, 0 }, 0, WHITE);
    }
    ui_rect_lines(view, 12, 1, TH.lineSoft);
    ui_text(F_REG, 12, "Drag to turn  ·  right-drag or Shift+drag to move  ·  wheel to zoom  ·  double-click to fit", view.x + 14, view.y + view.height - 22,
            TH.dark ? (Color){ 200, 210, 230, 160 } : (Color){ 60, 70, 90, 170 });
    /* legend: click a patch to hide or show it */
    float lx = view.x + view.width + 14, ly = y;
    ui_text(F_SEMI, 13, ui_cap("Patches"), lx, ly, head_color());
    ly += 24;
    Rectangle lr = { lx, ly, lw, view.height - 24 };
    static UiScroll ls;
    float top = ui_scroll_begin(&ls, lr), yy = top;
    for (int p = 0; p < m->nPatch; p++) {
        Rectangle row = { lx, yy, lw - 10, 40 };
        if (ui_row(ui_idn(ui_id("mv-patch"), p), row, false)) { V.hidden[p] = !V.hidden[p]; V.dirty = true; }
        ui_register(ui_idn(ui_id("mv-patch"), p), row, m->patch[p]);
        Color pc = patch_color(p, m->ptype[p]);
        ui_rect((Rectangle){ row.x + 8, row.y + 12, 14, 14 }, 4, V.hidden[p] ? ui_alpha(pc, 0.25f) : pc);
        ui_text_clip(F_MONO_MED, 12.5f, m->patch[p], row.x + 30, row.y + 4, row.width - 36, V.hidden[p] ? TH.text3 : TH.text);
        char sub[96]; pl_fmt_ui(sub, sizeof sub, "%s  ·  %ld faces%s", m->ptype[p], m->patchFaces[p], V.hidden[p] ? "  ·  hidden" : "");
        ui_text_clip(F_REG, 11.5f, sub, row.x + 30, row.y + 22, row.width - 36, TH.text3);
        yy += 42;
    }
    ui_scroll_end(&ls, yy - top);
    return view.y + view.height - r.y;
}
