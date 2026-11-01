/* page_stlmesh.c -- the Mesh page's STL mesher tab (user request, 2026-10-23): choose any STL, see what it is (size,
 * closed or not, its boundaries split by angle), set the mesh up the way STAR-CCM+'s automated mesher does, and mesh it
 * with snappyHexMesh here or with SALOME (NETGEN tetrahedra, polyhedra through polyDualMesh, Body Fitting hexahedra).
 * The part is shown in 3-D, a colour a boundary: click a face to pick its boundary, rename it, say what it is (a wall,
 * an inlet ...) and give it its conditions (page_setup.c's editor) right there (user request, 2026-10-24).
 * "Mesh an STL" on the Cases page makes a case for a bare STL and opens this tab with it. The settings live in
 * <case>/mesher/mesher.ini (stlmesh.c), which SALOME's script reads too.
 * The tab is a flow (user request, 2026-10-26): Geometry > Flow > Boundaries > Mesh settings > Generate, one step open at
 * a time, each the next one's way in; a step done folds to a line saying what was chosen. How far a case has got is in
 * <case>/mesher/steps. Everything here belongs to the case it was loaded for (T.casePath): a case switch drops it all,
 * the file dialog's answer included when the case changed while it was open.
 */
#define _GNU_SOURCE
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "app.h"
#include "orbit.h"
#include "rlgl.h"
#include "simsetup.h"
#include "stlmesh.h"

static struct {
    char   casePath[PL_PATH];   /* the case S and M belong to          */
    Stl    stl;
    MeshCfg m;
    bool   loaded, dirty;
    pid_t  picker;              /* the file dialog, while it is open   */
    char   pickOut[PL_PATH];
    bool   pickNewCase;         /* the pick makes a new case (Cases page) */
    char   pickCase[PL_PATH];   /* else the case it was opened for     */
    int    sel;                 /* the boundary picked (-1 none)       */
    int    reached, open;       /* the flow: the furthest step got to, the one unfolded (ST_*) */
    bool   adv;                 /* Mesh settings: the advanced ones shown */
    char   typed[PL_PATH];      /* the path field, for casePath only   */
    unsigned gen;               /* bumped on each case switch          */
    double removeArm;           /* Remove STL: the first click's time  */
    int    scrollTo;            /* a step was opened: bring its head up, a frame on (once the one before has folded) */
} T = { .picker = -1, .sel = -1 };

enum { ST_GEOM, ST_FLOW, ST_BOUND, ST_MESH, ST_GEN, ST__N };

/* ============================================================================ the flow's progress: mesher/steps */
static void steps_save(void)
{
    if (!T.casePath[0] || !T.loaded) return;
    char p[PL_PATH], d[PL_PATH], b[32];
    pl_join(d, sizeof d, T.casePath, "mesher");
    pl_join(p, sizeof p, d, "steps");
    snprintf(b, sizeof b, "%d %d\n", T.reached, T.open);
    char *old = pl_read_file(p, NULL);
    bool same = old && !strcmp(old, b);
    free(old);
    if (!same && pl_mkdirs(d)) pl_write_file(p, b, strlen(b));
}

/* a case meshed from an STL before there was a flow opens with every step there */
static void steps_load(const Case *c)
{
    T.reached = T.open = ST_GEOM;
    if (!T.loaded) return;
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, "mesher/steps");
    char *b = pl_read_file(p, NULL);
    if (!b || sscanf(b, "%d %d", &T.reached, &T.open) != 2) { T.reached = ST_GEN; T.open = c->s.meshed ? ST_GEN : ST_MESH; }
    free(b);
    if (T.reached < 0 || T.reached >= ST__N) T.reached = ST_GEN;
    if (T.open < 0 || T.open > T.reached) T.open = T.reached;
}

static void step_open(int k)
{
    if (k < 0 || k >= ST__N) return;
    if (k > T.reached) T.reached = k;
    T.open = k;
    T.scrollTo = 2;
    steps_save();
}

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
        T.sel = -1;
    } else mesher_apply_names(&T.m, &T.stl);
    T.loaded = true;
    save_cfg();
    if (fresh) { T.reached = T.open = ST_GEOM; steps_save(); }   /* another part: its settings are new, so is the flow */
    return true;
}

static void view_forget(void);

/* the case the page shows: another one drops everything of the last (its STL, settings, picked boundary, a half-typed
   path or name: an input still focused would otherwise commit the old case's text into this one) */
static void sync_case(Case *c)
{
    if (!c || !strcmp(T.casePath, c->s.path)) return;
    stl_free(&T.stl);
    memset(&T.m, 0, sizeof T.m);
    T.loaded = T.dirty = false;
    T.sel = -1;
    T.adv = false; T.removeArm = 0; T.scrollTo = 0;
    T.typed[0] = 0;
    T.gen++;
    ui_drop_focus();
    view_forget();
    pl_strlcpy(T.casePath, c->s.path, sizeof T.casePath);
    if (mesher_load(c->s.path, &T.m) && T.m.stl[0]) {
        char p[PL_PATH]; pl_strlcpy(p, T.m.stl, sizeof p);
        if (!pl_is_file(p)) pl_join(p, sizeof p, c->s.path, "constant/triSurface/geometry.stl");   /* the original moved: our copy */
        if (pl_is_file(p)) load_stl(p, false);
    }
    steps_load(c);
}

/* Remove STL: the case goes back to having none (its mesh, if any, stays) */
static void remove_stl(Case *c)
{
    char q[PL_PATH + 8], cmd[PL_PATH * 2 + 300];
    pl_shell_quote(q, sizeof q, c->s.path);
    if (!pl_fmt(cmd, sizeof cmd, "cd %s && rm -rf mesher constant/triSurface/geometry.stl constant/triSurface/geometry.eMesh "
                                 "constant/triSurface/regions salome/stl_mesh.py", q) || pl_run_wait(cmd, NULL, 0) != 0) {
        ui_toast(TOAST_ERR, "Could not remove the STL's files from %s", c->s.path); return;
    }
    T.casePath[0] = 0;                                    /* read again: nothing there now */
    sync_case(c);
    case_rescan(c);
    ui_toast(TOAST_OK, "Removed the STL and its mesher settings from %s", c->s.name);
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
    pl_strlcpy(T.pickCase, A.cs && !newCase ? A.cs->s.path : "", sizeof T.pickCase);
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
    char sp[PL_PATH]; pl_join(sp, sizeof sp, dir, "mesher/steps");
    pl_write_file(sp, "0 0\n", 4);                             /* a new part: its flow starts at Geometry */
    app_rescan_cases(true);
    app_open_case(dir, PG_MESH);
    A.meshTab = 3;
    pl_strlcpy(T.casePath, "", sizeof T.casePath);            /* sync_case loads it from the new folder */
    if (A.cs && !strcmp(A.cs->s.path, dir)) sync_case(A.cs);
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
        else if (!A.cs || strcmp(A.cs->s.path, T.pickCase))   /* chosen for a case that is no longer open: not this one's */
            ui_toast(TOAST_INFO, "Not loaded: %s was chosen for %s, which is no longer open", pl_basename(p), pl_basename(T.pickCase));
        else { sync_case(A.cs); load_stl(p, true); }
    }
    free(p);
}

void stlmesh_new_case(void) { start_picker(true); }
void stlmesh_choose(void) { start_picker(false); }
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

static void generate(Case *c);
static void generate_after_clean(void) { if (A.cs) generate(A.cs); }

static void generate(Case *c)
{
    if (!app_require_foam() || !app_can_start()) return;
    case_rescan(c);
    if (case_has_results(&c->s)) {                        /* fields on the old mesh: they'd stop the run on the new one */
        app_clean_then("The results were computed on the mesh that is about to be replaced, so they go first.", generate_after_clean);
        return;
    }
    if (salome_mesher() && !A.env.salome[0]) { ui_toast(TOAST_ERR, "SALOME is not installed: see the note under Mesher"); return; }
    if (!prepare(c)) return;
    job_reset(A.cj, "Mesh");
    job_add(A.cj, "clear", "Clear the old mesh", "rm -rf constant/polyMesh 0/polyMesh; echo cleared", "log.meshClear", false);
    if (!salome_mesher()) {
        job_add(A.cj, "surfaceFeatureExtract", "surfaceFeatureExtract", "surfaceFeatureExtract", "log.surfaceFeatureExtract", false);
        job_add(A.cj, "blockMesh", "blockMesh (background)", "blockMesh", "log.blockMesh", false);
        job_add(A.cj, "snappyHexMesh", "snappyHexMesh", SNAPPY_CMD, "log.snappyHexMesh", false);
        if (T.m.twoD) {                                   /* the slab's front face, one cell thick; front and back empty */
            Str ex; str_init(&ex);
            mesher_2d_script(&T.m, &T.stl, &ex);
            job_add(A.cj, "extrudeMesh", "extrudeMesh (to 2-D)", ex.s, "log.extrudeMesh", false);
            str_free(&ex);
        }
    } else {
        char q[PL_PATH + 8], cmd[PL_PATH + 600];
        pl_shell_quote(q, sizeof q, A.env.salome);
        snprintf(cmd, sizeof cmd,
                 "cd salome && rm -rf mesh.unv polyMesh && env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME %s -t --shutdown-servers=1 stl_mesh.py\n"
                 "rc=$?; [ -f mesh.unv ] || [ -d polyMesh ] || { echo \"Q-Continuum: SALOME did not write salome/mesh.unv (exit code $rc): the lines above say why\"; exit 1; }", q);
        job_add(A.cj, "salome", "SALOME (NETGEN)", cmd, "log.salome", false);
        Str conv; str_init(&conv);
        /* a mesh with polyhedra (Body Fitting's cut cells) comes as salome/polyMesh: UNV cannot hold them */
        str_cat(&conv, "if [ -d salome/polyMesh ]; then mkdir -p constant && cp -r salome/polyMesh constant/ && echo \"Copied salome/polyMesh\"; "
                       "else ideasUnvToFoam salome/mesh.unv; fi && echo && {\n");
        mesher_type_script(&T.m, &T.stl, &conv);
        str_cat(&conv, "}");
        job_add(A.cj, "ideasUnvToFoam", "ideasUnvToFoam", conv.s, "log.ideasUnvToFoam", false);
        str_free(&conv);
        if (T.m.mesher == MESHER_POLY && !T.m.twoD) {          /* 2-D: its triangles stay (polyhedra would span the depth) */
            char pd[96]; snprintf(pd, sizeof pd, "polyDualMesh %g -overwrite -concaveMultiCells", T.m.featureAngle);
            job_add(A.cj, "polyDualMesh", "polyDualMesh (to polyhedra)", pd, "log.polyDualMesh", false);
        }
    }
    job_add(A.cj, "checkMesh", "checkMesh", "checkMesh", "log.checkMesh", false);
    if (job_start(A.cj, c->s.path, app_bashrc(), GetTime())) ui_toast(TOAST_INFO, "Meshing %s: the Run page shows the log", pl_basename(T.m.stl));
    else ui_toast(TOAST_ERR, "Could not start the mesher");
}

void stlmesh_generate(Case *c) { sync_case(c); if (T.loaded) step_open(ST_GEN); generate(c); }   /* the step bar's: the flow is at its end */

void stlmesh_focus(Case *c, int step)
{
    sync_case(c);
    if (!T.loaded) step = ST_GEOM;
    if (step > T.reached) step = T.reached;              /* not past where the case has got */
    step_open(step);
}

int stlmesh_mesh_current(Case *c)
{
    sync_case(c);
    if (!T.loaded) return -1;
    MeshCfg mm = T.m;
    mesher_take_names(&mm, &T.stl);
    return mesher_mesh_current(c->s.path, &mm);
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
    if (after && *after) {
        if (ui_text_w(F_REG, 13, after) <= cr.width - fw - 14) ui_text(F_REG, 13, after, cr.x + fw + 14, cr.y + 9, TH.text3);
        else *y += ui_text_wrap(F_REG, 13, after, (Rectangle){ cr.x, cr.y + 40, cr.width, 80 }, 19, TH.text3, true) + 4;   /* too long beside it: under it */
    }
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

/* the part's thickness where thin (stl_thickness), worked out once a geometry: loaded, scaled */
static double part_thickness(void)
{
    static const float *tri; static int n; static double sc, was; static unsigned gen;
    static float bb[6];
    const Stl *s = &T.stl;
    if (!T.loaded) return 0;
    float now[6] = { s->bmin[0], s->bmin[1], s->bmin[2], s->bmax[0], s->bmax[1], s->bmax[2] };
    if (tri != s->tri || n != s->nTri || sc != s->scale || gen != T.gen || memcmp(bb, now, sizeof bb)) {
        tri = s->tri; n = s->nTri; sc = s->scale; gen = T.gen; memcpy(bb, now, sizeof bb);
        was = stl_thickness(s);
    }
    return was;
}

static void fmt_len(char *b, size_t cap, double v)
{
    if (v >= 1) snprintf(b, cap, "%.4g m", v);
    else if (v >= 1e-3) snprintf(b, cap, "%.4g mm", v * 1e3);
    else snprintf(b, cap, "%.3g µm", v * 1e6);
}

static void fmt_cells(char *b, size_t cap, double n);

/* a small note under a step's controls */
static float note(float x, float y, float w, const char *t)
{
    return ui_text_wrap(F_REG, 13, t, (Rectangle){ x, y, w, 200 }, 19, TH.text3, true) + 12;
}

/* ---------------------------------------------------------------------------- 1 Geometry: the STL, its units, its boundaries */
static float geometry_step(Case *c, float x, float y, float w)
{
    float y0 = y;
    float bw = ui_button_width(ICON_FOLDER_OPEN, "Choose STL...");
    ui_disable(T.picker > 0);
    if (ui_button(ui_id("stl-pick"), (Rectangle){ x, y, bw, 38 }, ICON_FOLDER_OPEN, T.picker > 0 ? "Choosing..." : "Choose STL...", BTN_PRIMARY)) start_picker(false);
    ui_disable(false);
    float ix = x + bw + 12, iw = w - bw - 12 - (T.loaded ? 90 : 0);
    UiId tid = ui_id("stl-path");
    if (!ui_input_focused(tid)) pl_strlcpy(T.typed, T.m.stl, sizeof T.typed);
    unsigned gen = T.gen;
    if (ui_input(tid, (Rectangle){ ix, y, iw, 38 }, T.typed, sizeof T.typed, A.env.wsl ? "or paste a path: /mnt/c/Users/you/Downloads/part.stl, or C:\\..." : "or a path to an .stl",
                 INPUT_MONO) && gen == T.gen && T.typed[0]) {
        char p[PL_PATH]; pl_expand_home(p, sizeof p, T.typed); pl_trim_inplace(p);
        /* a Windows path pasted from Explorer (with its quotes, as "Copy as path" gives it) */
        size_t n = strlen(p);
        if (n >= 2 && p[0] == '"' && p[n - 1] == '"') { memmove(p, p + 1, n - 2); p[n - 2] = 0; }
        if (A.env.wsl && strlen(p) > 2 && p[1] == ':') {
            char q[PL_PATH + 8], cmd[PL_PATH + 40], out[PL_PATH];
            pl_shell_quote(q, sizeof q, p); snprintf(cmd, sizeof cmd, "wslpath -u %s", q);
            if (pl_run_wait(cmd, out, sizeof out) == 0) { pl_trim_inplace(out); pl_strlcpy(p, out, sizeof p); }
        }
        if (strcmp(p, T.m.stl) || !T.loaded) { if (pl_is_file(p)) load_stl(p, true); else ui_toast(TOAST_ERR, "No file at %s", p); }
    }
    if (T.loaded && ui_button(ui_id("stl-reload"), (Rectangle){ x + w - 80, y, 80, 38 }, ICON_REFRESH_CW, "Reload", BTN_GHOST)) {
        char p[PL_PATH]; pl_strlcpy(p, T.m.stl, sizeof p);
        if (pl_is_file(p)) load_stl(p, false); else ui_toast(TOAST_ERR, "%s is gone: choose the STL again", p);
    }
    y += 52;
    if (!T.loaded) {
        const char *t = "Start with the part", *d = "Choose an STL (ASCII or binary), or paste its path. Next you say whether the fluid is inside it "
                                                     "(a duct, a manifold) or around it (a wing, a car), then name its boundaries, then mesh it.";
        float ch = callout_height(w, t, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_FOLDER_OPEN, t, d);
        return y + ch + 8 - y0;
    }
    const Stl *s = &T.stl;
    char a[32], b3[32], c3[32], line[400], tri[32];
    fmt_len(a, sizeof a, s->bmax[0] - s->bmin[0]); fmt_len(b3, sizeof b3, s->bmax[1] - s->bmin[1]); fmt_len(c3, sizeof c3, s->bmax[2] - s->bmin[2]);
    snprintf(tri, sizeof tri, "%d", s->nTri);
    char th[32] = ""; double thick = part_thickness();
    if (thick > 0) fmt_len(th, sizeof th, thick);
    pl_fmt_ui(line, sizeof line, "%s  ·  %s triangles (%s)  ·  %s × %s × %s  ·  area %.4g m²%s%s%s", pl_basename(T.m.stl), tri, s->binary ? "binary" : "ASCII",
              a, b3, c3, s->area, s->openEdges == 0 && s->nonManifold == 0 ? TextFormat("  ·  volume %.4g m³", s->volume) : "",
              th[0] ? "  ·  thinnest about " : "", th);
    y += ui_text_wrap(F_MONO, 12.5f, line, (Rectangle){ x, y, w, 80 }, 19, TH.text2, true) + 8;
    bool closed = s->openEdges == 0 && s->nonManifold == 0;
    char msg[300];
    if (closed) snprintf(msg, sizeof msg, "Closed (watertight): every edge has two triangles. It can be meshed inside or outside.");
    else snprintf(msg, sizeof msg, "%d open and %d non-manifold edges: not watertight. snappyHexMesh copes with small gaps (smaller than the cells there); "
                  "SALOME needs a closed surface. Fix it in your CAD, or with surfaceCheck and surfaceClean.", s->openEdges, s->nonManifold);
    float ch = callout_height(w, NULL, msg);
    callout((Rectangle){ x, y, w, ch }, closed ? TOAST_OK : TOAST_WARN, closed ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT, NULL, msg);
    y += ch + 14;
    /* scale: the STL's unit, checked against the size it gives */
    Rectangle cr = prop_row(x, &y, w, "Units of the STL", "STL files have no units; CAD usually writes millimetres. Check the size above looks right.", 34);
    static const char *units[] = { "m", "cm", "mm", "inch" };
    static const double uk[] = { 1, 0.01, 0.001, 0.0254 };
    int us = -1;
    for (int i = 0; i < 4; i++) if (fabs(T.m.scale - uk[i]) < 1e-12) us = i;
    int pick = us;
    float segW = fminf(280, cr.width - 110), fx = cr.x + segW + 12, fwd = fminf(140, cr.x + cr.width - fx);
    if (ui_segmented(ui_id("stl-units"), (Rectangle){ cr.x, cr.y, segW, 34 }, units, 4, &pick) && pick != us && pick >= 0) {
        T.m.scale = uk[pick];
        stl_set_scale(&T.stl, T.m.scale); stl_split(&T.stl, T.m.splitAngle);
        mesher_apply_names(&T.m, &T.stl);
        mesher_defaults(&T.m, &T.stl);                    /* sizes follow the part's new size */
        pl_strlcpy(T.m.stl, s->path, sizeof T.m.stl);
        save_cfg();
    }
    double sc = T.m.scale;
    if (num_field(ui_id("stl-scale"), (Rectangle){ fx, cr.y, fwd, 34 }, &sc, "%g", 1e-9, 1e9, "×") && sc != T.m.scale) {
        T.m.scale = sc; stl_set_scale(&T.stl, sc); stl_split(&T.stl, T.m.splitAngle); mesher_apply_names(&T.m, &T.stl); save_cfg();
    }
    double ang = T.m.splitAngle;
    char after[96]; snprintf(after, sizeof after, "%d boundar%s", s->nRegions, s->nRegions == 1 ? "y" : "ies");
    if (row_num(x, &y, w, "stl-split", "Split by angle", "Surfaces meeting at a sharper angle become separate boundaries (STAR-CCM+'s split by angle). "
                "0 keeps the file's own solids. You name them two steps on.", &ang, "%g", 0, 180, "°", after)) {
        T.m.splitAngle = ang; stl_split(&T.stl, ang); mesher_take_names(&T.m, &T.stl); save_cfg();
    }
    /* start over: the case without an STL (a mistake, or the wrong part) */
    bool armed = T.removeArm > 0 && ui_time() - T.removeArm < 4;
    const char *rl = armed ? "Click again to remove" : "Remove STL from this case";
    float rw = ui_button_width(ICON_TRASH_2, rl);
    if (ui_button(ui_id("stl-remove"), (Rectangle){ x, y + 4, rw, 32 }, ICON_TRASH_2, rl, armed ? BTN_DANGER : BTN_GHOST)) {
        if (armed) { T.removeArm = 0; remove_stl(c); return y + 44 - y0; }
        T.removeArm = ui_time();
    }
    if (armed) { ui_request_frames(30); ui_text(F_REG, 12.5f, "Deletes mesher/ and the STL's copy in constant/triSurface; the mesh, if any, stays.", x + rw + 14, y + 13, TH.text3); }
    y += 44;
    return y - y0;
}

/* ============================================================================ the part in 3-D: click a face to pick its boundary */
#define VIEW_EDGES 60000                                  /* triangle outlines drawn up to this many triangles */
static struct {
    Model    model[STL_MAX_REGIONS]; bool has[STL_MAX_REGIONS];
    double   sig;                                         /* the geometry the models were built from */
    RenderTexture2D rt; int rtW, rtH; bool rtOk;
    bool     dirty, dragging, panning, moved;
    Orbit    O;                                           /* the camera (orbit.h): turned freely or about one axis */
    Vector2  last, down;
    double   lastClick;
    int      hover, drawnSel, drawnHover;
    bool     engaged;                                     /* clicked in: the wheel zooms until the mouse leaves */
    bool     touched;                                     /* turned, moved or zoomed by hand: the fit is left alone */
    int      up;                                          /* the axis the view turns about and keeps up (0 x, 1 y, 2 z; -1: the case's) */
    int      fitW, fitH;                                  /* the view's size the last fit was for */
} SV = { .hover = -1, .drawnSel = -2, .drawnHover = -2, .up = -1, .O = { .upAxis = 1, .spin = -1, .back = { 0, 0, 1 }, .up = { 0, 1, 0 } } };

static Color region_color(int i) { return TH.series[i % 10]; }

/* another case: the view is rebuilt and fitted to its part */
static void view_forget(void)
{
    SV.hover = -1; SV.engaged = SV.dragging = SV.panning = false;
    SV.sig = -1; SV.dirty = true; SV.up = -1;
}

static double geometry_sig(void)
{
    const Stl *s = &T.stl;
    double h = s->nTri * 1.0 + T.gen * 1e3 + s->nRegions * 1e-3 + T.m.scale * 7.1 + T.m.splitAngle * 0.013 + (double)(uintptr_t)s->tri * 1e-9;
    for (int k = 0; k < 3; k++) h += s->bmin[k] * 3.3 + s->bmax[k] * 5.9;
    for (int r = 0; r < s->nRegions; r++) h += s->reg[r].nTri * (r + 1) * 1e-6;
    return h;
}

static void view_unload(void)
{
    for (int i = 0; i < STL_MAX_REGIONS; i++) if (SV.has[i]) { UnloadModel(SV.model[i]); SV.has[i] = false; }
}

/* one model a boundary, flat shaded by a fixed light (two-sided), as on Mesh > View */
static void view_build(void)
{
    view_unload();
    const Stl *s = &T.stl;
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
        Color base = region_color(r);
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
        SV.model[r] = LoadModelFromMesh(mesh);
        SV.has[r] = true;
    }
    SV.sig = geometry_sig();
}

/* the whole part in the view, whatever the view's shape: its bounding sphere inside the narrower of the two fields of view.
   A part much thinner one way (a profile, a plate) is looked at face on, a little from the side, not edge on. */
static void view_fit(float aspect)
{
    const Stl *s = &T.stl;
    SV.O.target = (Vector3){ (s->bmin[0] + s->bmax[0]) / 2, (s->bmin[1] + s->bmax[1]) / 2, (s->bmin[2] + s->bmax[2]) / 2 };
    float d[3] = { s->bmax[0] - s->bmin[0], s->bmax[1] - s->bmin[1], s->bmax[2] - s->bmin[2] };
    int thin = 0; for (int k = 1; k < 3; k++) if (d[k] < d[thin]) thin = k;
    float big = fmaxf(d[0], fmaxf(d[1], d[2]));
    int u = SV.up < 0 ? 1 : SV.up;
    SV.O.upAxis = u;                                      /* the camera's frame: y is the up axis (orbit.h) */
    if (d[thin] < 0.25f * big) {                          /* the camera along the thin axis, turned a little */
        if (thin == u) orbit_set(&SV.O, 0.5f, 1.2f);                     /* from above */
        else if (thin == (u + 2) % 3) orbit_set(&SV.O, 1.2f, 0.25f);     /* the frame's x */
        else orbit_set(&SV.O, 0.35f, 0.25f);                             /* the frame's z */
    } else orbit_set(&SV.O, 0.7f, 0.45f);
    /* as close as the box's corners allow, seen from there: each within the vertical and the horizontal field of view */
    float tv = tanf(35.f / 2 * DEG2RAD), th = tv * fmaxf(aspect, 0.1f);   /* view_camera's fovy */
    Vector3 back = SV.O.back, right = orbit_right(&SV.O), up = SV.O.up;
    float need = 1e-9f;
    for (int c = 0; c < 8; c++) {
        float p[3] = { (c & 1 ? d[0] : -d[0]) / 2, (c & 2 ? d[1] : -d[1]) / 2, (c & 4 ? d[2] : -d[2]) / 2 };
        float x = p[0] * right.x + p[1] * right.y + p[2] * right.z, y = p[0] * up.x + p[1] * up.y + p[2] * up.z;
        float z = p[0] * back.x + p[1] * back.y + p[2] * back.z;
        need = fmaxf(need, fmaxf(z + fabsf(x) / th, z + fabsf(y) / tv));
    }
    SV.O.dist = need * 1.15f;
    SV.dirty = true;
}

static Camera3D view_camera(void)
{
    return orbit_camera(&SV.O);
}

/* the boundary under a point of the view (-1: none): the nearest triangle the ray through it hits */
static int view_pick(Rectangle view, Vector2 mp)
{
    float s = ui_scale();
    Vector2 pos = { (mp.x - view.x) * s, (mp.y - view.y) * s };
    Ray ray = GetScreenToWorldRayEx(pos, view_camera(), SV.rtW, SV.rtH);
    float best = 1e30f; int hit = -1;
    const Stl *st = &T.stl;
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
    int u = SV.up < 0 ? 2 : SV.up, a = (u + 1) % 3, o;
    if (st->bmax[(u + 2) % 3] - st->bmin[(u + 2) % 3] > st->bmax[a] - st->bmin[a]) a = (u + 2) % 3;
    o = 3 - u - a;
    double L = 0; for (int k = 0; k < 3; k++) L = fmax(L, st->bmax[k] - st->bmin[k]);
    cell[0] = T.m.base; cell[1] = mesher_surface_cell(&T.m);
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
    if (T.open == ST_GEOM) {
        double lo[3] = { st->bmin[0], st->bmin[1], st->bmin[2] }, hi[3] = { st->bmax[0], st->bmax[1], st->bmax[2] };
        wire_box(lo, hi, dim);
    }
    if (T.open == ST_MESH) {
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
        if (T.m.domain == DOMAIN_EXTERNAL && !T.m.twoD) {
            double lo[3], hi[3];
            mesher_near_box(&T.m, st, lo, hi);
            wire_box(lo, hi, ui_alpha(TH.accent, 0.5f));
        }
        for (int i = 0; i < T.m.nZones; i++) {
            const MeshZone *z = &T.m.zone[i];
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
    Vector2 q = GetWorldToScreenEx(p, cam, SV.rtW, SV.rtH);
    if (q.x < -50 || q.y < -50 || q.x > SV.rtW + 50 || q.y > SV.rtH + 50) return;
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
    const Stl *st = &T.stl;
    Color dim = TH.dark ? (Color){ 240, 210, 130, 255 } : (Color){ 130, 85, 10, 255 };
    char b[48], l[32];
    if (T.open == ST_GEOM) {                              /* each size along the box edge it measures */
        for (int a = 0; a < 3; a++) {
            Vector3 p = { st->bmin[0], st->bmin[1], st->bmin[2] };
            (&p.x)[a] = (st->bmin[a] + st->bmax[a]) / 2;
            fmt_len(l, sizeof l, st->bmax[a] - st->bmin[a]);
            snprintf(b, sizeof b, "%c  %s", "XYZ"[a], l);
            label_at(cam, s, p, b, dim);
        }
    }
    if (T.open == ST_MESH) {
        double cell[2], at[2][3], L = 0;
        size_glyphs(st, cell, at);
        for (int k = 0; k < 3; k++) L = fmax(L, st->bmax[k] - st->bmin[k]);
        for (int g = 0; g < 2; g++) {
            if (cell[g] > 3 * L || cell[g] < L * 1e-4) continue;
            fmt_len(l, sizeof l, cell[g]);
            snprintf(b, sizeof b, "%s %s", g ? "surface cell" : "base cell", l);
            double lp[3] = { at[g][0] + cell[g] / 2, at[g][1] + cell[g] / 2, at[g][2] + cell[g] / 2 };
            lp[SV.up < 0 ? 2 : SV.up] -= cell[g] / 2 + 0.05 * L;       /* under the cube */
            label_at(cam, s, (Vector3){ (float)lp[0], (float)lp[1], (float)lp[2] }, b, g ? TH.accent : dim);
        }
    }
}

static void view_redraw(Rectangle r)
{
    float s = ui_scale();
    int w = (int)(r.width * s), h = (int)(r.height * s);
    if (w < 8 || h < 8) return;
    if (!SV.rtOk || w != SV.rtW || h != SV.rtH) {
        if (SV.rtOk) UnloadRenderTexture(SV.rt);
        SV.rt = LoadRenderTexture(w, h);
        SV.rtOk = SV.rt.id != 0; SV.rtW = w; SV.rtH = h;
    }
    if (!SV.rtOk) return;
    EndScissorMode();
    BeginTextureMode(SV.rt);
    ClearBackground(TH.dark ? (Color){ 10, 14, 22, 255 } : (Color){ 236, 240, 247, 255 });
    rlDrawRenderBatchActive();
    Camera3D cam = view_camera();
    BeginMode3D(cam);
    rlSetClipPlanes(SV.O.dist * 0.01, SV.O.dist * 20.0);
    rlDisableBackfaceCulling();
    const Stl *st = &T.stl;
    for (int r2 = 0; r2 < st->nRegions; r2++) {
        if (!SV.has[r2]) continue;
        /* the picked boundary stands out: the others dim; the one under the mouse brightens */
        unsigned char g = T.sel >= 0 ? (r2 == T.sel ? 255 : r2 == SV.hover ? 200 : 110) : (r2 == SV.hover ? 255 : 215);
        DrawModel(SV.model[r2], (Vector3){ 0, 0, 0 }, 1, (Color){ g, g, g, 255 });
    }
    /* outlines a hair toward the camera: every triangle's when there are few, the picked boundary's in the accent */
    Vector3 to = { cam.position.x - cam.target.x, cam.position.y - cam.target.y, cam.position.z - cam.target.z };
    float tl = sqrtf(to.x * to.x + to.y * to.y + to.z * to.z), k = SV.O.dist * 0.0015f / fmaxf(tl, 1e-12f);
    to.x *= k; to.y *= k; to.z *= k;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0 && st->nTri > VIEW_EDGES) continue;
        if (pass == 1 && (T.sel < 0 || st->reg[T.sel].nTri > VIEW_EDGES)) continue;
        Color ec = pass ? TH.accent : TH.dark ? (Color){ 0, 0, 0, 110 } : (Color){ 20, 30, 50, 80 };
        rlBegin(RL_LINES);
        rlColor4ub(ec.r, ec.g, ec.b, ec.a);
        for (int t = 0; t < st->nTri; t++) {
            if (pass == 1 && st->region[t] != T.sel) continue;
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
    /* external: the wind tunnel's box, and an arrow ahead of the body the way the flow goes */
    if (T.m.domain == DOMAIN_EXTERNAL) {
        double lo[3], hi[3];
        mesher_domain(&T.m, st, lo, hi);
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
        int f = T.m.flowDir / 2; float sg = T.m.flowDir % 2 ? -1.f : 1.f;
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
    orbit_draw_axes(&SV.O, ax, ay, al, s);
    EndTextureMode();
    ui_restore_2d();
    SV.dirty = false;
    SV.drawnSel = T.sel; SV.drawnHover = SV.hover;
}

static void select_region(int r);

static void view_panel(Rectangle view)
{
    if (SV.sig != geometry_sig() || (!SV.has[0] && T.stl.nRegions > 0)) { view_build(); SV.touched = false; SV.fitW = 0; }
    {   /* fitted to the view's real shape, again when it changes (a step with the view beside it, or above), until turned by hand */
        int vw = (int)(view.width * ui_scale()), vh = (int)(view.height * ui_scale());
        if (!SV.touched && vh > 0 && (vw != SV.fitW || vh != SV.fitH)) { view_fit((float)vw / vh); SV.fitW = vw; SV.fitH = vh; }
    }
    ui_rect(view, 12, TH.dark ? (Color){ 10, 14, 22, 255 } : (Color){ 236, 240, 247, 255 });
    if (SV.up < 0) { SV.up = mesher_up(&T.m) / 2; SV.touched = false; SV.fitW = 0; }   /* the case's up (CAD's z, or the tunnel's Up) */
    Rectangle upR = { view.x + view.width - 160, view.y + 10, 150, 30 };
    Rectangle turnR = { view.x + 10, view.y + 6, orbit_tools_w(true) + 16, 40 };       /* top left, or under Up when narrow */
    if (turnR.x + turnR.width > upR.x - 36) turnR.y = upR.y + upR.height + 8;
    bool hov = ui_hovered(view) && !ui_hovered(upR) && !ui_hovered(turnR) && A.modal == MD_NONE;
    Vector2 mp = ui_mouse();
    if (!hov) SV.engaged = false;
    if (hov && ui_pressed(view)) {
        SV.engaged = true;
        if (ui_time() - SV.lastClick < 0.35) { view_fit(view.height > 0 ? view.width / view.height : 1.5f); SV.touched = false; SV.lastClick = 0; }
        else SV.lastClick = ui_time();
        bool shift = inp_key_down(KEY_LEFT_SHIFT) || inp_key_down(KEY_RIGHT_SHIFT);
        SV.dragging = !shift; SV.panning = shift; SV.last = SV.down = mp; SV.moved = false;
    }
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) { SV.panning = true; SV.dragging = false; SV.last = SV.down = mp; SV.moved = true; }
    if (SV.dragging || SV.panning) {
        bool down = SV.dragging ? ui_mouse_down() : (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || ui_mouse_down());
        if (fabsf(mp.x - SV.down.x) + fabsf(mp.y - SV.down.y) > 4) SV.moved = true;
        if (!down) {
            if (!SV.moved && hov && T.open == ST_BOUND) select_region(view_pick(view, mp));   /* a click, not a drag: pick the boundary there */
            SV.dragging = SV.panning = false;
        } else {
            float dx = mp.x - SV.last.x, dy = mp.y - SV.last.y;
            if (SV.moved && (dx != 0 || dy != 0)) {
                SV.touched = true;
                if (SV.dragging) orbit_drag(&SV.O, SV.last, mp, (Vector2){ view.x + view.width / 2, view.y + view.height / 2 });
                else orbit_pan(&SV.O, dx, dy);
                SV.dirty = true;
            }
            SV.last = mp;
            ui_request_frames(2);
        }
    }
    /* the boundary under the mouse, named beside it (not while turning, nor on very large parts) */
    int hv = -1;
    if (hov && !SV.dragging && !SV.panning && T.stl.nTri <= 300000 && SV.rtOk && T.open == ST_BOUND) {
        static Vector2 at = { -1, -1 }; static int was = -1;
        if (mp.x != at.x || mp.y != at.y) { at = mp; was = view_pick(view, mp); }
        hv = was;
    }
    if (hv != SV.hover) { SV.hover = hv; SV.dirty = true; }
    if (hov) {
        if (orbit_keys(&SV.O)) { SV.dirty = true; SV.touched = true; }
        /* the view sits in a scrolling page: the wheel scrolls past it, and zooms once it is clicked (or with Ctrl) */
        if (SV.engaged || inp_key_down(KEY_LEFT_CONTROL) || inp_key_down(KEY_RIGHT_CONTROL)) {
            float wh = ui_take_wheel();
            if (wh != 0) { SV.O.dist *= powf(0.88f, wh); SV.dirty = true; SV.touched = true; }
        }
        ui_cursor(SV.panning ? MOUSE_CURSOR_RESIZE_ALL : MOUSE_CURSOR_POINTING_HAND);
    }
    if (T.sel != SV.drawnSel || SV.hover != SV.drawnHover) SV.dirty = true;
    {   /* the tunnel and the arrow follow the domain's settings */
        double ds = T.open * 37.0 + SV.up * 53.0 + T.m.base * 41.0 + T.m.targetPct * 0.43 + T.m.nZones * 3.1 + T.m.scale * 0.17 + T.m.domain + T.m.flowDir * 7.0 + mesher_up(&T.m) * 13.0 + T.m.up * 1.1 + T.m.down * 3.3 + T.m.side * 5.7 + T.m.top * 11.9 + T.m.below * 2.9 + T.m.ground * 0.7 + T.m.refLen * 17.3 + T.m.twoD * 23.0 + T.m.normal2D * 29.0;
        static double was = -1;
        if (ds != was) { was = ds; SV.dirty = true; }
    }
    if (SV.dirty) view_redraw(view);
    if (SV.rtOk) DrawTexturePro(SV.rt.texture, (Rectangle){ 0, 0, (float)SV.rtW, -(float)SV.rtH }, view, (Vector2){ 0, 0 }, 0, WHITE);
    ui_rect_lines(view, 12, 1, TH.lineSoft);
    ui_register(ui_id("stl-view"), view, "STL view");
    {   /* which axis is up: the view turns about it (STL files come with x, y or z up, whatever the CAD program likes) */
        static const char *ax[] = { "X", "Y", "Z" };
        int u = SV.up;
        ui_text(F_MED, 12, ui_cap("Up"), upR.x - 26, upR.y + 8, TH.dark ? (Color){ 200, 210, 230, 200 } : (Color){ 60, 70, 90, 200 });
        if (ui_segmented(ui_id("stl-view-up"), upR, ax, 3, &u) && u != SV.up) { SV.up = u; SV.touched = false; SV.fitW = 0; SV.dirty = true; }
        ui_tooltip(upR, "The axis that points up in the view");
    }
    ui_rect(turnR, 9, ui_alpha(TH.surface, 0.82f));
    switch (orbit_tools(&SV.O, "stl-view", turnR.x + 8, turnR.y + 5, true)) {
    case 1: SV.touched = true; SV.dirty = true; break;
    case 2: SV.touched = false; SV.fitW = 0; SV.dirty = true; break;      /* fitted afresh next frame */
    }
    if (SV.hover >= 0 && SV.hover < T.stl.nRegions) {
        const char *nm = T.stl.reg[SV.hover].name;
        float tw = ui_text_w(F_MONO_MED, 13, nm) + 18;
        Rectangle tag = { fminf(mp.x + 14, view.x + view.width - tw - 6), mp.y - 32, tw, 26 };
        ui_rect(tag, 6, ui_alpha(TH.surface, 0.92f));
        ui_rect_lines(tag, 6, 1, region_color(SV.hover));
        ui_text(F_MONO_MED, 13, nm, tag.x + 9, tag.y + 5, TH.text);
    }
    char how[240];
    if (T.open == ST_BOUND) snprintf(how, sizeof how, "Click a face to pick its boundary  ·  %s  ·  right-drag to move  ·  click, then wheel to zoom", orbit_hint(&SV.O));
    else {
        snprintf(how, sizeof how, "%s  ·  right-drag to move  ·  click, then wheel to zoom  ·  double-click to fit", orbit_hint(&SV.O));
        how[0] = (char)(how[0] - 'a' + 'A');
    }
    ui_text_clip(F_REG, 12, how, view.x + 96, view.y + view.height - 22, view.width - 110,             /* right of the axes */ TH.dark ? (Color){ 200, 210, 230, 160 } : (Color){ 60, 70, 90, 170 });
}

/* ============================================================================ the boundaries */
static const char *const TYPES[BT__N] = { "Wall", "Opening", "Inlet", "Outlet", "Symmetry" };
static const char *foam_type_of(int t) { return t == BT_WALL ? "wall" : t == BT_SYMMETRY ? "symmetry" : "patch"; }

static void select_region(int r)
{
    T.sel = r >= 0 && r < T.stl.nRegions ? r : -1;
    SV.dirty = true;
    if (T.sel >= 0) ui_focus_input(ui_idn(ui_id("stl-sel-name"), T.sel));     /* type the new name straight away */
}

static bool rename_region(int i, const char *typed)
{
    Stl *s = &T.stl;
    char nm[64]; pl_strlcpy(nm, typed, sizeof nm); pl_trim_inplace(nm);
    for (char *q = nm; *q; q++) if (*q == ' ' || *q == '-') *q = '_';   /* "front inlet" -> front_inlet */
    bool okName = nm[0] != 0 && !(nm[0] >= '0' && nm[0] <= '9');
    for (char *q = nm; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_')) okName = false;
    for (int k = 0; k < s->nRegions && okName; k++) if (k != i && !strcmp(s->reg[k].name, nm)) okName = false;
    if (!okName) { ui_toast(TOAST_ERR, "A boundary name is letters, digits and _ (not starting with a digit), and each one different"); return false; }
    if (!strcmp(nm, s->reg[i].name)) return true;
    char old[64]; pl_strlcpy(old, s->reg[i].name, sizeof old);
    pl_strlcpy(s->reg[i].name, nm, sizeof s->reg[i].name);
    save_cfg();
    if (A.cs) setup_rename(A.cs, old, nm);                 /* its conditions go with it */
    return true;
}

static void set_type(int i, int ty)
{
    if (T.stl.reg[i].type == ty) return;
    T.stl.reg[i].type = ty;
    save_cfg();
}

int stlmesh_patches(Case *c, SimPatch *out, int max, double *size, bool *external)
{
    sync_case(c);
    if (!T.loaded) return 0;
    const Stl *s = &T.stl;
    double L = 0; for (int k = 0; k < 3; k++) L = fmax(L, s->bmax[k] - s->bmin[k]);
    *size = L; *external = T.m.domain == DOMAIN_EXTERNAL;
    int n = 0;
    for (int i = 0; i < s->nRegions && n < max; i++) {
        if (!mesher_region_meshed(&T.m, s, i)) continue;   /* 2-D: the extrusion's end caps aren't in the mesh */
        out[n++] = (SimPatch){ s->reg[i].name, s->reg[i].type, foam_type_of(s->reg[i].type), true, s->reg[i].area };
    }
    if (T.m.twoD && n + 2 <= max) {
        out[n++] = (SimPatch){ "front2D", ROLE_WALL, "empty", false, 0 };
        out[n++] = (SimPatch){ "back2D", ROLE_WALL, "empty", false, 0 };
    }
    if (T.m.domain == DOMAIN_EXTERNAL) {                   /* the wind tunnel's own (stlmesh.c write_snappy, the SALOME script) */
        if (n < max) out[n++] = (SimPatch){ "inlet", ROLE_INLET, "patch", false, 0 };
        if (n < max) out[n++] = (SimPatch){ "outlet", ROLE_OUTLET, "patch", false, 0 };
        if (n < max) out[n++] = (SimPatch){ "farfield", ROLE_SYMMETRY, "patch", false, 0 };
        if (T.m.ground && n < max) out[n++] = (SimPatch){ "ground", ROLE_WALL, "wall", false, 0 };
    }
    return n;
}

void stlmesh_flow(Case *c, double v[3])
{
    sync_case(c);
    v[0] = v[1] = v[2] = 0;
    if (!T.loaded || T.m.domain != DOMAIN_EXTERNAL) { v[0] = 1; return; }
    v[T.m.flowDir / 2] = T.m.flowDir % 2 ? -1 : 1;
}

bool stlmesh_force_ref(Case *c, SimForceRef *r, bool *twoD)
{
    sync_case(c);
    *twoD = false;
    if (!T.loaded) return false;
    const Stl *s = &T.stl;
    double v[3]; stlmesh_flow(c, v);
    int f = fabs(v[0]) > 0.5 ? 0 : fabs(v[1]) > 0.5 ? 1 : 2, u = mesher_up(&T.m);
    for (int k = 0; k < 3; k++) { r->drag[k] = v[k]; r->lift[k] = 0; }
    r->lift[u / 2] = u % 2 ? -1 : 1;
    double len = s->bmax[f] - s->bmin[f];
    for (int k = 0; k < 3; k++) r->CofR[k] = (s->bmin[k] + s->bmax[k]) / 2;
    r->CofR[f] = v[f] > 0 ? s->bmin[f] + 0.25 * len : s->bmax[f] - 0.25 * len;   /* an airfoil's quarter chord */
    r->Uinf = 0;
    if (T.m.twoD) {
        int n = T.m.normal2D % 3;
        *twoD = true;
        r->lRef = len;
        r->aRef = len * (s->bmax[n] - s->bmin[n]);         /* the mesh is the part's depth thick (extrudeMeshDict) */
        return true;
    }
    r->lRef = mesher_ref_length(&T.m, s);
    /* the frontal area: the part seen along the flow, half the sum of |n . flow| over a closed surface */
    double a = 0;
    for (int t = 0; t < s->nTri; t++) {
        if (!mesher_region_meshed(&T.m, s, s->region[t])) continue;
        const float *p = s->tri + 9 * (size_t)t;
        double e1[3] = { p[3] - p[0], p[4] - p[1], p[5] - p[2] }, e2[3] = { p[6] - p[0], p[7] - p[1], p[8] - p[2] };
        double nr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        a += fabs(nr[f]) / 2;
    }
    r->aRef = a / 2;
    return true;
}

bool stlmesh_set_role(Case *c, const char *name, int role)
{
    sync_case(c);
    for (int i = 0; i < T.stl.nRegions; i++) if (!strcmp(T.stl.reg[i].name, name)) { set_type(i, role); return true; }
    return false;
}

/* the picked boundary: its name, what it is, and its conditions */
static float selection_panel(Case *c, float x, float y, float w, float h)
{
    Rectangle box = { x, y, w, h };
    ui_rect(box, 12, TH.surface2); ui_rect_lines(box, 12, 1, TH.lineSoft);
    float px = x + 18, pw = w - 36, py = y + 18;
    Stl *s = &T.stl;
    if (T.sel < 0 || T.sel >= s->nRegions) {
        ui_icon(ICON_CIRCLE_DOT, px + 10, py + 12, 18, TH.accent);
        ui_text(F_SEMI, 16, "Click a face of the part", px + 30, py + 2, TH.text);
        py += 34;
        py += ui_text_wrap(F_REG, 13, "Its boundary lights up here: rename it, say what it is (a wall, an inlet, an outlet ...) and set its "
                           "conditions. The list below picks one too. The flow, the fluid and the run are on Physics > Setup.",
                           (Rectangle){ px, py, pw, 300 }, 19, TH.text3, true) + 14;
        if (ui_button(ui_id("stl-go-setup"), (Rectangle){ px, py, ui_button_width(ICON_ATOM, "Physics > Setup"), 34 }, ICON_ATOM, "Physics > Setup", BTN_SECONDARY)) {
            A.physicsTab = 2; app_go(PG_PHYSICS);
        }
        return h;
    }
    StlRegion *R = &s->reg[T.sel];
    UiId nid = ui_idn(ui_id("stl-sel-name"), T.sel);       /* the id select_region focuses: made at the same level */
    ui_push_id(ui_idn(ui_id("stl-sel"), T.sel));
    ui_rect((Rectangle){ px, py + 4, 14, 14 }, 4, region_color(T.sel));
    char info[160]; pl_fmt_ui(info, sizeof info, "%d triangles  ·  %.3g m²", R->nTri, R->area);
    ui_text_clip(F_REG, 12.5f, info, px + 22, py + 3, pw - 60, TH.text3);
    if (ui_button(ui_id("close"), (Rectangle){ x + w - 46, y + 10, 30, 30 }, ICON_X, NULL, BTN_ICON)) { select_region(-1); ui_pop_id(); return h; }
    py += 30;
    ui_pop_id();
    ui_text(F_MED, 12.5f, ui_cap("Name"), px, py, TH.text2);
    char nm[64]; pl_strlcpy(nm, R->name, sizeof nm);
    if (ui_input(nid, (Rectangle){ px, py + 20, pw, 36 }, nm, sizeof nm, "boundary name", INPUT_MONO)) rename_region(T.sel, nm);
    py += 68;
    ui_text(F_MED, 12.5f, ui_cap("It is"), px, py, TH.text2);
    int ty = R->type;
    if (ui_segmented(ui_id("stl-sel-type"), (Rectangle){ px, py + 20, pw, 34 }, (const char **)TYPES, BT__N, &ty)) set_type(T.sel, ty);
    py += 66;
    py += setup_boundary_editor(c, R->name, px, py, pw);
    return fmaxf(h, py - y + 10);
}

/* ---------------------------------------------------------------------------- 3 Boundaries: names and kinds, in 3-D */
static void boundary_counts(int *in, int *out, int *wall, int *other)
{
    *in = *out = *wall = *other = 0;
    for (int i = 0; i < T.stl.nRegions; i++) {
        if (!mesher_region_meshed(&T.m, &T.stl, i)) continue;
        int t = T.stl.reg[i].type;
        if (t == BT_INLET) (*in)++; else if (t == BT_OUTLET || t == BT_PATCH) (*out)++; else if (t == BT_WALL) (*wall)++; else (*other)++;
    }
}

static float boundaries_step(Case *c, float x, float y, float w)
{
    float y0 = y;
    y += note(x, y, w, "Each boundary becomes an OpenFOAM patch with this name. Click a face of the part to pick its boundary, then name "
                       "it and say what it is; walls get prism layers. The list under the view picks them too.");
    Stl *s = &T.stl;
    if (T.sel >= s->nRegions) T.sel = -1;
    float pw = fminf(460, w * 0.42f), vh = 440;
    Rectangle view = { x, y, w - pw - 16, vh };
    view_panel(view);
    float ph = selection_panel(c, x + view.width + 16, y, pw, vh);
    y += fmaxf(vh, ph) + 16;
    for (int i = 0; i < s->nRegions; i++) {
        StlRegion *R = &s->reg[i];
        float rh = 44;
        if (ui_culled(y, y + rh)) {                         /* not drawn, but still where it is: a test script scrolls to it */
            ui_push_id(ui_idn(ui_id("stl-b"), i));
            ui_register(ui_id("name"), (Rectangle){ x + 40, y, 240, 34 }, NULL);
            ui_register(ui_id("type"), (Rectangle){ x + 292, y, 140, 34 }, NULL);
            ui_pop_id();
            y += rh; continue;
        }
        ui_push_id(ui_idn(ui_id("stl-b"), i));
        Rectangle sw = { x, y + 2, 30, 30 };                /* the colour in the view; click to pick it */
        if (ui_row(ui_id("pick"), sw, T.sel == i)) select_region(T.sel == i ? -1 : i);
        ui_rect((Rectangle){ sw.x + 8, sw.y + 8, 14, 14 }, 4, region_color(i));
        if (T.sel == i) ui_rect_lines(sw, 7, 1.5f, TH.accent);
        ui_tooltip(sw, "Pick it: it lights up in the view");
        char nm[64]; pl_strlcpy(nm, R->name, sizeof nm);
        if (ui_input(ui_id("name"), (Rectangle){ x + 40, y, 240, 34 }, nm, sizeof nm, "name", INPUT_MONO)) rename_region(i, nm);
        if (ui_input_focused(ui_id("name")) && T.sel != i) { T.sel = i; SV.dirty = true; }   /* typing in a row picks it in the view */
        int ty = R->type;
        if (ui_select(ui_id("type"), (Rectangle){ x + 292, y, 140, 34 }, TYPES, BT__N, &ty)) set_type(i, ty);
        char info[160]; pl_fmt_ui(info, sizeof info, "%d triangles  ·  %.3g m²  ·  %s", R->nTri, R->area, axis_word(R->n));
        ui_text_clip(F_REG, 13, info, x + 448, y + 9, w - 448, TH.text3);
        ui_pop_id();
        y += rh;
    }
    /* what the flow needs: a way in and a way out (a wind tunnel brings its own) */
    int in, out, wall, other;
    boundary_counts(&in, &out, &wall, &other);
    y += 8;
    if (T.m.domain == DOMAIN_EXTERNAL) {
        const char *d = "The wind tunnel adds its own inlet, outlet and far field (and the ground, when the body sits on it): the part's "
                        "own boundaries are usually all walls. Their conditions are on Physics > Setup.";
        float ch = callout_height(w, NULL, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_WIND, NULL, d);
        y += ch + 10;
    } else if (!in || !out) {
        char d[300];
        pl_fmt_ui(d, sizeof d, "Internal flow needs %s%s%s: click the face the flow comes in by and make it an Inlet, the one it leaves by an "
                  "Outlet (or an Opening). You can carry on and set them later on Physics > Setup.",
                  !in ? "an inlet" : "", !in && !out ? " and " : "", !out ? "an outlet" : "");
        float ch = callout_height(w, "No way in or out yet", d);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "No way in or out yet", d);
        y += ch + 10;
    } else {
        char d[160]; pl_fmt_ui(d, sizeof d, "%d inlet%s, %d outlet%s, %d wall%s%s", in, in == 1 ? "" : "s", out, out == 1 ? "" : "s", wall, wall == 1 ? "" : "s",
                                other ? " and symmetry planes" : "");
        float ch = callout_height(w, NULL, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_OK, ICON_CIRCLE_CHECK, NULL, d);
        y += ch + 10;
    }
    return y - y0;
}

/* 2-D: a profile (an airfoil, a duct's cross section) extruded along an axis, meshed one cell thick across it */
static void set_two_d(bool on, int axis)
{
    T.m.twoD = on; T.m.normal2D = axis;
    if (on && T.m.flowDir / 2 == axis) {                  /* the flow goes in the plane */
        T.m.flowDir = axis == 0 ? DIR_PY : DIR_PX;
        setup_flow_changed(A.cs);
    }
    T.m.upDir = mesher_up(&T.m);
    save_cfg();
}

static float profile_rows(float x, float y, float w);

static float two_d_rows(float x, float y, float w)
{
    float y0 = y;
    double thick = part_thickness();
    int ta = stl_thin_axis(&T.stl);
    double text = T.stl.bmax[ta] - T.stl.bmin[ta], other = 0;
    for (int k = 0; k < 3; k++) if (k != ta) other = fmax(other, T.stl.bmax[k] - T.stl.bmin[k]);
    /* the whole part thin along an axis, and much thinner than it is long: a profile extruded, best meshed 2-D */
    if (!T.m.twoD && thick > 0 && fabs(text - thick) < 0.2 * text && text < 0.25 * other) {
        char t[32], d[400]; fmt_len(t, sizeof t, text);
        pl_fmt_ui(d, sizeof d, "It is %s deep along %c and much larger the other ways: a profile extruded (an airfoil, a section). "
                  "Meshed 2-D, one cell thick across %c, it needs far fewer cells than in 3-D, where the cells must be smaller than %s.",
                  t, "XYZ"[ta], "XYZ"[ta], t);
        float ch = callout_height(w, "This looks 2-D", d) + 46;
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_LAYERS, "This looks 2-D", d);
        float tw = ui_button_width(ICON_LAYERS, "Mesh it 2-D");
        if (ui_button(ui_id("stl-2d-fix"), (Rectangle){ x + 42, y + ch - 44, tw, 34 }, ICON_LAYERS, "Mesh it 2-D", BTN_PRIMARY)) {
            set_two_d(true, ta);
            ui_toast(TOAST_OK, "2-D across %c: about %.0f thousand cells", "XYZ"[ta], mesher_estimate_cells(&T.m, &T.stl) / 1e3);
        }
        y += ch + 14;
    }
    Rectangle cr = prop_row(x, &y, w, "2-D", "A profile (an airfoil, a section) extruded along one axis: meshed one cell thick across it, with "
                            "its front and back empty, as OpenFOAM's 2-D cases are. Far fewer cells than 3-D.", 24);
    bool on = T.m.twoD;
    if (ui_switch(ui_id("stl-2d"), cr.x, cr.y + 2, &on)) set_two_d(on, on ? stl_thin_axis(&T.stl) : T.m.normal2D);
    if (T.m.twoD) {
        static const char *const AX[3] = { "Across X", "Across Y", "Across Z" };
        int a = T.m.normal2D;
        if (ui_segmented(ui_id("stl-2d-axis"), (Rectangle){ cr.x + 60, cr.y - 5, fminf(330, cr.width - 60), 34 }, (const char **)AX, 3, &a) && a != T.m.normal2D)
            set_two_d(true, a);
        char t[32]; fmt_len(t, sizeof t, T.stl.bmax[T.m.normal2D] - T.stl.bmin[T.m.normal2D]);
        char h[160]; pl_fmt_ui(h, sizeof h, "extruded %s along %c", t, "XYZ"[T.m.normal2D]);
        ui_text_clip(F_REG, 13, h, cr.x + 60 + fminf(330, cr.width - 60) + 12, cr.y + 2, cr.width - 420, TH.text3);
        y += profile_rows(x, y, w);
    }
    return y - y0;
}

/* 2-D: a profile exported coarse (few straight segments) made smooth, or back to the file it was made of */
static void smooth_profile(bool on)
{
    char err[300];
    if (!on) {
        if (!T.m.stlOriginal[0]) return;
        char orig[PL_PATH]; pl_strlcpy(orig, T.m.stlOriginal, sizeof orig);
        pl_strlcpy(T.m.stl, orig, sizeof T.m.stl); T.m.stlOriginal[0] = 0;
        if (load_stl(orig, false)) ui_toast(TOAST_OK, "Back to %s", pl_basename(orig));
        return;
    }
    char dir[PL_PATH], out[PL_PATH], name[200];
    pl_join(dir, sizeof dir, T.casePath, "mesher"); pl_mkdirs(dir);
    pl_strlcpy(name, pl_basename(T.m.stl), sizeof name);
    char *dot = strrchr(name, '.'); if (dot) *dot = 0;
    char fn[240]; snprintf(fn, sizeof fn, "%s-smooth.stl", name);
    pl_join(out, sizeof out, dir, fn);
    int np0; double k0; stl_profile_facets(&T.stl, T.m.normal2D, 30, &np0, &k0);
    /* points about the minimum surface cell apart, so the curvature refinement sees no corners */
    int no = stl_smooth_profile(&T.stl, T.m.normal2D, 30, T.m.base * fmin(T.m.minPct, T.m.targetPct) / 100, out, err, sizeof err);
    if (!no) { ui_toast(TOAST_ERR, "%s", err); return; }
    char orig[PL_PATH]; pl_strlcpy(orig, T.m.stl, sizeof orig);
    pl_strlcpy(T.m.stl, out, sizeof T.m.stl);
    if (!load_stl(out, false)) { pl_strlcpy(T.m.stl, orig, sizeof T.m.stl); return; }
    pl_strlcpy(T.m.stlOriginal, orig, sizeof T.m.stlOriginal);
    save_cfg();
    ui_toast(TOAST_OK, "The profile is smooth now: %d points instead of %d (mesher/%s)", no, np0, fn);
}

static float profile_rows(float x, float y, float w)
{
    if (!T.m.twoD || !T.loaded) return 0;
    static double sig = -1; static int nAx = -1, np; static double kink; static bool ok;
    if (sig != geometry_sig() || nAx != T.m.normal2D) { sig = geometry_sig(); nAx = T.m.normal2D; ok = stl_profile_facets(&T.stl, nAx, 30, &np, &kink); }
    float y0 = y;
    if (T.m.stlOriginal[0]) {
        char t[400]; pl_fmt_ui(t, sizeof t, "A smooth profile (%d points) made of %s, its sharp corners kept.", np, pl_basename(T.m.stlOriginal));
        float bw = ui_button_width(ICON_UNDO_2, "Use the original");
        ui_text_clip(F_REG, 13, t, x, y + 9, w - bw - 16, TH.text3);
        if (ui_button(ui_id("stl-unsmooth"), (Rectangle){ x + w - bw, y, bw, 34 }, ICON_UNDO_2, "Use the original", BTN_GHOST)) smooth_profile(false);
        return 46;
    }
    double resolve = 360.0 / fmax(3, T.m.curvature), limit = fmax(2, fmin(5, resolve));
    if (!ok || kink <= limit) return 0;
    /* the user's airfoil, 2026-10-26: 45 points round a 1 m chord; snappy refined at each corner and the layers broke there */
    char d[800], why[300];
    if (kink > resolve)
        pl_fmt_ui(why, sizeof why, "snappyHexMesh refines the cells at each corner (it resolves bends over %.1f°), the prism layers break "
                  "off where the cell size steps, and the flow sees the corners too.", resolve);
    else
        pl_fmt_ui(why, sizeof why, "The flow sees each corner: a pressure spike at every one, worst near the leading edge, and the "
                  "lift and drag come out a little off.");
    pl_fmt_ui(d, sizeof d, "Its outline is only %d straight segments, with corners up to %.1f°. %s Smooth the profile draws a spline "
              "through the points, about every %.3g mm, keeping sharp corners such as a trailing edge. Exporting the STL finer from "
              "CAD (a smaller chord tolerance) works as well.", np, kink, why, T.m.base * fmin(T.m.minPct, T.m.targetPct) / 100 * 1000);
    float ch = callout_height(w, "The profile is coarse", d) + 46;
    callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "The profile is coarse", d);
    float tw = ui_button_width(ICON_SPARKLES, "Smooth the profile");
    if (ui_button(ui_id("stl-smooth"), (Rectangle){ x + 42, y + ch - 44, tw, 34 }, ICON_SPARKLES, "Smooth the profile", BTN_PRIMARY)) smooth_profile(true);
    return ch + 14 + (y - y0);
}

/* a picture of each kind of flow, drawn: a pipe with the flow going through it; a wing with the flow going round it */
static bool flow_card(UiId id, Rectangle r, int kind, bool on)
{
    bool hov = ui_hovered(r);
    ui_rect(r, 12, on ? ui_alpha(TH.accent, 0.14f) : hov ? TH.hover : TH.surface2);
    ui_rect_lines(r, 12, on ? 2 : 1, on ? TH.accent : TH.lineSoft);
    ui_register(id, r, kind ? "External flow" : "Internal flow");
    Rectangle pic = { r.x + 16, r.y + 14, r.width - 32, 70 };
    Color part = TH.dark ? (Color){ 150, 160, 180, 255 } : (Color){ 110, 120, 140, 255 }, flow = TH.accent;
    float cy = pic.y + pic.height / 2;
    if (kind == 0) {                                      /* a pipe: two walls, arrows inside */
        DrawRectangleRounded((Rectangle){ pic.x + 20, cy - 26, pic.width - 40, 8 }, 1, 6, part);
        DrawRectangleRounded((Rectangle){ pic.x + 20, cy + 18, pic.width - 40, 8 }, 1, 6, part);
        for (int k = -1; k <= 1; k++) {
            float yy = cy + k * 9, x0 = pic.x + 30, x1 = pic.x + pic.width - 34;
            DrawLineEx((Vector2){ x0, yy }, (Vector2){ x1, yy }, 2, flow);
            DrawTriangle((Vector2){ x1 + 8, yy }, (Vector2){ x1, yy - 5 }, (Vector2){ x1, yy + 5 }, flow);
        }
    } else {                                              /* a wing: a teardrop, streamlines bending round it */
        float wx = pic.x + pic.width * 0.35f, ww = pic.width * 0.32f;
        DrawEllipse((int)(wx + ww * 0.3f), (int)cy, ww * 0.3f, 11, part);
        DrawTriangle((Vector2){ wx + ww * 0.3f, cy - 11 }, (Vector2){ wx + ww * 0.3f, cy + 11 }, (Vector2){ wx + ww, cy + 1 }, part);
        for (int k = -2; k <= 2; k++) {
            if (k == 0) continue;
            float base = cy + k * 13, bump = (k > 0 ? 1 : -1) * (14.f - 4.f * abs(k));
            Vector2 prev = { pic.x + 6, base };
            for (int q = 1; q <= 24; q++) {
                float t = q / 24.f, xx = pic.x + 6 + t * (pic.width - 20);
                float g = expf(-powf((xx - (wx + ww * 0.5f)) / (ww * 0.55f), 2));
                Vector2 cur = { xx, base + bump * g };
                DrawLineEx(prev, cur, 2, flow);
                prev = cur;
            }
            DrawTriangle((Vector2){ prev.x + 7, prev.y }, (Vector2){ prev.x, prev.y - 4 }, (Vector2){ prev.x, prev.y + 4 }, flow);
        }
    }
    ui_text(F_SEMI, 15, kind ? "Fluid flows AROUND the part" : "Fluid flows THROUGH the part", r.x + 16, r.y + 92, TH.text);
    ui_text_clip(F_REG, 12.5f, kind ? "A wing, a car, a building: it sits in a wind tunnel" : "A pipe, a duct, a manifold: the fluid fills its inside",
                 r.x + 16, r.y + 116, r.width - 32, TH.text3);
    if (on) ui_icon(ICON_CIRCLE_CHECK, r.x + r.width - 22, r.y + 22, 18, TH.accent);
    if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    return ui_clicked(r);
}

/* ---------------------------------------------------------------------------- 2 Flow: inside the part or round it, 2-D, the tunnel */
static float flow_step(float x, float y, float w)
{
    float y0 = y;
    int d = T.m.domain;
    float cw = fminf(360, (w - 16) / 2), ch = 150;
    for (int i = 0; i < 2; i++) {
        Rectangle cr = { x + i * (cw + 16), y, cw, ch };
        if (flow_card(ui_idn(ui_id("stl-domain"), i), cr, i, d == i)) d = i;
    }
    if (d != T.m.domain) {
        T.m.domain = d;
        mesher_size_defaults(&T.m, &T.stl, part_thickness());   /* the sizes suit each kind (and a thin part); the rest stays */
        save_cfg();
    }
    y += ch + 14;
    y += note(x, y, w, T.m.domain == DOMAIN_INTERNAL
                     ? "The fluid fills the inside of the part: a duct, a manifold, a room. The part must be closed, with its inlets and outlets "
                       "as faces of it (you name them next)."
                     : "The part is a body in a wind tunnel: a wing, a car, a building. Say which way the air goes and which way is up; the "
                       "tunnel's size is in reference lengths, and the view shows it.");
    y += two_d_rows(x, y, w);
    if (T.m.domain == DOMAIN_EXTERNAL) {
        /* which way the flow goes (the view's arrow), which way is up (the ground and the top), what L is */
        Rectangle cr = prop_row(x, &y, w, "Flow direction", "The way the air goes, in the STL's axes: the inlet is on the other side. "
                                "A blade or a wing: along its chord, not across its span.", 34);
        int fd = T.m.flowDir;
        if (ui_segmented(ui_id("stl-flow"), (Rectangle){ cr.x, cr.y, fminf(360, cr.width), 34 }, (const char **)DIR_NAME, DIR__N, &fd) && fd != T.m.flowDir) {
            if (T.m.twoD && fd / 2 == T.m.normal2D) ui_toast(TOAST_ERR, "In 2-D the flow is in the plane: not along %c, the axis it is one cell thick across", "XYZ"[T.m.normal2D]);
            else { T.m.flowDir = fd; T.m.upDir = mesher_up(&T.m); save_cfg(); setup_flow_changed(A.cs); }
        }
        /* up: one of the four directions square to the flow */
        const char *upNames[4]; int upDirs[4], nu = 0, cur = 0, ud = mesher_up(&T.m);
        for (int d = 0; d < DIR__N; d++) if (d / 2 != T.m.flowDir / 2) { if (d == ud) cur = nu; upDirs[nu] = d; upNames[nu++] = DIR_NAME[d]; }
        cr = prop_row(x, &y, w, "Up", "Where the top of the tunnel is; the ground (if the body sits on it) is the other way.", 34);
        if (ui_segmented(ui_id("stl-updir"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, upNames, nu, &cur) && upDirs[cur] != ud) {
            T.m.upDir = upDirs[cur]; save_cfg();
        }
        double Lauto = T.stl.bmax[T.m.flowDir / 2] - T.stl.bmin[T.m.flowDir / 2];
        char a[96];
        pl_fmt_ui(a, sizeof a, T.m.refLen > 0 ? "L = %.4g m (the body is %.4g m along the flow)" : "0: the body's length along the flow, L = %.4g m",
                  T.m.refLen > 0 ? T.m.refLen : Lauto, Lauto);
        if (row_num(x, &y, w, "stl-ref", "Reference length L", "The tunnel's distances below are in it. 0 takes the body's length along the flow "
                    "(a blade's chord); type a span or a diameter to measure in that instead.", &T.m.refLen, "%g", 0, 1e6, "m", a)) save_cfg();
        if (row_num(x, &y, w, "stl-up", "Upstream", "Inlet distance ahead of the body.", &T.m.up, "%g", 0, 100, "L", NULL)) save_cfg();
        if (row_num(x, &y, w, "stl-down", "Downstream", "Room for the wake.", &T.m.down, "%g", 0, 200, "L", NULL)) save_cfg();
        if (row_num(x, &y, w, "stl-side", "Sides", T.m.twoD ? "Across the flow, in the plane. (2-D: not across the extrusion, which is one cell.)"
                                                           : "Across the flow, both sides.", &T.m.side, "%g", 0, 100, "L", NULL)) save_cfg();
        if (row_num(x, &y, w, "stl-top", "Above", "Room over the body, toward Up.", &T.m.top, "%g", 0, 100, "L", NULL)) save_cfg();
        ui_disable(T.m.ground);
        if (row_num(x, &y, w, "stl-below", "Below", T.m.ground ? "On the ground: the floor touches the body." : "Room under the body, away from Up.",
                    &T.m.below, "%g", 0, 100, "L", NULL)) save_cfg();
        ui_disable(false);
        cr = prop_row(x, &y, w, "On the ground", "The floor is under the body (on the side away from Up), touching it, and is a wall (a car, a building).", 24);
        if (ui_switch(ui_id("stl-ground"), cr.x, cr.y + 2, &T.m.ground)) save_cfg();
    }
    return y - y0;
}

static float mesher_section(float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "Mesher", NULL);
    int ms = T.m.mesher;
    if (ui_select(ui_id("stl-mesher"), (Rectangle){ x, y, 360, 36 }, MESHER_NAME, MESHER__N, &ms) && ms != T.m.mesher) { T.m.mesher = ms; save_cfg(); }
    if (salome_mesher() && !T.m.twoD) {                  /* 2-D: SALOME always builds the profile's face as CAD */
        float sx = x + 380;
        if (T.m.mesher == MESHER_BODYFIT) ui_disable(true);
        if (ui_switch(ui_id("stl-remesh"), sx, y + 8, &T.m.remesh)) save_cfg();
        ui_disable(false);
        ui_text(F_MED, 13, "Remesh surface", sx + 46, y + 9, TH.text2);
        ui_tooltip((Rectangle){ sx, y, 180, 36 }, "Off: NETGEN meshes on the STL's own triangles (fast, any size); they are remeshed at the target size "
                   "first when they are much coarser than it or many are slivers. On: SALOME builds CAD from the STL and NETGEN remeshes "
                   "the surface at the target size (best for CAD-made STL with flat faces; slow above a few thousand triangles; needed "
                   "for prism layers).");
    }
    y += 44;
    static const char *const WHAT[MESHER__N] = {
        "Hexahedra trimmed at the surface, with prism layers on any part. Runs here, with OpenFOAM. The usual choice.",
        "SALOME's NETGEN tetrahedra: fills any closed shape, the cells are many and less accurate per cell.",
        "NETGEN tetrahedra joined into polyhedra by OpenFOAM's polyDualMesh: fewer cells, good on complex parts.",
        "SALOME's body-fitted hexahedra (cut cells at the surface): no prism layers." };
    static const char *const WHAT2D[MESHER__N] = {
        "2-D: hexahedra trimmed at the profile, made on a slab and cut to one cell. Its boundary layers can come out patchy on a thin "
        "profile; SALOME's 2-D meshers make them all round.",
        "2-D: SALOME cuts the profile out as CAD, meshes it with NETGEN triangles and boundary layers that follow the outline, and "
        "extrudes one cell (prisms). The best choice for an airfoil.",
        "2-D: as Tetrahedral (triangles, extruded to prisms): polyhedra would span the depth.",
        "2-D: as Tetrahedral, but mostly quadrilaterals (extruded to hexahedra), with the same boundary layers." };
    y += note(x, y, w, T.m.twoD ? WHAT2D[T.m.mesher] : WHAT[T.m.mesher]);
    /* the one to use, for someone who doesn't know: SALOME's for a 2-D profile (layers all round), else snappyHexMesh */
    {
        int rec = T.m.twoD && A.env.salome[0] ? MESHER_TET : MESHER_SNAPPY;
        static const char *const SHORT[MESHER__N] = { "snappyHexMesh", "SALOME tetrahedral", "SALOME polyhedral", "SALOME body-fitted" };
        char t[160];
        if (rec == T.m.mesher) {
            pl_fmt_ui(t, sizeof t, "Recommended for this part: %s.", SHORT[rec]);
            ui_icon(ICON_CIRCLE_CHECK, x + 9, y + 9, 15, TH.ok);
            ui_text(F_MED, 13, t, x + 24, y + 1, TH.ok);
            y += 28;
        } else {
            pl_fmt_ui(t, sizeof t, "Recommended for this part: %s%s.", SHORT[rec], T.m.twoD ? " (boundary layers all round the profile)" : "");
            ui_icon(ICON_SPARKLES, x + 9, y + 15, 15, TH.accent);
            ui_text_clip(F_MED, 13, t, x + 24, y + 7, w - 140, TH.text2);
            float uw = ui_button_width(NULL, "Use it");
            if (ui_button(ui_id("stl-use-rec"), (Rectangle){ x + fminf(w - uw, 24 + ui_text_w(F_MED, 13, t) + 14), y, uw, 30 }, NULL, "Use it", BTN_SECONDARY)) {
                T.m.mesher = rec; save_cfg();
            }
            y += 40;
        }
    }
    if (salome_mesher() && !A.env.salome[0]) {
        const char *t = "SALOME is not installed";
        const char *d = "Run bash ~/q-continuum_alpha2.0/tools/install_salome.sh (it downloads the official tarball and unpacks it into ~/salome; "
                        "if the site refuses the script, it opens the download page and waits for the file) and restart Q-Continuum. "
                        "Until then, the trimmed mesher works.";
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
    y += section_head(x, y, w, "Cell size", "Everything is sized from the base size, as in STAR-CCM+. Pick a density to start from; the "
                                           "numbers under it are what it sets.");
    /* the density: the defaults for this part and flow, the base size doubled or halved */
    {
        MeshCfg def = *m;
        mesher_size_defaults(&def, &T.stl, part_thickness());
        static const char *dn[] = { "Coarse", "Medium", "Fine", "Custom" };
        static const double dk[] = { 2, 1, 0.5 };
        int cur = 3;
        for (int i = 0; i < 3; i++)
            if (fabs(m->base - def.base * dk[i]) < 1e-9 * def.base && fabs(m->targetPct - def.targetPct) < 1e-9 && fabs(m->minPct - def.minPct) < 1e-9) cur = i;
        int pick = cur;
        if (ui_segmented(ui_id("stl-density"), (Rectangle){ x, y, fminf(420, w), 34 }, dn, 4, &pick) && pick != cur && pick < 3) {
            m->base = def.base * dk[pick]; m->targetPct = def.targetPct; m->minPct = def.minPct; m->prismPct = def.prismPct;
            save_cfg();
        } else if (pick == 3 && cur != 3) T.adv = true;   /* Custom: the numbers to change are below */
        y += 44;
        /* what that means, in numbers anyone can picture */
        double Lp = 0; for (int k = 0; k < 3; k++) Lp = fmax(Lp, T.stl.bmax[k] - T.stl.bmin[k]);
        double hs = mesher_surface_cell(m), n = mesher_estimate_cells(m, &T.stl);
        char t[200], cl[64]; fmt_cells(cl, sizeof cl, n);
        pl_fmt_ui(t, sizeof t, "About %.0f cells along the part's length at its surface, %s in all. Finer is more accurate and slower to run.",
                  hs > 0 ? Lp / hs : 0, cl);
        y += note(x, y, w, t);
    }
    char a[96], l1[32], l2[32];
    /* a part thinner than its surface cells: snappyHexMesh can't carve it out */
    double thick = part_thickness();
    int ta = stl_thin_axis(&T.stl);
    double text = T.stl.bmax[ta] - T.stl.bmin[ta];
    bool extrusion = thick > 0 && fabs(text - thick) < 0.2 * text;   /* the thinnest is the whole part along an axis: a profile extruded */
    if (m->twoD && extrusion && ta == m->normal2D) thick = 0;          /* 2-D: that depth is one cell anyway */
    if (thick > 0 && !mesher_fits(m, thick)) {
        char t2[32], ts[32], d[500];
        fmt_len(t2, sizeof t2, thick); fmt_len(ts, sizeof ts, m->base * m->targetPct / 100);
        pl_fmt_ui(d, sizeof d, "The part is about %s thick in places, but its surface cells are %s. snappyHexMesh can't carve a body thinner "
                  "than the cells it cuts: it comes out as a sliver, or not at all. Fit the sizes makes the surface cells smaller than the "
                  "thickness (whole refinement levels), the base size staying.", t2, ts);
        if (extrusion && !m->twoD) {
            char e2[300]; pl_fmt_ui(e2, sizeof e2, " It is the whole part along %c, so it looks like a profile extruded %s: Mesh it 2-D makes it one cell "
                                   "thick across %c instead (far fewer cells).", "XYZ"[ta], t2, "XYZ"[ta]);
            pl_strlcpy(d + strlen(d), e2, sizeof d - strlen(d));
        }
        float ch = callout_height(w, "Thinner than its cells", d) + 46;
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "Thinner than its cells", d);
        float fw = ui_button_width(ICON_WRENCH, "Fit the sizes"), bx = x + 42;
        if (extrusion && !m->twoD) {
            float tw = ui_button_width(ICON_LAYERS, "Mesh it 2-D");
            if (ui_button(ui_id("stl-2d-fix2"), (Rectangle){ bx, y + ch - 44, tw, 34 }, ICON_LAYERS, "Mesh it 2-D", BTN_PRIMARY)) {
                set_two_d(true, ta);
                ui_toast(TOAST_OK, "2-D across %c: about %.0f thousand cells", "XYZ"[ta], mesher_estimate_cells(m, &T.stl) / 1e3);
            }
            bx += tw + 10;
        }
        if (ui_button(ui_id("stl-fit"), (Rectangle){ bx, y + ch - 44, fw, 34 }, ICON_WRENCH, "Fit the sizes", extrusion && !m->twoD ? BTN_SECONDARY : BTN_PRIMARY)) {
            mesher_fit(m, thick); save_cfg();
            ui_toast(TOAST_OK, "Surface cells %g %% and %g %% of the base size: about %.0f thousand cells", m->targetPct, m->minPct,
                     mesher_estimate_cells(m, &T.stl) / 1e3);
        }
        y += ch + 14;
    }
    if (row_num(x, &y, w, "stl-base", "Base size", "The largest cell, away from the surface.", &m->base, "%.4g", 1e-9, 1e6, "m", NULL)) save_cfg();
    fmt_len(l1, sizeof l1, m->base * m->targetPct / 100);
    snprintf(a, sizeof a, "= %s", l1);
    if (row_num(x, &y, w, "stl-target", "Target surface size", "The cell size on the surface.", &m->targetPct, "%g", 0.01, 1000, "%", a)) save_cfg();
    if (!T.adv) return y - y0;
    fmt_len(l2, sizeof l2, m->base * m->minPct / 100);
    snprintf(a, sizeof a, "= %s", l2);
    if (row_num(x, &y, w, "stl-min", "Minimum surface size", "Reached on tight curves and sharp edges.", &m->minPct, "%g", 0.01, 1000, "%", a)) save_cfg();
    if (row_num(x, &y, w, "stl-curv", "Surface curvature", "Cells around a full circle on curved surfaces.", &m->curvature, "%g", 4, 360, "pts/circle", NULL)) save_cfg();
    if (row_num(x, &y, w, "stl-sgrow", "Surface growth rate", "How fast cells grow along the surface (SALOME).", &m->surfGrowth, "%g", 1.01, 3, NULL, NULL)) save_cfg();
    if (row_num(x, &y, w, "stl-vgrow", "Volume growth rate", "How fast cells grow away from the surface: lower is smoother and more cells.", &m->volGrowth, "%g", 1.01, 3, NULL, NULL)) save_cfg();
    if (row_num(x, &y, w, "stl-feat", "Feature angle", "Edges sharper than this are kept crisp (snapped to, not smoothed over).", &m->featureAngle, "%g", 1, 180, "°", NULL)) save_cfg();
    return y - y0;
}

/* SALOME's layers (assets/salome/stl_mesher.py): on its 2-D face always; in 3-D on the part rebuilt as CAD (Remesh surface),
   or grown out of the STL's own surface for a body in a wind tunnel that is all walls, off the ground; never Body Fitting's */
static bool salome_layers(const MeshCfg *m)
{
    if (m->twoD) return true;
    if (m->mesher == MESHER_BODYFIT) return false;
    if (m->remesh) return true;
    if (m->domain != DOMAIN_EXTERNAL || m->ground || !T.loaded) return false;
    for (int i = 0; i < T.stl.nRegions; i++) if (T.stl.reg[i].type != BT_WALL) return false;
    return true;
}

static float prism_section(float x, float y, float w)
{
    float y0 = y;
    MeshCfg *m = &T.m;
    bool none = salome_mesher() && !salome_layers(m);
    y += section_head(x, y, w, "Prism layers", m->twoD ? "Thin cells along the walls, for the boundary layer: in 2-D they follow the profile's outline."
                                                      : "Thin cells stacked on the walls, for the boundary layer.");
    Rectangle cr = prop_row(x, &y, w, "Prism layer mesher", NULL, 24);
    if (ui_switch(ui_id("stl-prism"), cr.x, cr.y + 2, &m->prism)) save_cfg();
    if (!m->prism) return y - y0;
    if (none) {                                           /* SALOME 3-D on the STL's triangles: viscous layers need CAD faces */
        const char *t2 = m->mesher == MESHER_BODYFIT ? "Body Fitting makes no prism layers" : "This mesh will have no prism layers";
        const char *d2 = m->mesher == MESHER_BODYFIT
            ? "Use the tetrahedral or polyhedral mesher with Remesh surface on (a CAD-made part with flat faces), or snappyHexMesh."
            : "On the STL's own triangles SALOME grows prism layers for a body in a wind tunnel whose boundaries are all walls, off the "
              "ground. Here (internal flow, an inlet or outlet on the part, or on the ground) switch Remesh surface on for a CAD-made "
              "part with flat faces, or use snappyHexMesh, which makes them on any part.";
        float ch = callout_height(w, t2, d2);
        callout((Rectangle){ x, y + 4, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, t2, d2);
        return y + ch + 18 - y0;
    }
    double n = m->layers;
    if (row_num(x, &y, w, "stl-nl", "Number of prism layers", NULL, &n, "%g", 1, 50, NULL, NULL)) { m->layers = (int)n; save_cfg(); }
    /* both meshers hold the outer layer to a share of the surface cell next to it (stlmesh.h mesher_layer_rel) */
    if (m->mesher == MESHER_SNAPPY) {
        /* snappy's layers collapse where the surface cells halve: a coarse STL's facet corners and the curvature refinement
           make steps all along a profile, and the layers broke at each (user's airfoil, 2026-10-26: 12 of 20 layers) */
        bool even = mesher_even_layers(m), was = even;
        Rectangle er = prop_row(x, &y, w, "Even surface cells under the layers",
                                "The walls with layers all at the minimum surface size, so the layers don't break where the size steps. "
                                "More cells; on by itself in 2-D.", 24);
        ui_switch(ui_id("stl-even"), er.x, er.y + 2, &even);
        if (even != was) { m->evenLayers = even == m->twoD ? -1 : even; save_cfg(); }
    }
    double T0 = m->base * m->prismPct / 100, h = mesher_layer_cell(m), Tm = mesher_layer_rel(m, NULL) * h;
    char a[96], l[32], t0[32]; fmt_len(t0, sizeof t0, T0); snprintf(a, sizeof a, "= %s", t0);
    if (row_num(x, &y, w, "stl-pt", "Prism layer total thickness", "Of the base size.", &m->prismPct, "%g", 0.1, 500, "%", a)) save_cfg();
    double r = m->stretch, first = r == 1 ? Tm / m->layers : Tm * (r - 1) / (pow(r, m->layers) - 1);
    fmt_len(l, sizeof l, first); snprintf(a, sizeof a, "first layer %s", l);
    if (row_num(x, &y, w, "stl-ps", "Prism layer stretching", "Each layer this much thicker than the one below.", &m->stretch, "%g", 1, 3, NULL, a)) save_cfg();
    char tl[32], hl[32], t2[480];
    fmt_len(tl, sizeof tl, Tm); fmt_len(hl, sizeof hl, h);
    if (fabs(Tm - T0) > 0.02 * T0)
        pl_fmt_ui(t2, sizeof t2, "%s makes these %s thick, not %s: the outer layer has to be %.0f%% to %.0f%% of the %s surface cells next to "
                  "it. Thinner, the jump from it to those cells fails %s and makes the flow solver diverge there. Fewer layers or more "
                  "stretching keep the thickness you asked for.", salome_mesher() ? "SALOME" : "snappyHexMesh", tl, t0,
                  LAYER_OUTER_MIN * 100, LAYER_OUTER_MAX * 100, hl, salome_mesher() ? "the solver" : "snappyHexMesh's quality checks");
    else if (salome_mesher())
        pl_fmt_ui(t2, sizeof t2, "%s on the walls, the outer one about the size of the %s surface cells beyond it.%s", tl, hl,
                  m->twoD ? "" : " SALOME's layers hold on flat CAD faces; on a curved part made of STL facets NETGEN can fail with them: the "
                                 "script then meshes without them and says so.");
    else if (mesher_even_layers(m))
        pl_fmt_ui(t2, sizeof t2, "snappyHexMesh sizes them to the %s surface cells, the same all along the layered walls, so they run "
                  "unbroken.", hl);
    else
        pl_fmt_ui(t2, sizeof t2, "snappyHexMesh sizes them to the %s surface cells: where the surface is refined further (curvature, the "
                  "minimum size) they thin with its cells, and can break off where the size steps (Even surface cells holds them).", hl);
    y += note(x, y + 4, w, t2);
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

/* ---------------------------------------------------------------------------- 4 Mesh settings: the mesher, the sizes, the layers */
static float mesh_step(float x, float y, float w)
{
    float y0 = y;
    y += mesher_section(x, y, w);
    y += sizes_section(x, y + 6, w) + 6;
    y += prism_section(x, y + 6, w) + 6;
    /* the rest when wanted: growth, curvature, feature angle (in the sizes above) and the refinement regions */
    if (T.m.nZones > 0) T.adv = true;
    ui_switch(ui_id("stl-adv"), x, y + 10, &T.adv);
    ui_text(F_MED, 13.5f, "Advanced: minimum size, curvature, growth, feature angle, refinement regions", x + 46, y + 11, TH.text2);
    y += 44;
    if (T.adv) y += zones_section(x, y + 6, w) + 6;
    return y - y0;
}

static double cells_estimate(void) { return mesher_estimate_cells(&T.m, &T.stl); }

static void fmt_cells(char *b, size_t cap, double n)
{
    if (n >= 1e6) pl_fmt_ui(b, cap, "about %.1f million cells", n / 1e6);
    else pl_fmt_ui(b, cap, "about %.0f thousand cells", fmax(1, n / 1e3));
}

/* ---------------------------------------------------------------------------- 5 Generate */
static float generate_step(Case *c, float x, float y, float w)
{
    float y0 = y;
    bool running = A.cj->active;
    double n = cells_estimate();
    char est[160], e2[96]; fmt_cells(e2, sizeof e2, n);
    e2[0] = 'A';
    pl_fmt_ui(est, sizeof est, "%s (a rough estimate)", e2);
    ui_text(F_SEMI, 16, est, x, y, n > 2e7 ? TH.warn : TH.text);
    if (n > 2e7) ui_text(F_REG, 13, "That is a lot for one machine: Coarse, or a larger base size, brings it down (Mesh settings).", x, y + 24, TH.text3);
    y += n > 2e7 ? 50 : 34;
    /* where the mesh on disk stands against these settings */
    int cur = c->s.meshed ? stlmesh_mesh_current(c) : -1;
    if (c->s.meshed && cur == 0 && !running) {
        const char *d = "The settings changed after the mesh on disk was made from them: generate it again for them to count.";
        float ch = callout_height(w, NULL, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, NULL, d);
        y += ch + 12;
    }
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
        ? "Generate runs salome/stl_mesh.py in SALOME without its window, converts salome/mesh.unv with ideasUnvToFoam (a mesh with "
          "polyhedra, Body Fitting's cut cells, comes as salome/polyMesh instead), sets the boundary types%s and runs checkMesh. Open in SALOME runs the same script in SALOME's window, to look at the groups and the mesh."
        : "Generate writes the dictionaries (the ones there before are kept as .orig) and runs surfaceFeatureExtract, blockMesh for the "
          "background, snappyHexMesh and checkMesh. Open in SALOME meshes the same STL with SALOME's script instead, in its window.%s";
    char hb[600]; pl_fmt_ui(hb, sizeof hb, how, T.m.mesher == MESHER_POLY ? ", turns the tetrahedra into polyhedra (polyDualMesh)" : "");
    y += note(x, y, w, hb) + 4;
    if (!running && A.cj->failed && !strcmp(A.cj->title, "Mesh") && !strcmp(A.cj->casePath, c->s.path)) {
        /* it failed: which step, what OpenFOAM said, and what usually fixes it, in words a beginner can act on */
        const JobStep *fs = NULL;
        for (int i = 0; i < A.cj->n && !fs; i++) if (A.cj->steps[i].st == JS_FAILED) fs = &A.cj->steps[i];
        char said[400] = "";
        if (fs && fs->log[0]) {
            int li = logset_find(&A.logs, fs->log);
            if (li >= 0 && A.logs.logs[li].fatalMsg[0]) pl_fmt_ui(said, sizeof said, " OpenFOAM said: %s", A.logs.logs[li].fatalMsg);
        }
        const char *id = fs ? fs->id : "", *fix =
            !strcmp(id, "snappyHexMesh") ? "This is usually a gap in the STL (Geometry says if it isn't closed) or a part thinner than its cells: try Fine under Cell size, or SALOME as the mesher."
            : !strcmp(id, "salome") ? "Check that Geometry says the part is closed; a smaller base size, or snappyHexMesh as the mesher, often gets past it."
            : !strcmp(id, "checkMesh") ? "The mesh was made but has bad cells: Fine under Cell size, or fewer prism layers, usually helps."
            : "A setting the tool could not read: Write files only, then look at the dictionary in system/ on the Files page.";
        char t[900]; pl_fmt_ui(t, sizeof t, "It stopped at %s.%s %s", fs ? fs->label : "a step", said, fix);
        float ch = callout_height(w, "The mesh could not be made", t) + 46;
        callout((Rectangle){ x, y, w, ch }, TOAST_ERR, ICON_TRIANGLE_ALERT, "The mesh could not be made", t);
        float lw = ui_button_width(ICON_FILE_TEXT, "Show the log");
        if (ui_button(ui_id("stl-fail-log"), (Rectangle){ x + 42, y + ch - 44, lw, 34 }, ICON_FILE_TEXT, "Show the log", BTN_SECONDARY)) app_go(PG_RUN);
        float rw = ui_button_width(ICON_REFRESH_CW, "Try again");
        if (ui_button(ui_id("stl-fail-retry"), (Rectangle){ x + 52 + lw, y + ch - 44, rw, 34 }, ICON_REFRESH_CW, "Try again", BTN_GHOST)) generate(c);
        y += ch + 14;
    }
    if (running && !strcmp(A.cj->title, "Mesh")) {
        char t[200]; pl_fmt_ui(t, sizeof t, "Step %d of %d: %s. The Run page shows the log.", A.cj->cur + 1, A.cj->n,
                               A.cj->steps[A.cj->cur < A.cj->n ? A.cj->cur : 0].label);
        float ch = callout_height(w, NULL, t);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_LOADER_CIRCLE, NULL, t);
        y += ch + 16;
    } else if (c->s.meshed) {
        /* what comes after a mesh: look at it, then the physics */
        float vw = ui_button_width(ICON_EYE, "View the mesh"), pw = ui_button_width(ICON_ATOM, "Next: Physics > Setup");
        if (ui_button(ui_id("stl-view-mesh"), (Rectangle){ x, y, vw, 36 }, ICON_EYE, "View the mesh", BTN_SECONDARY)) A.meshTab = 2;
        if (ui_button(ui_id("stl-to-physics"), (Rectangle){ x + vw + 10, y, pw, 36 }, ICON_ATOM, "Next: Physics > Setup", BTN_PRIMARY)) {
            A.physicsTab = 2; app_go(PG_PHYSICS);
        }
        y += 50;
    }
    if (c->s.meshed && !running) {                        /* the mesh itself, a slice through the part, right here */
        y += section_head(x, y + 6, w, "Your mesh", "A slice through the middle of the part (the STL's outline drawn over it). Drag to move, "
                                                     "wheel to zoom; View has it in 3-D.") + 6;
        if (A.set.meshView) y += xsection_panel((Rectangle){ x, y, w, 440 }, c, true) + 14;
        else {
            float bw2 = ui_button_width(ICON_EYE, "Show the mesh");
            if (ui_button(ui_id("stl-show-mesh"), (Rectangle){ x, y, bw2, 34 }, ICON_EYE, "Show the mesh", BTN_SECONDARY)) { A.set.meshView = true; settings_save(&A.set); }
            y += 48;
        }
    }
    if (c->s.meshed) { y += 6; y += page_mesh_patches(x, y, w); }
    return y - y0;
}

/* ============================================================================ the flow: a step's head, the page */
static const char *const STEP_TITLE[ST__N] = { "Geometry", "Flow", "Boundaries", "Mesh settings", "Generate" };
/* what each step is for, in a sentence a first-year student needs no CFD for */
static const char *const STEP_WHY[ST__N] = {
    "Load the 3-D model of your part (an STL file, which any CAD program can save) and check its size is right.",
    "Say whether the fluid goes through the part or around it. That decides the space the simulation fills.",
    "Name the part's surfaces and say what each one is: where the fluid comes in, where it leaves, and the solid walls.",
    "Choose how fine the mesh is: smaller cells give a more accurate answer but take longer. Medium is a good start.",
    "Build the mesh: the space is cut into small cells the flow is solved in. Then look at it, and go on to the physics." };

/* what a step folded says it holds */
static void step_summary(Case *c, int k, char *b, size_t cap)
{
    b[0] = 0;
    if (!T.loaded) { if (k == ST_GEOM) pl_strlcpy(b, "Choose the STL to mesh", cap); return; }
    const Stl *s = &T.stl;
    if (k == ST_GEOM) {
        char l[32]; double L = 0; for (int q = 0; q < 3; q++) L = fmax(L, s->bmax[q] - s->bmin[q]);
        fmt_len(l, sizeof l, L);
        pl_fmt_ui(b, cap, "%s  ·  %s across  ·  %s", pl_basename(T.m.stl), l, s->openEdges == 0 && s->nonManifold == 0 ? "closed" : "not watertight");
    } else if (k == ST_FLOW) {
        if (T.m.domain == DOMAIN_INTERNAL) pl_fmt_ui(b, cap, "Internal flow%s", T.m.twoD ? TextFormat(", 2-D across %c", "XYZ"[T.m.normal2D]) : "");
        else pl_fmt_ui(b, cap, "External: flow %s, up %s%s%s", DIR_NAME[T.m.flowDir], DIR_NAME[mesher_up(&T.m)], T.m.ground ? ", on the ground" : "",
                       T.m.twoD ? TextFormat(", 2-D across %c", "XYZ"[T.m.normal2D]) : "");
    } else if (k == ST_BOUND) {
        int in, out, wall, other; boundary_counts(&in, &out, &wall, &other);
        if (T.m.domain == DOMAIN_EXTERNAL && !in && !out)
            pl_fmt_ui(b, cap, "%d wall%s; the wind tunnel brings the inlet, outlet and far field", wall, wall == 1 ? "" : "s");
        else pl_fmt_ui(b, cap, "%d inlet%s, %d outlet%s, %d wall%s", in, in == 1 ? "" : "s", out, out == 1 ? "" : "s", wall, wall == 1 ? "" : "s");
    } else if (k == ST_MESH) {
        char bs[32], cl[96]; fmt_len(bs, sizeof bs, T.m.base); fmt_cells(cl, sizeof cl, cells_estimate());
        static const char *const SHORT[MESHER__N] = { "snappyHexMesh", "Tetrahedra", "Polyhedra", "Body fitting" };
        pl_fmt_ui(b, cap, "%s  ·  base %s  ·  %s  ·  %s", SHORT[T.m.mesher], bs, T.m.prism && T.m.mesher != MESHER_BODYFIT ? TextFormat("%d prism layers", T.m.layers) : "no prism layers", cl);
    } else {
        bool meshing = A.cj->active && !strcmp(A.cj->casePath, c->s.path) && !strcmp(A.cj->title, "Mesh");
        int cur = c->s.meshed ? stlmesh_mesh_current(c) : -1;
        if (meshing) pl_fmt_ui(b, cap, "Meshing: %s ...", A.cj->steps[A.cj->cur < A.cj->n ? A.cj->cur : 0].label);
        else if (!c->s.meshed) pl_strlcpy(b, "Not generated yet", cap);
        else if (cur == 0) pl_strlcpy(b, "Settings changed: generate again", cap);
        else if (c->s.nCells > 0) pl_fmt_ui(b, cap, "%ld cells on disk", c->s.nCells);
        else pl_strlcpy(b, "Generated", cap);
    }
}

/* a step's head: its number (a tick once passed), its title and, folded, what it holds. A step not reached yet is
   greyed: it opens when the one before is done. Returns the head's height. */
static float step_head(Case *c, int k, float x, float y, float w, float *cy)
{
    bool open = T.open == k, reached = k <= T.reached, passed = k < T.reached && !open;
    float h = 50;
    Rectangle r = { x, y, w, h };
    UiId id = ui_idn(ui_id("stl-step"), k);
    ui_disable(!reached);
    if (ui_row(id, r, open) && reached && !open) step_open(k);   /* one step is always open */
    ui_disable(false);
    ui_register(id, r, STEP_TITLE[k]);
    *cy = y + h / 2;
    float cx = x + 22, rad = 15;
    Color ring = !reached ? TH.lineSoft : open ? TH.accent : passed ? TH.ok : TH.text3;
    DrawCircleV((Vector2){ cx, *cy }, rad, open ? TH.accent : passed ? ui_alpha(TH.ok, 0.16f) : TH.surface2);
    DrawCircleLinesV((Vector2){ cx, *cy }, rad, ring);
    if (passed) ui_icon(ICON_CHECK, cx, *cy, 16, TH.ok);
    else { char n[4]; snprintf(n, sizeof n, "%d", k + 1); ui_text_center(F_SEMI, 14, n, (Rectangle){ cx - rad, *cy - rad, 2 * rad, 2 * rad }, open ? TH.surface : reached ? TH.text : TH.text3); }
    float tx = x + 52;
    const char *title = ui_cap(STEP_TITLE[k]);
    ui_text(F_SEMI, 17, title, tx, *cy - 11, reached ? TH.text : TH.text3);
    float tw = ui_text_w(F_SEMI, 17, title);
    char sum[300];
    if (!reached) pl_fmt_ui(sum, sizeof sum, "after %s", STEP_TITLE[k - 1]);
    else step_summary(c, k, sum, sizeof sum);
    if (sum[0] && !open) ui_text_clip(F_REG, 13.5f, sum, tx + tw + 16, *cy - 8, w - (tx - x) - tw - 60, reached ? TH.text2 : TH.text3);
    if (reached && !open) ui_icon(ICON_PENCIL, x + w - 22, *cy, 15, TH.text3);
    return h;
}

/* the open step's way on: what it leads to, and why not yet */
static float step_next(int k, float x, float y, float w)
{
    if (k >= ST_GEN) return 0;
    char lab[64]; pl_fmt_ui(lab, sizeof lab, "Next: %s", STEP_TITLE[k + 1]);
    ui_disable(!T.loaded);
    float bw = ui_button_width(ICON_ARROW_RIGHT, lab);
    if (ui_button(ui_id("stl-next"), (Rectangle){ x, y, bw, 40 }, ICON_ARROW_RIGHT, lab, BTN_PRIMARY)) step_open(k + 1);
    ui_disable(false);
    if (k + 1 < T.reached) {
        float jw = ui_button_width(ICON_PLAY, "Go to Generate");
        if (ui_button(ui_id("stl-skip"), (Rectangle){ x + bw + 10, y, jw, 40 }, ICON_PLAY, "Go to Generate", BTN_GHOST)) step_open(ST_GEN);
    }
    (void)w;
    return 50;
}

float page_stlmesh(Rectangle r, float x, float y, float w)
{
    Case *c = A.cs;
    float y0 = y;
    poll_picker();
    sync_case(c);
    if (!T.loaded) T.reached = T.open = ST_GEOM;
    float cys[ST__N];
    float bx = x + 52, bw = w - 52;
    for (int k = 0; k < ST__N; k++) {
        if (T.open == k && T.scrollTo == 1) {             /* the step just opened: its head to the top of the page */
            UiScroll *sc = &A.scr[4];
            ui_scroll_to(sc, y - r.y + sc->y - 16);
            T.scrollTo = 0;
        }
        y += step_head(c, k, x, y, w, &cys[k]);
        if (T.open == k) {
            y += 2;
            y += note(bx, y, bw, STEP_WHY[k]) + 2;
            /* the part beside the settings (a wide page) or above them, with what this step adds to it */
            /* beside the settings when they keep 640 px of their own, else above them, the page's width */
            bool side = T.loaded && (k == ST_GEOM || k == ST_FLOW || k == ST_MESH);
            float sx = bx, sw = bw, top = y, vh = 0;
            if (side) {
                float vw = fminf(620, fmaxf(380, bw * 0.4f));
                if (bw - vw - 20 >= 640) { sw = bw - vw - 20; vh = fminf(440, vw * 0.85f); view_panel((Rectangle){ bx + sw + 20, y, vw, vh }); }
                else { view_panel((Rectangle){ bx, y, bw, fminf(360, bw * 0.5f) }); y += fminf(360, bw * 0.5f) + 14; top = y; }
            }
            if (k == ST_GEOM) y += geometry_step(c, sx, y, sw);
            else if (k == ST_FLOW) y += flow_step(sx, y, sw);
            else if (k == ST_BOUND) y += boundaries_step(c, bx, y, bw);
            else if (k == ST_MESH) y += mesh_step(sx, y, sw);
            else y += generate_step(c, bx, y, bw);
            if (T.loaded) y += 4 + step_next(k, sx, y + 4, sw);
            if (side && vh > 0 && y < top + vh + 10) y = top + vh + 10;   /* the next head below the view, not under it */
        }
        y += 10;
    }
    if (T.scrollTo == 2) { T.scrollTo = 1; ui_request_frames(2); }
    /* the line joining the steps, the part done in the accent */
    for (int k = 0; k + 1 < ST__N; k++) {
        float a = cys[k] + 17, b = cys[k + 1] - 17;
        if (b > a) ui_line(x + 22, a, x + 22, b, 2, k < T.reached ? ui_alpha(TH.accent, 0.7f) : TH.lineSoft);
    }
    return y - y0;
}

bool stlmesh_two_d(Case *c)
{
    sync_case(c);
    return T.loaded && T.m.twoD;
}

/* a Setup template picked before meshing: the mesh it wants (prism layers on the walls for the boundary layer) */
bool stlmesh_template_hints(Case *c, bool layers)
{
    sync_case(c);
    if (!T.loaded || c->s.meshed || !layers || T.m.prism) return false;
    T.m.prism = true;
    save_cfg();
    return true;
}

/* the first layer of a layer stack as the mesher writes it: its total (mesher_layer_rel of the surface cell) split by the
   stretch over the count */
static double first_layer(const MeshCfg *m)
{
    double r = m->stretch > 1.0001 ? m->stretch : 1.0001, total = mesher_layer_rel(m, NULL) * mesher_layer_cell(m);
    return total * (r - 1) / (pow(r, m->layers) - 1);
}

bool stlmesh_wall_layers(Case *c, double *first, int *n, double *cell)
{
    sync_case(c);
    if (!T.loaded) return false;
    if (cell) *cell = mesher_layer_cell(&T.m);
    if (!T.m.prism || T.m.layers < 1) return false;
    if (first) *first = first_layer(&T.m);
    if (n) *n = T.m.layers;
    return true;
}

bool stlmesh_set_wall_layers(Case *c, double y1, double *first, int *n)
{
    sync_case(c);
    if (!T.loaded || !(y1 > 0)) return false;
    double h = mesher_layer_cell(&T.m), r = T.m.stretch > 1.0001 ? T.m.stretch : 1.2, total;
    int k = sim_layer_count(y1, 0.5 * h, r, &total);
    if (k >= 60) return false;
    MeshCfg m = T.m;
    m.prism = true; m.layers = k; m.stretch = r; m.prismPct = 100 * total / m.base;
    if (first) *first = first_layer(&m);
    if (n) *n = k;
    T.m = m;
    save_cfg();
    return true;
}

bool stlmesh_gravity(Case *c, double g[3])
{
    sync_case(c);
    if (!T.loaded) return false;
    int up = mesher_up(&T.m);                               /* the tunnel's Up (or CAD's z): gravity points the other way */
    g[0] = g[1] = g[2] = 0;
    g[up / 2] = up % 2 ? 9.81 : -9.81;
    return true;
}

int stlmesh_up_axis(Case *c)
{
    sync_case(c);
    if (!T.loaded) return -1;
    return SV.up >= 0 ? SV.up : mesher_up(&T.m) / 2;
}
