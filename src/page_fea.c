/* page_fea.c -- a part study in CalculiX, set up and seen like SolidWorks Simulation (docs/FEA_PLAN.md F2/F3, 2026-11-15):
 * a CalculiX project with a study.ini. Setup shows the part in 3-D: faces are clicked onto fixtures and loads, the material
 * picked, the part meshed (SALOME, in the background: assets/salome/fea_mesh.py); the run writes study.inp from it.
 * Results show the deformed part coloured by stress or displacement. */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "raylib.h"
#include "rlgl.h"
#include "app.h"
#include "ui.h"
#include "orbit.h"
#include "femesh.h"
#include "festudy.h"
#include "ccxmodel.h"
#include "program.h"
#include "pl_util.h"
#include "input.h"

extern const unsigned char asset_fea_mesh_py[];
extern const unsigned int asset_fea_mesh_py_len;

static struct {
    char    proj[PL_PATH];
    time_t  meshMt;
    FeMesh  m; bool ok;
    FeStudy s;
    int    *fedge; int nFedge;                         /* the edges between faces (CAD face outlines), pairs of nodes */
    /* the view */
    Orbit   O; RenderTexture2D rt; int rtW, rtH; bool rtOk, dirty, touched, dragging, panning, moved; Vector2 last, downAt;
    float   aspect, fitAspect;
    int     hoverG, active;                            /* active: fixture i, 100 + load i, 200 + thermal i, -1 none */
    int     resField;                                  /* results: 0 von Mises, 1 displacement */
    bool    results, showUndeformed;
    /* the mesh job */
    pid_t   pid; char log[PL_PATH], jobProj[PL_PATH]; double t0;
    /* results */
    CcxFrd  r; bool rOk; time_t rMt; char rPath[PL_PATH]; int *rmap; int rmapN;
} F = { .hoverG = -1, .active = -1, .pid = -1 };

/* ============================================================================ the study on disk */
bool fea_is_study(const char *proj)
{
    char p[PL_PATH]; pl_join(p, sizeof p, proj, "study.ini");
    return pl_is_file(p);
}

static bool unitless(const char *file)
{
    const char *e = strrchr(file, '.');
    return e && (!strcasecmp(e, ".stl") || !strcasecmp(e, ".brep") || !strcasecmp(e, ".med") || !strcasecmp(e, ".unv") || !strcasecmp(e, ".hdf"));
}

static int cmp_edge3(const void *x, const void *y)
{
    const int *p = x, *q = y;
    return p[0] != q[0] ? (p[0] > q[0]) - (p[0] < q[0]) : (p[1] > q[1]) - (p[1] < q[1]);
}
static void feature_edges(void)
{
    free(F.fedge); F.fedge = NULL; F.nFedge = 0;
    const FeMesh *m = &F.m;
    int cap = m->nf * 4 + 4, n = 0;
    int (*e)[3] = malloc(sizeof *e * (size_t)cap);
    if (!e) return;                                        /* out of memory: no face edges drawn */
    for (int f = 0; f < m->nf; f++) {
        int c = m->fk[f] == 4 || m->fk[f] == 8 ? 4 : 3;
        for (int j = 0; j < c; j++) {
            int a = m->fn[8 * f + j], b = m->fn[8 * f + (j + 1) % c];
            e[n][0] = a < b ? a : b; e[n][1] = a < b ? b : a; e[n][2] = m->fg[f]; n++;
        }
    }
    qsort(e, (size_t)n, sizeof e[0], cmp_edge3);
    F.fedge = malloc(sizeof(int) * 2 * (size_t)(n + 1));
    if (!F.fedge) { free(e); return; }
    for (int i = 0; i + 1 < n; i++)
        if (e[i][0] == e[i + 1][0] && e[i][1] == e[i + 1][1]) {
            if (e[i][2] != e[i + 1][2]) { F.fedge[2 * F.nFedge] = e[i][0]; F.fedge[2 * F.nFedge + 1] = e[i][1]; F.nFedge++; }
            i++;
        }
    free(e);
}

static void fit(void);
/* the open project's study and mesh, read again when they change; the mesh job watched */
static void fea_sync(const char *proj)
{
    if (F.pid > 0) {
        int st;
        if (waitpid(F.pid, &st, WNOHANG) == F.pid) {
            F.pid = -1;
            char *t = pl_read_file(F.log, NULL), last[300] = "";
            for (char *l = t; l && *l; ) { char *e = strchr(l, '\n'); if (e) *e = 0; if (!strncmp(l, "Q-Continuum: ", 13)) pl_strlcpy(last, l + 13, sizeof last); l = e ? e + 1 : NULL; }
            free(t);
            char mp[PL_PATH]; pl_join(mp, sizeof mp, F.jobProj, "part.qcmesh");
            bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0 && pl_is_file(mp);
            ui_toast(ok ? TOAST_OK : TOAST_ERR, ok ? "Meshed: %s" : "Meshing failed: %s", last[0] ? last : "see log.mesh in the project");
            F.meshMt = 0;
        }
    }
    char mp[PL_PATH]; pl_join(mp, sizeof mp, proj, "part.qcmesh");
    time_t mt = pl_mtime(mp);
    if (strcmp(F.proj, proj)) {
        pl_strlcpy(F.proj, proj, sizeof F.proj);
        fe_study_load(proj, &F.s);
        F.meshMt = 0; F.active = -1; F.touched = false; F.hoverG = -1;
        if (F.rOk) { ccx_frd_free(&F.r); F.rOk = false; } F.rMt = 0;
    }
    if (mt != F.meshMt) {
        if (F.ok) femesh_free(&F.m);
        char err[300];
        F.ok = mt && femesh_load(mp, &F.m, err, sizeof err);
        if (mt && !F.ok) ui_toast(TOAST_ERR, "%s", err);
        F.meshMt = mt;
        if (F.ok) { feature_edges(); F.touched = false; F.fitAspect = 0; }
        F.dirty = true;
    }
}

static void save_study(void) { fe_study_save(F.proj, &F.s); F.dirty = true; }

/* ============================================================================ meshing, in the background */
static bool mesh_busy(void) { return F.pid > 0; }

void fea_mesh_start(const char *proj)
{
    if (mesh_busy()) { ui_toast(TOAST_INFO, "Already meshing %s", pl_basename(F.jobProj)); return; }
    if (!A.env.salome[0]) { ui_toast(TOAST_ERR, "Meshing a part needs SALOME (Settings > Programs installs it)"); return; }
    FeStudy s; fe_study_load(proj, &s);
    char part[PL_PATH], script[PL_PATH], out[PL_PATH], md[PL_PATH];
    pl_join(part, sizeof part, proj, s.part);
    if (!pl_is_file(part)) { ui_toast(TOAST_ERR, "The part %s is not in the project folder", s.part); return; }
    pl_join(md, sizeof md, proj, ".qc-mesh"); pl_mkdirs(md);
    pl_join(script, sizeof script, md, "fea_mesh.py");
    if (!pl_write_file(script, (const char *)asset_fea_mesh_py, asset_fea_mesh_py_len)) { ui_toast(TOAST_ERR, "Could not write %s", script); return; }
    pl_join(out, sizeof out, proj, "part.qcmesh");
    pl_join(F.log, sizeof F.log, proj, "log.mesh");
    pl_strlcpy(F.jobProj, proj, sizeof F.jobProj);
    char qi[PL_PATH + 8], qo[PL_PATH + 8], qs[PL_PATH + 8], qp[PL_PATH + 8], qd[PL_PATH + 8], cmd[PL_PATH * 6];
    pl_shell_quote(qi, sizeof qi, part); pl_shell_quote(qo, sizeof qo, out); pl_shell_quote(qs, sizeof qs, A.env.salome);
    pl_shell_quote(qp, sizeof qp, script); pl_shell_quote(qd, sizeof qd, md);
    if (!pl_fmt(cmd, sizeof cmd, "cd %s && QC_IN=%s QC_OUT=%s QC_SIZE=%g QC_SCALE=%g QC_ORDER=%d env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME %s -t "
                                "--shutdown-servers=1 %s; [ -s %s ]", qd, qi, qo, s.size, s.scale, s.order, qs, qp, qo)) { ui_toast(TOAST_ERR, "The path is too long"); return; }
    F.pid = pl_spawn_shell(cmd, F.log);
    F.t0 = GetTime();
    if (F.pid <= 0) ui_toast(TOAST_ERR, "Could not start SALOME");
    else ui_toast(TOAST_INFO, "Meshing %s in SALOME (in the background)", s.part);
}

/* a new project from a part's file: the file copied in, a study.ini, meshing started */
bool fea_new_project(const char *file, double scale, const char *dst, char *err, int cap)
{
    if (!pl_is_file(file)) { snprintf(err, (size_t)cap, "%s is not a file", file); return false; }
    if (!pl_mkdirs(dst)) { snprintf(err, (size_t)cap, "Could not make %s", dst); return false; }
    char to[PL_PATH]; pl_join(to, sizeof to, dst, pl_basename(file));
    size_t n = 0; char *b = pl_read_file(file, &n);
    if (!b || !pl_write_file(to, b, n)) { free(b); snprintf(err, (size_t)cap, "Could not copy %s", file); return false; }
    free(b);
    FeStudy s; fe_study_default(&s);
    pl_strlcpy(s.part, pl_basename(file), sizeof s.part);
    s.scale = scale > 0 ? scale : 1;
    if (!fe_study_save(dst, &s)) { snprintf(err, (size_t)cap, "Could not write study.ini"); return false; }
    char ip[PL_PATH]; pl_join(ip, sizeof ip, dst, "study.inp");
    const char *stub = "** Q-Continuum writes this file from study.ini (Setup) each time the study runs.\n*STEP\n*STATIC\n*END STEP\n";
    pl_write_file(ip, stub, strlen(stub));
    return true;
}

/* before a run: study.inp written from the study (false, with why, when it can't run) */
bool fea_prepare_run(const char *proj, char *err, int cap)
{
    err[0] = 0;
    FeStudy s; fe_study_load(proj, &s);
    char mp[PL_PATH]; pl_join(mp, sizeof mp, proj, "part.qcmesh");
    FeMesh m;
    if (!pl_is_file(mp)) { snprintf(err, (size_t)cap, mesh_busy() ? "Still meshing the part: run when it is done" : "The part isn't meshed yet: Setup > Mesh"); return false; }
    if (!femesh_load(mp, &m, err, cap)) return false;
    FeSummary sum; fe_study_summary(&s, &m, &sum);
    bool ok = !sum.problem[0];
    if (!ok) snprintf(err, (size_t)cap, "%s", sum.problem);
    else ok = fe_study_write(&s, &m, proj, err, cap);
    femesh_free(&m);
    return ok;
}

/* ============================================================================ the 3-D view */
static void fit_distance(void)
{
    const FeMesh *m = &F.m;
    float d[3] = { (float)(m->hi[0] - m->lo[0]), (float)(m->hi[1] - m->lo[1]), (float)(m->hi[2] - m->lo[2]) };
    float tv = tanf(35.f / 2 * DEG2RAD), th = tv * fmaxf(F.aspect > 0 ? F.aspect : 1.6f, 0.1f);
    Vector3 back = F.O.back, right = orbit_right(&F.O), up = F.O.up;
    float need = 1e-9f;
    for (int c = 0; c < 8; c++) {
        float p[3] = { (c & 1 ? d[0] : -d[0]) / 2, (c & 2 ? d[1] : -d[1]) / 2, (c & 4 ? d[2] : -d[2]) / 2 };
        float x = p[0] * right.x + p[1] * right.y + p[2] * right.z, y = p[0] * up.x + p[1] * up.y + p[2] * up.z;
        float z = p[0] * back.x + p[1] * back.y + p[2] * back.z;
        need = fmaxf(need, fmaxf(z + fabsf(x) / th, z + fabsf(y) / tv));
    }
    F.O.dist = need * 1.15f;
}
static void fit(void)
{
    const FeMesh *m = &F.m;
    if (!F.touched) { F.O.upAxis = 2; F.O.spin = -1; orbit_set(&F.O, 0.75f, 0.5f); }
    F.O.target = (Vector3){ (float)(m->lo[0] + m->hi[0]) / 2, (float)(m->lo[1] + m->hi[1]) / 2, (float)(m->lo[2] + m->hi[2]) / 2 };
    fit_distance();
    F.fitAspect = F.aspect;
    F.dirty = true;
}

/* the face list of the item being picked for (a support, a load, a thermal condition), NULL when none */
static char (*active_faces(int **nf))[64]
{
    if (F.active >= 200 && F.active - 200 < F.s.nTh) { *nf = &F.s.th[F.active - 200].nf; return F.s.th[F.active - 200].face; }
    if (F.active >= 100 && F.active < 200 && F.active - 100 < F.s.nLoad) { *nf = &F.s.load[F.active - 100].nf; return F.s.load[F.active - 100].face; }
    if (F.active >= 0 && F.active < 100 && F.active < F.s.nFix) { *nf = &F.s.fix[F.active].nf; return F.s.fix[F.active].face; }
    return NULL;
}

static Color group_color(int g, bool *roleOut)
{
    Color base = TH.dark ? (Color){ 150, 160, 178, 255 } : (Color){ 176, 184, 198, 255 };
    Color fixC = { 45, 190, 160, 255 }, loadC = { 245, 150, 50, 255 }, thC = { 230, 80, 90, 255 };
    bool role = false, mech = fe_an_mechanical(F.s.analysis), therm = fe_an_thermal(F.s.analysis);
    for (int i = 0; mech && i < F.s.nFix; i++) for (int j = 0; j < F.s.fix[i].nf; j++) if (!strcasecmp(F.s.fix[i].face[j], F.m.gname[g])) { base = fixC; role = true; }
    for (int i = 0; mech && i < F.s.nLoad; i++) for (int j = 0; j < F.s.load[i].nf; j++) if (!strcasecmp(F.s.load[i].face[j], F.m.gname[g])) { base = loadC; role = true; }
    for (int i = 0; therm && i < F.s.nTh; i++) for (int j = 0; j < F.s.th[i].nf; j++) if (!strcasecmp(F.s.th[i].face[j], F.m.gname[g])) { base = thC; role = true; }
    if (roleOut) *roleOut = role;
    return base;
}
static bool in_active(int g)
{
    int *nf; char (*fc)[64] = active_faces(&nf);
    if (!fc || g < 0) return false;
    for (int j = 0; j < *nf; j++) if (!strcasecmp(fc[j], F.m.gname[g])) return true;
    return false;
}

static Color stress_color(float t)                       /* blue, cyan, green, yellow, red */
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    static const Color C[5] = { { 40, 70, 220, 255 }, { 30, 190, 230, 255 }, { 40, 200, 90, 255 }, { 240, 220, 40, 255 }, { 230, 50, 40, 255 } };
    float s = t * 4; int i = (int)s; if (i > 3) i = 3;
    return ui_mix(C[i], C[i + 1], s - i);
}

/* results: the value at node i of the mesh (via the .frd's node index), its displacement scaled */
static float res_value(int i, float *u3, float scale)
{
    u3[0] = u3[1] = u3[2] = 0;
    if (!F.rOk || !F.rmap || F.m.id[i] < 0 || F.m.id[i] >= F.rmapN) return 0;
    int k = F.rmap[F.m.id[i]];
    if (k < 0) return 0;
    if (F.r.u) for (int q = 0; q < 3; q++) u3[q] = scale * F.r.u[3 * k + q];
    if (F.resField == 1 && F.r.u) return sqrtf(F.r.u[3 * k] * F.r.u[3 * k] + F.r.u[3 * k + 1] * F.r.u[3 * k + 1] + F.r.u[3 * k + 2] * F.r.u[3 * k + 2]);
    return F.r.vm ? F.r.vm[k] : 0;
}
static float res_max(void) { return F.resField == 1 ? (float)F.r.uMax : (float)F.r.vmMax; }
static float res_scale(void)
{
    double L = 0; for (int k = 0; k < 3; k++) L = fmax(L, F.m.hi[k] - F.m.lo[k]);
    return F.showUndeformed || !F.r.u || F.r.uMax <= 0 ? 0.f : (float)(0.08 * L / F.r.uMax);   /* the largest movement drawn at 8 % of the part */
}

static void redraw(Rectangle r)
{
    float s = ui_scale();
    int w = (int)(r.width * s), h = (int)(r.height * s);
    if (w < 8 || h < 8) return;
    if (!F.rtOk || w != F.rtW || h != F.rtH) {
        if (F.rtOk) UnloadRenderTexture(F.rt);
        F.rt = LoadRenderTexture(w, h);
        F.rtOk = F.rt.id != 0; F.rtW = w; F.rtH = h;
    }
    if (!F.rtOk) return;
    EndScissorMode();
    BeginTextureMode(F.rt);
    ClearBackground(TH.dark ? (Color){ 10, 14, 22, 255 } : (Color){ 236, 240, 247, 255 });
    rlDrawRenderBatchActive();
    Camera3D cam = orbit_camera(&F.O);
    BeginMode3D(cam);
    rlSetClipPlanes(F.O.dist * 0.01, F.O.dist * 20.0);
    rlDisableBackfaceCulling();
    const FeMesh *m = &F.m;
    Vector3 eye = { F.O.back.x, F.O.back.y, F.O.back.z };
    bool res = F.results && F.rOk;
    float sc = res ? res_scale() : 0, vmax = res ? res_max() : 1;
    if (vmax <= 0) vmax = 1;
    Color gc[256]; int ngc = m->ng < 256 ? m->ng : 256;
    for (int g = 0; g < ngc; g++) {
        gc[g] = group_color(g, NULL);
        if (in_active(g)) gc[g] = ui_mix(gc[g], TH.accent, 0.55f);
        if (g == F.hoverG && !res) gc[g] = ui_mix(gc[g], WHITE, 0.35f);
    }
    rlBegin(RL_TRIANGLES);
    for (int f = 0; f < m->nf; f++) {
        int c = m->fk[f] == 4 || m->fk[f] == 8 ? 4 : 3, g = m->fg[f];
        const int *v = &m->fn[8 * f];
        double nr[3]; femesh_face_area(m, f, nr);
        float lit = 0.45f + 0.55f * fabsf((float)(nr[0] * eye.x + nr[1] * eye.y + nr[2] * eye.z));
        for (int t = 0; t < c - 2; t++) {
            int tri[3] = { v[0], v[t + 1], v[t + 2] };
            for (int q = 0; q < 3; q++) {
                int i = tri[q]; float u[3] = { 0, 0, 0 }; Color col;
                if (res) { float val = res_value(i, u, sc); col = stress_color(val / vmax); }
                else col = g < 256 ? gc[g] : gc[0];
                rlColor4ub((unsigned char)(col.r * lit), (unsigned char)(col.g * lit), (unsigned char)(col.b * lit), 255);
                rlVertex3f((float)m->x[3 * i] + u[0], (float)m->x[3 * i + 1] + u[1], (float)m->x[3 * i + 2] + u[2]);
            }
        }
    }
    rlEnd();
    /* the faces' outlines, a hair toward the camera */
    Vector3 to = { cam.position.x - cam.target.x, cam.position.y - cam.target.y, cam.position.z - cam.target.z };
    float tl = sqrtf(to.x * to.x + to.y * to.y + to.z * to.z), k = F.O.dist * 0.002f / fmaxf(tl, 1e-12f);
    to.x *= k; to.y *= k; to.z *= k;
    rlBegin(RL_LINES);
    Color ec = TH.dark ? (Color){ 10, 12, 20, 200 } : (Color){ 30, 40, 60, 160 };
    rlColor4ub(ec.r, ec.g, ec.b, ec.a);
    for (int e = 0; e < F.nFedge; e++) {
        for (int q = 0; q < 2; q++) {
            int i = F.fedge[2 * e + q]; float u[3] = { 0, 0, 0 };
            if (res) res_value(i, u, sc);
            rlVertex3f((float)m->x[3 * i] + u[0] + to.x, (float)m->x[3 * i + 1] + u[1] + to.y, (float)m->x[3 * i + 2] + u[2] + to.z);
        }
    }
    rlEnd();
    /* force arrows at the loaded faces' centres */
    if (!res) {
        double L = 0; for (int q = 0; q < 3; q++) L = fmax(L, m->hi[q] - m->lo[q]);
        for (int i = 0; i < F.s.nLoad; i++) {
            const FeLoad *d = &F.s.load[i];
            for (int j = 0; j < d->nf; j++) {
                int g = fe_group(m, d->face[j]); if (g < 0) continue;
                double dir[3] = { -m->gnorm[3 * g], -m->gnorm[3 * g + 1], -m->gnorm[3 * g + 2] };
                if (d->kind == LD_FORCE && d->dir != DIR_NORMAL) { int ax = (d->dir - 1) % 3; dir[0] = dir[1] = dir[2] = 0; dir[ax] = d->dir <= DIR_PZ ? 1 : -1; }
                if (d->value < 0) for (int q = 0; q < 3; q++) dir[q] = -dir[q];
                Vector3 tip = { (float)m->gcen[3 * g], (float)m->gcen[3 * g + 1], (float)m->gcen[3 * g + 2] };
                Vector3 tail = { tip.x - (float)(dir[0] * 0.18 * L), tip.y - (float)(dir[1] * 0.18 * L), tip.z - (float)(dir[2] * 0.18 * L) };
                Color ac = { 255, 120, 20, 255 };
                DrawCylinderEx(tail, tip, (float)(0.006 * L), (float)(0.006 * L), 8, ac);
                Vector3 hb = { tip.x - (float)(dir[0] * 0.05 * L), tip.y - (float)(dir[1] * 0.05 * L), tip.z - (float)(dir[2] * 0.05 * L) };
                DrawCylinderEx(hb, tip, (float)(0.02 * L), 0, 12, ac);
            }
        }
    }
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    EndMode3D();
    rlSetClipPlanes(RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR);
    orbit_draw_axes(&F.O, 46 * s, h - 46 * s, 28 * s, s);
    EndTextureMode();
    ui_restore_2d();
    F.dirty = false;
}

/* the boundary face under a point of the view (-1: none), as drawn (deformed on Results), and where the ray hits it */
static int pick_face(Rectangle view, Vector2 mp, Vector3 *at)
{
    float s = ui_scale();
    Ray ray = GetScreenToWorldRayEx((Vector2){ (mp.x - view.x) * s, (mp.y - view.y) * s }, orbit_camera(&F.O), F.rtW, F.rtH);
    float best = 1e30f; int hit = -1;
    const FeMesh *m = &F.m;
    bool res = F.results && F.rOk; float sc = res ? res_scale() : 0;
    for (int f = 0; f < m->nf; f++) {
        const int *v = &m->fn[8 * f]; int c = m->fk[f] == 4 || m->fk[f] == 8 ? 4 : 3;
        Vector3 P[4];
        for (int j = 0; j < c; j++) {
            float u[3] = { 0, 0, 0 }; if (res) res_value(v[j], u, sc);
            P[j] = (Vector3){ (float)m->x[3 * v[j]] + u[0], (float)m->x[3 * v[j] + 1] + u[1], (float)m->x[3 * v[j] + 2] + u[2] };
        }
        for (int t = 0; t < c - 2; t++) {
            RayCollision h = GetRayCollisionTriangle(ray, P[0], P[t + 1], P[t + 2]);
            if (h.hit && h.distance < best) { best = h.distance; hit = f; if (at) *at = h.point; }
        }
    }
    return hit;
}
static int pick(Rectangle view, Vector2 mp) { int f = pick_face(view, mp, NULL); return f >= 0 ? F.m.fg[f] : -1; }

static void toggle_face(int g)
{
    int *nf; char (*fc)[64] = active_faces(&nf);
    if (!fc || g < 0) return;
    for (int j = 0; j < *nf; j++)
        if (!strcasecmp(fc[j], F.m.gname[g])) { memmove(fc[j], fc[j + 1], 64 * (size_t)(*nf - j - 1)); (*nf)--; save_study(); return; }
    if (*nf < FE_MAXFACES) { pl_strlcpy(fc[(*nf)++], F.m.gname[g], 64); save_study(); }
}

/* the view: turned by dragging, zoomed by the wheel; a click (not a drag) picks a face for the active item */
static void view(Rectangle v, bool picking)
{
    ui_rect(v, 12, TH.dark ? (Color){ 10, 14, 22, 255 } : (Color){ 236, 240, 247, 255 });
    F.aspect = v.height > 0 ? v.width / v.height : 1.6f;
    if (!F.touched && fabsf(F.aspect - F.fitAspect) > 0.01f) fit();
    Rectangle turnR = { v.x + 12, v.y + 10, orbit_tools_w(true) + 16, 40 };
    bool hov = ui_hovered(v) && !ui_hovered(turnR) && A.modal == MD_NONE;
    Vector2 mp = ui_mouse();
    if (hov && ui_pressed(v)) {
        bool shift = inp_key_down(KEY_LEFT_SHIFT) || inp_key_down(KEY_RIGHT_SHIFT);
        F.dragging = !shift; F.panning = shift; F.last = mp; F.downAt = mp; F.moved = false;
    }
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) { F.panning = true; F.dragging = false; F.last = mp; F.moved = true; }
    if (F.dragging || F.panning) {
        bool down = F.dragging ? ui_mouse_down() : (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || ui_mouse_down());
        if (!down) {
            if (F.dragging && !F.moved && picking && hov) toggle_face(pick(v, mp));   /* a click: the face onto the active item */
            F.dragging = F.panning = false;
        } else {
            float dx = mp.x - F.last.x, dy = mp.y - F.last.y;
            if (fabsf(mp.x - F.downAt.x) + fabsf(mp.y - F.downAt.y) > 4) F.moved = true;
            if (F.moved && (dx != 0 || dy != 0)) {
                if (F.dragging) orbit_drag(&F.O, F.last, mp, (Vector2){ v.x + v.width / 2, v.y + v.height / 2 });
                else orbit_pan(&F.O, dx, dy);
                F.dirty = true; F.touched = true;
            }
            F.last = mp;
            ui_request_frames(2);
        }
    }
    if (hov) {
        if (orbit_keys(&F.O)) F.dirty = true;
        float wh = ui_take_wheel();
        if (wh != 0) { F.O.dist *= powf(0.88f, wh); F.dirty = true; F.touched = true; }
        if (!F.dragging && !F.panning && !F.results) {
            int g = pick(v, mp);
            if (g != F.hoverG) { F.hoverG = g; F.dirty = true; }
        }
        ui_cursor(F.panning ? MOUSE_CURSOR_RESIZE_ALL : MOUSE_CURSOR_POINTING_HAND);
    } else if (F.hoverG >= 0 && !F.results) { F.hoverG = -1; F.dirty = true; }
    if (F.dirty) redraw(v);
    if (F.rtOk) DrawTexturePro(F.rt.texture, (Rectangle){ 0, 0, (float)F.rtW, -(float)F.rtH }, v, (Vector2){ 0, 0 }, 0, WHITE);
    ui_rect_lines(v, 12, 1, TH.lineSoft);
    ui_register(ui_id("fea-view"), v, "Part view");
    ui_rect(turnR, 9, ui_alpha(TH.surface, 0.82f));
    int ot = orbit_tools(&F.O, "fea", turnR.x + 8, turnR.y + 5, true);
    if (ot == 1) { F.dirty = true; F.touched = true; } else if (ot == 2) { F.touched = false; fit(); }
    /* results: the largest value marked; the value under the mouse */
    if (F.results && F.rOk) {
        float s = ui_scale(), sc = res_scale();
        int at = F.resField == 1 ? F.r.uMaxAt : F.r.vmMaxAt;
        if (at >= 0 && at < F.r.n) {
            Vector3 P = { F.r.x[3 * at], F.r.x[3 * at + 1], F.r.x[3 * at + 2] };
            if (F.r.u) { P.x += sc * F.r.u[3 * at]; P.y += sc * F.r.u[3 * at + 1]; P.z += sc * F.r.u[3 * at + 2]; }
            Vector2 q = GetWorldToScreenEx(P, orbit_camera(&F.O), F.rtW, F.rtH);
            Vector2 sp = { v.x + q.x / s, v.y + q.y / s };
            if (CheckCollisionPointRec(sp, v)) {
                DrawCircleV(sp, 5, (Color){ 230, 50, 40, 255 }); DrawCircleLinesV(sp, 7, WHITE);
                char t[80]; pl_fmt_ui(t, sizeof t, F.resField == 1 ? "Max %.4g mm" : "Max %.4g MPa", F.resField == 1 ? F.r.uMax : F.r.vmMax);
                float tw = ui_text_w(F_MED, 12, t) + 14;
                Rectangle tr = { fminf(sp.x + 10, v.x + v.width - tw - 4), sp.y - 30, tw, 22 };
                ui_rect(tr, 6, ui_alpha(TH.surface, 0.92f)); ui_text(F_MED, 12, t, tr.x + 7, tr.y + 4, TH.text);
            }
        }
        if (hov && !F.dragging && !F.panning) {
            Vector3 hp; int f = pick_face(v, mp, &hp);
            if (f >= 0) {
                const int *vv = &F.m.fn[8 * f]; int best = vv[0]; float bd = 1e30f;
                for (int j = 0; j < F.m.fk[f]; j++) {
                    float u[3]; res_value(vv[j], u, sc);
                    float dx = (float)F.m.x[3 * vv[j]] + u[0] - hp.x, dy = (float)F.m.x[3 * vv[j] + 1] + u[1] - hp.y, dz = (float)F.m.x[3 * vv[j] + 2] + u[2] - hp.z;
                    float d = dx * dx + dy * dy + dz * dz; if (d < bd) { bd = d; best = vv[j]; }
                }
                int keep = F.resField; float u[3];
                F.resField = 0; float vm = res_value(best, u, 0);
                F.resField = 1; float um = res_value(best, u, 0);
                F.resField = keep;
                char t[160]; pl_fmt_ui(t, sizeof t, "von Mises %.4g MPa · moved %.4g mm · node %d", vm, um, F.m.id[best]);
                float tw = ui_text_w(F_MED, 12.5f, t) + 20;
                Rectangle tr = { fminf(mp.x + 14, v.x + v.width - tw - 6), mp.y + 16, tw, 26 };
                ui_rect(tr, 7, ui_alpha(TH.surface, 0.94f)); ui_rect_lines(tr, 7, 1, TH.lineSoft);
                ui_text(F_MED, 12.5f, t, tr.x + 10, tr.y + 5, TH.text);
                ui_request_frames(1);
            }
        }
    }
    /* the face under the mouse */
    if (hov && F.hoverG >= 0 && !F.results) {
        char t[160]; pl_fmt_ui(t, sizeof t, "%s · %.4g mm²%s", F.m.gname[F.hoverG], F.m.garea[F.hoverG], F.m.gplanar[F.hoverG] ? " · flat" : "");
        float tw = ui_text_w(F_MED, 12.5f, t) + 20;
        Rectangle tr = { fminf(mp.x + 14, v.x + v.width - tw - 6), mp.y + 16, tw, 26 };
        ui_rect(tr, 7, ui_alpha(TH.surface, 0.94f)); ui_rect_lines(tr, 7, 1, TH.lineSoft);
        ui_text(F_MED, 12.5f, t, tr.x + 10, tr.y + 5, TH.text);
        ui_request_frames(1);
    }
    char how[200]; snprintf(how, sizeof how, "%s%s  ·  right-drag to move  ·  wheel to zoom", picking ? "Click faces to add or remove them  ·  " : "", orbit_hint(&F.O));
    ui_text_clip(F_REG, 12, how, v.x + 96, v.y + v.height - 22, v.width - 110, TH.dark ? (Color){ 200, 210, 230, 160 } : (Color){ 60, 70, 90, 170 });
}

/* ============================================================================ Setup */
static double part_size(void) { double L = 0; if (F.ok) for (int k = 0; k < 3; k++) L = fmax(L, F.m.hi[k] - F.m.lo[k]); return L; }

static float faces_line(float x, float y, float w, char faces[][64], int nf)
{
    char t[600] = ""; double A = 0;
    for (int j = 0; j < nf; j++) { int g = F.ok ? fe_group(&F.m, faces[j]) : -1; if (g >= 0) A += F.m.garea[g]; }
    if (!nf) pl_strlcpy(t, "No faces yet: Pick faces, then click them in the view", sizeof t);
    else {
        pl_fmt_ui(t, sizeof t, "%d face%s, %.4g mm²: ", nf, nf == 1 ? "" : "s", A);
        for (int j = 0; j < nf && strlen(t) < 500; j++) pl_fmt_ui(t + strlen(t), sizeof t - strlen(t), "%s%s", j ? ", " : "", faces[j]);
    }
    return ui_text_wrap(F_REG, 12.5f, t, (Rectangle){ x, y, w, 60 }, 18, nf ? TH.text3 : TH.warn, true);
}

/* an item's head: icon, title, Pick faces (when it acts on faces) and remove */
static float item_head(float x, float y, float w, const char *icon, Color ic, const char *title, int act, bool faces, bool *del)
{
    bool on = F.active == act;
    if (on) ui_rect((Rectangle){ x - 8, y - 4, w + 16, 30 }, 8, ui_alpha(TH.accent, 0.12f));
    ui_icon(icon, x + 8, y + 11, 15, ic);
    ui_text_clip(F_SEMI, 14, title, x + 24, y + 2, w - 160, TH.text);
    if (faces) {
        float bw = ui_button_width(NULL, on ? "Done" : "Pick faces");
        if (ui_button(ui_idn(ui_id("fea-pick"), act), (Rectangle){ x + w - bw - 36, y - 2, bw, 28 }, NULL, on ? "Done" : "Pick faces", on ? BTN_PRIMARY : BTN_GHOST))
            { F.active = on ? -1 : act; F.dirty = true; }
    }
    *del = ui_button(ui_idn(ui_id("fea-del"), act), (Rectangle){ x + w - 30, y - 2, 30, 28 }, ICON_TRASH_2, NULL, BTN_GHOST);
    return 32;
}

/* a number with its unit after it; true when changed (an empty or bad entry keeps the old value) */
static bool fnum(const char *id, int k, float x, float y, float w, double *v, const char *unit, double lo, double hi)
{
    static char ed[512][32]; static UiId owner[512];
    UiId uid = ui_idn(ui_id(id), k);
    int slot = (int)(uid % 512);
    if (owner[slot] != uid || !ui_input_focused(uid)) { pl_fmt_ui(ed[slot], sizeof ed[slot], "%.6g", *v); owner[slot] = uid; }
    float uw = unit && *unit ? ui_text_w(F_REG, 13, unit) + 10 : 0;
    bool ch = false;
    if (ui_input(uid, (Rectangle){ x, y, w - uw, 32 }, ed[slot], sizeof ed[slot], NULL, INPUT_NUMERIC | INPUT_MONO)) {
        char *e; double d = strtod(ed[slot], &e);
        if (e != ed[slot] && d >= lo && d <= hi) { *v = d; ch = true; }
        else ui_toast(TOAST_ERR, "Give a number from %g to %g", lo, hi);
    }
    if (uw > 0) ui_text(F_REG, 13, unit, x + w - uw + 8, y + 8, TH.text3);
    return ch;
}

static float label_row(const char *lab, float x, float y, float lw) { ui_text_clip(F_MED, 13, lab, x, y + 8, lw - 8, TH.text2); return lw; }

/* ---- what is asked: the analysis, as the question it answers */
static const char *const AN_Q[AN__N] = {
    "Will it break, and how far does it bend?", "Past yield, or bending a long way", "How does it vibrate, and at what frequencies?",
    "At what load does it buckle?", "How hot does it get, and where?", "How much stress does heating cause?",
    "A sudden load or a knock: how does it move?", "Shaken at a frequency: how much does it move?" };
static const char *const AN_ICON[AN__N] = { ICON_ZAP, ICON_CHART_LINE, ICON_ORBIT, ICON_LAYERS, ICON_THERMOMETER, ICON_FLAME,
                                            ICON_ACTIVITY, ICON_WAVES };
static bool anOpen;                                    /* the eight shown (else the chosen one and Change) */
static float analysis_cards(float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "What do you want to find out?", NULL);
    /* a study with supports, loads or heat already has its question: the chosen card alone, Change to see them all */
    bool started = F.s.nFix || F.s.nLoad || F.s.nTh;
    if (started && !anOpen) {
        int a = F.s.analysis;
        Rectangle r = { x, y, w, 46 };
        ui_rect(r, 9, ui_alpha(TH.accent, 0.16f)); ui_rect_lines(r, 9, 2, TH.accent);
        ui_icon(AN_ICON[a], r.x + 20, r.y + 23, 16, TH.accent);
        ui_text_clip(F_SEMI, 13.5f, FE_AN_NAME[a], r.x + 40, r.y + 6, r.width - 150, TH.text);
        ui_text_clip(F_REG, 12, AN_Q[a], r.x + 40, r.y + 25, r.width - 150, TH.text3);
        ui_register(ui_idn(ui_id("fea-an"), a), r, FE_AN_NAME[a]);
        float bw = ui_button_width(NULL, "Change");
        if (ui_button(ui_id("fea-an-change"), (Rectangle){ r.x + r.width - bw - 8, r.y + 7, bw, 32 }, NULL, "Change", BTN_GHOST)) anOpen = true;
        return y - y0 + 56;
    }
    for (int a = 0; a < AN__N; a++) {
        Rectangle r = { x, y, w, 46 };
        bool on = F.s.analysis == a, hov = ui_hovered(r);
        UiId id = ui_idn(ui_id("fea-an"), a);
        ui_register(id, r, FE_AN_NAME[a]);
        ui_rect(r, 9, on ? ui_alpha(TH.accent, 0.16f) : hov ? TH.hover : TH.surface2);
        if (on) ui_rect_lines(r, 9, 2, TH.accent);
        ui_icon(AN_ICON[a], r.x + 20, r.y + 23, 16, on ? TH.accent : TH.text3);
        ui_text_clip(F_SEMI, 13.5f, FE_AN_NAME[a], r.x + 40, r.y + 6, r.width - 50, on ? TH.text : TH.text2);
        ui_text_clip(F_REG, 12, AN_Q[a], r.x + 40, r.y + 25, r.width - 50, TH.text3);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (ui_clicked(r)) anOpen = false;
        if (ui_clicked(r) && !on) {
            F.s.analysis = a; F.active = -1;
            if ((a == AN_DYNAMIC) && F.s.time >= 0.999 && F.s.time <= 1.001) F.s.time = 0.01;   /* a knock lasts milliseconds */
            save_study();
        }
        y += 50;
    }
    return y - y0 + 6;
}

/* ---- the material: a library name, its values (the ones this analysis uses), editable */
static float material_rows(float x, float y, float w)
{
    float y0 = y;
    const char *names[32]; int n = 0, sel = 0;
    for (int i = 0; i < CCX_LIB_N && n < 32; i++) { names[n] = CCX_LIB[i].name; if (!strcasecmp(CCX_LIB[i].name, F.s.material)) sel = n; n++; }
    FeMatProps p; fe_study_props(&F.s, &p);
    y += section_head(x, y, w, "Material", F.s.custom ? "Your values (the library's changed)" : "From the library: Edit values to change them");
    if (ui_select(ui_id("fea-mat"), (Rectangle){ x, y, w - 120, 34 }, names, n, &sel)) {
        pl_strlcpy(F.s.material, names[sel], sizeof F.s.material); F.s.custom = false; save_study();
    }
    bool edit = F.s.custom;
    if (ui_button(ui_id("fea-mat-edit"), (Rectangle){ x + w - 112, y, 112, 34 }, NULL, edit ? "Library" : "Edit values", BTN_GHOST)) {
        if (edit) F.s.custom = false; else { F.s.custom = true; F.s.props = p; }
        save_study();
    }
    y += 44;
    int an = F.s.analysis;
    bool mech = fe_an_mechanical(an), therm = fe_an_thermal(an), dyn = an == AN_FREQUENCY || an == AN_DYNAMIC || an == AN_HARMONIC || an == AN_BUCKLE || (an == AN_HEAT && F.s.transient);
    struct { const char *lab, *unit; double *v; double k, lo, hi; bool show; } R[] = {
        { "Stiffness E", "GPa", &F.s.props.E, 1e9, 1e-6, 1e4, mech },
        { "Poisson's ratio", "", &F.s.props.nu, 1, 0, 0.499, mech },
        { "Density", "kg/m³", &F.s.props.rho, 1, 1e-3, 1e5, dyn || mech },
        { "Yields at", "MPa", &F.s.props.yield, 1e6, 0, 1e5, mech },
        { "Breaks at (UTS)", "MPa", &F.s.props.uts, 1e6, 0, 1e5, an == AN_NONLINEAR },
        { "Stretches to (elongation)", "%", &F.s.props.elong, 0.01, 0, 1000, an == AN_NONLINEAR },
        { "Conductivity", "W/(m K)", &F.s.props.k, 1, 1e-6, 1e4, therm },
        { "Specific heat", "J/(kg K)", &F.s.props.cp, 1, 1e-3, 1e6, therm },
        { "Expansion", "1e-6/K", &F.s.props.alpha, 1e-6, 0, 1e4, an == AN_THERMSTRESS },
    };
    FeMatProps shown = p;
    double *sv[] = { &shown.E, &shown.nu, &shown.rho, &shown.yield, &shown.uts, &shown.elong, &shown.k, &shown.cp, &shown.alpha };
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) {
        if (!R[i].show) continue;
        label_row(R[i].lab, x + 8, y, 190);
        if (edit) {
            double v = *R[i].v / R[i].k;
            if (fnum("fea-matv", (int)i, x + 190, y, w - 190, &v, R[i].unit, R[i].lo, R[i].hi)) { *R[i].v = v * R[i].k; save_study(); }
            y += 38;
        } else {
            char t[64]; pl_fmt_ui(t, sizeof t, "%.4g %s", *sv[i] / R[i].k, R[i].unit);
            ui_text(F_MONO, 13, t, x + 190, y + 8, TH.text);
            y += 28;
        }
    }
    return y - y0 + 10;
}

/* ---- the analysis's own settings */
static float analysis_rows(float x, float y, float w)
{
    float y0 = y;
    int an = F.s.analysis;
    if (an == AN_STATIC) return 0;
    y += section_head(x, y, w, "Settings", NULL);
    float lw = 190;
    if (an == AN_NONLINEAR) {
        double n = F.s.inc > 0 ? 1 / F.s.inc : 10;
        label_row("Load steps (at least)", x + 8, y, lw);
        if (fnum("fea-inc", 0, x + lw, y, w - lw, &n, "", 2, 10000)) { F.s.inc = 1 / n; save_study(); }
        y += 38;
        y += ui_text_wrap(F_REG, 12.5f, "The load goes on in steps, with large deflection followed and the material yielding past its yield strength "
                          "(bilinear: yield, then UTS at the elongation). CalculiX halves a step that won't converge.",
                          (Rectangle){ x + 8, y, w - 8, 80 }, 18, TH.text3, true) + 8;
    }
    if (an == AN_FREQUENCY || an == AN_DYNAMIC || an == AN_HARMONIC || an == AN_BUCKLE) {
        double m = F.s.modes;
        label_row(an == AN_BUCKLE ? "Buckling shapes" : "Modes", x + 8, y, lw);
        if (fnum("fea-modes", 0, x + lw, y, w - lw, &m, "", 1, an == AN_BUCKLE ? 10 : 200)) { F.s.modes = (int)m; save_study(); }
        y += 38;
    }
    if (an == AN_HEAT || an == AN_THERMSTRESS) {
        static const char *TM[2] = { "Steady", "In time" };
        int tr = F.s.transient;
        label_row("Time", x + 8, y, lw);
        if (ui_segmented(ui_id("fea-trans"), (Rectangle){ x + lw, y, w - lw, 32 }, TM, 2, &tr)) { F.s.transient = tr; if (tr && F.s.time <= 1) F.s.time = 60; save_study(); }
        y += 40;
        label_row(an == AN_THERMSTRESS ? "Stress-free at" : "Starting at", x + 8, y, lw);
        if (fnum("fea-T0", 0, x + lw, y, w - lw, &F.s.T0, "K", 0, 1e5)) save_study();
        y += 38;
    }
    if ((an == AN_HEAT || an == AN_THERMSTRESS) && F.s.transient) {
        label_row("For", x + 8, y, lw);
        if (fnum("fea-time", 0, x + lw, y, w - lw, &F.s.time, "s", 1e-9, 1e9)) save_study();
        y += 38;
    }
    if (an == AN_DYNAMIC) {
        label_row("For", x + 8, y, lw);
        if (fnum("fea-time", 1, x + lw, y, w - lw, &F.s.time, "s", 1e-9, 1e6)) save_study();
        y += 38;
        int a = F.s.amp;
        label_row("The load", x + 8, y, lw);
        static const char *AMP[AMP__N] = { "Steps on", "Ramps up", "A pulse" };
        if (ui_segmented(ui_id("fea-amp"), (Rectangle){ x + lw, y, w - lw, 32 }, AMP, AMP__N, &a)) { F.s.amp = a; save_study(); }
        y += 40;
        if (F.s.amp != AMP_STEP) {
            label_row(F.s.amp == AMP_RAMP ? "Rising over" : "Lasting", x + 8, y, lw);
            if (fnum("fea-ampT", 0, x + lw, y, w - lw, &F.s.ampT, "s", 1e-9, 1e6)) save_study();
            y += 38;
        }
    }
    if (an == AN_HARMONIC) {
        label_row("From", x + 8, y, lw);
        if (fnum("fea-flo", 0, x + lw, y, w - lw, &F.s.fLo, "Hz", 0, 1e9)) save_study();
        y += 38;
        label_row("To", x + 8, y, lw);
        if (fnum("fea-fhi", 0, x + lw, y, w - lw, &F.s.fHi, "Hz", 1e-6, 1e9)) save_study();
        y += 38;
        double np = F.s.nPts;
        label_row("Points between modes", x + 8, y, lw);
        if (fnum("fea-npts", 0, x + lw, y, w - lw, &np, "", 2, 1000)) { F.s.nPts = (int)np; save_study(); }
        y += 38;
    }
    if (an == AN_DYNAMIC || an == AN_HARMONIC) {
        double pc = F.s.damping * 100;
        label_row("Damping", x + 8, y, lw);
        if (fnum("fea-damp", 0, x + lw, y, w - lw, &pc, "% of critical", 0, 100)) { F.s.damping = pc / 100; save_study(); }
        y += 38;
        y += ui_text_wrap(F_REG, 12.5f, "Steel and aluminium parts on their own: 0.5 to 2 %; bolted assemblies 2 to 5 %; rubber mounts 5 to 15 %.",
                          (Rectangle){ x + 8, y, w - 8, 60 }, 18, TH.text3, true) + 8;
    }
    return y - y0 + 6;
}

/* ---- supports */
static float support_rows(float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "Supports", F.s.analysis == AN_FREQUENCY ? "Where it is held (none: it vibrates free, with six zero modes)" : "Where the part is held: bolted, welded or resting.");
    static const char *FXN[FX__N] = { "Fixed (can't move)", "Held in x only", "Held in y only", "Held in z only", "Roller (slides on the face)",
                                       "Pin (turns, held round its axis)", "Moved a set distance" };
    static const char *DIRA[6] = { "+x", "+y", "+z", "-x", "-y", "-z" };
    for (int i = 0; i < F.s.nFix; i++) {
        FeFixture *fx = &F.s.fix[i];
        char ttl[64]; pl_fmt_ui(ttl, sizeof ttl, "Support %d", i + 1);
        bool del = false;
        y += item_head(x, y, w, ICON_SQUARE, (Color){ 45, 190, 160, 255 }, ttl, i, true, &del);
        if (del) { memmove(&F.s.fix[i], &F.s.fix[i + 1], sizeof(FeFixture) * (size_t)(F.s.nFix - i - 1)); F.s.nFix--; F.active = -1; save_study(); break; }
        int k = fx->kind;
        if (ui_select(ui_idn(ui_id("fea-fxkind"), i), (Rectangle){ x + 24, y, w - 24, 32 }, FXN, FX__N, &k)) { fx->kind = k; if (k == FX_DISP && fx->dir < DIR_PX) fx->dir = DIR_PX; save_study(); }
        y += 38;
        if (fx->kind == FX_DISP) {
            if (fnum("fea-fxv", i, x + 24, y, 140, &fx->value, "mm", -1e6, 1e6)) save_study();
            int d = fx->dir >= DIR_PX ? fx->dir - 1 : 0;
            if (ui_select(ui_idn(ui_id("fea-fxdir"), i), (Rectangle){ x + 174, y, w - 174, 32 }, DIRA, 6, &d)) { fx->dir = d + 1; save_study(); }
            y += 38;
        }
        if (fx->kind == FX_PIN || fx->kind == FX_ROLLER) {
            const char *why = NULL; double ax[3], c[3], R;
            int g = fx->nf ? fe_group(&F.m, fx->face[0]) : -1;
            if (fx->kind == FX_PIN && g >= 0 && !fe_cylinder(&F.m, g, ax, c, &R)) why = "A pin needs a round face (a hole or a shaft): this one isn't";
            if (fx->kind == FX_ROLLER && g >= 0 && !F.m.gplanar[g]) why = "A roller holds a flat face square to itself: this one isn't flat";
            if (why) y += ui_text_wrap(F_REG, 12.5f, why, (Rectangle){ x + 24, y, w - 24, 40 }, 18, TH.warn, true) + 4;
            else if (fx->kind == FX_PIN && g >= 0) { char t[160]; pl_fmt_ui(t, sizeof t, "Round face: radius %.4g mm, axis (%.2f, %.2f, %.2f)", R, ax[0], ax[1], ax[2]);
                ui_text_clip(F_REG, 12.5f, t, x + 24, y, w - 24, TH.text3); y += 20; }
        }
        y += faces_line(x + 24, y, w - 24, fx->face, fx->nf) + 14;
    }
    if (F.s.nFix < FE_MAXITEM && ui_button(ui_id("fea-addfix"), (Rectangle){ x, y, ui_button_width(ICON_PLUS, "Support"), 32 }, ICON_PLUS, "Support", BTN_SECONDARY)) {
        FeFixture *fx = &F.s.fix[F.s.nFix++]; memset(fx, 0, sizeof *fx);
        F.active = F.s.nFix - 1; save_study();
    }
    return y - y0 + 48;
}

/* ---- loads */
static const char *const LD_TITLE[LD__N] = { "Force", "Pressure", "Gravity", "Rotation", "Bearing load", "Torque" };
static const char *const LD_UNIT[LD__N] = { "N", "MPa", "m/s²", "rpm", "N", "N m" };
static const char *const LD_ICON[LD__N] = { ICON_ZAP, ICON_GAUGE, ICON_ARROW_RIGHT, ICON_FAN, ICON_CIRCLE_DOT, ICON_ROTATE_CCW };
static float load_rows(float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, F.s.analysis == AN_BUCKLE ? "Loads (the buckling load is a multiple of these)" : "Loads",
                      "Forces in newtons, shared over the faces; pressures in MPa; gravity and rotation act on the whole part.");
    static const char *DIRN[DIR__N] = { "Normal (into the face)", "+x", "+y", "+z", "-x", "-y", "-z" };
    static const char *DIRA[6] = { "+x", "+y", "+z", "-x", "-y", "-z" };
    for (int i = 0; i < F.s.nLoad; i++) {
        FeLoad *d = &F.s.load[i];
        char ttl[64]; pl_fmt_ui(ttl, sizeof ttl, "%s %d", LD_TITLE[d->kind], i + 1);
        bool del = false, faces = d->kind != LD_GRAVITY && d->kind != LD_ROTATION;
        y += item_head(x, y, w, LD_ICON[d->kind], (Color){ 245, 150, 50, 255 }, ttl, 100 + i, faces, &del);
        if (del) { memmove(&F.s.load[i], &F.s.load[i + 1], sizeof(FeLoad) * (size_t)(F.s.nLoad - i - 1)); F.s.nLoad--; F.active = -1; save_study(); break; }
        float vw = d->kind == LD_PRESSURE || d->kind == LD_TORQUE ? w - 24 : 160;
        if (fnum("fea-val", i, x + 24, y, vw, &d->value, LD_UNIT[d->kind], -1e12, 1e12)) save_study();
        if (d->kind == LD_FORCE) {
            int dir = d->dir;
            if (ui_select(ui_idn(ui_id("fea-dir"), i), (Rectangle){ x + 24 + vw + 10, y, w - 34 - vw, 32 }, DIRN, DIR__N, &dir)) { d->dir = dir; save_study(); }
        } else if (d->kind == LD_GRAVITY || d->kind == LD_ROTATION || d->kind == LD_BEARING) {
            int dir = d->dir >= DIR_PX ? d->dir - 1 : 2 + 3 * (d->kind == LD_GRAVITY);
            if (d->dir < DIR_PX) { d->dir = dir + 1; save_study(); }
            if (ui_select(ui_idn(ui_id("fea-dir"), i), (Rectangle){ x + 24 + vw + 10, y, w - 34 - vw, 32 }, DIRA, 6, &dir)) { d->dir = dir + 1; save_study(); }
        }
        y += 38;
        if (d->kind == LD_ROTATION) {
            label_row("About the axis through", x + 24, y, 170);
            for (int q = 0; q < 3; q++) if (fnum("fea-pt", i * 3 + q, x + 194 + q * ((w - 194) / 3), y, (w - 194) / 3 - 6, &d->pt[q], q == 2 ? "mm" : "", -1e9, 1e9)) save_study();
            y += 38;
        }
        if (d->kind == LD_GRAVITY) { y += ui_text_wrap(F_REG, 12.5f, "9.81 m/s² is Earth's; more for a part in a vehicle that accelerates or lands hard.",
                                                       (Rectangle){ x + 24, y, w - 24, 40 }, 18, TH.text3, true) + 8; continue; }
        if (d->kind == LD_ROTATION) continue;
        if ((d->kind == LD_BEARING || d->kind == LD_TORQUE) && d->nf) {
            double ax[3], c[3], R; int g = fe_group(&F.m, d->face[0]);
            if (g >= 0 && !fe_cylinder(&F.m, g, ax, c, &R))
                y += ui_text_wrap(F_REG, 12.5f, "This load needs a round face (a hole or a shaft): this one isn't", (Rectangle){ x + 24, y, w - 24, 40 }, 18, TH.warn, true) + 4;
        }
        y += faces_line(x + 24, y, w - 24, d->face, d->nf) + 14;
    }
    if (F.s.nLoad < FE_MAXITEM) {
        float bx = x;
        static const int ADD[] = { LD_FORCE, LD_PRESSURE, LD_GRAVITY, LD_ROTATION, LD_BEARING, LD_TORQUE };
        static const double DEF[] = { 100, 1, 9.81, 3000, 1000, 10 };
        for (int k = 0; k < 6; k++) {
            float bw = ui_button_width(ICON_PLUS, LD_TITLE[ADD[k]]);
            if (bx + bw > x + w) { bx = x; y += 38; }
            if (ui_button(ui_idn(ui_id("fea-addload"), k), (Rectangle){ bx, y, bw, 32 }, ICON_PLUS, LD_TITLE[ADD[k]], BTN_SECONDARY)) {
                FeLoad *d = &F.s.load[F.s.nLoad++]; memset(d, 0, sizeof *d); d->kind = ADD[k]; d->value = DEF[k];
                d->dir = ADD[k] == LD_GRAVITY ? DIR_MZ : ADD[k] == LD_ROTATION ? DIR_PZ : ADD[k] == LD_BEARING ? DIR_MY : DIR_NORMAL;
                F.active = ADD[k] == LD_GRAVITY || ADD[k] == LD_ROTATION ? -1 : 100 + F.s.nLoad - 1; save_study();
            }
            bx += bw + 8;
        }
    }
    return y - y0 + 48;
}

/* ---- thermal conditions */
static const char *const TH_TITLE[TH__N] = { "Temperature", "Heat flux in", "Convection", "Radiation", "Heat made inside" };
static float thermal_rows(float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "Heat", "Faces held at a temperature, heat coming in, convection and radiation to the surroundings, heat made inside (a coil, a chip).");
    for (int i = 0; i < F.s.nTh; i++) {
        FeThermal *t = &F.s.th[i];
        char ttl[64]; pl_fmt_ui(ttl, sizeof ttl, "%s %d", TH_TITLE[t->kind], i + 1);
        bool del = false, faces = t->kind != TH_HEATGEN;
        y += item_head(x, y, w, ICON_THERMOMETER, (Color){ 230, 80, 90, 255 }, ttl, 200 + i, faces, &del);
        if (del) { memmove(&F.s.th[i], &F.s.th[i + 1], sizeof(FeThermal) * (size_t)(F.s.nTh - i - 1)); F.s.nTh--; F.active = -1; save_study(); break; }
        switch (t->kind) {
        case TH_TEMP: if (fnum("fea-thv", i, x + 24, y, 160, &t->value, "K", 0, 1e5)) save_study();
            { char c[48]; pl_fmt_ui(c, sizeof c, "= %.1f °C", t->value - 273.15); ui_text(F_REG, 13, c, x + 194, y + 8, TH.text3); } break;
        case TH_FLUX: if (fnum("fea-thv", i, x + 24, y, 200, &t->value, "W/m²", -1e12, 1e12)) save_study(); break;
        case TH_CONVECT:
            if (fnum("fea-thv", i, x + 24, y, (w - 24) / 2 - 4, &t->value, "W/(m² K)", 0, 1e9)) save_study();
            if (fnum("fea-tha", i, x + 24 + (w - 24) / 2, y, (w - 24) / 2, &t->value2, "K air", 0, 1e5)) save_study();
            break;
        case TH_RADIATE:
            if (fnum("fea-thv", i, x + 24, y, (w - 24) / 2 - 4, &t->value, "emissivity", 0, 1)) save_study();
            if (fnum("fea-tha", i, x + 24 + (w - 24) / 2, y, (w - 24) / 2, &t->value2, "K round", 0, 1e5)) save_study();
            break;
        default: if (fnum("fea-thv", i, x + 24, y, 200, &t->value, "W in all", -1e12, 1e12)) save_study(); break;
        }
        y += 38;
        if (t->kind == TH_CONVECT) y += ui_text_wrap(F_REG, 12.5f, "Still air 5 to 25, a fan 25 to 250, water 500 to 10 000 W/(m² K).", (Rectangle){ x + 24, y, w - 24, 40 }, 18, TH.text3, true) + 6;
        if (faces) y += faces_line(x + 24, y, w - 24, t->face, t->nf) + 14;
        else y += 8;
    }
    if (F.s.nTh < FE_MAXITEM) {
        float bx = x;
        static const double DEF[TH__N] = { 373.15, 10000, 20, 0.9, 10 };
        for (int k = 0; k < TH__N; k++) {
            float bw = ui_button_width(ICON_PLUS, TH_TITLE[k]);
            if (bx + bw > x + w) { bx = x; y += 38; }
            if (ui_button(ui_idn(ui_id("fea-addth"), k), (Rectangle){ bx, y, bw, 32 }, ICON_PLUS, TH_TITLE[k], BTN_SECONDARY)) {
                FeThermal *t = &F.s.th[F.s.nTh++]; memset(t, 0, sizeof *t); t->kind = k; t->value = DEF[k]; t->value2 = 293.15;
                F.active = k == TH_HEATGEN ? -1 : 200 + F.s.nTh - 1; save_study();
            }
            bx += bw + 8;
        }
    }
    return y - y0 + 48;
}

void fea_setup_page(Rectangle r)
{
    Case *c = A.cs;
    if (!c) return;
    fea_sync(c->s.path);
    if (mesh_busy()) ui_request_frames(30);
    float lw = fminf(470, r.width * 0.38f);
    Rectangle left = { r.x, r.y, lw, r.height };
    DrawRectangleRec((Rectangle){ r.x + lw - 1, r.y, 1, r.height }, TH.lineSoft);
    static UiScroll sc;
    float top = ui_scroll_begin(&sc, left);
    float x = left.x + 26, w = lw - 52, y = top + 26;
    ui_text(F_SEMI, TH.lcars ? 26 : 20, ui_cap("Study"), x, y - (TH.lcars ? 3 : 0), head_color()); y += 38;
    char t[400];
    y += analysis_cards(x, y, w);
    /* ---- the part and its mesh */
    if (F.ok) pl_fmt_ui(t, sizeof t, "%s · %.4g x %.4g x %.4g mm · %d nodes, %d %s", F.s.part, F.m.hi[0] - F.m.lo[0], F.m.hi[1] - F.m.lo[1], F.m.hi[2] - F.m.lo[2],
                        F.m.n, F.m.ne, F.m.ne && femesh_ccx_type(F.m.ek[0]) ? femesh_ccx_type(F.m.ek[0]) : "elements");
    else pl_fmt_ui(t, sizeof t, "%s · not meshed yet", F.s.part);
    y += section_head(x, y, w, "Part", t);
    bool changed = false;
    if (unitless(F.s.part)) {
        static const char *U[] = { "Millimetres", "Metres", "Inches" };
        static const double UK[] = { 1, 1000, 25.4 };
        int u = F.s.scale == 1000 ? 1 : F.s.scale == 25.4 ? 2 : 0;
        ui_text(F_MED, 13, "The file's unit", x, y + 8, TH.text2);
        if (ui_select(ui_id("fea-unit"), (Rectangle){ x + 130, y, w - 130, 34 }, U, 3, &u)) { F.s.scale = UK[u]; changed = true; }
        y += 44;
    }
    {
        static const char *SZ[] = { "Coarse", "Medium", "Fine" };
        double L = part_size();
        int sel = F.s.size <= 0 ? 1 : L > 0 ? (F.s.size >= L / 13 ? 0 : F.s.size >= L / 28 ? 1 : 2) : 1;
        ui_text(F_MED, 13, "Mesh", x, y + 8, TH.text2);
        if (ui_segmented(ui_id("fea-size"), (Rectangle){ x + 130, y, w - 130, 34 }, SZ, 3, &sel)) {
            F.s.size = L > 0 ? L / (sel == 0 ? 10 : sel == 1 ? 20 : 40) : (sel == 1 ? 0 : F.s.size); changed = true;
        }
        y += 40;
        pl_fmt_ui(t, sizeof t, "Elements about %.3g mm across%s", F.s.size > 0 ? F.s.size : (L > 0 ? L / 20 : 0), F.s.order == 2 ? ", quadratic (C3D10: right in bending)" : "");
        if (F.s.size > 0 || L > 0) { ui_text_clip(F_REG, 12.5f, t, x + 130, y, w - 130, TH.text3); y += 24; }
    }
    if (changed) save_study();
    if (mesh_busy()) {
        ui_spinner(x + 10, y + 16, 8, TH.accent);
        pl_fmt_ui(t, sizeof t, "Meshing in SALOME, %.0f s", GetTime() - F.t0);
        ui_text(F_MED, 13.5f, t, x + 28, y + 7, TH.text);
        y += 40;
    } else {
        const char *lab = F.ok ? "Mesh again" : "Mesh the part";
        if (ui_button(ui_id("fea-mesh"), (Rectangle){ x, y, w, 38 }, ICON_BOXES, lab, F.ok ? BTN_SECONDARY : BTN_PRIMARY)) fea_mesh_start(c->s.path);
        y += 50;
    }
    y += material_rows(x, y, w);
    y += analysis_rows(x, y, w);
    if (fe_an_mechanical(F.s.analysis)) { y += support_rows(x, y, w); if (F.s.analysis != AN_FREQUENCY) y += load_rows(x, y, w); }
    if (fe_an_thermal(F.s.analysis)) y += thermal_rows(x, y, w);
    /* ---- ready? */
    if (F.ok) {
        FeSummary sum; fe_study_summary(&F.s, &F.m, &sum);
        if (sum.problem[0]) {
            float ch = callout_height(w, NULL, sum.problem);
            callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, NULL, sum.problem);
            ui_register(ui_id("fea-problem"), (Rectangle){ x, y, w, ch }, sum.problem);
            y += ch + 12;
        } else {
            double fm = sqrt(sum.force[0] * sum.force[0] + sum.force[1] * sum.force[1] + sum.force[2] * sum.force[2]);
            if (fe_an_mechanical(F.s.analysis) && F.s.analysis != AN_FREQUENCY)
                pl_fmt_ui(t, sizeof t, "Ready: %s; face loads %.4g N in all (%.4g, %.4g, %.4g); %d nodes held", FE_AN_NAME[F.s.analysis], fm, sum.force[0], sum.force[1], sum.force[2], sum.fixedNodes);
            else pl_fmt_ui(t, sizeof t, "Ready: %s", FE_AN_NAME[F.s.analysis]);
            ui_icon(ICON_CIRCLE_CHECK, x + 8, y + 9, 14, TH.ok);
            y += ui_text_wrap(F_REG, 13, t, (Rectangle){ x + 24, y, w - 24, 60 }, 19, TH.text2, true) + 10;
        }
        ui_disable(sum.problem[0] != 0 || mesh_busy() || A.cj->active);
        if (ui_button(ui_id("fea-run"), (Rectangle){ x, y, w, 42 }, ICON_PLAY, "Run the study", BTN_PRIMARY)) { A.runPending = true; app_go(PG_RUN); }
        ui_disable(false);
        y += 54;
    }
    ui_scroll_end(&sc, y - top + 20);

    /* ---- the view */
    Rectangle vr = { r.x + lw + 20, r.y + 20, r.width - lw - 40, r.height - 40 };
    if (!F.ok) {
        ui_rect(vr, 12, TH.dark ? (Color){ 10, 14, 22, 255 } : (Color){ 236, 240, 247, 255 });
        const char *msg = mesh_busy() ? "Meshing the part: it appears here when SALOME is done." : "Mesh the part to see it here and pick its faces.";
        ui_text_wrap(F_REG, 14, msg, (Rectangle){ vr.x + vr.width / 2 - 200, vr.y + vr.height / 2 - 20, 400, 60 }, 21, TH.text3, true);
        return;
    }
    F.results = false;
    int *nf; char (*fc)[64] = active_faces(&nf);
    if (fc) {
        const char *what = F.active >= 200 ? TH_TITLE[F.s.th[F.active - 200].kind] : F.active >= 100 ? LD_TITLE[F.s.load[F.active - 100].kind] : "Support";
        int idx = (F.active >= 200 ? F.active - 200 : F.active >= 100 ? F.active - 100 : F.active) + 1;
        pl_fmt_ui(t, sizeof t, "Picking faces for %s %d: click a face to add it, again to take it off", what, idx);
        ui_text_clip(F_MED, 13.5f, t, vr.x, vr.y - 2, vr.width, TH.accent);
        vr.y += 22; vr.height -= 22;
    } else if (F.active >= 0) F.active = -1;
    view(vr, fc != NULL);
}

/* ============================================================================ Results */
static void load_results(const char *proj)
{
    char p[PL_PATH]; pl_join(p, sizeof p, proj, "study.frd");
    time_t mt = pl_mtime(p);
    if (mt == F.rMt && !strcmp(F.rPath, p)) return;
    if (F.rOk) ccx_frd_free(&F.r);
    free(F.rmap); F.rmap = NULL; F.rmapN = 0;
    F.rOk = mt && ccx_frd_load(p, &F.r);
    F.rMt = mt; pl_strlcpy(F.rPath, p, sizeof F.rPath);
    if (F.rOk) {
        int mx = 0; for (int i = 0; i < F.r.n; i++) if (F.r.id[i] > mx) mx = F.r.id[i];
        F.rmapN = mx + 1; F.rmap = malloc(sizeof(int) * (size_t)F.rmapN);
        if (!F.rmap) { F.rmapN = 0; F.rOk = false; }
        else {
            for (int i = 0; i < F.rmapN; i++) F.rmap[i] = -1;
            for (int i = 0; i < F.r.n; i++) if (F.r.id[i] >= 0) F.rmap[F.r.id[i]] = i;
        }
    }
    F.dirty = true;
}

/* the deformed part coloured by stress or displacement, a colour bar beside it; the height used (0: no results) */
float fea_results_panel(float x, float y, float w)
{
    Case *c = A.cs;
    if (!c || !fea_is_study(c->s.path)) return 0;
    fea_sync(c->s.path);
    load_results(c->s.path);
    if (!F.ok || !F.rOk) return 0;
    float y0 = y;
    static const char *FN[] = { "Von Mises stress", "Displacement" };
    int fsel = F.resField;
    y += section_head(x, y, w, "The part", "Deformed (exaggerated so the shape shows) and coloured; drag to turn it.");
    if (ui_segmented(ui_id("fea-field"), (Rectangle){ x, y, 320, 32 }, FN, 2, &fsel)) { F.resField = fsel; F.dirty = true; }
    bool und = F.showUndeformed;
    ui_checkbox(ui_id("fea-undeformed"), (Rectangle){ x + 340, y + 5, 200, 22 }, &und, "Undeformed shape");
    if (und != F.showUndeformed) { F.showUndeformed = und; F.dirty = true; }
    y += 44;
    float bar = 90;
    Rectangle vr = { x, y, w - bar - 16, fminf(470, fmaxf(300, w * 0.42f)) };
    F.results = true;
    view(vr, false);
    /* the colour bar */
    float bx = x + w - bar, by = vr.y + 26, bh = vr.height - 60;
    float vmax = res_max();
    for (int i = 0; i < 64; i++) {
        float t0 = 1 - i / 64.f;
        DrawRectangleRec((Rectangle){ bx, by + bh * i / 64.f, 18, bh / 64.f + 1 }, stress_color(t0));
    }
    ui_rect_lines((Rectangle){ bx, by, 18, bh }, 2, 1, TH.lineSoft);
    const char *unit = F.resField == 1 ? "mm" : "MPa";
    for (int k = 0; k <= 4; k++) {
        char v[32]; pl_fmt_ui(v, sizeof v, "%.3g", vmax * (1 - k / 4.0));
        ui_text(F_MONO, 11.5f, v, bx + 24, by + bh * k / 4.f - 7, TH.text2);
    }
    ui_text(F_MED, 12, unit, bx, by - 22, TH.text2);
    char t[200];
    if (F.resField == 0) pl_fmt_ui(t, sizeof t, "Largest %.4g MPa", F.r.vmMax); else pl_fmt_ui(t, sizeof t, "Largest %.4g mm", F.r.uMax);
    ui_text_clip(F_REG, 11.5f, t, bx - 10, by + bh + 10, bar + 10, TH.text3);
    y = vr.y + vr.height + 16;
    return y - y0;
}

void fea_forget(void)
{
    if (F.rtOk) { UnloadRenderTexture(F.rt); F.rtOk = false; }
    F.proj[0] = 0;
}
