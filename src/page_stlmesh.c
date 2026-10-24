/* page_stlmesh.c -- the Mesh page's STL mesher tab (user request, 2026-10-23): choose any STL, see what it is (size,
 * closed or not, its boundaries split by angle), set the mesh up the way STAR-CCM+'s automated mesher does, and mesh it
 * with snappyHexMesh here or with SALOME (NETGEN tetrahedra, polyhedra through polyDualMesh, Body Fitting hexahedra).
 * "Mesh an STL" on the Cases page makes a case for a bare STL and opens this tab with it. The settings live in
 * <case>/mesher/mesher.ini (stlmesh.c), which SALOME's script reads too.
 */
#define _GNU_SOURCE
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "app.h"
#include "stlmesh.h"

static struct {
    char   casePath[PL_PATH];   /* the case S and M belong to          */
    Stl    stl;
    MeshCfg m;
    bool   loaded, dirty;
    pid_t  picker;              /* the file dialog, while it is open   */
    char   pickOut[PL_PATH];
    bool   pickNewCase;         /* the pick makes a new case (Cases page) */
} T = { .picker = -1 };

/* ============================================================================ loading */
static void save_cfg(void)
{
    if (!T.casePath[0]) return;
    mesher_take_names(&T.m, &T.stl);
    mesher_save(T.casePath, &T.m);
    T.dirty = false;
}

static bool load_stl(const char *path, bool fresh)
{
    Stl s;
    if (!stl_load(path, &s)) { ui_toast(TOAST_ERR, "%s", s.err); return false; }
    stl_free(&T.stl);
    T.stl = s;
    if (T.m.scale <= 0) T.m.scale = 1;
    stl_set_scale(&T.stl, T.m.scale);
    stl_split(&T.stl, T.m.splitAngle);
    if (fresh) {
        pl_strlcpy(T.m.stl, path, sizeof T.m.stl);
        /* a part a thousand times too large is in millimetres: say so and pick mm */
        double L = 0; for (int k = 0; k < 3; k++) L = fmax(L, T.stl.bmax[k] - T.stl.bmin[k]);
        if (L > 20 && T.m.scale == 1) { T.m.scale = 0.001; stl_set_scale(&T.stl, 0.001); stl_split(&T.stl, T.m.splitAngle);
                                        ui_toast(TOAST_INFO, "The part is %.0f units across: read as millimetres (Scale changes it)", L); }
        mesher_defaults(&T.m, &T.stl);
        stl_split(&T.stl, T.m.splitAngle);                /* split again at the default angle */
        mesher_take_names(&T.m, &T.stl);
    } else mesher_apply_names(&T.m, &T.stl);
    T.loaded = true;
    save_cfg();
    return true;
}

static void sync_case(Case *c)
{
    if (!strcmp(T.casePath, c->s.path)) return;
    stl_free(&T.stl);
    memset(&T.m, 0, sizeof T.m);
    T.loaded = false;
    pl_strlcpy(T.casePath, c->s.path, sizeof T.casePath);
    if (mesher_load(c->s.path, &T.m) && T.m.stl[0]) {
        char p[PL_PATH]; pl_strlcpy(p, T.m.stl, sizeof p);
        if (!pl_is_file(p)) pl_join(p, sizeof p, c->s.path, "constant/triSurface/geometry.stl");   /* the original moved: our copy */
        if (pl_is_file(p)) load_stl(p, false);
    }
}

/* ============================================================================ the file dialog */
static void start_picker(bool newCase)
{
    if (T.picker > 0) return;
    snprintf(T.pickOut, sizeof T.pickOut, "/tmp/qc-pick-%d.txt", (int)getpid());
    unlink(T.pickOut);
    char q[PL_PATH + 8]; pl_shell_quote(q, sizeof q, T.pickOut);
    char cmd[PL_PATH + 1400];
    if (A.env.wsl)
        /* Windows' own Open dialog, on top; its path comes back as C:\..., wslpath makes it /mnt/c/... */
        snprintf(cmd, sizeof cmd,
                 "p=$(powershell.exe -NoProfile -STA -Command \"Add-Type -AssemblyName System.Windows.Forms; "
                 "\\$f = New-Object System.Windows.Forms.Form -Property @{TopMost=\\$true}; "
                 "\\$d = New-Object System.Windows.Forms.OpenFileDialog; \\$d.Title = 'Choose an STL to mesh'; "
                 "\\$d.Filter = 'STL files (*.stl)|*.stl;*.STL|All files (*.*)|*.*'; "
                 "if (\\$d.ShowDialog(\\$f) -eq 'OK') { \\$d.FileName }\" 2>/dev/null | tr -d '\\r'); "
                 "[ -n \"$p\" ] && wslpath -u \"$p\" > %s; true", q);
    else
        snprintf(cmd, sizeof cmd,
                 "{ zenity --file-selection --title='Choose an STL to mesh' --file-filter='STL files | *.stl *.STL' 2>/dev/null "
                 "|| kdialog --getopenfilename ~ '*.stl *.STL' 2>/dev/null; } > %s; true", q);
    T.picker = pl_spawn_shell(cmd, "/dev/null");
    T.pickNewCase = newCase;
    if (T.picker <= 0) ui_toast(TOAST_ERR, "Could not open a file dialog: type the STL's path instead");
}

static void new_case_from(const char *stl)
{
    char ws[PL_PATH], name[128], dir[PL_PATH];
    pl_expand_home(ws, sizeof ws, A.set.workspace);
    pl_strlcpy(name, pl_basename(stl), sizeof name);
    char *dot = strrchr(name, '.'); if (dot) *dot = 0;
    for (char *c = name; *c; c++) if (*c == ' ' || *c == '/' || *c == '\'' || *c == '"') *c = '_';
    pl_join(dir, sizeof dir, ws, name);
    for (int k = 2; pl_exists(dir) && k < 100; k++) { char n2[160]; snprintf(n2, sizeof n2, "%s-%d", name, k); pl_join(dir, sizeof dir, ws, n2); }
    char sys[PL_PATH]; pl_join(sys, sizeof sys, dir, "system");
    if (!pl_mkdirs(sys)) { ui_toast(TOAST_ERR, "Could not make %s", dir); return; }
    /* the minimal dictionaries go in with the first Generate; a controlDict now, so the folder is a case */
    MeshCfg m; memset(&m, 0, sizeof m); m.scale = 1;
    Stl s;
    if (!stl_load(stl, &s)) { ui_toast(TOAST_ERR, "%s", s.err); return; }
    stl_split(&s, 30);
    mesher_defaults(&m, &s);
    pl_strlcpy(m.stl, stl, sizeof m.stl);
    char err[256];
    mesher_prepare(dir, &m, &s, err, sizeof err);              /* writes controlDict and friends, the copy of the STL */
    stl_free(&s);
    app_rescan_cases(true);
    app_open_case(dir, PG_MESH);
    A.meshTab = 3;
    pl_strlcpy(T.casePath, "", sizeof T.casePath);            /* sync_case loads it from the new folder */
    ui_toast(TOAST_OK, "Made the case %s for %s", pl_basename(dir), pl_basename(stl));
}

static void poll_picker(void)
{
    if (T.picker <= 0) return;
    ui_request_frames(2);
    int st;
    if (waitpid(T.picker, &st, WNOHANG) != T.picker) return;
    T.picker = -1;
    char *p = pl_read_file(T.pickOut, NULL);
    unlink(T.pickOut);
    if (!p) return;
    pl_trim_inplace(p);
    if (p[0]) {
        if (T.pickNewCase) new_case_from(p);
        else if (A.cs) { sync_case(A.cs); load_stl(p, true); }
    }
    free(p);
}

void stlmesh_new_case(void) { start_picker(true); }
void stlmesh_tick(void) { poll_picker(); }

/* ============================================================================ meshing */
static bool salome_mesher(void) { return T.m.mesher != MESHER_SNAPPY; }

static bool prepare(Case *c)
{
    if (!T.loaded) { ui_toast(TOAST_ERR, "Choose an STL first"); return false; }
    char err[300];
    if (!mesher_prepare(c->s.path, &T.m, &T.stl, err, sizeof err)) { ui_toast(TOAST_ERR, "%s", err); return false; }
    T.dirty = false;
    case_rescan(c);
    return true;
}

static void generate(Case *c)
{
    if (!app_require_foam() || !app_can_start()) return;
    if (c->s.procTimes > 0) { ui_toast(TOAST_ERR, "processor* holds results for the current mesh. Clean the case first."); return; }
    if (salome_mesher() && !A.env.salome[0]) { ui_toast(TOAST_ERR, "SALOME is not installed: see the note under Mesher"); return; }
    if (!prepare(c)) return;
    job_reset(A.cj, "Mesh");
    job_add(A.cj, "clear", "Clear the old mesh", "rm -rf constant/polyMesh 0/polyMesh; echo cleared", "log.meshClear", false);
    if (!salome_mesher()) {
        job_add(A.cj, "surfaceFeatureExtract", "surfaceFeatureExtract", "surfaceFeatureExtract", "log.surfaceFeatureExtract", false);
        job_add(A.cj, "blockMesh", "blockMesh (background)", "blockMesh", "log.blockMesh", false);
        job_add(A.cj, "snappyHexMesh", "snappyHexMesh", "snappyHexMesh -overwrite", "log.snappyHexMesh", false);
    } else {
        char q[PL_PATH + 8], cmd[PL_PATH + 600];
        pl_shell_quote(q, sizeof q, A.env.salome);
        snprintf(cmd, sizeof cmd,
                 "cd salome && rm -f mesh.unv && env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME %s -t --shutdown-servers=1 stl_mesh.py\n"
                 "rc=$?; [ -f mesh.unv ] || { echo \"Q-Continuum: SALOME did not write salome/mesh.unv (exit code $rc): the lines above say why\"; exit 1; }", q);
        job_add(A.cj, "salome", "SALOME (NETGEN)", cmd, "log.salome", false);
        Str conv; str_init(&conv);
        str_cat(&conv, "ideasUnvToFoam salome/mesh.unv && echo && {\n");
        mesher_type_script(&T.m, &T.stl, &conv);
        str_cat(&conv, "}");
        job_add(A.cj, "ideasUnvToFoam", "ideasUnvToFoam", conv.s, "log.ideasUnvToFoam", false);
        str_free(&conv);
        if (T.m.mesher == MESHER_POLY) {
            char pd[96]; snprintf(pd, sizeof pd, "polyDualMesh %g -overwrite -concaveMultiCells", T.m.featureAngle);
            job_add(A.cj, "polyDualMesh", "polyDualMesh (to polyhedra)", pd, "log.polyDualMesh", false);
        }
    }
    job_add(A.cj, "checkMesh", "checkMesh", "checkMesh", "log.checkMesh", false);
    if (job_start(A.cj, c->s.path, app_bashrc(), GetTime())) ui_toast(TOAST_INFO, "Meshing %s: the Run page shows the log", pl_basename(T.m.stl));
    else ui_toast(TOAST_ERR, "Could not start the mesher");
}

static void open_salome(Case *c)
{
    if (!A.env.salome[0]) { ui_toast(TOAST_ERR, "SALOME is not installed"); return; }
    if (!prepare(c)) return;
    char dir[PL_PATH], qd[PL_PATH + 8], qs[PL_PATH + 8], lp[PL_PATH], sp[PL_PATH], cmd[PL_PATH * 2 + 96];
    pl_join(dir, sizeof dir, c->s.path, "salome");
    pl_join(sp, sizeof sp, dir, "stl_mesh.py");
    pl_join(lp, sizeof lp, dir, ".qc-open-stl.py");
    /* SALOME's GUI runs a script without __file__: set it, then run ours */
    Str py; str_init(&py);
    str_cat(&py, "__file__ = \"");
    for (const char *q = sp; *q; q++) { if (*q == '\\' || *q == '"') str_catf(&py, "\\%c", *q); else str_catf(&py, "%c", *q); }
    str_cat(&py, "\"\nexec(compile(open(__file__).read(), __file__, \"exec\"))\n");
    bool ok = pl_write_file(lp, py.s, strlen(py.s));
    str_free(&py);
    if (!ok) { ui_toast(TOAST_ERR, "Could not write %s", lp); return; }
    pl_shell_quote(qd, sizeof qd, dir);
    pl_shell_quote(qs, sizeof qs, A.env.salome);
    if (!pl_fmt(cmd, sizeof cmd, "cd %s && env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME %s start -g .qc-open-stl.py > /dev/null 2>&1", qd, qs)) {
        ui_toast(TOAST_ERR, "The case path is too long to start SALOME from"); return;
    }
    pl_spawn_detached(cmd);
    ui_toast(TOAST_INFO, "Starting SALOME with the STL mesher's script: the geometry, its groups and the mesh appear in its tree");
}

/* ============================================================================ the form */
static bool num_field(UiId id, Rectangle r, double *v, const char *fmt, double lo, double hi, const char *unit)
{
    char b[64]; snprintf(b, sizeof b, fmt, *v);
    float uw = unit ? ui_text_w(F_MONO, 12, unit) + 12 : 0;
    if (unit) ui_text(F_MONO, 12, unit, r.x + r.width - uw + 6, r.y + (r.height - 12) / 2 - 1, TH.text3);
    Rectangle ir = { r.x, r.y, r.width - uw, r.height };
    if (!ui_input(id, ir, b, sizeof b, NULL, INPUT_MONO | INPUT_NUMERIC)) return false;
    char *e; double d = strtod(b, &e);
    while (e && *e == ' ') e++;
    if (e == b || (e && *e) || d < lo || d > hi) { ui_toast(TOAST_ERR, "Give a number from %g to %g", lo, hi); return false; }
    *v = d;
    T.dirty = true;
    return true;
}

static float row_num(float x, float *y, float w, const char *id, const char *label, const char *hint, double *v, const char *fmt,
                     double lo, double hi, const char *unit, const char *after)
{
    Rectangle cr = prop_row(x, y, w, label, hint, 34);
    float fw = fminf(200, cr.width);
    bool ch = num_field(ui_id(id), (Rectangle){ cr.x, cr.y, fw, 34 }, v, fmt, lo, hi, unit);
    if (after && *after) ui_text_clip(F_REG, 13, after, cr.x + fw + 14, cr.y + 9, cr.width - fw - 14, TH.text3);
    return ch;
}

static const char *axis_word(const float n[3])
{
    static char b[32];
    int k = 0;
    for (int i = 1; i < 3; i++) if (fabsf(n[i]) > fabsf(n[k])) k = i;
    if (fabsf(n[k]) < 0.9f) return "curved / mixed";
    snprintf(b, sizeof b, "faces %c%c", n[k] > 0 ? '+' : '-', "xyz"[k]);
    return b;
}

static void fmt_len(char *b, size_t cap, double v)
{
    if (v >= 1) snprintf(b, cap, "%.4g m", v);
    else if (v >= 1e-3) snprintf(b, cap, "%.4g mm", v * 1e3);
    else snprintf(b, cap, "%.3g µm", v * 1e6);
}

static float geometry_section(Case *c, float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "Geometry", "Any STL, ASCII or binary; an ASCII file's named solids are kept apart. The file is copied into the case "
                                          "(constant/triSurface/geometry.stl, scaled to metres and split) each time the mesh is generated.");
    static char typed[PL_PATH];
    float bw = ui_button_width(ICON_FOLDER_OPEN, "Choose STL...");
    ui_disable(T.picker > 0);
    if (ui_button(ui_id("stl-pick"), (Rectangle){ x, y, bw, 38 }, ICON_FOLDER_OPEN, T.picker > 0 ? "Choosing..." : "Choose STL...", BTN_PRIMARY)) start_picker(false);
    ui_disable(false);
    float ix = x + bw + 12, iw = w - bw - 12 - 90;
    UiId tid = ui_id("stl-path");
    if (!ui_input_focused(tid)) pl_strlcpy(typed, T.m.stl, sizeof typed);
    if (ui_input(tid, (Rectangle){ ix, y, iw, 38 }, typed, sizeof typed, A.env.wsl ? "or a path: /mnt/c/Users/you/Downloads/part.stl" : "or a path to an .stl",
                 INPUT_MONO)) {
        char p[PL_PATH]; pl_expand_home(p, sizeof p, typed); pl_trim_inplace(p);
        /* a Windows path pasted from Explorer */
        if (A.env.wsl && strlen(p) > 2 && p[1] == ':') {
            char q[PL_PATH + 8], cmd[PL_PATH + 40], out[PL_PATH];
            pl_shell_quote(q, sizeof q, p); snprintf(cmd, sizeof cmd, "wslpath -u %s", q);
            if (pl_run_wait(cmd, out, sizeof out) == 0) { pl_trim_inplace(out); pl_strlcpy(p, out, sizeof p); }
        }
        if (pl_is_file(p)) load_stl(p, true); else ui_toast(TOAST_ERR, "No file at %s", p);
    }
    ui_disable(!T.loaded);
    if (ui_button(ui_id("stl-reload"), (Rectangle){ x + w - 80, y, 80, 38 }, ICON_REFRESH_CW, "Reload", BTN_GHOST)) {
        char p[PL_PATH]; pl_strlcpy(p, T.m.stl, sizeof p);
        if (pl_is_file(p)) load_stl(p, false);
    }
    ui_disable(false);
    y += 52;
    if (!T.loaded) {
        const char *t = "No STL yet", *d = "Choose one, or type its path. A closed surface (watertight) meshes inside; an open one, or a body "
                                          "to put in a wind tunnel, meshes outside (Domain below).";
        float ch = callout_height(w, t, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_FOLDER_OPEN, t, d);
        return y + ch + 20 - y0;
    }
    const Stl *s = &T.stl;
    char a[32], b3[32], c3[32], line[400], tri[32];
    fmt_len(a, sizeof a, s->bmax[0] - s->bmin[0]); fmt_len(b3, sizeof b3, s->bmax[1] - s->bmin[1]); fmt_len(c3, sizeof c3, s->bmax[2] - s->bmin[2]);
    snprintf(tri, sizeof tri, "%d", s->nTri);
    pl_fmt_ui(line, sizeof line, "%s  ·  %s triangles (%s)  ·  %s × %s × %s  ·  area %.4g m²%s", pl_basename(T.m.stl), tri, s->binary ? "binary" : "ASCII",
              a, b3, c3, s->area, s->openEdges == 0 && s->nonManifold == 0 ? TextFormat("  ·  volume %.4g m³", s->volume) : "");
    ui_text_clip(F_MONO, 12.5f, line, x, y, w, TH.text2);
    y += 26;
    bool closed = s->openEdges == 0 && s->nonManifold == 0;
    char msg[300];
    if (closed) snprintf(msg, sizeof msg, "Closed (watertight): every edge has two triangles. It can be meshed inside or outside.");
    else snprintf(msg, sizeof msg, "%d open and %d non-manifold edges: not watertight. snappyHexMesh copes with small gaps (smaller than the cells there); "
                  "SALOME needs a closed surface. Fix it in your CAD, or with surfaceCheck and surfaceClean.", s->openEdges, s->nonManifold);
    float ch = callout_height(w, NULL, msg);
    callout((Rectangle){ x, y, w, ch }, closed ? TOAST_OK : TOAST_WARN, closed ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT, NULL, msg);
    y += ch + 14;
    /* scale: the STL's unit */
    Rectangle cr = prop_row(x, &y, w, "Units of the STL", "STL files have no units. CAD usually writes millimetres.", 34);
    static const char *units[] = { "m", "cm", "mm", "inch" };
    static const double uk[] = { 1, 0.01, 0.001, 0.0254 };
    int us = -1;
    for (int i = 0; i < 4; i++) if (fabs(T.m.scale - uk[i]) < 1e-12) us = i;
    int pick = us;
    if (ui_segmented(ui_id("stl-units"), (Rectangle){ cr.x, cr.y, 280, 34 }, units, 4, &pick) && pick != us && pick >= 0) {
        T.m.scale = uk[pick];
        double keepBase = T.m.base;
        stl_set_scale(&T.stl, T.m.scale); stl_split(&T.stl, T.m.splitAngle);
        mesher_apply_names(&T.m, &T.stl);
        mesher_defaults(&T.m, &T.stl);                    /* sizes follow the part's new size */
        (void)keepBase;
        pl_strlcpy(T.m.stl, s->path, sizeof T.m.stl);
        save_cfg();
    }
    double sc = T.m.scale;
    if (num_field(ui_id("stl-scale"), (Rectangle){ cr.x + 296, cr.y, 140, 34 }, &sc, "%g", 1e-9, 1e9, "× ") && sc != T.m.scale) {
        T.m.scale = sc; stl_set_scale(&T.stl, sc); stl_split(&T.stl, T.m.splitAngle); mesher_apply_names(&T.m, &T.stl); save_cfg();
    }
    double ang = T.m.splitAngle;
    char after[96]; snprintf(after, sizeof after, "%d boundar%s", s->nRegions, s->nRegions == 1 ? "y" : "ies");
    if (row_num(x, &y, w, "stl-split", "Split by angle", "Surfaces meeting at a sharper angle become separate boundaries (STAR-CCM+'s split by angle). "
                "0 keeps the file's own solids.", &ang, "%g", 0, 180, "°", after)) {
        T.m.splitAngle = ang; stl_split(&T.stl, ang); mesher_take_names(&T.m, &T.stl); save_cfg();
    }
    (void)c;
    return y - y0;
}

static float boundaries_section(float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "Boundaries", "Each becomes an OpenFOAM patch with this name. Walls get prism layers; inlets and outlets are plain patches.");
    Stl *s = &T.stl;
    static const char *types[BT__N] = { "Wall", "Patch", "Inlet", "Outlet", "Symmetry" };
    for (int i = 0; i < s->nRegions; i++) {
        StlRegion *R = &s->reg[i];
        float rh = 44;
        if (ui_culled(y, y + rh)) { y += rh; continue; }
        ui_push_id(ui_idn(ui_id("stl-b"), i));
        char nm[64]; pl_strlcpy(nm, R->name, sizeof nm);
        if (ui_input(ui_id("name"), (Rectangle){ x, y, 240, 34 }, nm, sizeof nm, "name", INPUT_MONO)) {
            pl_trim_inplace(nm);
            bool okName = nm[0] != 0;
            for (char *q = nm; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_')) okName = false;
            for (int k = 0; k < s->nRegions && okName; k++) if (k != i && !strcmp(s->reg[k].name, nm)) okName = false;
            if (okName) { pl_strlcpy(R->name, nm, sizeof R->name); save_cfg(); }
            else ui_toast(TOAST_ERR, "A boundary name is letters, digits and _, and each one different");
        }
        int ty = R->type;
        if (ui_select(ui_id("type"), (Rectangle){ x + 252, y, 140, 34 }, types, BT__N, &ty)) { R->type = ty; save_cfg(); }
        char info[160]; pl_fmt_ui(info, sizeof info, "%d triangles  ·  %.3g m²  ·  %s", R->nTri, R->area, axis_word(R->n));
        ui_text_clip(F_REG, 13, info, x + 408, y + 9, w - 408, TH.text3);
        ui_pop_id();
        y += rh;
    }
    return y - y0 + 10;
}

static float domain_section(float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "Domain", "Internal: the fluid is inside the STL (a duct, a manifold, a room). External: the STL is a body in a "
                                        "wind tunnel along +x with z up; the tunnel's size is in body lengths.");
    static const char *dm[] = { "Internal flow", "External flow" };
    int d = T.m.domain;
    if (ui_segmented(ui_id("stl-domain"), (Rectangle){ x, y, 320, 34 }, dm, 2, &d) && d != T.m.domain) {
        T.m.domain = d;
        char keep[PL_PATH]; pl_strlcpy(keep, T.m.stl, sizeof keep);
        double sc = T.m.scale; int mz = T.m.mesher;
        mesher_defaults(&T.m, &T.stl);                    /* the sizes suit each kind */
        T.m.domain = d; T.m.scale = sc; T.m.mesher = mz; pl_strlcpy(T.m.stl, keep, sizeof T.m.stl);
        save_cfg();
    }
    y += 50;
    if (T.m.domain == DOMAIN_EXTERNAL) {
        double L = 0; for (int k = 0; k < 3; k++) L = fmax(L, T.stl.bmax[k] - T.stl.bmin[k]);
        char a[64];
        snprintf(a, sizeof a, "body length L = %.4g m", L);
        if (row_num(x, &y, w, "stl-up", "Upstream", "Inlet distance ahead of the body.", &T.m.up, "%g", 0, 100, "L", a)) save_cfg();
        if (row_num(x, &y, w, "stl-down", "Downstream", "Room for the wake.", &T.m.down, "%g", 0, 200, "L", NULL)) save_cfg();
        if (row_num(x, &y, w, "stl-side", "Sides", NULL, &T.m.side, "%g", 0, 100, "L", NULL)) save_cfg();
        if (row_num(x, &y, w, "stl-top", "Above", NULL, &T.m.top, "%g", 0, 100, "L", NULL)) save_cfg();
        Rectangle cr = prop_row(x, &y, w, "On the ground", "The floor touches the body's lowest point and is a wall (a car, a building).", 24);
        if (ui_switch(ui_id("stl-ground"), cr.x, cr.y + 2, &T.m.ground)) save_cfg();
    }
    return y - y0;
}

static float mesher_section(float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "Mesher", "The meshing models, as in STAR-CCM+: trimmed cells (snappyHexMesh, runs here), or SALOME's tetrahedra, "
                                        "polyhedra (NETGEN tetrahedra joined by OpenFOAM's polyDualMesh) or body-fitted hexahedra.");
    int ms = T.m.mesher;
    if (ui_select(ui_id("stl-mesher"), (Rectangle){ x, y, 360, 36 }, MESHER_NAME, MESHER__N, &ms) && ms != T.m.mesher) { T.m.mesher = ms; save_cfg(); }
    if (salome_mesher()) {
        float sx = x + 380;
        if (T.m.mesher == MESHER_BODYFIT) ui_disable(true);
        if (ui_switch(ui_id("stl-remesh"), sx, y + 8, &T.m.remesh)) save_cfg();
        ui_disable(false);
        ui_text(F_MED, 13, "Remesh surface", sx + 46, y + 9, TH.text2);
        ui_tooltip((Rectangle){ sx, y, 180, 36 }, "Off: the STL's own triangles are the boundary mesh (fast, any size). On: SALOME builds CAD from it and "
                   "NETGEN remeshes the surface at the target size (best for CAD-made STL with flat faces).");
    }
    y += 50;
    if (salome_mesher() && !A.env.salome[0]) {
        const char *t = "SALOME is not installed";
        const char *d = "Download SALOME 9.14 for Ubuntu 24.04 from salome-platform.org (Downloads) into your Windows Downloads folder, then run "
                        "bash ~/q-continuum_alpha2.0/tools/install_salome.sh and restart Q-Continuum. Until then, the trimmed mesher works.";
        float ch = callout_height(w, t, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, t, d);
        y += ch + 16;
    } else if (salome_mesher()) {
        char t[PL_PATH + 64]; pl_fmt_ui(t, sizeof t, "SALOME %s  ·  %s", A.env.salomeVersion, A.env.salome);
        ui_text_clip(F_MONO, 12.5f, t, x, y - 6, w, TH.text3);
        y += 18;
    }
    return y - y0;
}

static float sizes_section(float x, float y, float w)
{
    float y0 = y;
    MeshCfg *m = &T.m;
    y += section_head(x, y, w, "Reference values", "Everything is sized from the base size, as in STAR-CCM+. The trimmed mesher halves cells "
                                                  "per refinement level, so its percentages round to 100, 50, 25, 12.5 ...");
    char a[96], l1[32], l2[32];
    if (row_num(x, &y, w, "stl-base", "Base size", "The largest cell, away from the surface.", &m->base, "%.4g", 1e-9, 1e6, "m", NULL)) save_cfg();
    fmt_len(l1, sizeof l1, m->base * m->targetPct / 100);
    snprintf(a, sizeof a, "= %s", l1);
    if (row_num(x, &y, w, "stl-target", "Target surface size", "The cell size on the surface.", &m->targetPct, "%g", 0.01, 1000, "%", a)) save_cfg();
    fmt_len(l2, sizeof l2, m->base * m->minPct / 100);
    snprintf(a, sizeof a, "= %s", l2);
    if (row_num(x, &y, w, "stl-min", "Minimum surface size", "Reached on tight curves and sharp edges.", &m->minPct, "%g", 0.01, 1000, "%", a)) save_cfg();
    if (row_num(x, &y, w, "stl-curv", "Surface curvature", "Cells around a full circle on curved surfaces.", &m->curvature, "%g", 4, 360, "pts/circle", NULL)) save_cfg();
    if (row_num(x, &y, w, "stl-sgrow", "Surface growth rate", "How fast cells grow along the surface (SALOME).", &m->surfGrowth, "%g", 1.01, 3, NULL, NULL)) save_cfg();
    if (row_num(x, &y, w, "stl-vgrow", "Volume growth rate", "How fast cells grow away from the surface: lower is smoother and more cells.", &m->volGrowth, "%g", 1.01, 3, NULL, NULL)) save_cfg();
    if (row_num(x, &y, w, "stl-feat", "Feature angle", "Edges sharper than this are kept crisp (snapped to, not smoothed over).", &m->featureAngle, "%g", 1, 180, "°", NULL)) save_cfg();
    return y - y0;
}

static float prism_section(float x, float y, float w)
{
    float y0 = y;
    MeshCfg *m = &T.m;
    y += section_head(x, y, w, "Prism layers", "Thin cells stacked on the walls, for the boundary layer.");
    Rectangle cr = prop_row(x, &y, w, "Prism layer mesher", m->mesher == MESHER_BODYFIT ? "Not with body-fitted hexahedra." : NULL, 24);
    if (ui_switch(ui_id("stl-prism"), cr.x, cr.y + 2, &m->prism)) save_cfg();
    if (m->prism && m->mesher != MESHER_BODYFIT) {
        double n = m->layers;
        if (row_num(x, &y, w, "stl-nl", "Number of prism layers", NULL, &n, "%g", 1, 50, NULL, NULL)) { m->layers = (int)n; save_cfg(); }
        double T0 = m->base * m->prismPct / 100;
        char a[96], l[32]; fmt_len(l, sizeof l, T0); snprintf(a, sizeof a, "= %s", l);
        if (row_num(x, &y, w, "stl-pt", "Prism layer total thickness", "Of the base size.", &m->prismPct, "%g", 0.1, 500, "%", a)) save_cfg();
        double r = m->stretch, first = r == 1 ? T0 / m->layers : T0 * (r - 1) / (pow(r, m->layers) - 1);
        fmt_len(l, sizeof l, first); snprintf(a, sizeof a, "first layer %s", l);
        if (row_num(x, &y, w, "stl-ps", "Prism layer stretching", "Each layer this much thicker than the one below.", &m->stretch, "%g", 1, 3, NULL, a)) save_cfg();
    }
    return y - y0;
}

static float zones_section(float x, float y, float w)
{
    float y0 = y;
    MeshCfg *m = &T.m;
    y += section_head(x, y, w, "Volumetric controls", "Finer cells inside a box, sphere or cylinder: a wake, a gap, a jet. Coordinates in metres.");
    static const char *kinds[] = { "Box", "Sphere", "Cylinder" };
    static const char *const AX[3] = { "x", "y", "z" };
    int del = -1;
    for (int i = 0; i < m->nZones; i++) {
        MeshZone *z = &m->zone[i];
        ui_push_id(ui_idn(ui_id("stl-z"), i));
        Rectangle box = { x, y, w, z->kind == ZONE_SPHERE ? 92 : 132 };
        ui_rect(box, 8, TH.surface2); ui_rect_lines(box, 8, 1, TH.lineSoft);
        int k = z->kind;
        if (ui_select(ui_id("kind"), (Rectangle){ x + 12, y + 12, 130, 32 }, kinds, 3, &k) && k != z->kind) { z->kind = k; save_cfg(); }
        double sz = z->sizePct;
        ui_text(F_REG, 13, "Size", x + 160, y + 20, TH.text2);
        char l[32], a[48]; fmt_len(l, sizeof l, m->base * z->sizePct / 100); snprintf(a, sizeof a, "= %s", l);
        if (num_field(ui_id("size"), (Rectangle){ x + 200, y + 12, 110, 32 }, &sz, "%g", 0.01, 1000, "%")) { z->sizePct = sz; save_cfg(); }
        ui_text(F_REG, 13, a, x + 320, y + 20, TH.text3);
        if (ui_button(ui_id("del"), (Rectangle){ x + w - 44, y + 12, 32, 32 }, ICON_TRASH_2, NULL, BTN_ICON)) del = i;
        const char *la = z->kind == ZONE_BOX ? "Min" : z->kind == ZONE_SPHERE ? "Centre" : "End 1";
        float fy = y + 52;
        ui_text(F_REG, 13, la, x + 12, fy + 8, TH.text2);
        for (int c = 0; c < 3; c++)
            if (num_field(ui_idn(ui_id("a"), c), (Rectangle){ x + 70 + c * 130, fy, 120, 32 }, &z->a[c], "%.4g", -1e9, 1e9, AX[c])) save_cfg();
        if (z->kind == ZONE_SPHERE || z->kind == ZONE_CYLINDER) {
            ui_text(F_REG, 13, "Radius", x + 470, fy + 8, TH.text2);
            if (num_field(ui_id("r"), (Rectangle){ x + 530, fy, 120, 32 }, &z->r, "%.4g", 1e-9, 1e9, "m")) save_cfg();
        }
        if (z->kind != ZONE_SPHERE) {
            fy += 40;
            ui_text(F_REG, 13, z->kind == ZONE_BOX ? "Max" : "End 2", x + 12, fy + 8, TH.text2);
            for (int c = 0; c < 3; c++)
                if (num_field(ui_idn(ui_id("b"), c), (Rectangle){ x + 70 + c * 130, fy, 120, 32 }, &z->b[c], "%.4g", -1e9, 1e9, AX[c])) save_cfg();
        }
        ui_pop_id();
        y += box.height + 10;
    }
    if (del >= 0) {
        for (int i = del; i < m->nZones - 1; i++) m->zone[i] = m->zone[i + 1];
        m->nZones--; save_cfg();
    }
    ui_disable(m->nZones >= STL_MAX_ZONES);
    float bx = x;
    for (int k = 0; k < 3; k++) {
        char lab[32]; snprintf(lab, sizeof lab, "Add %s", k == 0 ? "box" : k == 1 ? "sphere" : "cylinder");
        float bw = ui_button_width(ICON_PLUS, lab);
        if (ui_button(ui_idn(ui_id("stl-zadd"), k), (Rectangle){ bx, y, bw, 34 }, ICON_PLUS, lab, BTN_SECONDARY)) {
            MeshZone *z = &m->zone[m->nZones++];
            memset(z, 0, sizeof *z);
            z->kind = k; z->sizePct = 50;
            const Stl *s = &T.stl;
            double L = 0, c[3];
            for (int q = 0; q < 3; q++) { L = fmax(L, s->bmax[q] - s->bmin[q]); c[q] = (s->bmin[q] + s->bmax[q]) / 2; }
            if (k == ZONE_BOX) {                           /* external: the wake behind the body; internal: the middle third */
                for (int q = 0; q < 3; q++) {
                    double h = (s->bmax[q] - s->bmin[q]) * (m->domain == DOMAIN_EXTERNAL ? 0.75 : 0.17);
                    z->a[q] = c[q] - h; z->b[q] = c[q] + h;
                }
                if (m->domain == DOMAIN_EXTERNAL) { z->a[0] = s->bmin[0] - 0.2 * L; z->b[0] = s->bmax[0] + 1.5 * L; }
            } else if (k == ZONE_SPHERE) { for (int q = 0; q < 3; q++) z->a[q] = c[q]; z->r = L * 0.3; }
            else { for (int q = 0; q < 3; q++) z->a[q] = z->b[q] = c[q]; z->a[0] = s->bmin[0]; z->b[0] = s->bmax[0]; z->r = L * 0.15; }
            save_cfg();
        }
        bx += bw + 10;
    }
    ui_disable(false);
    y += 50;
    return y - y0;
}

float page_stlmesh(Rectangle r, float x, float y, float w)
{
    Case *c = A.cs;
    float y0 = y;
    poll_picker();
    sync_case(c);
    bool running = A.cj->active;
    y += geometry_section(c, x, y, w);
    if (!T.loaded) return y - y0;
    y += boundaries_section(x, y + 6, w) + 6;
    y += domain_section(x, y + 6, w) + 6;
    y += mesher_section(x, y + 6, w) + 6;
    y += sizes_section(x, y + 6, w) + 6;
    y += prism_section(x, y + 6, w) + 6;
    y += zones_section(x, y + 6, w) + 6;

    /* the estimate and the buttons */
    double n = mesher_estimate_cells(&T.m, &T.stl);
    char est[160];
    if (n >= 1e6) pl_fmt_ui(est, sizeof est, "About %.1f million cells (a rough estimate)", n / 1e6);
    else pl_fmt_ui(est, sizeof est, "About %.0f thousand cells (a rough estimate)", fmax(1, n / 1e3));
    ui_divider(x, y, w);
    y += 18;
    ui_text(F_SEMI, 16, est, x, y, n > 2e7 ? TH.warn : TH.text);
    if (n > 2e7) ui_text(F_REG, 13, "That is a lot for one machine: a larger base size or target size brings it down.", x, y + 24, TH.text3);
    y += 50;
    float bx = x, bw;
    ui_disable(running);
    bw = ui_button_width(ICON_PLAY, "Generate mesh");
    if (ui_button(ui_id("stl-gen"), (Rectangle){ bx, y, bw, 42 }, ICON_PLAY, "Generate mesh", BTN_PRIMARY)) generate(c);
    bx += bw + 10;
    bw = ui_button_width(ICON_FILE_TEXT, "Write files only");
    if (ui_button(ui_id("stl-write"), (Rectangle){ bx, y, bw, 42 }, ICON_FILE_TEXT, "Write files only", BTN_SECONDARY) && prepare(c))
        ui_toast(TOAST_OK, salome_mesher() ? "Wrote salome/stl_mesh.py, mesher/mesher.ini and the STL" : "Wrote the snappyHexMesh, blockMesh and feature dictionaries");
    bx += bw + 10;
    ui_disable(running || !A.env.salome[0]);
    bw = ui_button_width(ICON_EXTERNAL_LINK, "Open in SALOME");
    if (ui_button(ui_id("stl-salome"), (Rectangle){ bx, y, bw, 42 }, ICON_EXTERNAL_LINK, "Open in SALOME", BTN_GHOST)) open_salome(c);
    ui_disable(false);
    y += 56;
    const char *how = salome_mesher()
        ? "Generate runs salome/stl_mesh.py in SALOME without its window, converts salome/mesh.unv with ideasUnvToFoam, sets the boundary types"
          "%s and runs checkMesh. Open in SALOME runs the same script in SALOME's window, to look at the groups and the mesh."
        : "Generate writes the dictionaries (the ones there before are kept as .orig) and runs surfaceFeatureExtract, blockMesh for the "
          "background, snappyHexMesh and checkMesh. Open in SALOME meshes the same STL with SALOME's script instead, in its window.%s";
    char hb[600]; pl_fmt_ui(hb, sizeof hb, how, T.m.mesher == MESHER_POLY ? ", turns the tetrahedra into polyhedra (polyDualMesh)" : "");
    y += ui_text_wrap(F_REG, 13, hb, (Rectangle){ x, y, w, 80 }, 19, TH.text3, true) + 16;
    if (running && !strcmp(A.cj->title, "Mesh")) {
        char t[200]; pl_fmt_ui(t, sizeof t, "Step %d of %d: %s. The Run page shows the log.", A.cj->cur + 1, A.cj->n,
                               A.cj->steps[A.cj->cur < A.cj->n ? A.cj->cur : 0].label);
        float ch = callout_height(w, NULL, t);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_LOADER_CIRCLE, NULL, t);
        y += ch + 16;
    }
    if (c->s.meshed) { y += 10; y += page_mesh_patches(x, y, w); }
    (void)r;
    return y - y0;
}
