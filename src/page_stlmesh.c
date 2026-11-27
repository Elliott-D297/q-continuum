/* page_stlmesh.c -- the Mesh page's STL mesher tab (user request, 2026-10-23): choose any STL, see what it is (size,
 * closed or not, its boundaries split by angle), set the mesh up the way STAR-CCM+'s automated mesher does, and mesh it
 * with snappyHexMesh here or with SALOME (NETGEN tetrahedra, polyhedra through polyDualMesh, Body Fitting hexahedra).
 * The part is shown in 3-D, a colour a boundary: click a face to pick its boundary, rename it, say what it is (a wall,
 * an inlet ...) and give it its conditions (page_setup.c's editor) right there (user request, 2026-10-24).
 * "Mesh an STL" on the Cases page makes a case for a bare STL and opens this tab with it. The settings live in
 * <case>/mesher/mesher.ini (stlmesh.c), which SALOME's script reads too.
 * The tab is a flow (user request, 2026-10-26): Geometry > Flow > Boundaries > Mesh settings > Generate, one step open at
 * a time, each the next one's way in; a step done folds to a line saying what was chosen. How far a case has got is in
 * <case>/mesher/steps. Everything here belongs to the case it was loaded for (STLP.casePath): a case switch drops it all,
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

StlPage STLP = { .picker = -1, .sel = -1 };            /* the STL mesher page's state (page_stlmesh_int.h) */


/* ============================================================================ the flow's progress: mesher/steps */
static void steps_save(void)
{
    if (!STLP.casePath[0] || !STLP.loaded) return;
    char p[PL_PATH], d[PL_PATH], b[32];
    pl_join(d, sizeof d, STLP.casePath, "mesher");
    pl_join(p, sizeof p, d, "steps");
    snprintf(b, sizeof b, "%d %d\n", STLP.reached, STLP.open);
    char *old = pl_read_file(p, NULL);
    bool same = old && !strcmp(old, b);
    free(old);
    if (!same && pl_mkdirs(d)) pl_write_file(p, b, strlen(b));
}

/* a case meshed from an STL before there was a flow opens with every step there */
static void steps_load(const Case *c)
{
    STLP.reached = STLP.open = ST_GEOM;
    if (!STLP.loaded) return;
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, "mesher/steps");
    char *b = pl_read_file(p, NULL);
    if (!b || sscanf(b, "%d %d", &STLP.reached, &STLP.open) != 2) { STLP.reached = ST_GEN; STLP.open = c->s.meshed ? ST_GEN : ST_MESH; }
    free(b);
    if (STLP.reached < 0 || STLP.reached >= ST__N) STLP.reached = ST_GEN;
    if (STLP.open < 0 || STLP.open > STLP.reached) STLP.open = STLP.reached;
}

static void step_open(int k)
{
    if (k < 0 || k >= ST__N) return;
    if (k > STLP.reached) STLP.reached = k;
    STLP.open = k;
    STLP.scrollTo = 2;
    steps_save();
}

/* ============================================================================ loading */
void sm_save_cfg(void)
{
    if (!STLP.casePath[0]) return;
    mesher_take_names(&STLP.m, &STLP.stl);
    mesher_save(STLP.casePath, &STLP.m);
    STLP.dirty = false;
}

static const char *s_cad;   /* load_stl: the CAD file this STL was converted from (its unit is known: no mm guess) */

static bool load_stl(const char *path, bool fresh)
{
    Stl s;
    if (!stl_load(path, &s)) { ui_toast(TOAST_ERR, "%s", s.err); return false; }
    stl_free(&STLP.stl);
    STLP.stl = s;
    if (fresh && STLP.casePath[0]) { char fp[PL_PATH]; pl_join(fp, sizeof fp, STLP.casePath, "mesher/faces.ini"); unlink(fp); }   /* another part: its own face ids */
    if (STLP.m.scale <= 0) STLP.m.scale = 1;
    stl_set_scale(&STLP.stl, STLP.m.scale);
    stl_split_part(&STLP.stl, STLP.m.splitAngle, STLP.casePath);
    if (fresh) {
        pl_strlcpy(STLP.m.stl, path, sizeof STLP.m.stl);
        pl_strlcpy(STLP.m.cad, s_cad ? s_cad : "", sizeof STLP.m.cad);
        if (s_cad) { STLP.m.scale = 1; stl_set_scale(&STLP.stl, 1); }   /* converted to metres from the file's own unit */
        /* a part a thousand times too large is in millimetres: say so and pick mm */
        double L = 0; for (int k = 0; k < 3; k++) L = fmax(L, STLP.stl.bmax[k] - STLP.stl.bmin[k]);
        if (L > 20 && STLP.m.scale == 1 && !s_cad) { STLP.m.scale = 0.001; stl_set_scale(&STLP.stl, 0.001); stl_split_part(&STLP.stl, STLP.m.splitAngle, STLP.casePath);
                                        ui_toast(TOAST_INFO, "The part is %.0f units across: read as millimetres (Scale changes it)", L); }
        mesher_defaults(&STLP.m, &STLP.stl);
        STLP.m.locPicked = false;                                              /* another part: a point picked on the last means nothing */
        stl_split_part(&STLP.stl, STLP.m.splitAngle, STLP.casePath);                /* split again at the default angle */
        mesher_take_names(&STLP.m, &STLP.stl);
        STLP.sel = -1;
    } else mesher_apply_names(&STLP.m, &STLP.stl);
    STLP.loaded = true;
    sm_save_cfg();
    if (fresh) { STLP.reached = STLP.open = ST_GEOM; steps_save(); }   /* another part: its settings are new, so is the flow */
    return true;
}

void sm_view_forget(void);
void sm_pick_reset(void);

/* the case the page shows: another one drops everything of the last (its STL, settings, picked boundary, a half-typed
   path or name: an input still focused would otherwise commit the old case's text into this one) */
static void sync_case(Case *c)
{
    if (!c || !strcmp(STLP.casePath, c->s.path)) return;
    stl_free(&STLP.stl);
    memset(&STLP.m, 0, sizeof STLP.m);
    STLP.loaded = STLP.dirty = false;
    STLP.sel = -1;
    STLP.adv = false; STLP.removeArm = 0; STLP.scrollTo = 0;
    sm_pick_reset();
    STLP.typed[0] = 0;
    STLP.gen++;
    ui_drop_focus();
    sm_view_forget();
    pl_strlcpy(STLP.casePath, c->s.path, sizeof STLP.casePath);
    if (mesher_load(c->s.path, &STLP.m) && STLP.m.stl[0]) {
        char p[PL_PATH]; pl_strlcpy(p, STLP.m.stl, sizeof p);
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
    STLP.casePath[0] = 0;                                    /* read again: nothing there now */
    sync_case(c);
    case_rescan(c);
    ui_toast(TOAST_OK, "Removed the STL and its mesher settings from %s", c->s.name);
}

/* ============================================================================ the file dialog */
static void start_picker(bool newCase)
{
    if (STLP.picker > 0) { ui_toast(TOAST_INFO, "The file dialog is already open: finish or cancel it first"); return; }
    snprintf(STLP.pickOut, sizeof STLP.pickOut, "/tmp/qc-pick-%d.txt", (int)getpid());
    unlink(STLP.pickOut);
    char q[PL_PATH + 8]; pl_shell_quote(q, sizeof q, STLP.pickOut);
    char cmd[PL_PATH + 1400];
    const char *testPick = getenv("QC_TEST_PICK");          /* testing: the dialog "answers" this path */
    if (testPick && *testPick) {
        char tq[PL_PATH + 8]; pl_shell_quote(tq, sizeof tq, testPick);
        if (!pl_fmt(cmd, sizeof cmd, "printf '%%s' %s > %s", tq, q)) return;
    } else if (A.env.wsl)
        /* Windows' own Open dialog, on top; its path comes back as C:\..., wslpath makes it /mnt/c/... */
        pl_win_open_dialog_cmd(cmd, sizeof cmd, "Choose a part to mesh (STL, STEP, IGES)",
                               "Parts (*.stl;*.step;*.stp;*.iges;*.igs;*.brep)|*.stl;*.STL;*.step;*.STEP;*.stp;*.STP;*.iges;*.IGES;*.igs;*.IGS;*.brep|"
                               "STL (*.stl)|*.stl;*.STL|STEP (*.step;*.stp)|*.step;*.STEP;*.stp;*.STP|All files (*.*)|*.*", q);
    else
        snprintf(cmd, sizeof cmd,
                 "{ zenity --file-selection --title='Choose a part to mesh (STL, STEP, IGES)' "
                 "--file-filter='Parts | *.stl *.STL *.step *.STEP *.stp *.STP *.iges *.IGES *.igs *.IGS *.brep' "
                 "--file-filter='All files | *' 2>/dev/null "
                 "|| kdialog --getopenfilename ~ '*.stl *.STL *.step *.STEP *.stp *.STP *.iges *.IGES *.igs *.IGS *.brep' 2>/dev/null; } > %s; true", q);
    STLP.picker = pl_spawn_shell(cmd, "/dev/null");
    STLP.pickNewCase = newCase;
    pl_strlcpy(STLP.pickCase, A.cs && !newCase ? A.cs->s.path : "", sizeof STLP.pickCase);
    if (STLP.picker <= 0) ui_toast(TOAST_ERR, "Could not open a file dialog: type the part's path instead");
}

/* a new case's folder in the workspace, named after the part (a free name: part, part-2, ...) with its system/ */
static bool new_case_dir(const char *part, char *dir, size_t cap)
{
    char ws[PL_PATH], name[128];
    pl_expand_home(ws, sizeof ws, A.set.workspace);
    pl_strlcpy(name, pl_basename(part), sizeof name);
    char *dot = strrchr(name, '.'); if (dot) *dot = 0;
    for (char *c = name; *c; c++) if (*c == ' ' || *c == '/' || *c == '\'' || *c == '"') *c = '_';
    pl_join(dir, cap, ws, name);
    for (int k = 2; pl_exists(dir) && k < 100; k++) { char n2[160]; snprintf(n2, sizeof n2, "%s-%d", name, k); pl_join(dir, cap, ws, n2); }
    char sys[PL_PATH]; pl_join(sys, sizeof sys, dir, "system");
    if (!pl_mkdirs(sys)) { ui_toast(TOAST_ERR, "Could not make %s", dir); return false; }
    return true;
}

/* the new case made of an STL (cad: the CAD file it was converted from, or NULL) */
static void new_case_finish(const char *dir, const char *stl, const char *cad)
{
    /* the minimal dictionaries go in with the first Generate; a controlDict now, so the folder is a case */
    MeshCfg m; memset(&m, 0, sizeof m); m.scale = 1;
    Stl s;
    if (!stl_load(stl, &s)) { ui_toast(TOAST_ERR, "%s", s.err); return; }
    stl_split_part(&s, 30, dir);                               /* also its box */
    if (!cad) {                                                /* as load_stl: a part a thousand times too large is in millimetres */
        double L = 0; for (int k = 0; k < 3; k++) L = fmax(L, s.bmax[k] - s.bmin[k]);
        if (L > 20) { m.scale = 0.001; stl_set_scale(&s, 0.001); stl_split_part(&s, 30, dir);
                      ui_toast(TOAST_INFO, "The part is %.0f units across: read as millimetres (Scale changes it)", L); }
    }
    mesher_defaults(&m, &s);
    pl_strlcpy(m.stl, stl, sizeof m.stl);
    if (cad) { pl_strlcpy(m.cad, cad, sizeof m.cad); m.remesh = true; }
    char err[256];
    mesher_prepare(dir, &m, &s, err, sizeof err);              /* writes controlDict and friends, the copy of the STL */
    stl_free(&s);
    char sp[PL_PATH]; pl_join(sp, sizeof sp, dir, "mesher/steps");
    pl_write_file(sp, "0 0\n", 4);                             /* a new part: its flow starts at Geometry */
    app_rescan_cases(true);
    app_open_case(dir, PG_MESH);
    A.meshTab = 3;
    pl_strlcpy(STLP.casePath, "", sizeof STLP.casePath);            /* sync_case loads it from the new folder */
    if (A.cs && !strcmp(A.cs->s.path, dir)) sync_case(A.cs);
    ui_toast(TOAST_OK, "Made the case %s for %s", pl_basename(dir), pl_basename(cad ? cad : stl));
}

/* ---------------------------------------------------------------------------- the meshing tutorial's example cases
   (tour.c): the part written into a new case (example-duct, example-sphere, example-airfoil, or -2 ...), its boundaries
   named and typed from its solids, the flow set as the tutorial describes it, opened at Geometry in the STL mesher */
bool stlmesh_example_case(int k)
{
    if (k < 0 || k >= MESH_EX__N) return false;
    const MeshExample *ex = &MESH_EXAMPLES[k];
    char dir[PL_PATH];
    if (!new_case_dir(ex->caseName, dir, sizeof dir)) return false;
    char err[300];
    if (!mesh_example_setup(k, dir, err, sizeof err)) { ui_toast(TOAST_ERR, "%s", err); return false; }
    char sp[PL_PATH]; pl_join(sp, sizeof sp, dir, "mesher/steps");
    pl_write_file(sp, "4 0\n", 4);                            /* every step reachable (the tutorial opens each), Geometry open */
    app_rescan_cases(true);
    app_open_case(dir, PG_MESH);
    A.meshTab = 3;
    pl_strlcpy(STLP.casePath, "", sizeof STLP.casePath);
    if (A.cs && !strcmp(A.cs->s.path, dir)) sync_case(A.cs);
    ui_toast(TOAST_OK, "Made the example case %s", pl_basename(dir));
    return A.cs != NULL;
}

/* the open case is that example (its folder example-duct, example-duct-2 ...) */
bool stlmesh_example_open(int k)
{
    if (!A.cs || k < 0 || k >= MESH_EX__N) return false;
    const char *b = pl_basename(A.cs->s.path), *n = MESH_EXAMPLES[k].caseName;
    size_t L = strlen(n);
    return !strncmp(b, n, L) && (b[L] == 0 || b[L] == '-');
}

/* ============================================================================ CAD files: STEP, IGES, BREP -> STL
   SALOME (assets/salome/cad_to_stl.py) reads the file in its own unit and writes <case>/mesher/<part>.stl in metres, and
   <part>.brep, the exact shape for SALOME's CAD route; without SALOME, Gmsh's command line makes the STL. It runs in the
   background (SALOME starts in seconds); the part loads when it is done. */
static struct {
    pid_t  pid;
    char   cad[PL_PATH], out[PL_PATH], log[PL_PATH], casePath[PL_PATH];
    bool   newCase, fresh;
    double t0;
} CV = { .pid = -1 };

static bool cad_busy(void) { return CV.pid > 0; }

static void cad_start(const char *cad, const char *casePath, bool newCase, bool fresh)
{
    if (cad_busy()) { ui_toast(TOAST_INFO, "Still reading %s: one part at a time", pl_basename(CV.cad)); return; }
    char gmsh[PL_PATH] = "";
    if (!A.env.salome[0] && !pl_which("gmsh", gmsh, sizeof gmsh)) {
        ui_toast(TOAST_ERR, "Reading %s needs SALOME (Settings > Programs installs it) or Gmsh; or save the part as STL from your CAD program",
                 pl_basename(cad));
        return;
    }
    char md[PL_PATH], stem[128], name[160], script[PL_PATH];
    pl_join(md, sizeof md, casePath, "mesher");
    if (!pl_mkdirs(md)) { ui_toast(TOAST_ERR, "Could not make %s", md); return; }
    pl_strlcpy(stem, pl_basename(cad), sizeof stem);
    char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
    for (char *c = stem; *c; c++) if (*c == ' ' || *c == '\'' || *c == '"') *c = '_';
    snprintf(name, sizeof name, "%s.stl", stem);
    pl_join(CV.out, sizeof CV.out, md, name);
    pl_join(CV.log, sizeof CV.log, md, "cad.log");
    pl_strlcpy(CV.cad, cad, sizeof CV.cad);
    pl_strlcpy(CV.casePath, casePath, sizeof CV.casePath);
    CV.newCase = newCase; CV.fresh = fresh;
    char qi[PL_PATH + 8], qo[PL_PATH + 8], qs[PL_PATH + 8], qd[PL_PATH + 8], cmd[PL_PATH * 5 + 600];
    pl_shell_quote(qi, sizeof qi, cad);
    pl_shell_quote(qo, sizeof qo, CV.out);
    pl_shell_quote(qd, sizeof qd, md);
    unlink(CV.out);
    if (A.env.salome[0]) {
        pl_join(script, sizeof script, md, "cad_to_stl.py");
        if (!pl_write_file(script, (const char *)asset_cad_to_stl_py, asset_cad_to_stl_py_len)) { ui_toast(TOAST_ERR, "Could not write %s", script); return; }
        char qp[PL_PATH + 8]; pl_shell_quote(qp, sizeof qp, script);
        pl_shell_quote(qs, sizeof qs, A.env.salome);
        if (!pl_fmt(cmd, sizeof cmd, "cd %s && QC_CAD_IN=%s QC_CAD_OUT=%s env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME %s -t --shutdown-servers=1 %s; "
                                  "[ -s %s ]", qd, qi, qo, qs, qp, qo)) { ui_toast(TOAST_ERR, "The path to %s is too long", pl_basename(cad)); return; }
    } else {
        /* Gmsh: OpenCASCADE reads the file's unit into millimetres, scaled to metres (the options go before the file, or
           they come too late for the import); facets from the curvature (36 a circle, as the mesher's default) */
        pl_shell_quote(qs, sizeof qs, gmsh);
        if (!pl_fmt(cmd, sizeof cmd, "cd %s && %s -setnumber Geometry.OCCScaling 0.001 -setnumber Mesh.MeshSizeFromCurvature 36 "
                                  "-setnumber Mesh.Algorithm 6 %s -2 -format stl -bin -o %s && [ -s %s ] "
                                  "&& echo 'Q-Continuum: wrote %s with Gmsh'", qd, qs, qi, qo, qo, name)) { ui_toast(TOAST_ERR, "The path to %s is too long", pl_basename(cad)); return; }
    }
    CV.pid = pl_spawn_shell(cmd, CV.log);
    CV.t0 = GetTime();
    if (CV.pid <= 0) { ui_toast(TOAST_ERR, "Could not start %s", A.env.salome[0] ? "SALOME" : "Gmsh"); return; }
    ui_toast(TOAST_INFO, "Reading %s with %s%s", pl_basename(cad), A.env.salome[0] ? "SALOME" : "Gmsh",
             newCase ? ": the case opens when it is done" : "");
}

/* what the converter said: its summary (the last 'Q-Continuum:' line other than "wrote ..."), else its last line */
static void cad_last_words(char *out, size_t cap)
{
    out[0] = 0;
    char *b = pl_read_file(CV.log, NULL);
    if (!b) return;
    char last[300] = "";
    for (char *line = strtok(b, "\n"); line; line = strtok(NULL, "\n")) {
        char *q = strstr(line, "Q-Continuum:");
        if (q && (strncmp(q + 13, "wrote ", 6) || !out[0])) pl_strlcpy(out, q + 13, cap);
        else if (!q && !strstr(line, "*****") && line[strspn(line, " \t\r")]) pl_strlcpy(last, line, sizeof last);
    }
    free(b);
    if (!out[0]) pl_strlcpy(out, last, cap);
    pl_trim_inplace(out);
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
}

static void cad_poll(void)
{
    if (CV.pid <= 0) return;
    ui_request_frames(2);
    int st;
    if (waitpid(CV.pid, &st, WNOHANG) != CV.pid) return;
    CV.pid = -1;
    bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0 && pl_is_file(CV.out);
    char said[300]; cad_last_words(said, sizeof said);
    if (!ok) { ui_toast(TOAST_ERR, "Could not read %s: %s (mesher/cad.log has it all)", pl_basename(CV.cad), said[0] ? said : "no reason given"); return; }
    if (CV.newCase) { new_case_finish(CV.casePath, CV.out, CV.cad); return; }
    if (!A.cs || strcmp(A.cs->s.path, CV.casePath)) {
        ui_toast(TOAST_INFO, "Not loaded: %s was read for %s, which is no longer open", pl_basename(CV.cad), pl_basename(CV.casePath));
        return;
    }
    sync_case(A.cs);
    s_cad = CV.cad;
    bool loaded = load_stl(CV.out, CV.fresh);
    s_cad = NULL;
    if (loaded && !CV.fresh) { pl_strlcpy(STLP.m.cad, CV.cad, sizeof STLP.m.cad); sm_save_cfg(); }
    if (loaded) ui_toast(TOAST_OK, "%s", said[0] ? said : "Read the CAD file");
}

/* a part chosen for this case: an STL loads now, a CAD file is converted first */
static void take_part(const char *path)
{
    if (stl_is_cad(path)) cad_start(path, STLP.casePath, false, true);
    else load_stl(path, true);
}

static void new_case_from(const char *part)
{
    char dir[PL_PATH];
    if (!stl_is_cad(part)) {
        Stl s;                                                  /* a bad STL makes no folder */
        if (!stl_load(part, &s)) { ui_toast(TOAST_ERR, "%s", s.err); return; }
        stl_free(&s);
    }
    if (!new_case_dir(part, dir, sizeof dir)) return;
    if (stl_is_cad(part)) cad_start(part, dir, true, true);
    else new_case_finish(dir, part, NULL);
}

static void poll_picker(void)
{
    if (STLP.picker <= 0) return;
    ui_request_frames(2);
    int st;
    if (waitpid(STLP.picker, &st, WNOHANG) != STLP.picker) return;
    STLP.picker = -1;
    char *p = pl_read_file(STLP.pickOut, NULL);
    unlink(STLP.pickOut);
    if (!p) return;
    pl_trim_inplace(p);
    if (p[0]) {
        if (STLP.pickNewCase) new_case_from(p);
        else if (!A.cs || strcmp(A.cs->s.path, STLP.pickCase))   /* chosen for a case that is no longer open: not this one's */
            ui_toast(TOAST_INFO, "Not loaded: %s was chosen for %s, which is no longer open", pl_basename(p), pl_basename(STLP.pickCase));
        else { sync_case(A.cs); take_part(p); }
    }
    free(p);
}

void stlmesh_new_case(void) { start_picker(true); }

/* "What mesh do I need?" picks a mesher (and layers) for the open STL case */
bool stlmesh_suggest(Case *c, int mesher, bool layers)
{
    if (!c || mesher < 0 || mesher >= MESHER__N) return false;
    sync_case(c);
    if (!STLP.loaded) return false;                                   /* no part yet: the page asks for one first */
    STLP.m.mesher = mesher;
    if (layers && STLP.m.layers <= 0) STLP.m.layers = 5;
    STLP.m.prism = layers ? true : STLP.m.prism;
    sm_save_cfg();
    return true;
}
void stlmesh_choose(void) { start_picker(false); }
void stlmesh_tick(void) { poll_picker(); cad_poll(); }

/* ============================================================================ meshing */
static bool salome_mesher(void) { return MESHERS[STLP.m.mesher].engine == ENG_SALOME; }

static bool prepare(Case *c)
{
    if (!STLP.loaded) { ui_toast(TOAST_ERR, "Choose an STL first"); return false; }
    char err[300];
    if (!mesher_prepare(c->s.path, &STLP.m, &STLP.stl, err, sizeof err)) { ui_toast(TOAST_ERR, "%s", err); return false; }
    STLP.dirty = false;
    /* hand edits to the dictionaries (plan step 9, E1): kept, or replaced where the settings now say otherwise */
    const char *kept = mesher_write_kept(), *repl = mesher_write_replaced();
    int nk = 0, nr = 0; for (const char *q = kept; *q; q++) nk += *q == '\n'; for (const char *q = repl; *q; q++) nr += *q == '\n';
    if (nr) {
        char first[200]; snprintf(first, sizeof first, "%.*s", (int)strcspn(repl, "\n"), repl);
        ui_toast(TOAST_WARN, "%d hand edit%s replaced, as the mesher's settings now say otherwise (%s%s); %d kept", nr, nr == 1 ? "" : "s", first, nr > 1 ? " ..." : "", nk);
    } else if (nk) {
        char first[200]; snprintf(first, sizeof first, "%.*s", (int)strcspn(kept, "\n"), kept);
        ui_toast(TOAST_INFO, "Kept %d hand edit%s to the mesher's dictionaries (%s%s)", nk, nk == 1 ? "" : "s", first, nk > 1 ? " ..." : "");
    }
    case_rescan(c);
    return true;
}

static void generate(Case *c);
static bool g_layersOnly;
static void generate_after_clean(void) { if (A.cs) generate(A.cs); }
/* Redo the layers only (E3): the kept mesh before the layers, the second stage again with the layer settings now. The flag
   is taken (and cleared) as generate() starts, so a cancelled clean can't leave the next Generate layers-only */
static void redo_layers(Case *c) { g_layersOnly = true; generate(c); }
static void relayer_after_clean(void) { if (A.cs) redo_layers(A.cs); }

/* memory before meshing (plan step 2, gap L10): free memory read every 2 s, not each frame; a mesh whose snappyHexMesh peak
   would pass 90 % of it is held back until "Generate anyway" for that estimate (any way Generate is reached: button, Ctrl+M) */
static double gen_armed_for;
static double free_mb(void)
{
    static double v, at = -10;
    if (GetTime() - at > 2) { v = mem_available_mb(); at = GetTime(); }
    return v;
}
static bool mesh_too_big(double cells, double *needMB)
{
    double need = calc_mesh_mb(cells), avail = free_mb();
    if (needMB) *needMB = need;
    return avail > 0 && need > 0.9 * avail && gen_armed_for != cells;
}

/* ---- snappyHexMesh's stages (plan step 9, E3 and E4). With prism layers it runs twice: castellate and snap (addLayers off),
   the mesh then kept in mesher/snapped/ with the dictionaries that made it, and the layers alone (castellatedMesh and snap
   off) on that mesh. "Redo the layers only" puts the kept mesh back and runs the second stage: layer tuning without a
   remesh. With mesh_processes > 1 each stage runs decomposed (decomposePar -no-fields, mpirun snappyHexMesh -parallel,
   reconstructParMesh -constant) on its own decomposeParDict, the case's own left alone. */
#define SNAP_KEEP "mesher/snapped"
static bool layers_split(void) { return STLP.m.prism && STLP.m.layers > 0 && (MESHERS[STLP.m.mesher].flags & MF_LAYERS); }
static int mesh_procs(void)
{
    int np = STLP.m.meshProcs > 1 ? STLP.m.meshProcs : 1;
    if (A.env.nCpu > 0 && np > A.env.nCpu) np = A.env.nCpu;
    return np;
}

/* one snappyHexMesh run on dictionary dict, serial or decomposed; the layered points moved from 0/polyMesh to constant/ */
static void snappy_run(Str *o, const char *dict, const char *extra, int np)
{
    if (np > 1) {
        str_catf(o, "decomposePar -force -no-fields -decomposeParDict system/decomposeParDict.qc-mesh > log.decomposePar.mesh 2>&1 "
                    "|| { echo 'Q-Continuum: decomposePar failed (log.decomposePar.mesh)'; exit 1; }\n"
                    "OVS=; mpirun --version 2>&1 | grep -q 'Open MPI' && OVS=--oversubscribe\n"
                    "mpirun $OVS %s-np %d snappyHexMesh -parallel -overwrite -decomposeParDict system/decomposeParDict.qc-mesh -dict %s %s || exit 1\n"
                    "for d in processor*; do if [ -f $d/0/polyMesh/points ]; then mv -f $d/0/polyMesh/points $d/constant/polyMesh/points && rm -rf $d/0/polyMesh; fi; done\n"
                    "reconstructParMesh -constant || exit 1\n"          /* (it takes no -decomposeParDict: it reads processor*) */
                    "rm -rf processor*\n", getuid() == 0 ? "--allow-run-as-root " : "", np, dict, extra);
    } else
        str_catf(o, "snappyHexMesh -overwrite -dict %s %s || exit 1\n", dict, extra);
    str_cat(o, "if [ -f 0/polyMesh/points ]; then mv -f 0/polyMesh/points constant/polyMesh/points && rm -rf 0/polyMesh && "
               "echo 'Q-Continuum: moved the layered points from 0/polyMesh to constant/polyMesh'; fi\n");
}

static void snappy_jobs(Case *c, MeshOverrides *ov, bool layersOnly)
{
    int np = mesh_procs();
    if (np > 1) {                                          /* its own decomposition: the run's decomposeParDict stays as it is */
        char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, "system/decomposeParDict.qc-mesh");
        char t[600]; snprintf(t, sizeof t, "FoamFile { version 2.0; format ascii; class dictionary; object decomposeParDict; }\n"
                                         "// Q-Continuum's STL mesher: snappyHexMesh on %d processes (the run has its own decomposeParDict)\n"
                                         "numberOfSubdomains %d;\nmethod scotch;\n", np, np);
        pl_write_file(p, t, strlen(t));
    }
    const char *extra = mo_cmd_extra(ov, "snappyHexMesh");
    bool split = layers_split();
    char np_s[48] = ""; if (np > 1) snprintf(np_s, sizeof np_s, ", %d processes", np);
    if (!layersOnly) {
        char sfe[200], bm[200];
        snprintf(sfe, sizeof sfe, "surfaceFeatureExtract %s", mo_cmd_extra(ov, "surfaceFeatureExtract"));
        snprintf(bm, sizeof bm, "blockMesh %s", mo_cmd_extra(ov, "blockMesh"));
        job_add(A.cj, "surfaceFeatureExtract", "surfaceFeatureExtract", sfe, "log.surfaceFeatureExtract", false);
        job_add(A.cj, "blockMesh", "blockMesh (background)", bm, "log.blockMesh", false);
        Str o; str_init(&o);
        if (split) {
            str_cat(&o, "cp system/snappyHexMeshDict system/snappyHexMeshDict.qc-snap && "
                        "foamDictionary system/snappyHexMeshDict.qc-snap -entry addLayers -set false > /dev/null || exit 1\n");
            snappy_run(&o, "system/snappyHexMeshDict.qc-snap", extra, np);
            str_cat(&o, "rm -rf " SNAP_KEEP " && mkdir -p " SNAP_KEEP " && cp -r constant/polyMesh " SNAP_KEEP "/ && "
                        "cp system/snappyHexMeshDict system/blockMeshDict " SNAP_KEEP "/ && "
                        "echo 'Q-Continuum: the mesh before the layers kept in " SNAP_KEEP " (Redo the layers only starts from it)'\n");
        } else snappy_run(&o, "system/snappyHexMeshDict", extra, np);
        char title[96]; snprintf(title, sizeof title, "snappyHexMesh%s%s", split ? ": castellate and snap" : "", np_s);
        job_add(A.cj, "snappyHexMesh", title, o.s, "log.snappyHexMesh", false);
        str_free(&o);
    } else
        job_add(A.cj, "restore", "The mesh before the layers", "rm -rf constant/polyMesh 0/polyMesh && cp -r " SNAP_KEEP "/polyMesh constant/ && "
                "echo 'Q-Continuum: the mesh before the layers, from " SNAP_KEEP "'", "log.meshClear", false);
    if (split) {
        Str o; str_init(&o);
        str_cat(&o, "cp system/snappyHexMeshDict system/snappyHexMeshDict.qc-layers && "
                    "foamDictionary system/snappyHexMeshDict.qc-layers -entry castellatedMesh -set false > /dev/null && "
                    "foamDictionary system/snappyHexMeshDict.qc-layers -entry snap -set false > /dev/null && "
                    "foamDictionary system/snappyHexMeshDict.qc-layers -entry addLayers -set true > /dev/null || exit 1\n");
        snappy_run(&o, "system/snappyHexMeshDict.qc-layers", extra, np);
        char title[96]; snprintf(title, sizeof title, "snappyHexMesh: prism layers%s", np_s);
        job_add(A.cj, "snappyLayers", title, o.s, "log.snappyLayers", false);
        str_free(&o);
    }
}

/* the kept mesh still fits: made from the same surface, background and refinement as the dictionaries now written
   (everything but addLayersControls and the layers per patch the same); why not, when it doesn't */
static bool snapped_fits(Case *c, char *why, int cap)
{
    char a[PL_PATH], b[PL_PATH];
    pl_join(a, sizeof a, c->s.path, SNAP_KEEP "/polyMesh/owner");
    if (!pl_is_file(a)) { snprintf(why, (size_t)cap, "Generate the mesh once with prism layers on first: its mesh before the layers is kept for this"); return false; }
    pl_join(a, sizeof a, c->s.path, SNAP_KEEP "/blockMeshDict"); pl_join(b, sizeof b, c->s.path, "system/blockMeshDict");
    char *x = pl_read_file(a, NULL), *y = pl_read_file(b, NULL);
    bool same = x && y && !strcmp(x, y);
    free(x); free(y);
    if (!same) { snprintf(why, (size_t)cap, "The base size or the tunnel changed since the mesh was made: Generate the whole mesh"); return false; }
    pl_join(a, sizeof a, c->s.path, SNAP_KEEP "/snappyHexMeshDict"); pl_join(b, sizeof b, c->s.path, "system/snappyHexMeshDict");
    FDFile *fa = fd_load(a), *fb = fd_load(b);
    same = fa && fb;
    static const char *const KEYS[] = { "geometry", "castellatedMeshControls", "snapControls", "mergeTolerance", "castellatedMesh", "snap" };
    for (size_t i = 0; same && i < sizeof KEYS / sizeof KEYS[0]; i++) {
        const FDEntry *ea = fd_child(&fa->root, KEYS[i]), *eb = fd_child(&fb->root, KEYS[i]);
        if (!ea != !eb) same = false;
        else if (ea && (ea->end - ea->start != eb->end - eb->start || strncmp(fa->text + ea->start, fb->text + eb->start, (size_t)(ea->end - ea->start)))) same = false;
    }
    fd_free(fa); fd_free(fb);
    if (!same) { snprintf(why, (size_t)cap, "The surface, refinement or snapping settings changed since the mesh was made: Generate the whole mesh"); return false; }
    return true;
}

static void generate(Case *c)
{
    bool lo = g_layersOnly; g_layersOnly = false;
    if (!app_require_foam() || !app_can_start()) return;
    case_rescan(c);
    if (case_has_results(&c->s)) {                        /* fields on the old mesh: they'd stop the run on the new one */
        app_clean_then("The results were computed on the mesh that is about to be replaced, so they go first.", lo ? relayer_after_clean : generate_after_clean);
        return;
    }
    if (salome_mesher() && !A.env.salome[0]) { ui_toast(TOAST_ERR, "SALOME is not installed: see the note under Mesher"); return; }
    double need;
    if (STLP.loaded && mesh_too_big(mesher_estimate_cells(&STLP.m, &STLP.stl), &need)) {
        ui_toast(TOAST_ERR, "Meshing this needs about %.1f GB and only %.1f GB is free: Mesh > Generate says more, and has Generate anyway",
                 need / 1024, free_mb() / 1024);
        return;
    }
    if (!prepare(c)) return;
    if (lo) {                                              /* the kept mesh must come from what is written now */
        char why[200];
        if (!snapped_fits(c, why, sizeof why)) { ui_toast(TOAST_WARN, "%s", why); return; }
    }
    MeshOverrides ov; mo_init(&ov); mo_load(c->s.path, &ov);   /* the Advanced panel's command-line options */
    job_reset(A.cj, "Mesh");
    job_add(A.cj, "clear", "Clear the old mesh", "rm -rf constant/polyMesh 0/polyMesh processor*; echo cleared", "log.meshClear", false);
    if (STLP.m.mesher == MESHER_FOAMY) {
        if (STLP.m.twoD) job_add(A.cj, "foamyQuadMesh", "foamyQuadMesh + extrude2DMesh", mesher_foamy2d_cmd(), "log.foamyQuadMesh", false);
        else job_add(A.cj, "foamyHexMesh", "foamyHexMesh (slow)", mesher_foamy_cmd(), "log.foamyHexMesh", false);
        Str ty; str_init(&ty);
        if (STLP.m.twoD) mesher_foamy2d_after(&STLP.m, &STLP.stl, &ty);
        str_cat(&ty, "echo 'patch types:'\n");
        mesher_type_script(&STLP.m, &STLP.stl, &ty);
        job_add(A.cj, "patchTypes", "Patch types", ty.s, "log.patchTypes", false);
        str_free(&ty);
    } else if (mesher_is_cfmesh(STLP.m.mesher)) {
        /* cfMesh: feature edges into the surface (.fms), the mesher, the patch types, the check */
        char prog[64];
        Str fe; str_init(&fe);
        if (STLP.m.twoD) mesher_cf2d_script(&STLP.m, true, &fe);
        char fa[32]; snprintf(fa, sizeof fa, "%g", STLP.m.featureAngle);
        str_catf(&fe, "surfaceFeatureEdges -angle %s constant/triSurface/cfmesh.stl constant/triSurface/cfmesh.fms", mo_cmd_value(&ov, "surfaceFeatureEdges", "-angle", fa));
        job_add(A.cj, "surfaceFeatureEdges", "surfaceFeatureEdges", fe.s, "log.surfaceFeatureEdges", false);
        str_free(&fe);
        snprintf(prog, sizeof prog, "%s", mesher_program(&STLP.m));
        char progCmd[200]; snprintf(progCmd, sizeof progCmd, "%s %s", prog, mo_cmd_extra(&ov, "cfMesh"));
        /* cfMesh's layers can turn cells inside out (user's intake, pMesh: 117 cells with 10 layers, none with 6 or 3, and
           cfMesh's own layer optimisation didn't help): checked, and made again with half as many */
        char cfc[1400];
        snprintf(cfc, sizeof cfc,
                 "%s || exit 1\n"
                 "L=$(grep -oE 'nLayers [0-9]+' system/meshDict | head -1 | grep -oE '[0-9]+')\n"
                 "if [ -n \"$L\" ] && [ \"$L\" -gt 1 ]; then checkMesh -constant > log.cfCheck 2>&1\n"
                 "  n=$(grep -oE 'Number of negative volume cells: [0-9]+' log.cfCheck | grep -oE '[0-9]+$')\n"
                 "  if [ -n \"$n\" ]; then h=$((L / 2)); echo; echo \"Q-Continuum: $n cells inside out with $L prism layers: "
                 "meshing again with $h (cfMesh's layers fold on this part; system/meshDict says $h until the next Generate)\"; echo\n"
                 "    sed -i \"s/nLayers $L;/nLayers $h;/g\" system/meshDict && { [ ! -f mesher/written/system/meshDict ] || "
                 "sed -i \"s/nLayers $L;/nLayers $h;/g\" mesher/written/system/meshDict; } && %s || exit 1; fi\n"
                 "fi", progCmd, progCmd);
        job_add(A.cj, prog, prog, cfc, TextFormat("log.%s", prog), false);
        Str ty; str_init(&ty);
        if (STLP.m.twoD) mesher_cf2d_script(&STLP.m, false, &ty);
        str_cat(&ty, "echo 'patch types:'\n");
        mesher_type_script(&STLP.m, &STLP.stl, &ty);
        job_add(A.cj, "patchTypes", "Patch types", ty.s, "log.patchTypes", false);
        str_free(&ty);
    } else if (!salome_mesher()) {
        snappy_jobs(c, &ov, lo);
        if (STLP.m.twoD) {                                   /* the slab's front face, one cell thick; front and back empty */
            Str ex; str_init(&ex);
            mesher_2d_script(&STLP.m, &STLP.stl, &ex);
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
        mesher_type_script(&STLP.m, &STLP.stl, &conv);
        str_cat(&conv, "}");
        job_add(A.cj, "ideasUnvToFoam", "ideasUnvToFoam", conv.s, "log.ideasUnvToFoam", false);
        str_free(&conv);
    }
    if (mesher_poly_convert(&STLP.m)) {                      /* any 3-D mesh into polyhedra (2-D: they would span the depth) */
        /* the dual of thin prism layers can come out inside out (user's intake, 2026-11-10: 125 negative cells after cfMesh
           tetMesh, 127 after cartesianMesh, none without layers): the mesh before is kept and put back then */
        char pd[2000], fa[32], pflags[160];
        snprintf(fa, sizeof fa, "%g", STLP.m.featureAngle);
        snprintf(pflags, sizeof pflags, "%s -overwrite%s%s%s", mo_cmd_value(&ov, "polyDualMesh", "featureAngle", fa),
                 mo_cmd_flag(&ov, "polyDualMesh", "-concaveMultiCells", true) ? " -concaveMultiCells" : "",
                 mo_cmd_flag(&ov, "polyDualMesh", "-splitAllFaces", false) ? " -splitAllFaces" : "",
                 mo_cmd_flag(&ov, "polyDualMesh", "-doNotPreserveFaceZones", false) ? " -doNotPreserveFaceZones" : "");
        snprintf(pd, sizeof pd,
                 "rm -rf .qc-prepoly && cp -r constant/polyMesh .qc-prepoly || exit 1\n"
                 "if ! polyDualMesh %s; then rm -rf constant/polyMesh && mv .qc-prepoly constant/polyMesh; "
                 "echo \"Q-Continuum: polyDualMesh stopped on this mesh (above: usually a face it would make with only two corners, "
                 "where the layers pinch): kept the mesh as the mesher made it. cfMesh pMesh or SALOME's polyhedral mesher make "
                 "polyhedra directly.\"; exit 0; fi\n"
                 "checkMesh -constant > log.polyDualCheck 2>&1\n"
                 "n=$(grep -oE 'Number of negative volume cells: [0-9]+' log.polyDualCheck | grep -oE '[0-9]+$')\n"
                 "if [ -n \"$n\" ]; then rm -rf constant/polyMesh && mv .qc-prepoly constant/polyMesh; "
                 "echo \"Q-Continuum: the polyhedra came out with $n cells inside out (polyDualMesh cannot take this part's thin prism "
                 "layers): kept the mesh as the mesher made it. To convert, turn the prism layers off.\"; "
                 "else rm -rf .qc-prepoly; echo 'Q-Continuum: polyhedra checked: no cells inside out'; fi", pflags);
        job_add(A.cj, "polyDualMesh", "polyDualMesh (to polyhedra)", pd, "log.polyDualMesh", false);
    }
    char cm[300];
    snprintf(cm, sizeof cm, "checkMesh%s%s%s %s", mo_cmd_flag(&ov, "checkMesh", "-allGeometry", false) ? " -allGeometry" : "",
             mo_cmd_flag(&ov, "checkMesh", "-allTopology", false) ? " -allTopology" : "",
             mo_cmd_flag(&ov, "checkMesh", "-meshQuality", false) ? " -meshQuality" : "", mo_cmd_extra(&ov, "checkMesh"));
    extrude_add_steps(c, A.cj);                            /* an extrusion in the mesh's recipe (Block builder > Extrude) */
    job_add(A.cj, "checkMesh", "checkMesh", cm, "log.checkMesh", false);
    mo_free(&ov);
    if (job_start(A.cj, c->s.path, app_bashrc(), GetTime())) ui_toast(TOAST_INFO, "Meshing %s: the Run page shows the log", pl_basename(STLP.m.cad[0] ? STLP.m.cad : STLP.m.stl));
    else ui_toast(TOAST_ERR, "Could not start the mesher");
}

void stlmesh_generate(Case *c) { sync_case(c); if (STLP.loaded) step_open(ST_GEN); generate(c); }   /* the step bar's: the flow is at its end */

void stlmesh_focus(Case *c, int step)
{
    sync_case(c);
    if (!STLP.loaded) step = ST_GEOM;
    if (step > STLP.reached) step = STLP.reached;              /* not past where the case has got */
    step_open(step);
}

int stlmesh_mesh_current(Case *c)
{
    sync_case(c);
    if (!STLP.loaded) return -1;
    MeshCfg mm = STLP.m;
    mesher_take_names(&mm, &STLP.stl);
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
    STLP.dirty = true;
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
    const Stl *s = &STLP.stl;
    if (!STLP.loaded) return 0;
    float now[6] = { s->bmin[0], s->bmin[1], s->bmin[2], s->bmax[0], s->bmax[1], s->bmax[2] };
    if (tri != s->tri || n != s->nTri || sc != s->scale || gen != STLP.gen || memcmp(bb, now, sizeof bb)) {
        tri = s->tri; n = s->nTri; sc = s->scale; gen = STLP.gen; memcpy(bb, now, sizeof bb);
        was = stl_thickness(s);
    }
    return was;
}

void sm_fmt_len(char *b, size_t cap, double v)
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

/* ---------------------------------------------------------------------------- geometry preparation ("every mesher" step 6)
   A CAD part's operations (salome_tool.py ops), in order, in <case>/mesher/geometry.ini; Apply runs them on the CAD file
   in the background and the result (mesher/<stem>-prep.stl + .brep) becomes the part. */
enum { GO_HEAL, GO_SEW, GO_FUSE, GO_MERGE, GO_SMALL, GO_HOLES, GO_CAP, GO_FLUID, GO_REMOVE, GO__N };
static const char *const GO_WORD[GO__N] = { "heal", "sew", "fuse", "merge_faces", "remove_small_faces", "remove_holes", "cap_openings", "fluid_inside", "remove_faces" };
static const char *GO_NAME[GO__N] = { "Heal (fix tolerances, drop tiny edges)", "Sew loose faces into a solid", "Fuse the solids into one",
                                      "Merge faces that continue one another", "Remove faces smaller than", "Fill round holes smaller than",
                                      "Cap the openings", "Fluid volume: the passage inside the part", "Remove faces picked on Boundaries" };
#define GO_MAX 16
static struct { char casePath[PL_PATH]; int n, kind[GO_MAX]; double val[GO_MAX]; char ids[GO_MAX][200]; char result[300]; bool loaded; } G;

static double go_default(int k) { return k == GO_SMALL ? 0.1 : k == GO_HOLES ? 0.005 : 0; }   /* % of the area; m */

static void geo_save(void)
{
    if (!STLP.casePath[0]) return;
    Str b; str_init(&b);
    str_cat(&b, "# Q-Continuum: the CAD part's preparation (Mesh > STL mesher > Geometry), in order. remove_small_faces: % of the\n"
                "# area; remove_holes: radius in m\n");
    for (int i = 0; i < G.n; i++) str_catf(&b, "op = %s %g %s\n", GO_WORD[G.kind[i]], G.val[i], G.kind[i] == GO_REMOVE ? G.ids[i] : "");
    char p[PL_PATH]; pl_join(p, sizeof p, STLP.casePath, "mesher/geometry.ini");
    pl_write_file(p, b.s, b.len);
    str_free(&b);
}

static void geo_load(void)
{
    if (G.loaded && !strcmp(G.casePath, STLP.casePath)) return;
    memset(&G, 0, sizeof G);
    pl_strlcpy(G.casePath, STLP.casePath, sizeof G.casePath);
    G.loaded = true;
    char p[PL_PATH]; pl_join(p, sizeof p, STLP.casePath, "mesher/geometry.ini");
    char *t = pl_read_file(p, NULL);
    if (!t) return;
    for (char *l = strtok(t, "\n"); l && G.n < GO_MAX; l = strtok(NULL, "\n")) {
        char w[64], ids[200] = ""; double v = 0;
        if (sscanf(l, "op = %63s %lf %199s", w, &v, ids) < 1) continue;
        for (int k = 0; k < GO__N; k++) if (!strcmp(w, GO_WORD[k])) { G.kind[G.n] = k; G.val[G.n] = v; pl_strlcpy(G.ids[G.n], ids, sizeof G.ids[0]); G.n++; }
    }
    free(t);
}

/* the original part: the CAD reader's STL (mesher/<stem>.stl), not a prepared one */
static void geo_original_stl(char *out, size_t cap)
{
    char stem[200]; pl_strlcpy(stem, pl_basename(STLP.m.cad), sizeof stem);
    char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
    for (char *c = stem; *c; c++) if (*c == ' ' || *c == '\'' || *c == '"') *c = '_';
    char f[240]; snprintf(f, sizeof f, "mesher/%s.stl", stem);
    pl_join(out, cap, STLP.casePath, f);
}

static void geo_apply(void)
{
    if (!G.n) { ui_toast(TOAST_INFO, "Add an operation first"); return; }
    char cad[PL_PATH * 2], outq[PL_PATH * 2], out[PL_PATH], orig[PL_PATH];
    geo_original_stl(orig, sizeof orig);
    pl_strlcpy(out, orig, sizeof out);
    char *dot = strrchr(out, '.'); if (dot) *dot = 0;
    pl_strlcpy(out + strlen(out), "-prep", sizeof out - strlen(out));
    stool_quote(cad, sizeof cad, STLP.m.cad); stool_quote(outq, sizeof outq, out);
    Str r; str_init(&r);
    str_catf(&r, "{\"cmd\": \"ops\", \"cad\": \"%s\", \"out\": \"%s\", \"ops\": [", cad, outq);
    for (int i = 0; i < G.n; i++) {
        int k = G.kind[i];
        str_catf(&r, "%s{\"op\": \"%s\"", i ? ", " : "", GO_WORD[k]);
        if (k == GO_SMALL) str_catf(&r, ", \"below\": %g", G.val[i] / 100);
        if (k == GO_HOLES) str_catf(&r, ", \"radius\": %g", G.val[i]);
        if (k == GO_REMOVE) str_catf(&r, ", \"faces\": [%s]", G.ids[i]);
        str_cat(&r, "}");
    }
    str_cat(&r, "]}");
    if (stool_run(STLP.casePath, "geoprep", r.s)) { G.result[0] = 0; ui_toast(TOAST_INFO, "Preparing the geometry in SALOME"); }
    str_free(&r);
}

static void geo_poll(void)
{
    QJ *a = stool_take("geoprep");
    if (!a) return;
    if (!qj_true(qj_get(a, "ok"))) {
        pl_fmt_ui(G.result, sizeof G.result, "Not done: %s", qj_str(qj_get(a, "error"), "no reason given"));
        ui_toast(TOAST_ERR, "%s", G.result);
    } else if (A.cs && !strcmp(A.cs->s.path, G.casePath)) {
        const char *stl = qj_str(qj_get(a, "stl"), "");
        pl_fmt_ui(G.result, sizeof G.result, "Prepared: %d faces (was %d), volume %.4g m³ (was %.4g)%s", (int)qj_num(qj_get(a, "faces"), 0),
                  (int)qj_num(qj_get(a, "faces_before"), 0), qj_num(qj_get(a, "volume"), 0), qj_num(qj_get(a, "volume_before"), 0),
                  qj_true(qj_get(a, "closed")) ? "" : ", NOT closed");
        char cad[PL_PATH]; pl_strlcpy(cad, STLP.m.cad, sizeof cad);
        s_cad = cad;
        if (load_stl(stl, true)) ui_toast(TOAST_OK, "%s", G.result);
        s_cad = NULL;
    }
    qj_free(a);
}

static float prep_section(float x, float y, float w)
{
    geo_load();
    geo_poll();
    float y0 = y;
    y += section_head(x, y, w, "Prepare the geometry", "SALOME works on the CAD file in the background, in this order, and the result "
                                                        "becomes the part. Fluid volume turns a solid part (an intake, a valve body) into "
                                                        "the passage inside it, its openings capped.");
    bool busy = stool_busy("geoprep");
    int del = -1;
    for (int i = 0; i < G.n; i++) {
        ui_push_id(ui_idn(ui_id("geo-op"), i));
        char lab[96]; snprintf(lab, sizeof lab, "%d.  %s", i + 1, GO_NAME[G.kind[i]]);
        ui_text_clip(F_MED, 13.5f, lab, x, y + 9, w - 260, TH.text);
        int k = G.kind[i];
        if (k == GO_REMOVE) ui_text_clip(F_MONO, 12.5f, TextFormat("faces %s", G.ids[i]), x + w - 240, y + 9, 190, TH.text3);
        if (k == GO_SMALL || k == GO_HOLES) {
            double v = k == GO_HOLES ? G.val[i] * 1000 : G.val[i];
            if (num_field(ui_id("val"), (Rectangle){ x + w - 240, y, 150, 34 }, &v, "%g", 1e-6, 1e6, k == GO_HOLES ? "mm" : "% area"))
                { G.val[i] = k == GO_HOLES ? v / 1000 : v; geo_save(); }
        }
        if (ui_button(ui_id("del"), (Rectangle){ x + w - 40, y + 1, 32, 32 }, ICON_TRASH_2, NULL, BTN_ICON)) del = i;
        ui_pop_id();
        y += 42;
    }
    if (del >= 0) { for (int i = del; i < G.n - 1; i++) { G.kind[i] = G.kind[i + 1]; G.val[i] = G.val[i + 1]; } G.n--; geo_save(); }
    ui_disable(busy || G.n >= GO_MAX);
    int add = -1;
    if (ui_select_ph(ui_id("geo-add"), (Rectangle){ x, y, fminf(380, w - 240), 34 }, (const char **)GO_NAME, GO__N - 1, &add, "Add an operation...") && add >= 0) {
        G.kind[G.n] = add; G.val[G.n] = go_default(add); G.n++; geo_save();
    }
    ui_disable(busy || !G.n || !A.env.salome[0]);
    float aw = ui_button_width(ICON_PLAY, "Apply");
    if (ui_button(ui_id("geo-apply"), (Rectangle){ x + fminf(380, w - 240) + 12, y, aw, 34 }, ICON_PLAY, busy ? "Working..." : "Apply", BTN_PRIMARY)) geo_apply();
    ui_disable(busy);
    char orig[PL_PATH]; geo_original_stl(orig, sizeof orig);
    if (strcmp(STLP.m.stl, orig) && pl_is_file(orig)) {
        float ow = ui_button_width(ICON_UNDO_2, "Use the original");
        if (ui_button(ui_id("geo-orig"), (Rectangle){ x + fminf(380, w - 240) + aw + 22, y, ow, 34 }, ICON_UNDO_2, "Use the original", BTN_GHOST)) {
            char cad[PL_PATH]; pl_strlcpy(cad, STLP.m.cad, sizeof cad);
            s_cad = cad; load_stl(orig, true); s_cad = NULL;
            G.result[0] = 0;
        }
    }
    ui_disable(false);
    y += 44;
    if (busy) {
        char t[80]; snprintf(t, sizeof t, "SALOME is working on it  ·  %.0f s", stool_seconds());
        ui_text(F_REG, 13, t, x, y, TH.text3); y += 24;
    } else if (G.result[0]) y += note(x, y, w, G.result);
    return y - y0 + 6;
}

/* ---------------------------------------------------------------------------- 1 Geometry: the STL, its units, its boundaries */
static float geometry_step(Case *c, float x, float y, float w)
{
    float y0 = y;
    float bw = ui_button_width(ICON_FOLDER_OPEN, "Choose part...");
    bool reading = cad_busy() && !strcmp(CV.casePath, STLP.casePath);
    ui_disable(STLP.picker > 0 || reading);
    if (ui_button(ui_id("stl-pick"), (Rectangle){ x, y, bw, 38 }, ICON_FOLDER_OPEN, STLP.picker > 0 ? "Choosing..." : reading ? "Reading..." : "Choose part...",
                  BTN_PRIMARY)) start_picker(false);
    float ix = x + bw + 12, iw = w - bw - 12 - (STLP.loaded ? 90 : 0);
    UiId tid = ui_id("stl-path");
    if (!ui_input_focused(tid)) pl_strlcpy(STLP.typed, STLP.m.cad[0] ? STLP.m.cad : STLP.m.stl, sizeof STLP.typed);
    unsigned gen = STLP.gen;
    if (ui_input(tid, (Rectangle){ ix, y, iw, 38 }, STLP.typed, sizeof STLP.typed, A.env.wsl ? "or paste a path: /mnt/c/Users/you/Downloads/part.step, or C:\\..." : "or a path to an .stl or .step",
                 INPUT_MONO) && gen == STLP.gen && STLP.typed[0]) {
        char p[PL_PATH]; pl_expand_home(p, sizeof p, STLP.typed); pl_trim_inplace(p);
        /* a Windows path pasted from Explorer (with its quotes, as "Copy as path" gives it) */
        size_t n = strlen(p);
        if (n >= 2 && p[0] == '"' && p[n - 1] == '"') { memmove(p, p + 1, n - 2); p[n - 2] = 0; }
        if (A.env.wsl && strlen(p) > 2 && p[1] == ':') {
            char q[PL_PATH + 8], cmd[PL_PATH + 40], out[PL_PATH];
            pl_shell_quote(q, sizeof q, p); snprintf(cmd, sizeof cmd, "wslpath -u %s", q);
            if (pl_run_wait(cmd, out, sizeof out) == 0) { pl_trim_inplace(out); pl_strlcpy(p, out, sizeof p); }
        }
        if ((strcmp(p, STLP.m.stl) && strcmp(p, STLP.m.cad)) || !STLP.loaded) { if (pl_is_file(p)) take_part(p); else ui_toast(TOAST_ERR, "No file at %s", p); }
    }
    ui_disable(false);
    if (STLP.loaded && !reading && ui_button(ui_id("stl-reload"), (Rectangle){ x + w - 80, y, 80, 38 }, ICON_REFRESH_CW, "Reload", BTN_GHOST)) {
        char p[PL_PATH]; pl_strlcpy(p, STLP.m.stl, sizeof p);
        if (STLP.m.cad[0] && pl_is_file(STLP.m.cad)) cad_start(STLP.m.cad, STLP.casePath, false, false);   /* the CAD may have changed */
        else if (pl_is_file(p)) load_stl(p, false); else ui_toast(TOAST_ERR, "%s is gone: choose the STL again", p);
    }
    y += 52;
    if (reading) {
        char t[PL_PATH + 40], d[400];
        pl_fmt_ui(t, sizeof t, "Reading %s  ·  %.0f s", pl_basename(CV.cad), GetTime() - CV.t0);
        snprintf(d, sizeof d, "%s turns the CAD into a fine STL in metres (from the file's own unit) and keeps the exact shape for "
                              "SALOME's CAD route. A few seconds for most parts; a big assembly takes longer.", A.env.salome[0] ? "SALOME" : "Gmsh");
        float ch = callout_height(w, t, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_REFRESH_CW, t, d);
        y += ch + 8;
        if (!STLP.loaded) return y - y0;
    }
    if (!STLP.loaded) {
        const char *t = "Start with the part", *d = "Choose an STL (ASCII or binary) or a CAD file (STEP, IGES), or paste its path. Next you say whether "
                                                     "the fluid is inside it (a duct, a manifold) or around it (a wing, a car), then name its boundaries, then mesh it.";
        float ch = callout_height(w, t, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_FOLDER_OPEN, t, d);
        return y + ch + 8 - y0;
    }
    const Stl *s = &STLP.stl;
    char a[32], b3[32], c3[32], line[400], tri[32];
    sm_fmt_len(a, sizeof a, s->bmax[0] - s->bmin[0]); sm_fmt_len(b3, sizeof b3, s->bmax[1] - s->bmin[1]); sm_fmt_len(c3, sizeof c3, s->bmax[2] - s->bmin[2]);
    snprintf(tri, sizeof tri, "%d", s->nTri);
    char th[32] = ""; double thick = part_thickness();
    if (thick > 0) sm_fmt_len(th, sizeof th, thick);
    pl_fmt_ui(line, sizeof line, "%s  ·  %s triangles (%s)  ·  %s × %s × %s  ·  area %.4g m²%s%s%s", STLP.m.cad[0] ? TextFormat("%s (CAD)", pl_basename(STLP.m.cad)) : pl_basename(STLP.m.stl), tri,
              STLP.m.cad[0] ? "made from the CAD" : s->binary ? "binary" : "ASCII",
              a, b3, c3, s->area, s->openEdges == 0 && s->nonManifold == 0 ? TextFormat("  ·  volume %.4g m³", s->volume) : "",
              th[0] ? "  ·  thinnest about " : "", th);
    y += ui_text_wrap(F_MONO, 12.5f, line, (Rectangle){ x, y, w, 80 }, 19, TH.text2, true) + 8;
    bool closed = s->openEdges == 0 && s->nonManifold == 0 && !s->oom;
    char msg[300];
    if (s->oom) snprintf(msg, sizeof msg, "Too big to check in the memory free now: whether it is closed is not known, and it is one "
                         "boundary (walls). Close other programs and open it again, or export it coarser.");
    else if (closed) snprintf(msg, sizeof msg, "Closed (watertight): every edge has two triangles. It can be meshed inside or outside.");
    else snprintf(msg, sizeof msg, "%d open and %d non-manifold edges: not watertight. snappyHexMesh copes with small gaps (smaller than the cells there); "
                  "SALOME needs a closed surface. Fix it in your CAD, or with surfaceCheck and surfaceClean.", s->openEdges, s->nonManifold);
    float ch = callout_height(w, NULL, msg);
    callout((Rectangle){ x, y, w, ch }, closed ? TOAST_OK : TOAST_WARN, closed ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT, NULL, msg);
    y += ch + 14;
    /* scale: the STL's unit, checked against the size it gives */
    Rectangle cr = prop_row(x, &y, w, STLP.m.cad[0] ? "Units" : "Units of the STL", STLP.m.cad[0]
                              ? "Read from the CAD file and converted to metres: change it only if the size above is wrong."
                              : "STL files have no units; CAD usually writes millimetres. Check the size above looks right.", 34);
    static const char *units[] = { "m", "cm", "mm", "inch" };
    static const double uk[] = { 1, 0.01, 0.001, 0.0254 };
    int us = -1;
    for (int i = 0; i < 4; i++) if (fabs(STLP.m.scale - uk[i]) < 1e-12) us = i;
    int pick = us;
    float segW = fminf(280, cr.width - 110), fx = cr.x + segW + 12, fwd = fminf(140, cr.x + cr.width - fx);
    if (ui_segmented(ui_id("stl-units"), (Rectangle){ cr.x, cr.y, segW, 34 }, units, 4, &pick) && pick != us && pick >= 0) {
        STLP.m.scale = uk[pick];
        stl_set_scale(&STLP.stl, STLP.m.scale); stl_split_part(&STLP.stl, STLP.m.splitAngle, STLP.casePath);
        mesher_apply_names(&STLP.m, &STLP.stl);
        mesher_defaults(&STLP.m, &STLP.stl);                    /* sizes follow the part's new size */
        pl_strlcpy(STLP.m.stl, s->path, sizeof STLP.m.stl);
        sm_save_cfg();
    }
    double sc = STLP.m.scale;
    if (num_field(ui_id("stl-scale"), (Rectangle){ fx, cr.y, fwd, 34 }, &sc, "%g", 1e-9, 1e9, "×") && sc != STLP.m.scale) {
        STLP.m.scale = sc; stl_set_scale(&STLP.stl, sc); stl_split_part(&STLP.stl, STLP.m.splitAngle, STLP.casePath); mesher_apply_names(&STLP.m, &STLP.stl); sm_save_cfg();
    }
    double ang = STLP.m.splitAngle;
    char after[96]; snprintf(after, sizeof after, "%d boundar%s", s->nRegions, s->nRegions == 1 ? "y" : "ies");
    if (row_num(x, &y, w, "stl-split", "Split by angle", "Surfaces meeting at a sharper angle become separate boundaries (STAR-CCM+'s split by angle). "
                "0 keeps the file's own solids. You name them two steps on.", &ang, "%g", 0, 180, "°", after)) {
        STLP.m.splitAngle = ang;
        if (STLP.stl.face) { char fp[PL_PATH]; pl_join(fp, sizeof fp, STLP.casePath, "mesher/faces.ini"); unlink(fp); }   /* CAD faces: regrouped by the angle */
        stl_split_part(&STLP.stl, ang, STLP.casePath); mesher_take_names(&STLP.m, &STLP.stl); sm_save_cfg();
    }
    /* start over: the case without an STL (a mistake, or the wrong part) */
    if (STLP.m.cad[0]) y += prep_section(x, y + 4, w) + 4;    /* a CAD part: operations on its exact shape */
    bool armed = STLP.removeArm > 0 && ui_time() - STLP.removeArm < 4;
    const char *rl = armed ? "Click again to remove" : "Remove STL from this case";
    float rw = ui_button_width(ICON_TRASH_2, rl);
    if (ui_button(ui_id("stl-remove"), (Rectangle){ x, y + 4, rw, 32 }, ICON_TRASH_2, rl, armed ? BTN_DANGER : BTN_GHOST)) {
        if (armed) { STLP.removeArm = 0; remove_stl(c); return y + 44 - y0; }
        STLP.removeArm = ui_time();
    }
    if (armed) { ui_request_frames(30); ui_text(F_REG, 12.5f, "Deletes mesher/ and the STL's copy in constant/triSurface; the mesh, if any, stays.", x + rw + 14, y + 13, TH.text3); }
    y += 44;
    return y - y0;
}

/* the part in 3-D (picking faces, what each step adds to the view): page_stlview.c */
/* ============================================================================ the boundaries */
static const char *const TYPES[BT__N] = { "Wall", "Opening", "Inlet", "Outlet", "Symmetry" };
static const char *foam_type_of(int t) { return t == BT_WALL ? "wall" : t == BT_SYMMETRY ? "symmetry" : "patch"; }

void sm_select_region(int r)
{
    STLP.sel = r >= 0 && r < STLP.stl.nRegions ? r : -1;
    STLV.dirty = true;
    if (STLP.sel >= 0) ui_focus_input(ui_idn(ui_id("stl-sel-name"), STLP.sel));     /* type the new name straight away */
}

static bool rename_region(int i, const char *typed)
{
    Stl *s = &STLP.stl;
    char nm[64]; pl_strlcpy(nm, typed, sizeof nm); pl_trim_inplace(nm);
    for (char *q = nm; *q; q++) if (*q == ' ' || *q == '-') *q = '_';   /* "front inlet" -> front_inlet */
    bool okName = nm[0] != 0 && !(nm[0] >= '0' && nm[0] <= '9');
    for (char *q = nm; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_')) okName = false;
    for (int k = 0; k < s->nRegions && okName; k++) if (k != i && !strcmp(s->reg[k].name, nm)) okName = false;
    if (!okName) { ui_toast(TOAST_ERR, "A boundary name is letters, digits and _ (not starting with a digit), and each one different"); return false; }
    if (!strcmp(nm, s->reg[i].name)) return true;
    char old[64]; pl_strlcpy(old, s->reg[i].name, sizeof old);
    pl_strlcpy(s->reg[i].name, nm, sizeof s->reg[i].name);
    if (s->face) {                                        /* a CAD face group: its faces carry the new name */
        StlName *g = faces_load(STLP.casePath, s->nFaces);
        if (g) {
            for (int f = 1; f <= s->nFaces; f++) if (!strcmp(g[f], old)) pl_strlcpy(g[f], nm, 64);
            faces_save(STLP.casePath, (const StlName *)g, s->nFaces);
            free(g);
        }
        for (int t = 0; t < s->nTri; t++) if (!strcmp(s->solid[t], old)) pl_strlcpy(s->solid[t], nm, 64);
    }
    sm_save_cfg();
    if (A.cs) setup_rename(A.cs, old, nm);                 /* its conditions go with it */
    return true;
}

static void set_type(int i, int ty)
{
    if (STLP.stl.reg[i].type == ty) return;
    STLP.stl.reg[i].type = ty;
    sm_save_cfg();
}

int stlmesh_patches(Case *c, SimPatch *out, int max, double *size, bool *external)
{
    sync_case(c);
    if (!STLP.loaded) return 0;
    const Stl *s = &STLP.stl;
    double L = 0; for (int k = 0; k < 3; k++) L = fmax(L, s->bmax[k] - s->bmin[k]);
    *size = L; *external = STLP.m.domain == DOMAIN_EXTERNAL;
    int n = 0;
    for (int i = 0; i < s->nRegions && n < max; i++) {
        if (!mesher_region_meshed(&STLP.m, s, i)) continue;   /* 2-D: the extrusion's end caps aren't in the mesh */
        out[n++] = (SimPatch){ s->reg[i].name, s->reg[i].type, foam_type_of(s->reg[i].type), true, s->reg[i].area };
    }
    if (STLP.m.twoD && n + 2 <= max) {
        out[n++] = (SimPatch){ "front2D", ROLE_WALL, "empty", false, 0 };
        out[n++] = (SimPatch){ "back2D", ROLE_WALL, "empty", false, 0 };
    }
    if (STLP.m.domain == DOMAIN_EXTERNAL) {                   /* the wind tunnel's own (stlmesh.c write_snappy, the SALOME script) */
        if (n < max) out[n++] = (SimPatch){ "inlet", ROLE_INLET, "patch", false, 0 };
        if (n < max) out[n++] = (SimPatch){ "outlet", ROLE_OUTLET, "patch", false, 0 };
        if (n < max) out[n++] = (SimPatch){ "farfield", ROLE_SYMMETRY, "patch", false, 0 };
        if (STLP.m.ground && n < max) out[n++] = (SimPatch){ "ground", ROLE_WALL, "wall", false, 0 };
    }
    return n;
}

void stlmesh_flow(Case *c, double v[3])
{
    sync_case(c);
    v[0] = v[1] = v[2] = 0;
    if (!STLP.loaded || STLP.m.domain != DOMAIN_EXTERNAL) { v[0] = 1; return; }
    v[STLP.m.flowDir / 2] = STLP.m.flowDir % 2 ? -1 : 1;
}

bool stlmesh_force_ref(Case *c, SimForceRef *r, bool *twoD)
{
    sync_case(c);
    *twoD = false;
    if (!STLP.loaded) return false;
    const Stl *s = &STLP.stl;
    double v[3]; stlmesh_flow(c, v);
    int f = fabs(v[0]) > 0.5 ? 0 : fabs(v[1]) > 0.5 ? 1 : 2, u = mesher_up(&STLP.m);
    for (int k = 0; k < 3; k++) { r->drag[k] = v[k]; r->lift[k] = 0; }
    r->lift[u / 2] = u % 2 ? -1 : 1;
    double len = s->bmax[f] - s->bmin[f];
    for (int k = 0; k < 3; k++) r->CofR[k] = (s->bmin[k] + s->bmax[k]) / 2;
    r->CofR[f] = v[f] > 0 ? s->bmin[f] + 0.25 * len : s->bmax[f] - 0.25 * len;   /* an airfoil's quarter chord */
    r->Uinf = 0;
    if (STLP.m.twoD) {
        int n = STLP.m.normal2D % 3;
        *twoD = true;
        r->lRef = len;
        r->aRef = len * (s->bmax[n] - s->bmin[n]);         /* the mesh is the part's depth thick (extrudeMeshDict) */
        return true;
    }
    r->lRef = mesher_ref_length(&STLP.m, s);
    /* the frontal area: the part seen along the flow, half the sum of |n . flow| over a closed surface */
    double a = 0;
    for (int t = 0; t < s->nTri; t++) {
        if (!mesher_region_meshed(&STLP.m, s, s->region[t])) continue;
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
    for (int i = 0; i < STLP.stl.nRegions; i++) if (!strcmp(STLP.stl.reg[i].name, name)) { set_type(i, role); return true; }
    return false;
}

/* the picked boundary: its name, what it is, and its conditions */
static float selection_panel(Case *c, float x, float y, float w, float h)
{
    Rectangle box = { x, y, w, h };
    ui_rect(box, 12, TH.surface2); ui_rect_lines(box, 12, 1, TH.lineSoft);
    float px = x + 18, pw = w - 36, py = y + 18;
    Stl *s = &STLP.stl;
    if (STLP.sel < 0 || STLP.sel >= s->nRegions) {
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
    StlRegion *R = &s->reg[STLP.sel];
    UiId nid = ui_idn(ui_id("stl-sel-name"), STLP.sel);       /* the id select_region focuses: made at the same level */
    ui_push_id(ui_idn(ui_id("stl-sel"), STLP.sel));
    ui_rect((Rectangle){ px, py + 4, 14, 14 }, 4, sm_region_color(STLP.sel));
    char info[160]; pl_fmt_ui(info, sizeof info, "%d triangles  ·  %.3g m²", R->nTri, R->area);
    ui_text_clip(F_REG, 12.5f, info, px + 22, py + 3, pw - 60, TH.text3);
    if (ui_button(ui_id("close"), (Rectangle){ x + w - 46, y + 10, 30, 30 }, ICON_X, NULL, BTN_ICON)) { sm_select_region(-1); ui_pop_id(); return h; }
    py += 30;
    ui_pop_id();
    ui_text(F_MED, 12.5f, ui_cap("Name"), px, py, TH.text2);
    char nm[64]; pl_strlcpy(nm, R->name, sizeof nm);
    if (ui_input(nid, (Rectangle){ px, py + 20, pw, 36 }, nm, sizeof nm, "boundary name", INPUT_MONO)) rename_region(STLP.sel, nm);
    py += 68;
    ui_text(F_MED, 12.5f, ui_cap("It is"), px, py, TH.text2);
    int ty = R->type;
    if (ui_segmented(ui_id("stl-sel-type"), (Rectangle){ px, py + 20, pw, 34 }, (const char **)TYPES, BT__N, &ty)) set_type(STLP.sel, ty);
    py += 66;
    py += setup_boundary_editor(c, R->name, px, py, pw);
    return fmaxf(h, py - y + 10);
}

/* ---------------------------------------------------------------------------- 3 Boundaries: names and kinds, in 3-D */
static void boundary_counts(int *in, int *out, int *wall, int *other)
{
    *in = *out = *wall = *other = 0;
    for (int i = 0; i < STLP.stl.nRegions; i++) {
        if (!mesher_region_meshed(&STLP.m, &STLP.stl, i)) continue;
        int t = STLP.stl.reg[i].type;
        if (t == BT_INLET) (*in)++; else if (t == BT_OUTLET || t == BT_PATCH) (*out)++; else if (t == BT_WALL) (*wall)++; else (*other)++;
    }
}

/* the kind a boundary's name asks for (-1: it doesn't say): inlet / inflow, outlet / outflow / exhaust; a word in the name,
   not a part of another (a "pinlet" says nothing) */
static int name_role(const char *name)
{
    char l[64]; int n = 0;
    for (const char *q = name; *q && n < 63; q++) l[n++] = (char)(*q >= 'A' && *q <= 'Z' ? *q + 32 : *q);
    l[n] = 0;
    static const struct { const char *w; int role; } W[] = { { "inlet", BT_INLET }, { "inflow", BT_INLET }, { "outlet", BT_OUTLET },
                                                           { "outflow", BT_OUTLET }, { "exhaust", BT_OUTLET } };
    for (size_t i = 0; i < sizeof W / sizeof W[0]; i++) {
        const char *p = strstr(l, W[i].w);
        if (!p) continue;
        bool start = p == l || p[-1] == '_' || (p[-1] >= '0' && p[-1] <= '9');
        if (start) return W[i].role;
    }
    return -1;
}

/* the picked faces into a boundary (new or one there is): faces.ini changed, the part split by it again */
static void group_picked(const char *name)
{
    Stl *s = &STLP.stl;
    StlName *g = faces_load(STLP.casePath, s->nFaces);
    if (!g) {
        g = calloc((size_t)s->nFaces + 1, sizeof *g);
        if (!g) { ui_toast(TOAST_ERR, "Out of memory: the faces were not grouped"); return; }
        faces_default(s, STLP.m.splitAngle, g);
    }
    for (int f = 1; f <= s->nFaces && f < STLPK.cap; f++) if (STLPK.on[f]) pl_strlcpy(g[f], name, 64);
    faces_save(STLP.casePath, (const StlName *)g, s->nFaces);
    free(g);
    MeshCfg keep = STLP.m;
    stl_split_part(&STLP.stl, STLP.m.splitAngle, STLP.casePath);
    mesher_apply_names(&keep, &STLP.stl);                   /* the kinds by name: the new one a wall until said otherwise */
    sm_save_cfg();
    for (int r = 0; r < s->nRegions; r++) if (!strcmp(s->reg[r].name, name)) { STLP.sel = r; break; }
    STLV.dirty = true;
    ui_toast(TOAST_OK, "%d face%s now in %s", STLPK.n, STLPK.n == 1 ? "" : "s", name);
}

static float faces_bar(float x, float y, float w)
{
    float y0 = y;
    Stl *s = &STLP.stl;
    double area = 0; char ids[200] = ""; int k = 0;
    for (int t = 0; t < s->nTri; t++) if (sm_in_pick(s, t)) {
        const float *p = s->tri + 9 * (size_t)t;
        float e1[3] = { p[3] - p[0], p[4] - p[1], p[5] - p[2] }, e2[3] = { p[6] - p[0], p[7] - p[1], p[8] - p[2] };
        float c[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        area += 0.5 * sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    }
    for (int f = 1; f < STLPK.cap && f <= s->nFaces; f++) if (STLPK.on[f]) { if (k++) pl_strlcpy(ids + strlen(ids), ",", sizeof ids - strlen(ids)); snprintf(ids + strlen(ids), sizeof ids - strlen(ids), "%d", f); }
    Rectangle box = { x, y, w, 96 };
    ui_rect(box, 10, TH.surface2); ui_rect_lines(box, 10, 1, TH.lineSoft);
    char t[300];
    if (STLPK.n) pl_fmt_ui(t, sizeof t, "%d face%s picked  ·  %.4g m²  ·  CAD face%s %s", STLPK.n, STLPK.n == 1 ? "" : "s", area, STLPK.n == 1 ? "" : "s", ids);
    else pl_strlcpy(t, "CAD faces: click a face to pick it, shift-click to pick more, then make them a boundary of their own.", sizeof t);
    ui_text_clip(STLPK.n ? F_MED : F_REG, 13.5f, t, x + 14, y + 10, w - 60, STLPK.n ? TH.text : TH.text3);
    if (STLPK.n && ui_button(ui_id("pk-clear"), (Rectangle){ x + w - 42, y + 6, 30, 30 }, ICON_X, NULL, BTN_ICON)) sm_pick_clear();
    ui_disable(!STLPK.n);                                    /* the same height picked or not: the view doesn't jump on the first click */
    static char nm[64];
    float by = y + 46, bx = x + 14;
    ui_input(ui_id("pk-name"), (Rectangle){ bx, by, 200, 36 }, nm, sizeof nm, "new boundary name", INPUT_MONO);
    bx += 210;
    float mw = ui_button_width(ICON_PLUS, "Make a boundary");
    if (ui_button(ui_id("pk-make"), (Rectangle){ bx, by, mw, 36 }, ICON_PLUS, "Make a boundary", BTN_PRIMARY) || ui_input_entered(ui_id("pk-name"))) {
        char n2[64]; pl_strlcpy(n2, nm, sizeof n2); pl_trim_inplace(n2);
        for (char *q = n2; *q; q++) if (*q == ' ' || *q == '-') *q = '_';
        bool ok = n2[0] && !(n2[0] >= '0' && n2[0] <= '9');
        for (char *q = n2; *q && ok; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_')) ok = false;
        if (!ok) ui_toast(TOAST_ERR, "A boundary name is letters, digits and _ (not starting with a digit)");
        else { group_picked(n2); nm[0] = 0; }
    }
    bx += mw + 10;
    static const char *names[STL_MAX_REGIONS]; for (int r = 0; r < s->nRegions; r++) names[r] = s->reg[r].name;
    int into = -1;
    float sw = fminf(220, w - (bx - x) - 200);
    if (sw > 120 && ui_select_ph(ui_id("pk-into"), (Rectangle){ bx, by, sw, 36 }, names, s->nRegions, &into, "Add to a boundary...") && into >= 0) {
        char n2[64]; pl_strlcpy(n2, s->reg[into].name, sizeof n2); group_picked(n2);
    }
    if (sw > 120) bx += sw + 10;
    float rw = ui_button_width(ICON_TRASH_2, "Remove from the part");
    ui_disable(!STLPK.n || !STLP.m.cad[0] || !A.env.salome[0] || stool_busy(NULL));
    if (ui_button(ui_id("pk-remove"), (Rectangle){ fminf(bx, x + w - rw - 14), by, rw, 36 }, ICON_TRASH_2, "Remove from the part", BTN_GHOST)) {
        geo_load();
        if (G.n < GO_MAX) {
            G.kind[G.n] = GO_REMOVE; G.val[G.n] = 0; pl_strlcpy(G.ids[G.n], ids, sizeof G.ids[0]); G.n++;
            geo_save(); geo_apply(); sm_pick_clear();
            ui_toast(TOAST_INFO, "Removing the faces (Geometry > Prepare the geometry shows it): SALOME heals the part round them");
        }
    }
    ui_disable(false);
    return box.height + 12 - (y - y0) + (y - y0);
}

static float boundaries_step(Case *c, float x, float y, float w)
{
    float y0 = y;
    y += note(x, y, w, "Each boundary becomes an OpenFOAM patch with this name. Click a face of the part to pick its boundary, then name "
                       "it and say what it is; walls get prism layers. The list under the view picks them too.");
    Stl *s = &STLP.stl;
    if (STLP.sel >= s->nRegions) STLP.sel = -1;
    if (s->face) y += faces_bar(x, y, w);                /* a CAD part: its faces picked into boundaries (above the view: in sight while picking) */
    float pw = fminf(460, w * 0.42f), vh = 440;
    Rectangle view = { x, y, w - pw - 16, vh };
    sm_view_panel(view);
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
        if (ui_row(ui_id("pick"), sw, STLP.sel == i)) sm_select_region(STLP.sel == i ? -1 : i);
        ui_rect((Rectangle){ sw.x + 8, sw.y + 8, 14, 14 }, 4, sm_region_color(i));
        if (STLP.sel == i) ui_rect_lines(sw, 7, 1.5f, TH.accent);
        ui_tooltip(sw, "Pick it: it lights up in the view");
        char nm[64]; pl_strlcpy(nm, R->name, sizeof nm);
        if (ui_input(ui_id("name"), (Rectangle){ x + 40, y, 240, 34 }, nm, sizeof nm, "name", INPUT_MONO)) rename_region(i, nm);
        if (ui_input_focused(ui_id("name")) && STLP.sel != i) { STLP.sel = i; STLV.dirty = true; }   /* typing in a row picks it in the view */
        int ty = R->type;
        if (ui_select(ui_id("type"), (Rectangle){ x + 292, y, 140, 34 }, TYPES, BT__N, &ty)) set_type(i, ty);
        char info[160]; pl_fmt_ui(info, sizeof info, "%d triangles  ·  %.3g m²  ·  %s", R->nTri, R->area, axis_word(R->n));
        ui_text_clip(F_REG, 13, info, x + 448, y + 9, w - 448, TH.text3);
        ui_pop_id();
        y += rh;
    }
    /* named like an inlet or outlet but still another kind (the user's intake, 2026-11-10: Outlet2, Outlet_3 and Outlet_4
       left Walls, so snappy grew prism layers over three of the four outlets and the flow had one way out) */
    {
        char names[300] = ""; int nMis = 0;
        for (int i = 0; i < s->nRegions; i++) {
            if (!mesher_region_meshed(&STLP.m, s, i)) continue;
            int want = name_role(s->reg[i].name), ty = s->reg[i].type;
            if (want < 0 || ty == want || (want == BT_OUTLET && ty == BT_PATCH)) continue;   /* an outlet as an opening: fine */
            if (nMis < 4) pl_fmt_ui(names + strlen(names), sizeof names - strlen(names), "%s%s (%s)", nMis ? ", " : "", s->reg[i].name, TYPES[s->reg[i].type]);
            nMis++;
        }
        if (nMis) {
            char d[600];
            pl_fmt_ui(d, sizeof d, "%s%s %s named like an inlet or outlet but %s another kind. A wall gets prism layers and no flow "
                      "through it, so the fluid would have fewer ways in or out than the names say.", names, nMis > 4 ? ", ..." : "",
                      nMis == 1 ? "is" : "are", nMis == 1 ? "is" : "are");
            float ch = callout_height(w, "Names and kinds disagree", d) + 46;
            callout((Rectangle){ x, y + 8, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "Names and kinds disagree", d);
            float fw = ui_button_width(ICON_WRENCH, "Make them what their names say");
            if (ui_button(ui_id("stl-name-kinds"), (Rectangle){ x + 42, y + 8 + ch - 44, fw, 34 }, ICON_WRENCH, "Make them what their names say", BTN_PRIMARY)) {
                for (int i = 0; i < s->nRegions; i++) {
                    int want = name_role(s->reg[i].name), ty = s->reg[i].type;
                    if (want >= 0 && mesher_region_meshed(&STLP.m, s, i) && ty != want && !(want == BT_OUTLET && ty == BT_PATCH)) set_type(i, want);
                }
                ui_toast(TOAST_OK, "Set %d boundar%s from %s name%s", nMis, nMis == 1 ? "y" : "ies", nMis == 1 ? "its" : "their", nMis == 1 ? "" : "s");
            }
            y += ch + 18;
        }
    }
    /* what the flow needs: a way in and a way out (a wind tunnel brings its own) */
    int in, out, wall, other;
    boundary_counts(&in, &out, &wall, &other);
    y += 8;
    if (STLP.m.domain == DOMAIN_EXTERNAL) {
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
    STLP.m.twoD = on; STLP.m.normal2D = axis;
    if (on && STLP.m.flowDir / 2 == axis) {                  /* the flow goes in the plane */
        STLP.m.flowDir = axis == 0 ? DIR_PY : DIR_PX;
        setup_flow_changed(A.cs);
    }
    STLP.m.upDir = mesher_up(&STLP.m);
    sm_save_cfg();
}

static float profile_rows(float x, float y, float w);

static float two_d_rows(float x, float y, float w)
{
    float y0 = y;
    double thick = part_thickness();
    int ta = stl_thin_axis(&STLP.stl);
    double text = STLP.stl.bmax[ta] - STLP.stl.bmin[ta], other = 0;
    for (int k = 0; k < 3; k++) if (k != ta) other = fmax(other, STLP.stl.bmax[k] - STLP.stl.bmin[k]);
    /* the whole part thin along an axis, and much thinner than it is long: a profile extruded, best meshed 2-D */
    if (!STLP.m.twoD && thick > 0 && fabs(text - thick) < 0.2 * text && text < 0.25 * other) {
        char t[32], d[400]; sm_fmt_len(t, sizeof t, text);
        pl_fmt_ui(d, sizeof d, "It is %s deep along %c and much larger the other ways: a profile extruded (an airfoil, a section). "
                  "Meshed 2-D, one cell thick across %c, it needs far fewer cells than in 3-D, where the cells must be smaller than %s.",
                  t, "XYZ"[ta], "XYZ"[ta], t);
        float ch = callout_height(w, "This looks 2-D", d) + 46;
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_LAYERS, "This looks 2-D", d);
        float tw = ui_button_width(ICON_LAYERS, "Mesh it 2-D");
        if (ui_button(ui_id("stl-2d-fix"), (Rectangle){ x + 42, y + ch - 44, tw, 34 }, ICON_LAYERS, "Mesh it 2-D", BTN_PRIMARY)) {
            set_two_d(true, ta);
            ui_toast(TOAST_OK, "2-D across %c: about %.0f thousand cells", "XYZ"[ta], mesher_estimate_cells(&STLP.m, &STLP.stl) / 1e3);
        }
        y += ch + 14;
    }
    Rectangle cr = prop_row(x, &y, w, "2-D", "A profile (an airfoil, a section) extruded along one axis: meshed [[empty|one cell thick]] across it, with "
                            "its front and back empty, as OpenFOAM's 2-D cases are. Far fewer cells than 3-D.", 24);
    bool on = STLP.m.twoD;
    if (ui_switch(ui_id("stl-2d"), cr.x, cr.y + 2, &on)) set_two_d(on, on ? stl_thin_axis(&STLP.stl) : STLP.m.normal2D);
    if (STLP.m.twoD) {
        static const char *const AX[3] = { "Across X", "Across Y", "Across Z" };
        int a = STLP.m.normal2D;
        if (ui_segmented(ui_id("stl-2d-axis"), (Rectangle){ cr.x + 60, cr.y - 5, fminf(330, cr.width - 60), 34 }, (const char **)AX, 3, &a) && a != STLP.m.normal2D)
            set_two_d(true, a);
        char t[32]; sm_fmt_len(t, sizeof t, STLP.stl.bmax[STLP.m.normal2D] - STLP.stl.bmin[STLP.m.normal2D]);
        char h[160]; pl_fmt_ui(h, sizeof h, "extruded %s along %c", t, "XYZ"[STLP.m.normal2D]);
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
        if (!STLP.m.stlOriginal[0]) return;
        char orig[PL_PATH]; pl_strlcpy(orig, STLP.m.stlOriginal, sizeof orig);
        pl_strlcpy(STLP.m.stl, orig, sizeof STLP.m.stl); STLP.m.stlOriginal[0] = 0;
        if (load_stl(orig, false)) ui_toast(TOAST_OK, "Back to %s", pl_basename(orig));
        return;
    }
    char dir[PL_PATH], out[PL_PATH], name[200];
    pl_join(dir, sizeof dir, STLP.casePath, "mesher"); pl_mkdirs(dir);
    pl_strlcpy(name, pl_basename(STLP.m.stl), sizeof name);
    char *dot = strrchr(name, '.'); if (dot) *dot = 0;
    char fn[240]; snprintf(fn, sizeof fn, "%s-smooth.stl", name);
    pl_join(out, sizeof out, dir, fn);
    int np0; double k0; stl_profile_facets(&STLP.stl, STLP.m.normal2D, 30, &np0, &k0);
    /* points about the minimum surface cell apart, so the curvature refinement sees no corners */
    int no = stl_smooth_profile(&STLP.stl, STLP.m.normal2D, 30, STLP.m.base * fmin(STLP.m.minPct, STLP.m.targetPct) / 100, out, err, sizeof err);
    if (!no) { ui_toast(TOAST_ERR, "%s", err); return; }
    char orig[PL_PATH]; pl_strlcpy(orig, STLP.m.stl, sizeof orig);
    pl_strlcpy(STLP.m.stl, out, sizeof STLP.m.stl);
    if (!load_stl(out, false)) { pl_strlcpy(STLP.m.stl, orig, sizeof STLP.m.stl); return; }
    pl_strlcpy(STLP.m.stlOriginal, orig, sizeof STLP.m.stlOriginal);
    sm_save_cfg();
    ui_toast(TOAST_OK, "The profile is smooth now: %d points instead of %d (mesher/%s)", no, np0, fn);
}

static float profile_rows(float x, float y, float w)
{
    if (!STLP.m.twoD || !STLP.loaded) return 0;
    static double sig = -1; static int nAx = -1, np; static double kink; static bool ok;
    if (sig != sm_geometry_sig() || nAx != STLP.m.normal2D) { sig = sm_geometry_sig(); nAx = STLP.m.normal2D; ok = stl_profile_facets(&STLP.stl, nAx, 30, &np, &kink); }
    float y0 = y;
    if (STLP.m.stlOriginal[0]) {
        char t[400]; pl_fmt_ui(t, sizeof t, "A smooth profile (%d points) made of %s, its sharp corners kept.", np, pl_basename(STLP.m.stlOriginal));
        float bw = ui_button_width(ICON_UNDO_2, "Use the original");
        ui_text_clip(F_REG, 13, t, x, y + 9, w - bw - 16, TH.text3);
        if (ui_button(ui_id("stl-unsmooth"), (Rectangle){ x + w - bw, y, bw, 34 }, ICON_UNDO_2, "Use the original", BTN_GHOST)) smooth_profile(false);
        return 46;
    }
    double resolve = 360.0 / fmax(3, STLP.m.curvature), limit = fmax(2, fmin(5, resolve));
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
              "CAD (a smaller chord tolerance) works as well.", np, kink, why, STLP.m.base * fmin(STLP.m.minPct, STLP.m.targetPct) / 100 * 1000);
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
    int d = STLP.m.domain;
    float cw = fminf(360, (w - 16) / 2), ch = 150;
    for (int i = 0; i < 2; i++) {
        Rectangle cr = { x + i * (cw + 16), y, cw, ch };
        if (flow_card(ui_idn(ui_id("stl-domain"), i), cr, i, d == i)) d = i;
    }
    if (d != STLP.m.domain) {
        STLP.m.domain = d;
        if (STLP.m.locPicked) { STLP.m.locPicked = false; ui_toast(TOAST_INFO, "The picked point in the fluid was for the other kind of flow: worked out again"); }
        mesher_size_defaults(&STLP.m, &STLP.stl, part_thickness());   /* the sizes suit each kind (and a thin part); the rest stays */
        sm_save_cfg();
    }
    y += ch + 14;
    y += note(x, y, w, STLP.m.domain == DOMAIN_INTERNAL
                     ? "The fluid fills the inside of the part: a duct, a manifold, a room. The part must be closed, with its inlets and outlets "
                       "as faces of it (you name them next)."
                     : "The part is a body in a wind tunnel: a wing, a car, a building. Say which way the air goes and which way is up; the "
                       "tunnel's size is in reference lengths, and the view shows it.");
    y += two_d_rows(x, y, w);
    {   /* E8: the point in the fluid (locationInMesh), worked out or picked on the part, with its check */
        double L[3]; bool ok; sm_loc_now(L, &ok);
        Rectangle cr = prop_row(x, &y, w, "Point in the fluid", "snappyHexMesh and cfMesh keep the cells on the side of the surface this point is on "
                                "([[location-in-mesh|locationInMesh]]). Worked out from the part; pick it on the part for a part with more than one "
                                "space inside, where the worked-out one can land in the wrong one.", 34);
        char t[160]; pl_fmt_ui(t, sizeof t, "(%.4g, %.4g, %.4g)  %s  ·  %s", L[0], L[1], L[2], STLP.m.locPicked ? "picked" : "worked out", ok ? "in the fluid" : "NOT in the fluid");
        ui_text_clip(F_MONO, 12.5f, t, cr.x, cr.y + 9, cr.width, ok ? TH.text2 : TH.err);
        y += 4;
        float bx = cr.x, bw = ui_button_width(ICON_CIRCLE_DOT, STLP.pickLoc ? "Click on the part ..." : "Pick on the part");
        if (ui_button(ui_id("stl-loc-pick"), (Rectangle){ bx, y, bw, 32 }, ICON_CIRCLE_DOT, STLP.pickLoc ? "Click on the part ..." : "Pick on the part", STLP.pickLoc ? BTN_PRIMARY : BTN_SECONDARY))
            STLP.pickLoc = !STLP.pickLoc;
        bx += bw + 8;
        if (STLP.m.locPicked) {
            bw = ui_button_width(ICON_UNDO_2, "Work it out");
            if (ui_button(ui_id("stl-loc-auto"), (Rectangle){ bx, y, bw, 32 }, ICON_UNDO_2, "Work it out", BTN_GHOST)) { STLP.m.locPicked = false; sm_save_cfg(); STLV.dirty = true; }
        }
        y += 44;
        if (!ok) {
            const char *d = "The point isn't in the fluid: the mesher would keep the wrong side (the inside of a body, or nothing at all). Pick it on the "
                            "part: click the inside face of a duct, or the outside of a body.";
            float ch = callout_height(w, NULL, d);
            callout((Rectangle){ x, y, w, ch }, TOAST_ERR, ICON_TRIANGLE_ALERT, NULL, d);
            y += ch + 10;
        }
    }
    if (STLP.m.domain == DOMAIN_EXTERNAL) {
        /* which way the flow goes (the view's arrow), which way is up (the ground and the top), what L is */
        Rectangle cr = prop_row(x, &y, w, "Flow direction", "The way the air goes, in the STL's axes: the inlet is on the other side. "
                                "A blade or a wing: along its chord, not across its span.", 34);
        int fd = STLP.m.flowDir;
        if (ui_segmented(ui_id("stl-flow"), (Rectangle){ cr.x, cr.y, fminf(360, cr.width), 34 }, (const char **)DIR_NAME, DIR__N, &fd) && fd != STLP.m.flowDir) {
            if (STLP.m.twoD && fd / 2 == STLP.m.normal2D) ui_toast(TOAST_ERR, "In 2-D the flow is in the plane: not along %c, the axis it is one cell thick across", "XYZ"[STLP.m.normal2D]);
            else { STLP.m.flowDir = fd; STLP.m.upDir = mesher_up(&STLP.m); sm_save_cfg(); setup_flow_changed(A.cs); }
        }
        /* up: one of the four directions square to the flow */
        const char *upNames[4]; int upDirs[4], nu = 0, cur = 0, ud = mesher_up(&STLP.m);
        for (int d = 0; d < DIR__N; d++) if (d / 2 != STLP.m.flowDir / 2) { if (d == ud) cur = nu; upDirs[nu] = d; upNames[nu++] = DIR_NAME[d]; }
        cr = prop_row(x, &y, w, "Up", "Where the top of the [[domain|tunnel]] is; the ground (if the body sits on it) is the other way.", 34);
        if (ui_segmented(ui_id("stl-updir"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, upNames, nu, &cur) && upDirs[cur] != ud) {
            STLP.m.upDir = upDirs[cur]; sm_save_cfg();
        }
        double Lauto = STLP.stl.bmax[STLP.m.flowDir / 2] - STLP.stl.bmin[STLP.m.flowDir / 2];
        char a[96];
        pl_fmt_ui(a, sizeof a, STLP.m.refLen > 0 ? "L = %.4g m (the body is %.4g m along the flow)" : "0: the body's length along the flow, L = %.4g m",
                  STLP.m.refLen > 0 ? STLP.m.refLen : Lauto, Lauto);
        if (row_num(x, &y, w, "stl-ref", "Reference length L", "The tunnel's distances below are in it. 0 takes the body's length along the flow "
                    "(a blade's chord); type a span or a diameter to measure in that instead.", &STLP.m.refLen, "%g", 0, 1e6, "m", a)) sm_save_cfg();
        if (row_num(x, &y, w, "stl-up", "Upstream", "Inlet distance ahead of the body.", &STLP.m.up, "%g", 0, 100, "L", NULL)) sm_save_cfg();
        if (row_num(x, &y, w, "stl-down", "Downstream", "Room for the wake.", &STLP.m.down, "%g", 0, 200, "L", NULL)) sm_save_cfg();
        if (row_num(x, &y, w, "stl-side", "Sides", STLP.m.twoD ? "Across the flow, in the plane. (2-D: not across the extrusion, which is one cell.)"
                                                           : "Across the flow, both sides.", &STLP.m.side, "%g", 0, 100, "L", NULL)) sm_save_cfg();
        if (row_num(x, &y, w, "stl-top", "Above", "Room over the body, toward Up.", &STLP.m.top, "%g", 0, 100, "L", NULL)) sm_save_cfg();
        ui_disable(STLP.m.ground);
        if (row_num(x, &y, w, "stl-below", "Below", STLP.m.ground ? "On the ground: the floor touches the body." : "Room under the body, away from Up.",
                    &STLP.m.below, "%g", 0, 100, "L", NULL)) sm_save_cfg();
        ui_disable(false);
        cr = prop_row(x, &y, w, "On the ground", "The floor is under the body (on the side away from Up), touching it, and is a wall (a car, a building).", 24);
        if (ui_switch(ui_id("stl-ground"), cr.x, cr.y + 2, &STLP.m.ground)) sm_save_cfg();
    }
    return y - y0;
}

static float mesher_section(float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "Mesher", NULL);
    int ms = STLP.m.mesher;
    /* listed by engine: OpenFOAM's, then SALOME's (MESHERS keeps mesher.ini's order) */
    static const char *names[MESHER__N]; static int order[MESHER__N]; int no = 0;
    for (int e = ENG_OPENFOAM; e <= ENG_SALOME; e++) for (int k = 0; k < MESHER__N; k++) if (MESHERS[k].engine == e) { order[no] = k; names[no++] = MESHERS[k].name; }
    int shown = 0; for (int i = 0; i < no; i++) if (order[i] == ms) shown = i;
    float dw = fminf(440, w - 230);                       /* the names are long ("OpenFOAM · Cartesian hex: cfMesh cartesianMesh") */
    if (ui_select(ui_id("stl-mesher"), (Rectangle){ x, y, dw, 36 }, names, no, &shown) && order[shown] != STLP.m.mesher) { STLP.m.mesher = order[shown]; sm_save_cfg(); }
    if (salome_mesher() && !STLP.m.twoD) {                  /* 2-D: SALOME always builds the profile's face as CAD */
        float sx = x + dw + 20;
        if (MESHERS[STLP.m.mesher].flags & MF_CAD) { ui_disable(true); bool on = true; ui_switch(ui_id("stl-remesh-cad"), sx, y + 8, &on); }
        else
        if (ui_switch(ui_id("stl-remesh"), sx, y + 8, &STLP.m.remesh)) sm_save_cfg();
        ui_disable(false);
        if (MESHERS[STLP.m.mesher].flags & MF_CAD) ui_tooltip((Rectangle){ sx, y, 200, 36 }, "This mesher works on CAD faces: always on");
        bool cad = STLP.m.cad[0] && fabs(STLP.m.scale - 1) < 1e-12;
        ui_text(F_MED, 13, cad ? "Mesh the exact CAD" : "Remesh surface", sx + 46, y + 9, TH.text2);
        if (cad) ui_tooltip((Rectangle){ sx, y, 200, 36 }, "On: NETGEN meshes the CAD file's own faces at the target size: round holes stay round, "
                            "fast, and it can grow prism layers. Off: NETGEN meshes on the STL made from it.");
        else ui_tooltip((Rectangle){ sx, y, 180, 36 }, "Off: NETGEN meshes on the STL's own triangles (fast, any size); they are remeshed at the target size "
                   "first when they are much coarser than it or many are slivers. On: SALOME builds CAD from the STL and NETGEN remeshes "
                   "the surface at the target size (best for CAD-made STL with flat faces; slow above a few thousand triangles; needed "
                   "for prism layers).");
    }
    y += 44;
    {   /* what it makes; a mesher that can't run here says why */
        const MesherInfo *M = &MESHERS[STLP.m.mesher];
        y += note(x, y, w, STLP.m.twoD ? (M->what2D ? M->what2D : "2-D: this mesher meshes 3-D parts only; pick another for a profile.") : M->what);
        if (M->engine == ENG_SALOME && !STLP.m.twoD && !STLP.m.cad[0]) {   /* user's intake, 2026-11-10: 110 k STL triangles became
                                                                            110 k CAD faces, and NETGEN's layers need CAD faces */
            const char *t = "SALOME works better from a STEP file";
            const char *d = "From an STL, SALOME has to build CAD out of its triangles: slow on a fine STL (one face per triangle), and "
                            "its prism layers only grow on CAD faces. Open the part's STEP or IGES file under Geometry instead: SALOME "
                            "then meshes the real faces, round holes stay round, and the prism layers work.";
            float ch = callout_height(w, t, d);
            callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_INFO, t, d);
            y += ch + 12;
        }
        if (STLP.m.mesher == MESHER_SGMSH) {                 /* Gmsh's own choice */
            Rectangle cr = prop_row(x, &y, w, "Gmsh 3-D algorithm", "Delaunay: robust. Frontal: better-shaped cells, slower. HXT: fast, "
                                    "parallel, for big parts. MMG3D: Delaunay then optimised. R-tree: experimental.", 34);
            int ga = STLP.m.gmshAlgo;
            if (ui_segmented(ui_id("stl-gmsh-algo"), (Rectangle){ cr.x, cr.y, fminf(420, cr.width), 34 }, (const char **)GMSH_ALGO_NAME, GMSH_ALGOS, &ga) && ga != STLP.m.gmshAlgo) {
                STLP.m.gmshAlgo = ga; sm_save_cfg();
            }
        }
        if (M->engine == ENG_SALOME && !(M->flags & MF_CAD) && !STLP.m.remesh && !STLP.m.twoD) {   /* the STL's triangles: who remeshes them */
            static const char *RM[2] = { "NETGEN", "MMG" };
            Rectangle cr = prop_row(x, &y, w, "Surface remesher", "When the STL's triangles are much coarser than the target size, or "
                                    "slivers, they are remeshed first. MMG keeps sharp edges and the shape closer on curved parts.", 34);
            int rm = STLP.m.surfRemesher;
            if (ui_segmented(ui_id("stl-surf-remesh"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, RM, 2, &rm) && rm != STLP.m.surfRemesher) {
                STLP.m.surfRemesher = rm; sm_save_cfg();
            }
        }
        if (!STLP.m.twoD && !(M->flags & MF_POLY)) {         /* any 3-D mesh can be made polyhedral afterwards */
            ui_switch(ui_id("stl-polyconv"), x, y + 2, &STLP.m.polyConvert) ? sm_save_cfg() : (void)0;
            ui_text(F_MED, 13, "Convert to polyhedra afterwards (polyDualMesh)", x + 46, y + 3, TH.text2);
            ui_tooltip((Rectangle){ x, y, 380, 26 }, "Each cell of the finished mesh becomes a polyhedron round its corner points: fewer, better-connected "
                       "cells, often a faster-converging run. Layers stay layers.");
            y += 34;
            if (STLP.m.polyConvert && STLP.m.prism && STLP.m.layers > 0 && (M->flags & MF_LAYERS)) {
                const char *t = "Polyhedra and prism layers";
                const char *d = "polyDualMesh can turn thin prism layers inside out (on some parts a hundred or so cells). "
                                "Generate checks the result and keeps the mesh as the mesher made it if any cell is inside out.";
                float ch = callout_height(w, t, d);
                callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, t, d);
                y += ch + 12;
            }
        }
        char why[300];
        const FoamInstall *fi = env_active(&A.env, &A.set);
        if (!mesher_available(STLP.m.mesher, fi ? fi->root : "", A.env.salome[0] != 0, why, sizeof why) && M->engine == ENG_OPENFOAM) {
            float ch = callout_height(w, "Not in this OpenFOAM", why);
            callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "Not in this OpenFOAM", why);
            y += ch + 12;
        }
    }

    /* the one to use, for someone who doesn't know (tests/compare_meshers.sh, 2026-11-10: every mesher on a box, the flange,
       a sphere in a tunnel and the NACA profile): a block, structured hexahedra (perfect cells, the fewest); a 2-D profile,
       SALOME's triangles with layers all round (the fewest cells); anything else snappyHexMesh (the only one that passed every
       check on the curved flange) */
    {
        bool block = !STLP.m.twoD && STLP.m.domain == DOMAIN_INTERNAL && STLP.stl.nRegions == 6;
        for (int i = 0; i < STLP.stl.nRegions && block; i++) {
            const float *n = STLP.stl.reg[i].n;
            if (fmaxf(fabsf(n[0]), fmaxf(fabsf(n[1]), fabsf(n[2]))) < 0.999f) block = false;   /* a flat face square to an axis */
        }
        bool sa = A.env.salome[0] != 0;
        int rec = block && sa ? MESHER_SHEXA : STLP.m.twoD && sa ? MESHER_TET : MESHER_SNAPPY;
        const char *why = rec == MESHER_SHEXA ? "a block: perfect hexahedra in the fewest cells"
                        : rec == MESHER_TET ? "layers all round the profile, the fewest cells"
                        : "the most robust: it passed every check on curved parts";
        char t[200];
        pl_fmt_ui(t, sizeof t, "Recommended for this part: %s (%s).", MESHERS[rec].shortName, why);
        if (rec == STLP.m.mesher) {
            ui_icon(ICON_CIRCLE_CHECK, x + 9, y + 9, 15, TH.ok);
            ui_text_clip(F_MED, 13, t, x + 24, y + 1, w - 24, TH.ok);
            y += 28;
        } else {
            ui_icon(ICON_SPARKLES, x + 9, y + 15, 15, TH.accent);
            float uw = ui_button_width(NULL, "Use it");
            float tw = fminf(ui_text_w(F_MED, 13, t), w - uw - 40);
            ui_text_clip(F_MED, 13, t, x + 24, y + 7, tw, TH.text2);
            if (ui_button(ui_id("stl-use-rec"), (Rectangle){ x + 24 + tw + 14, y, uw, 30 }, NULL, "Use it", BTN_SECONDARY)) {
                STLP.m.mesher = rec; sm_save_cfg();
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
    MeshCfg *m = &STLP.m;
    y += section_head(x, y, w, "Cell size", "Everything is sized from the [[base-size|base size]], as in STAR-CCM+. Pick a density to start from; the "
                                           "numbers under it are what it sets.");
    /* the density: the defaults for this part and flow, the base size doubled or halved */
    {
        MeshCfg def = *m;
        mesher_size_defaults(&def, &STLP.stl, part_thickness());
        static const char *dn[] = { "Coarse", "Medium", "Fine", "Custom" };
        static const double dk[] = { 2, 1, 0.5 };
        int cur = 3;
        for (int i = 0; i < 3; i++)
            if (fabs(m->base - def.base * dk[i]) < 1e-9 * def.base && fabs(m->targetPct - def.targetPct) < 1e-9 && fabs(m->minPct - def.minPct) < 1e-9) cur = i;
        int pick = cur;
        if (ui_segmented(ui_id("stl-density"), (Rectangle){ x, y, fminf(420, w), 34 }, dn, 4, &pick) && pick != cur && pick < 3) {
            m->base = def.base * dk[pick]; m->targetPct = def.targetPct; m->minPct = def.minPct; m->prismPct = def.prismPct;
            sm_save_cfg();
        } else if (pick == 3 && cur != 3) STLP.adv = true;   /* Custom: the numbers to change are below */
        y += 44;
        /* what that means, in numbers anyone can picture */
        double Lp = 0; for (int k = 0; k < 3; k++) Lp = fmax(Lp, STLP.stl.bmax[k] - STLP.stl.bmin[k]);
        double hs = mesher_surface_cell(m), n = mesher_estimate_cells(m, &STLP.stl);
        char t[400], cl[64]; fmt_cells(cl, sizeof cl, n);
        pl_fmt_ui(t, sizeof t, "About %.0f cells along the part's length at its [[surface-size|surface]], %s in all. Finer is more accurate and slower to run, until "
                  "the answer [[mesh-independence|stops changing]].",
                  hs > 0 ? Lp / hs : 0, cl);
        y += note(x, y, w, t);
        Working wk; wk_surface_cell(&wk, m->base, m->targetPct, MESHERS[m->mesher].flags & MF_OCTREE, Lp);
        y += teach_working("stl-wk-size", x, y - 6, w, &wk) - 2;
    }
    char a[96], l1[32], l2[32];
    /* a part thinner than its surface cells: snappyHexMesh can't carve it out */
    double thick = part_thickness();
    int ta = stl_thin_axis(&STLP.stl);
    double text = STLP.stl.bmax[ta] - STLP.stl.bmin[ta];
    bool extrusion = thick > 0 && fabs(text - thick) < 0.2 * text;   /* the thinnest is the whole part along an axis: a profile extruded */
    if (m->twoD && extrusion && ta == m->normal2D) thick = 0;          /* 2-D: that depth is one cell anyway */
    if (thick > 0 && !mesher_fits(m, thick)) {
        char t2[32], ts[32], d[500];
        sm_fmt_len(t2, sizeof t2, thick); sm_fmt_len(ts, sizeof ts, m->base * m->targetPct / 100);
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
                ui_toast(TOAST_OK, "2-D across %c: about %.0f thousand cells", "XYZ"[ta], mesher_estimate_cells(m, &STLP.stl) / 1e3);
            }
            bx += tw + 10;
        }
        if (ui_button(ui_id("stl-fit"), (Rectangle){ bx, y + ch - 44, fw, 34 }, ICON_WRENCH, "Fit the sizes", extrusion && !m->twoD ? BTN_SECONDARY : BTN_PRIMARY)) {
            mesher_fit(m, thick); sm_save_cfg();
            ui_toast(TOAST_OK, "Surface cells %g %% and %g %% of the base size: about %.0f thousand cells", m->targetPct, m->minPct,
                     mesher_estimate_cells(m, &STLP.stl) / 1e3);
        }
        y += ch + 14;
    }
    if (row_num(x, &y, w, "stl-base", "Base size", "The largest [[cell]], away from the surface.", &m->base, "%.4g", 1e-9, 1e6, "m", NULL)) sm_save_cfg();
    sm_fmt_len(l1, sizeof l1, m->base * m->targetPct / 100);
    snprintf(a, sizeof a, "= %s", l1);
    if (row_num(x, &y, w, "stl-target", "Target surface size", "The [[surface-size|cell size on the surface]].", &m->targetPct, "%g", 0.01, 1000, "%", a)) sm_save_cfg();
    if (!STLP.adv) return y - y0;
    sm_fmt_len(l2, sizeof l2, m->base * m->minPct / 100);
    snprintf(a, sizeof a, "= %s", l2);
    if (row_num(x, &y, w, "stl-min", "Minimum surface size", "Reached on tight curves and sharp edges.", &m->minPct, "%g", 0.01, 1000, "%", a)) sm_save_cfg();
    if (row_num(x, &y, w, "stl-curv", "Surface curvature", "Cells around a full circle on curved surfaces ([[curvature]]).", &m->curvature, "%g", 4, 360, "pts/circle", NULL)) sm_save_cfg();
    if (row_num(x, &y, w, "stl-sgrow", "Surface growth rate", "How fast cells grow along the surface (SALOME).", &m->surfGrowth, "%g", 1.01, 3, NULL, NULL)) sm_save_cfg();
    if (row_num(x, &y, w, "stl-vgrow", "Volume growth rate", "How fast cells grow away from the surface ([[expansion-ratio|growth rate]]): lower is smoother and more cells.", &m->volGrowth, "%g", 1.01, 3, NULL, NULL)) sm_save_cfg();
    if (row_num(x, &y, w, "stl-feat", "Feature angle", "[[feature-edge|Edges]] sharper than this are kept crisp (snapped to, not smoothed over).", &m->featureAngle, "%g", 1, 180, "°", NULL)) sm_save_cfg();
    return y - y0;
}

/* SALOME's layers (assets/salome/stl_mesher.py): on its 2-D face always; in 3-D on the part rebuilt as CAD (Remesh surface),
   or grown out of the STL's own surface for a body in a wind tunnel that is all walls, off the ground; never Body Fitting's */
static bool salome_layers(const MeshCfg *m)
{
    if (m->twoD) return m->mesher != MESHER_SGMSH && m->mesher != MESHER_SHEXA && m->mesher != MESHER_SPRISM;
    if (!(MESHERS[m->mesher].flags & MF_LAYERS)) return false;
    if (MESHERS[m->mesher].flags & MF_CAD) return true;
    if (m->remesh) return true;
    if (m->domain != DOMAIN_EXTERNAL || m->ground || !STLP.loaded) return false;
    for (int i = 0; i < STLP.stl.nRegions; i++) if (STLP.stl.reg[i].type != BT_WALL) return false;
    return true;
}

static float prism_section(float x, float y, float w)
{
    float y0 = y;
    MeshCfg *m = &STLP.m;
    bool none = salome_mesher() && !salome_layers(m);
    y += section_head(x, y, w, "Prism layers", m->twoD ? "Thin cells along the walls, for the [[boundary-layer]]: in 2-D they follow the profile's outline."
                                                      : "Thin [[prism-layers|cells stacked on the walls]], for the [[boundary-layer]].");
    Rectangle cr = prop_row(x, &y, w, "Prism layer mesher", NULL, 24);
    if (ui_switch(ui_id("stl-prism"), cr.x, cr.y + 2, &m->prism)) sm_save_cfg();
    if (!m->prism) return y - y0;
    if (none) {                                           /* SALOME 3-D on the STL's triangles: viscous layers need CAD faces */
        bool noLayers = !(MESHERS[m->mesher].flags & MF_LAYERS);
        const char *t2 = noLayers ? TextFormat("%s makes no prism layers", MESHERS[m->mesher].shortName) : "This mesh will have no prism layers";
        const char *d2 = noLayers
            ? "Use the tetrahedral or polyhedral mesher with Remesh surface on (a CAD-made part with flat faces), or snappyHexMesh."
            : "On the STL's own triangles SALOME grows prism layers for a body in a wind tunnel whose boundaries are all walls, off the "
              "ground. Here (internal flow, an inlet or outlet on the part, or on the ground) switch Remesh surface on for a CAD-made "
              "part with flat faces, or use snappyHexMesh, which makes them on any part.";
        float ch = callout_height(w, t2, d2);
        callout((Rectangle){ x, y + 4, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, t2, d2);
        return y + ch + 18 - y0;
    }
    double n = m->layers;
    if (row_num(x, &y, w, "stl-nl", "Number of prism layers", NULL, &n, "%g", 1, 50, NULL, NULL)) { m->layers = (int)n; sm_save_cfg(); }
    /* both meshers hold the outer layer to a share of the surface cell next to it (stlmesh.h mesher_layer_rel) */
    if (m->mesher == MESHER_SNAPPY) {
        /* snappy's layers collapse where the surface cells halve: a coarse STL's facet corners and the curvature refinement
           make steps all along a profile, and the layers broke at each (user's airfoil, 2026-10-26: 12 of 20 layers) */
        mesher_resolve_even(m, &STLP.stl);
        bool even = mesher_even_layers(m), was = even, autoEven = m->twoD || m->evenAuto;
        Rectangle er = prop_row(x, &y, w, "Even surface cells under the layers",
                                "The walls with layers all at the minimum surface size, so the layers don't break where the size steps. "
                                "More cells: a fifth more on a curved part, many times more on a flat-sided one. On by itself in 2-D and where it "
                                "costs at most half again as many cells.", 24);
        ui_switch(ui_id("stl-even"), er.x, er.y + 2, &even);
        if (even != was) { m->evenLayers = even == autoEven ? -1 : even; sm_save_cfg(); }
    }
    double T0 = m->base * m->prismPct / 100, h = mesher_layer_cell(m), Tm = mesher_layer_rel(m, NULL) * h;
    char a[96], l[32], t0[32]; sm_fmt_len(t0, sizeof t0, T0); snprintf(a, sizeof a, "= %s", t0);
    if (row_num(x, &y, w, "stl-pt", "Prism layer total thickness", "[[layer-thickness|The whole stack]], as a share of the base size.", &m->prismPct, "%g", 0.1, 500, "%", a)) sm_save_cfg();
    double r = m->stretch, first = r == 1 ? Tm / m->layers : Tm * (r - 1) / (pow(r, m->layers) - 1);
    sm_fmt_len(l, sizeof l, first); snprintf(a, sizeof a, "first layer %s", l);
    if (row_num(x, &y, w, "stl-ps", "Prism layer stretching", "Each layer this much thicker than the one below (the [[expansion-ratio]]).", &m->stretch, "%g", 1, 3, NULL, a)) sm_save_cfg();
    char tl[32], hl[32], t2[480];
    sm_fmt_len(tl, sizeof tl, Tm); sm_fmt_len(hl, sizeof hl, h);
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
    Working wk; wk_layer_stack(&wk, Tm, m->layers, r, h);
    y += teach_working("stl-wk-layers", x, y, w, &wk);
    /* the first layer from y+, for the flow this part will see: the Calculators dialog, sent back here */
    float cw = ui_button_width(ICON_SIGMA, "First layer from y+");
    if (ui_button(ui_id("stl-calc-yplus"), (Rectangle){ x, y, cw, 32 }, ICON_SIGMA, "First layer from y+", BTN_SECONDARY)) calc_open(CALC_WALL);
    ui_tooltip((Rectangle){ x, y, cw, 32 }, "Work out the first layer height for a y+ from the flow speed, the fluid (cryogens too) and the part's size, then send it here");
    y += 42;
    return y - y0;
}

static float zones_section(float x, float y, float w)
{
    float y0 = y;
    MeshCfg *m = &STLP.m;
    y += section_head(x, y, w, "Volumetric controls", "[[refinement-region|Finer cells]] inside a box, sphere or cylinder: a wake, a gap, a jet. Coordinates in metres.");
    static const char *kinds[] = { "Box", "Sphere", "Cylinder" };
    static const char *const AX[3] = { "x", "y", "z" };
    int del = -1;
    for (int i = 0; i < m->nZones; i++) {
        MeshZone *z = &m->zone[i];
        ui_push_id(ui_idn(ui_id("stl-z"), i));
        Rectangle box = { x, y, w, z->kind == ZONE_SPHERE ? 92 : 132 };
        ui_rect(box, 8, TH.surface2); ui_rect_lines(box, 8, 1, TH.lineSoft);
        int k = z->kind;
        if (ui_select(ui_id("kind"), (Rectangle){ x + 12, y + 12, 130, 32 }, kinds, 3, &k) && k != z->kind) { z->kind = k; sm_save_cfg(); }
        double sz = z->sizePct;
        ui_text(F_REG, 13, "Size", x + 160, y + 20, TH.text2);
        char l[32], a[48]; sm_fmt_len(l, sizeof l, m->base * z->sizePct / 100); snprintf(a, sizeof a, "= %s", l);
        if (num_field(ui_id("size"), (Rectangle){ x + 200, y + 12, 110, 32 }, &sz, "%g", 0.01, 1000, "%")) { z->sizePct = sz; sm_save_cfg(); }
        ui_text(F_REG, 13, a, x + 320, y + 20, TH.text3);
        if (ui_button(ui_id("del"), (Rectangle){ x + w - 44, y + 12, 32, 32 }, ICON_TRASH_2, NULL, BTN_ICON)) del = i;
        const char *la = z->kind == ZONE_BOX ? "Min" : z->kind == ZONE_SPHERE ? "Centre" : "End 1";
        float fy = y + 52;
        ui_text(F_REG, 13, la, x + 12, fy + 8, TH.text2);
        for (int c = 0; c < 3; c++)
            if (num_field(ui_idn(ui_id("a"), c), (Rectangle){ x + 70 + c * 130, fy, 120, 32 }, &z->a[c], "%.4g", -1e9, 1e9, AX[c])) sm_save_cfg();
        if (z->kind == ZONE_SPHERE || z->kind == ZONE_CYLINDER) {
            ui_text(F_REG, 13, "Radius", x + 470, fy + 8, TH.text2);
            if (num_field(ui_id("r"), (Rectangle){ x + 530, fy, 120, 32 }, &z->r, "%.4g", 1e-9, 1e9, "m")) sm_save_cfg();
        }
        if (z->kind != ZONE_SPHERE) {
            fy += 40;
            ui_text(F_REG, 13, z->kind == ZONE_BOX ? "Max" : "End 2", x + 12, fy + 8, TH.text2);
            for (int c = 0; c < 3; c++)
                if (num_field(ui_idn(ui_id("b"), c), (Rectangle){ x + 70 + c * 130, fy, 120, 32 }, &z->b[c], "%.4g", -1e9, 1e9, AX[c])) sm_save_cfg();
        }
        ui_pop_id();
        y += box.height + 10;
    }
    if (del >= 0) {
        for (int i = del; i < m->nZones - 1; i++) m->zone[i] = m->zone[i + 1];
        m->nZones--; sm_save_cfg();
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
            const Stl *s = &STLP.stl;
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
            sm_save_cfg();
        }
        bx += bw + 10;
    }
    ui_disable(false);
    y += 50;
    return y - y0;
}

/* ---------------------------------------------------------------------------- 4 Mesh settings: the mesher, the sizes, the layers */
/* presets (plan step 10, E10): these settings kept by name for other parts, or a kept one used here */
static void preset_dir(char *out, size_t cap)              /* beside the settings: $XDG_CONFIG_HOME/q-continuum, else ~/.config/q-continuum */
{
    const char *x = getenv("XDG_CONFIG_HOME");
    if (x && *x) snprintf(out, cap, "%s/q-continuum/mesh-presets", x);
    else snprintf(out, cap, "%s/.config/q-continuum/mesh-presets", pl_home());
}
static float presets_row(float x, float y, float w)
{
    char dir[PL_PATH]; preset_dir(dir, sizeof dir);
    static char names[64][64]; static int n = -1; static double at;
    if (n < 0 || GetTime() - at > 3) { n = mesher_preset_list(dir, names, 64); at = GetTime(); }
    ui_text(F_REG, 13.5f, "Presets", x, y + 9, TH.text2);
    float px = x + 96;
    if (n > 0) {
        const char *items[64]; for (int i = 0; i < n; i++) items[i] = names[i];
        int sel = -1;
        if (ui_select_ph(ui_id("stl-preset"), (Rectangle){ px, y, 220, 34 }, items, n, &sel, "Use a preset ...") && sel >= 0) {
            if (mesher_preset_apply(dir, names[sel], &STLP.m, &STLP.stl)) { sm_save_cfg(); ui_toast(TOAST_OK, "Preset %s: its sizes, layers and mesher (the base size scaled to this part)", names[sel]); }
            else ui_toast(TOAST_ERR, "Could not read the preset %s", names[sel]);
        }
        px += 230;
    }
    static char nm[64];
    float nw = fmaxf(110, fminf(180, x + w - px - 188 - ui_button_width(ICON_SAVE, "Save as preset")));   /* narrow: the name box gives way */
    ui_input(ui_id("stl-preset-name"), (Rectangle){ px, y, nw, 34 }, nm, sizeof nm, "name", 0);
    px += nw - 180;
    float bw = ui_button_width(ICON_SAVE, "Save as preset");
    if (ui_button(ui_id("stl-preset-save"), (Rectangle){ px + 188, y, bw, 34 }, ICON_SAVE, "Save as preset", BTN_GHOST)) {
        bool okName = nm[0] && !strchr(nm, '/') && nm[0] != '.';
        if (!okName) ui_toast(TOAST_ERR, "Give the preset a name (no /)");
        else if (mesher_preset_save(dir, nm, &STLP.m, &STLP.stl)) { ui_toast(TOAST_OK, "Saved the preset %s (~/.config/q-continuum/mesh-presets)", nm); n = -1; nm[0] = 0; }
        else ui_toast(TOAST_ERR, "Could not write the preset");
    }
    ui_tooltip((Rectangle){ px + 188, y, bw, 34 }, "These sizes, layers, mesher and quality limits kept by name: Use a preset on any other part");
    return 46;
}

static float mesh_step(float x, float y, float w)
{
    float y0 = y;
    y += presets_row(x, y, w);
    y += mesher_section(x, y, w);
    y += sizes_section(x, y + 6, w) + 6;
    y += prism_section(x, y + 6, w) + 6;
    /* the rest when wanted: growth, curvature, feature angle (in the sizes above) and the refinement regions */
    if (STLP.m.nZones > 0) STLP.adv = true;
    ui_switch(ui_id("stl-adv"), x, y + 10, &STLP.adv);
    int mine = meshadv_count(STLP.casePath, &STLP.m);
    char al[160];
    pl_fmt_ui(al, sizeof al, "Advanced: every OpenFOAM meshing setting, refinement regions, minimum size, growth%s", "");
    ui_text_clip(F_MED, 13.5f, al, x + 46, y + 11, w - 46, TH.text2);
    if (mine && !STLP.adv) {                                  /* hidden, still applied: say so */
        char t[96]; pl_fmt_ui(t, sizeof t, "%d OpenFOAM setting%s of yours in use", mine, mine == 1 ? "" : "s");
        ui_text_clip(F_REG, 12.5f, t, x + 46, y + 31, w - 46, TH.accent);
        y += 18;
    }
    y += 44;
    if (STLP.adv) {
        y += zones_section(x, y + 6, w) + 6;
        y += meshadv_panel(STLP.casePath, &STLP.m, &STLP.stl, x, y + 10, w) + 10;
    }
    return y - y0;
}

static double cells_estimate(void) { return mesher_estimate_cells(&STLP.m, &STLP.stl); }

double stlmesh_cells_estimate(Case *c)
{
    if (!c) return 0;
    sync_case(c);
    return STLP.loaded ? cells_estimate() : 0;
}

static void fmt_cells(char *b, size_t cap, double n)
{
    if (n >= 1e6) pl_fmt_ui(b, cap, "about %.1f million cells", n / 1e6);
    else pl_fmt_ui(b, cap, "about %.0f thousand cells", fmax(1, n / 1e3));
}

/* after a SALOME mesh: the prism layers asked for and not made, said out loud with the reason (mesh overhaul plan step 3,
   gap L11). The script says so in log.salome when it drops them (NETGEN grows viscous layers on CAD faces, not on an STL's
   bare triangles, unless the part is an all-wall body off the ground); checkMesh counting no prisms says so for the rest.
   Reads the logs again only when they change. */
static bool layers_missing(Case *c, char *why, size_t cap)
{
    static char path[PL_PATH], said[400]; static time_t mtS, mtC; static long prisms; static bool have;
    char ls[PL_PATH], lc[PL_PATH];
    pl_join(ls, sizeof ls, c->s.path, "log.salome"); pl_join(lc, sizeof lc, c->s.path, "log.checkMesh");
    time_t a = pl_mtime(ls), b = pl_mtime(lc);
    if (strcmp(path, c->s.path) || a != mtS || b != mtC) {
        pl_strlcpy(path, c->s.path, sizeof path); mtS = a; mtC = b; said[0] = 0; prisms = -1; have = false;
        char *t = a ? pl_read_file(ls, NULL) : NULL;
        for (char *q = t ? strstr(t, "Q-Continuum: ") : NULL; q; q = strstr(q + 13, "Q-Continuum: ")) {
            char *e = strchr(q, '\n'); size_t n = e ? (size_t)(e - q) : strlen(q);
            char line[400]; snprintf(line, sizeof line, "%.*s", (int)(n - 13 < sizeof line ? n - 13 : sizeof line - 1), q + 13);
            if (strstr(line, "without prism layers") || strstr(line, "without them") || strstr(line, "this mesh has none")) {
                pl_strlcpy(said, line, sizeof said); have = true;
            }
        }
        free(t);
        char *k = b ? pl_read_file(lc, NULL) : NULL;
        const char *p = k ? strstr(k, "prisms:") : NULL;
        if (p) prisms = atol(p + 7);
        free(k);
    }
    bool poly = STLP.m.mesher == MESHER_POLY || STLP.m.polyConvert;  /* polyDualMesh turns the prisms into polyhedra: only the log tells */
    if (have) { snprintf(why, cap, "SALOME said: %s.", said); return true; }
    if (!poly && prisms == 0) { snprintf(why, cap, "checkMesh counts no prisms in it."); return true; }
    return false;
}

static float layers_missing_notice(Case *c, float x, float y, float w)
{
    MeshCfg *m = &STLP.m;
    if (!c->s.meshed || !salome_mesher() || !m->prism || m->layers <= 0 || stlmesh_mesh_current(c) == 0) return 0;
    char why[460];
    if (!layers_missing(c, why, sizeof why)) return 0;
    char t[1100], hl[32];
    sm_fmt_len(hl, sizeof hl, mesher_surface_cell(m));
    bool said = !strncmp(why, "SALOME said", 11);          /* its words name the fixes already: add only the STEP file */
    pl_fmt_ui(t, sizeof t, "You asked for %d prism layers; this mesh has none. %s Without them the cells on the walls are as big as the "
              "surface cells (%s): y+ is far above 1, so a resolved wall isn't, and the wall shear stress, drag and heat transfer come out "
              "wrong. %s", m->layers, why, hl, said ? "A STEP file of the part (Geometry) also gives SALOME the CAD faces it grows layers on." :
              "SALOME grows layers on CAD faces: give it the part as a STEP file (Geometry), or switch Remesh surface on for a CAD-made "
              "part with flat faces, or use snappyHexMesh, which makes layers on any STL.");
    const char *title = "This mesh has no prism layers";
    float ch = callout_height(w, title, t);
    callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, title, t);
    return ch + 14;
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
    /* memory and time before meshing (plan step 2, gap L10): a mesh that can't fit is held back until the user insists */
    double avail = free_mb();
    CalcCostIn ci = { .cells = n, .solver = CS_STEADY, .steps = 1000, .procs = run_procs_for_check(), .memAvailMB = avail, .nCpu = A.env.nCpu };
    CalcCostOut co; calc_cost(&ci, &co, NULL);
    bool tooBig = mesh_too_big(n, NULL);
    {
        char t[300], tm[48];
        if (co.hours < 1) pl_fmt_ui(tm, sizeof tm, "%.0f min", fmax(1, co.hours * 60)); else pl_fmt_ui(tm, sizeof tm, "%.1f h", co.hours);
        pl_fmt_ui(t, sizeof t, "Meshing needs about %.1f GB of the %.1f GB free; a steady run about %.1f GB and %s for 1000 iterations on %d process%s.",
                  co.meshMB / 1024, avail / 1024, co.solveMB / 1024, tm, ci.procs, ci.procs == 1 ? "" : "es");
        float cw = ui_button_width(ICON_SIGMA, "Calculators");
        float th2 = ui_text_wrap(F_REG, 13, t, (Rectangle){ x, y, w - cw - 16, 80 }, 19, co.level ? TH.warn : TH.text3, true);
        if (ui_button(ui_id("stl-calc-cost"), (Rectangle){ x + w - cw, y - 4, cw, 30 }, ICON_SIGMA, "Calculators", BTN_GHOST)) calc_open(CALC_COST);
        ui_tooltip((Rectangle){ x + w - cw, y - 4, cw, 30 }, "Memory and run time worked out: other solvers, iterations and process counts");
        y += fmaxf(26, th2) + 10;
    }
    if (tooBig) {
        char t[400];
        pl_fmt_ui(t, sizeof t, "Meshing this needs about %.1f GB and only %.1f GB is free: Linux would stop snappyHexMesh part way (\"Killed\"), "
                  "or the whole machine would crawl. A larger base size or Coarse under Cell size brings it down: half the cell size is "
                  "eight times the cells.", co.meshMB / 1024, avail / 1024);
        float ch = callout_height(w, "This mesh won't fit in memory", t) + 46;
        callout((Rectangle){ x, y, w, ch }, TOAST_ERR, ICON_TRIANGLE_ALERT, "This mesh won't fit in memory", t);
        float aw = ui_button_width(NULL, "Generate anyway");
        if (ui_button(ui_id("stl-gen-anyway"), (Rectangle){ x + 42, y + ch - 44, aw, 34 }, NULL, "Generate anyway", BTN_GHOST)) { gen_armed_for = n; tooBig = false; }
        y += ch + 12;
    }
    /* where the mesh on disk stands against these settings */
    int cur = c->s.meshed ? stlmesh_mesh_current(c) : -1;
    if (c->s.meshed && cur == 0 && !running) {
        const char *d = "The settings changed after the mesh on disk was made from them: generate it again for them to count.";
        float ch = callout_height(w, NULL, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, NULL, d);
        y += ch + 12;
    }
    float bx = x, bw;
    ui_disable(running || tooBig);
    bw = ui_button_width(ICON_PLAY, "Generate mesh");
    if (ui_button(ui_id("stl-gen"), (Rectangle){ bx, y, bw, 42 }, ICON_PLAY, "Generate mesh", BTN_PRIMARY)) generate(c);
    bx += bw + 10;
    ui_disable(running);
    bw = ui_button_width(ICON_FILE_TEXT, "Write files only");
    if (ui_button(ui_id("stl-write"), (Rectangle){ bx, y, bw, 42 }, ICON_FILE_TEXT, "Write files only", BTN_SECONDARY) && prepare(c))
        ui_toast(TOAST_OK, salome_mesher() ? "Wrote salome/stl_mesh.py, mesher/mesher.ini and the STL" : "Wrote the snappyHexMesh, blockMesh and feature dictionaries");
    bx += bw + 10;
    if (!salome_mesher() && STLP.m.mesher == MESHER_SNAPPY && layers_split()) {   /* E3: layer tuning without a remesh */
        char lp[PL_PATH]; pl_join(lp, sizeof lp, c->s.path, SNAP_KEEP "/polyMesh/owner");
        bool have = pl_is_file(lp);
        ui_disable(running || !have);
        bw = ui_button_width(ICON_LAYERS, "Redo the layers only");
        if (ui_button(ui_id("stl-relayer"), (Rectangle){ bx, y, bw, 42 }, ICON_LAYERS, "Redo the layers only", BTN_SECONDARY)) redo_layers(c);
        ui_tooltip((Rectangle){ bx, y, bw, 42 }, have ? "The mesh before the layers (kept from the last Generate) gets the prism layers again with the layer settings "
                                                        "now: minutes instead of a whole remesh. Other changes need Generate mesh."
                                                      : "Generate the mesh once with prism layers: the mesh before the layers is kept, and this redoes only them");
        bx += bw + 10;
    }
    ui_disable(running || !A.env.salome[0]);
    bw = ui_button_width(ICON_EXTERNAL_LINK, "Open in SALOME");
    if (ui_button(ui_id("stl-salome"), (Rectangle){ bx, y, bw, 42 }, ICON_EXTERNAL_LINK, "Open in SALOME", BTN_GHOST)) open_salome(c);
    ui_disable(false);
    y += 56;
    if (!salome_mesher() && STLP.m.mesher == MESHER_SNAPPY) {   /* E4: snappyHexMesh in parallel */
        double np = STLP.m.meshProcs > 1 ? STLP.m.meshProcs : 1;
        ui_text(F_REG, 13.5f, "Processes", x, y + 9, TH.text2);
        char b[16]; snprintf(b, sizeof b, "%g", np);
        if (ui_input(ui_id("stl-nprocs"), (Rectangle){ x + 96, y, 70, 32 }, b, sizeof b, NULL, INPUT_MONO | INPUT_NUMERIC)) {
            char *e; double v = strtod(b, &e);
            if (e == b || *e || !isfinite(v) || v < 1 || v > 1024) ui_toast(TOAST_ERR, "Give a number of processes from 1 to 1024");
            else { STLP.m.meshProcs = (int)v; sm_save_cfg(); }
        }
        char t[200]; pl_fmt_ui(t, sizeof t, "%s (this machine has %d cores)", np > 1 ? "snappyHexMesh in parallel: decomposePar, -parallel, reconstructParMesh"
                                                                                       : "1: snappyHexMesh in serial; more runs it decomposed", A.env.nCpu);
        ui_text_clip(F_REG, 12.5f, t, x + 178, y + 9, w - 178, TH.text3);
        y += 42;
    }
    const char *how = salome_mesher()
        ? "Generate runs salome/stl_mesh.py in SALOME without its window, converts salome/mesh.unv with ideasUnvToFoam (a mesh with "
          "polyhedra, Body Fitting's cut cells, comes as salome/polyMesh instead), sets the boundary types%s and runs checkMesh. Open in SALOME runs the same script in SALOME's window, to look at the groups and the mesh."
        : "Generate writes the dictionaries (the ones there before are kept as .orig; your hand edits to them are kept) and runs "
          "surfaceFeatureExtract, blockMesh for the background, snappyHexMesh (with prism layers in two stages, castellate and snap then "
          "the layers, so Redo the layers only can repeat just them) and checkMesh. Open in SALOME meshes the same STL with SALOME's script instead, in its window.%s";
    char hb[600]; pl_fmt_ui(hb, sizeof hb, how, STLP.m.mesher == MESHER_POLY ? ", turns the tetrahedra into polyhedra (polyDualMesh)" : "");
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
            : !strcmp(id, "ideasUnvToFoam") ? "SALOME made the mesh but OpenFOAM could not convert it: the log says which element or group it stopped at."
            : !strcmp(id, "polyDualMesh") ? "The tetrahedra could not be joined into polyhedra: the Tetrahedral mesher gives a mesh from the same settings."
            : !strcmp(id, "extrudeMesh") ? "The slab could not be cut to one cell: check that the 2-D axis (Flow) is the one the profile is extruded along."
            : !strcmp(id, "surfaceFeatureExtract") || !strcmp(id, "blockMesh") ? "A dictionary the mesher wrote could not be read: Write files only, then look at it in system/ on the Files page."
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
        y += layers_missing_notice(c, x, y, w);
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
    "Load the 3-D model of your part (an STL, or a STEP or IGES file straight from CAD) and check its size is right.",
    "Say whether the fluid goes through the part or around it. That decides the space the simulation fills.",
    "Name the part's surfaces and say what each one is: where the fluid comes in, where it leaves, and the solid walls.",
    "Choose how fine the mesh is: smaller cells give a more accurate answer but take longer. Medium is a good start.",
    "Build the mesh: the space is cut into small cells the flow is solved in. Then look at it, and go on to the physics." };

/* what a step folded says it holds */
static void step_summary(Case *c, int k, char *b, size_t cap)
{
    b[0] = 0;
    if (!STLP.loaded) { if (k == ST_GEOM) pl_strlcpy(b, "Choose the STL to mesh", cap); return; }
    const Stl *s = &STLP.stl;
    if (k == ST_GEOM) {
        char l[32]; double L = 0; for (int q = 0; q < 3; q++) L = fmax(L, s->bmax[q] - s->bmin[q]);
        sm_fmt_len(l, sizeof l, L);
        pl_fmt_ui(b, cap, "%s  ·  %s across  ·  %s", pl_basename(STLP.m.cad[0] ? STLP.m.cad : STLP.m.stl), l, s->openEdges == 0 && s->nonManifold == 0 ? "closed" : "not watertight");
    } else if (k == ST_FLOW) {
        if (STLP.m.domain == DOMAIN_INTERNAL) pl_fmt_ui(b, cap, "Internal flow%s", STLP.m.twoD ? TextFormat(", 2-D across %c", "XYZ"[STLP.m.normal2D]) : "");
        else pl_fmt_ui(b, cap, "External: flow %s, up %s%s%s", DIR_NAME[STLP.m.flowDir], DIR_NAME[mesher_up(&STLP.m)], STLP.m.ground ? ", on the ground" : "",
                       STLP.m.twoD ? TextFormat(", 2-D across %c", "XYZ"[STLP.m.normal2D]) : "");
    } else if (k == ST_BOUND) {
        int in, out, wall, other; boundary_counts(&in, &out, &wall, &other);
        if (STLP.m.domain == DOMAIN_EXTERNAL && !in && !out)
            pl_fmt_ui(b, cap, "%d wall%s; the wind tunnel brings the inlet, outlet and far field", wall, wall == 1 ? "" : "s");
        else pl_fmt_ui(b, cap, "%d inlet%s, %d outlet%s, %d wall%s", in, in == 1 ? "" : "s", out, out == 1 ? "" : "s", wall, wall == 1 ? "" : "s");
    } else if (k == ST_MESH) {
        char bs[32], cl[96]; sm_fmt_len(bs, sizeof bs, STLP.m.base); fmt_cells(cl, sizeof cl, cells_estimate());
        pl_fmt_ui(b, cap, "%s  ·  base %s  ·  %s  ·  %s", MESHERS[STLP.m.mesher].shortName, bs, STLP.m.prism && (MESHERS[STLP.m.mesher].flags & MF_LAYERS) ? TextFormat("%d prism layers", STLP.m.layers) : "no prism layers", cl);
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
    bool open = STLP.open == k, reached = k <= STLP.reached, passed = k < STLP.reached && !open;
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
    if (!reached) pl_fmt_ui(sum, sizeof sum, "after %s", STEP_TITLE[k - 1]);  // NOLINT(clang-analyzer-core.CallAndMessage): false positive, step 0 is always reached, so k >= 1 here
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
    ui_disable(!STLP.loaded);
    float bw = ui_button_width(ICON_ARROW_RIGHT, lab);
    if (ui_button(ui_id("stl-next"), (Rectangle){ x, y, bw, 40 }, ICON_ARROW_RIGHT, lab, BTN_PRIMARY)) step_open(k + 1);
    ui_disable(false);
    if (k + 1 < STLP.reached) {
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
    if (!STLP.loaded) STLP.reached = STLP.open = ST_GEOM;
    float cys[ST__N];
    float bx = x + 52, bw = w - 52;
    for (int k = 0; k < ST__N; k++) {
        if (STLP.open == k && STLP.scrollTo == 1) {             /* the step just opened: its head to the top of the page */
            UiScroll *sc = &A.scr[4];
            ui_scroll_to(sc, y - r.y + sc->y - 16);
            STLP.scrollTo = 0;
        }
        y += step_head(c, k, x, y, w, &cys[k]);
        if (STLP.open == k) {
            y += 2;
            y += note(bx, y, bw, STEP_WHY[k]) + 2;
            /* the part beside the settings (a wide page) or above them, with what this step adds to it */
            /* beside the settings when they keep 640 px of their own, else above them, the page's width */
            bool side = STLP.loaded && (k == ST_GEOM || k == ST_FLOW || k == ST_MESH);
            float sx = bx, sw = bw, top = y, vh = 0;
            if (side) {
                float vw = fminf(620, fmaxf(380, bw * 0.4f));
                if (bw - vw - 20 >= 640) { sw = bw - vw - 20; vh = fminf(440, vw * 0.85f); sm_view_panel((Rectangle){ bx + sw + 20, y, vw, vh }); }
                else { sm_view_panel((Rectangle){ bx, y, bw, fminf(360, bw * 0.5f) }); y += fminf(360, bw * 0.5f) + 14; top = y; }
            }
            if (k == ST_GEOM) y += geometry_step(c, sx, y, sw);
            else if (k == ST_FLOW) y += flow_step(sx, y, sw);
            else if (k == ST_BOUND) y += boundaries_step(c, bx, y, bw);
            else if (k == ST_MESH) y += mesh_step(sx, y, sw);
            else y += generate_step(c, bx, y, bw);
            if (STLP.loaded) y += 4 + step_next(k, sx, y + 4, sw);
            if (side && vh > 0 && y < top + vh + 10) y = top + vh + 10;   /* the next head below the view, not under it */
        }
        y += 10;
    }
    if (STLP.scrollTo == 2) { STLP.scrollTo = 1; ui_request_frames(2); }
    /* the line joining the steps, the part done in the accent */
    for (int k = 0; k + 1 < ST__N; k++) {
        float a = cys[k] + 17, b = cys[k + 1] - 17;
        if (b > a) ui_line(x + 22, a, x + 22, b, 2, k < STLP.reached ? ui_alpha(TH.accent, 0.7f) : TH.lineSoft);
    }
    return y - y0;
}

bool stlmesh_two_d(Case *c)
{
    sync_case(c);
    return STLP.loaded && STLP.m.twoD;
}

/* a Setup template picked before meshing: the mesh it wants (prism layers on the walls for the boundary layer) */
bool stlmesh_template_hints(Case *c, bool layers)
{
    sync_case(c);
    if (!STLP.loaded || c->s.meshed || !layers || STLP.m.prism) return false;
    STLP.m.prism = true;
    sm_save_cfg();
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
    if (!STLP.loaded) return false;
    if (cell) *cell = mesher_layer_cell(&STLP.m);
    if (!STLP.m.prism || STLP.m.layers < 1) return false;
    if (first) *first = first_layer(&STLP.m);
    if (n) *n = STLP.m.layers;
    return true;
}

bool stlmesh_set_wall_layers(Case *c, double y1, double *first, int *n)
{
    sync_case(c);
    if (!STLP.loaded || !(y1 > 0)) return false;
    double h = mesher_layer_cell(&STLP.m), r = STLP.m.stretch > 1.0001 ? STLP.m.stretch : 1.2, total;
    int k = sim_layer_count(y1, 0.5 * h, r, &total);
    if (k >= 60) return false;
    MeshCfg m = STLP.m;
    m.prism = true; m.layers = k; m.stretch = r; m.prismPct = 100 * total / m.base;
    if (first) *first = first_layer(&m);
    if (n) *n = k;
    STLP.m = m;
    sm_save_cfg();
    return true;
}

bool stlmesh_gravity(Case *c, double g[3])
{
    sync_case(c);
    if (!STLP.loaded) return false;
    int up = mesher_up(&STLP.m);                               /* the tunnel's Up (or CAD's z): gravity points the other way */
    g[0] = g[1] = g[2] = 0;
    g[up / 2] = up % 2 ? 9.81 : -9.81;
    return true;
}

int stlmesh_up_axis(Case *c)
{
    sync_case(c);
    if (!STLP.loaded) return -1;
    return STLV.up >= 0 ? STLV.up : mesher_up(&STLP.m) / 2;
}
